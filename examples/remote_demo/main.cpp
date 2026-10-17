/// remote_demo：M3-W4 双端远程演示（docs/13 §5-W4）。
///
/// 两种模式：
///  - `--serve`：进程内起 rtdbd 核心（`server::Server`），可选预置演示表 +
///    周期写入，供另一进程的 `remote_demo` 订阅观察；
///  - 默认（客户端模式）：`rtdb_client` 连接 + 订阅打印，演示回调交付、
///    断线重连续传与溢出重取（整表 SCAN + `ResetTo`）路径。
///
/// 运行示例（两个终端）：
///   remote_demo --serve --listen 127.0.0.1:5599 --rows 3 --write-ms 300
///   remote_demo --host 127.0.0.1 --port 5599 --seconds 10
///
/// 参数默认值与 `--serve` 预置一致（盐/用户/表名），直接跑即可互通。

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rtdb/rtdb_client.hpp"
#include "server/server.hpp"

namespace {

std::atomic<bool> g_stop{false};

extern "C" void OnSignal(int) { g_stop.store(true); }

struct DemoArgs {
    // 服务端模式
    bool serve = false;
    std::string path = "remote_demo.img";
    std::string listen = "127.0.0.1:5599";
    std::string salt = "demo-salt";
    std::string user = "demo";
    std::string password = "demo123";
    uint32_t rows = 0;        ///< 预置行数
    uint32_t write_ms = 500;  ///< 周期写间隔（0 = 不写）
    // 客户端模式
    std::string host = "127.0.0.1";
    uint16_t port = 5599;
    std::string table = "demo";
    uint32_t seconds = 0;  ///< 运行秒数（0 = 直到 Ctrl+C）
    uint32_t exit_after_ms = 0;
};

void PrintUsage() {
    std::printf(
        "用法：remote_demo [选项]\n"
        "  服务端模式：--serve [--path 文件] [--listen host:port] [--salt 盐]\n"
        "              [--user 名=口令] [--rows N] [--write-ms 毫秒] [--exit-after-ms 毫秒]\n"
        "  客户端模式：[--host 主机] [--port 端口] [--user 名] [--password 口令]\n"
        "              [--salt 盐] [--table 表名] [--seconds N]\n");
}

bool ParseArgs(int argc, char** argv, DemoArgs* a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](std::string* out) -> bool {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        std::string v;
        if (k == "--serve") {
            a->serve = true;
        } else if (k == "--path" && next(&v)) {
            a->path = v;
        } else if (k == "--listen" && next(&v)) {
            a->listen = v;
        } else if (k == "--salt" && next(&v)) {
            a->salt = v;
        } else if (k == "--table" && next(&v)) {
            a->table = v;
        } else if (k == "--password" && next(&v)) {
            a->password = v;
        } else if (k == "--host" && next(&v)) {
            a->host = v;
        } else if (k == "--port" && next(&v)) {
            a->port = static_cast<uint16_t>(std::stoi(v));
        } else if (k == "--rows" && next(&v)) {
            a->rows = static_cast<uint32_t>(std::stoi(v));
        } else if (k == "--write-ms" && next(&v)) {
            a->write_ms = static_cast<uint32_t>(std::stoi(v));
        } else if (k == "--seconds" && next(&v)) {
            a->seconds = static_cast<uint32_t>(std::stoi(v));
        } else if (k == "--exit-after-ms" && next(&v)) {
            a->exit_after_ms = static_cast<uint32_t>(std::stoi(v));
        } else if (k == "--user" && next(&v)) {
            const size_t eq = v.find('=');
            if (eq == std::string::npos) {
                a->user = v;  // 客户端模式：仅用户名
            } else {
                a->user = v.substr(0, eq);
                a->password = v.substr(eq + 1);
            }
        } else if (k == "--help" || k == "-h") {
            PrintUsage();
            return false;
        } else {
            std::fprintf(stderr, "未知参数：%s\n", k.c_str());
            PrintUsage();
            return false;
        }
    }
    return true;
}

rtdb::TableDef MakeDemoTableDef() {
    rtdb::TableDef def;
    def.name = "demo";
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", rtdb::DataType::kFloat64, 0, true, false});
    def.columns.push_back({"msg", rtdb::DataType::kUtf8Var, 0, true, false});
    def.max_rows_hint = 2048;
    return def;
}

// ---------------- 服务端模式 ----------------

int RunServe(const DemoArgs& a) {
    rtdb::server::Config cfg;
    cfg.instance = "remote_demo";
    cfg.path = a.path;
    cfg.initial_size_mb = 64;
    cfg.fsync = rtdb::FsyncPolicy::PerSecond;
    cfg.auth_salt = a.salt;
    cfg.max_sessions = 16;
    if (!rtdb::IsOk(rtdb::server::ParseListen(a.listen, &cfg.listen_host, &cfg.listen_port))) {
        std::fprintf(stderr, "监听地址非法：%s\n", a.listen.c_str());
        return 1;
    }
    rtdb::server::UserEntry u;
    u.name = a.user;
    u.digest = rtdb::server::HashPassword(cfg.auth_salt, a.password);
    cfg.users.push_back(u);

    rtdb::server::Server srv;
    const rtdb::Err se = srv.Start(cfg);
    if (!rtdb::IsOk(se)) {
        std::fprintf(stderr, "服务端启动失败：%s\n", rtdb::ErrorMessage(se));
        return 1;
    }
    std::printf("rtdbd 核心已监听 127.0.0.1:%u（映像 %s）\n", static_cast<unsigned>(srv.port()),
                a.path.c_str());

    // 演示表（重跑复用）。
    auto ct = srv.engine()->CreateTable(MakeDemoTableDef());
    if (!ct.IsOk() && ct.Error() != rtdb::Err::kTableExists) {
        std::fprintf(stderr, "建表失败：%s\n", rtdb::ErrorMessage(ct.Error()));
        srv.Stop();
        return 1;
    }
    auto th = srv.engine()->OpenTable("demo");
    if (!th.IsOk()) {
        std::fprintf(stderr, "打开演示表失败\n");
        srv.Stop();
        return 1;
    }
    for (uint32_t i = 1; i <= a.rows; ++i) {
        (void)srv.engine()->Insert(
            th.Value(), {rtdb::ColumnValue::Int(static_cast<int64_t>(i)),
                         rtdb::ColumnValue::Real(static_cast<double>(i) * 10.0),
                         rtdb::ColumnValue::Text("预置行 " + std::to_string(i))});
    }
    std::printf("演示表 demo 就绪（预置 %u 行；周期写 %u ms）\n", a.rows, a.write_ms);

    std::thread writer;
    if (a.write_ms > 0) {
        writer = std::thread([&srv, &th, &a] {
            int64_t next = static_cast<int64_t>(a.rows) + 1;
            const auto t0 = std::chrono::steady_clock::now();
            while (!g_stop.load()) {
                const double sec =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const double val = static_cast<double>(next) + sec;  // 变值便于观察
                const rtdb::Err e = srv.engine()
                                        ->Insert(th.Value(),
                                                 {rtdb::ColumnValue::Int(next),
                                                  rtdb::ColumnValue::Real(val),
                                                  rtdb::ColumnValue::Text(
                                                      "周期行 " + std::to_string(next))})
                                        .Error();
                if (rtdb::IsOk(e)) ++next;
                std::this_thread::sleep_for(std::chrono::milliseconds(a.write_ms));
            }
        });
    }

    const auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (a.exit_after_ms != 0 &&
            std::chrono::steady_clock::now() - t0 >=
                std::chrono::milliseconds(a.exit_after_ms)) {
            break;
        }
    }
    g_stop.store(true);
    if (writer.joinable()) writer.join();
    std::printf("退出中（会话收敛 + 终刷）…\n");
    srv.Stop();
    const rtdb::server::Server::Stats st = srv.stats();
    std::printf("服务端统计：连接 %llu、认证成功 %llu、请求 %llu、帧违例 %llu、载荷错误 %llu\n",
                static_cast<unsigned long long>(st.accepted),
                static_cast<unsigned long long>(st.auth_ok),
                static_cast<unsigned long long>(st.requests),
                static_cast<unsigned long long>(st.frame_errors),
                static_cast<unsigned long long>(st.payload_errors));
    return 0;
}

// ---------------- 客户端模式 ----------------

const char* KindName(rtdb::ChangeEvent::Kind k) {
    switch (k) {
        case rtdb::ChangeEvent::Kind::kInsert:
            return "insert";
        case rtdb::ChangeEvent::Kind::kUpdate:
            return "update";
        case rtdb::ChangeEvent::Kind::kDelete:
            return "delete";
    }
    return "?";
}

std::string ValueToStr(const rtdb::ColumnValue& v) {
    if (v.is_null()) return "NULL";
    switch (v.type()) {
        case rtdb::DataType::kFloat32:
        case rtdb::DataType::kFloat64:
            return std::to_string(v.as_real());
        case rtdb::DataType::kUtf8Var:
        case rtdb::DataType::kBytesVar:
            return std::string(v.as_bytes());
        default:
            return std::to_string(v.as_int());
    }
}

std::string RowToStr(const std::vector<rtdb::ColumnValue>& row) {
    std::string s = "[";
    for (size_t i = 0; i < row.size(); ++i) {
        if (i != 0) s += ", ";
        s += ValueToStr(row[i]);
    }
    return s + "]";
}

int RunClient(const DemoArgs& a) {
    rtdb::ClientOptions o;
    o.host = a.host;
    o.port = a.port;
    o.user = a.user;
    o.password = a.password;
    o.auth_salt = a.salt;

    auto cr = rtdb::Client::Connect(o);
    if (!cr.IsOk()) {
        std::fprintf(stderr, "连接失败（%s:%u）：%s\n", a.host.c_str(), static_cast<unsigned>(a.port),
                     rtdb::ErrorMessage(cr.Error()));
        std::fprintf(stderr, "提示：可先在另一终端执行 remote_demo --serve\n");
        return 1;
    }
    std::unique_ptr<rtdb::Client> client = cr.TakeValue();
    std::printf("已连接 %s:%u（会话 %u）\n", a.host.c_str(), static_cast<unsigned>(a.port),
                client->session());

    auto ping = client->Ping();
    if (ping.IsOk()) {
        std::printf("PING：committed_seq=%lld 心跳=%llu ttl=%u ms\n",
                    static_cast<long long>(ping.Value().committed_seq),
                    static_cast<unsigned long long>(ping.Value().writer_heartbeat_ns),
                    ping.Value().session_ttl_ms);
    }
    auto desc = client->DescribeTable(a.table);
    if (!desc.IsOk()) {
        std::fprintf(stderr, "描述表 %s 失败：%s\n", a.table.c_str(),
                     rtdb::ErrorMessage(desc.Error()));
        return 1;
    }
    std::printf("表 %s：%zu 列（", a.table.c_str(), desc.Value().col_names.size());
    for (size_t i = 0; i < desc.Value().col_names.size(); ++i) {
        std::printf("%s%s", i == 0 ? "" : ", ", desc.Value().col_names[i].c_str());
    }
    std::printf("）\n");

    std::atomic<uint64_t> received{0};
    rtdb::SubscriptionOptions so;
    so.tables = {a.table};
    so.delta_wait_ms = 1000;
    so.on_state = [](bool ready, uint32_t attempt) {
        if (ready) {
            std::printf("订阅就绪\n");
        } else {
            std::printf("订阅重连中（第 %u 次）…\n", attempt);
        }
    };
    so.on_change = [&received](const rtdb::ChangeEvent& ev) {
        const uint64_t n = received.fetch_add(1) + 1;
        if (ev.kind == rtdb::ChangeEvent::Kind::kDelete) {
            std::printf("#%llu seq=%llu %s 表=%s 主键=%s\n", static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(ev.seq), KindName(ev.kind),
                        ev.table.c_str(), ValueToStr(ev.key).c_str());
        } else {
            std::printf("#%llu seq=%llu %s 表=%s 行=%s\n", static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(ev.seq), KindName(ev.kind),
                        ev.table.c_str(), RowToStr(ev.row).c_str());
        }
    };
    so.on_overflow = [&client, &a](rtdb::Subscription& sub) {
        std::printf("订阅游标越窗（历史被环回收）：整表重取 + 重置游标\n");
        uint64_t committed = 0;
        auto p = client->Ping();
        if (p.IsOk() && p.Value().committed_seq >= 0) {
            committed = static_cast<uint64_t>(p.Value().committed_seq);
        }
        uint64_t rows = 0;
        auto s = client->ScanPaged(a.table, 256, [&rows](const rtdb::ClientScanResult& page) {
            rows += page.rows.size();
            return true;
        });
        if (s.IsOk()) {
            std::printf("重扫完成：%llu 行；游标重置到 seq=%llu\n",
                        static_cast<unsigned long long>(s.Value()),
                        static_cast<unsigned long long>(committed));
        } else {
            std::fprintf(stderr, "重扫失败：%s\n", rtdb::ErrorMessage(s.Error()));
        }
        sub.ResetTo(committed);
    };

    auto sr = client->Subscribe(so);
    if (!sr.IsOk()) {
        std::fprintf(stderr, "订阅失败：%s\n", rtdb::ErrorMessage(sr.Error()));
        return 1;
    }
    std::unique_ptr<rtdb::Subscription> sub = sr.TakeValue();
    if (!sub->WaitReady(5000)) {
        std::fprintf(stderr, "订阅未在预算内就绪：%s\n", sub->last_error().c_str());
    }

    const auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (a.seconds != 0 &&
            std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(a.seconds)) {
            break;
        }
    }
    std::printf("已收变更 %llu 条；关闭订阅与连接…\n",
                static_cast<unsigned long long>(received.load()));
    sub->Close();
    client->Close();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    DemoArgs a;
    if (!ParseArgs(argc, argv, &a)) return 1;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    return a.serve ? RunServe(a) : RunClient(a);
}
