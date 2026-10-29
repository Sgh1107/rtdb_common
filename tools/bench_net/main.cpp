/// bench_net：M3 网络基准（docs/13 §5-W5①/§6 性能面）。
///
/// 测量面（回环默认；`--host/--port` 可对远端 rtdbd）：
///  1. 列式块写吞吐：`UpsertBatch`（RecordBlock，batch 行/请求，同键复用）；
///  2. ops 批写吞吐：`ExecuteBatch`（batch 条/请求）；
///  3. 单条写 / PING 往返延迟分布（p50/p99/max，µs）；
///  4. 订阅交付延迟：写 1 行 → 变更回调到达（p50/p99/max，µs）；
///  5. 分页扫描吞吐（信息项）。
///
/// 输出 = `BENCHNET ...` 行（docs/14 证据采集口径）。`--ci` 引入保守下限
/// 断言（回环 ≥50k 行/s、单条写/PING P99 ≤5ms 量级；消毒构建由
/// ASAN_OPTIONS/TSAN_OPTIONS/UBSAN_OPTIONS 环境变量自动放宽），退出码
/// 非 0 表示未达标；非 CI 运行仅打印（信息），不做门禁。
///
/// 用法：
///   bench_net [--rows N] [--batch K] [--ops-rows M] [--write-samples S]
///             [--sub-samples S] [--ping-samples S] [--page-rows P]
///             [--table T] [--image FILE] [--keep] [--ci] [--relaxed]
///             [--host H --port P --user U --password PW --salt S]
///
/// 默认：rows=200000 batch=512（CI：rows=50000）——同键覆盖写，槽位占用
/// = 2×batch（块键 + ops 键）+ 延迟样本数（表 row_cap 固定 2048）。
/// 批行数上限受 WAL 记录区（64KB）约束：3×int64 表实测 512 行安全、
/// 768 行即 306（kLimitExceeded），故默认保守取 512。

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rtdb/record_block.hpp"
#include "rtdb/rtdb_client.hpp"
#include "server/config.hpp"
#include "server/server.hpp"

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

// 行数据区上限为 64KB（schema.hpp kMaxRowDataBytes）；3×int64 + null 位图
// 经 PlanRowLayout 得 rec_fixed_len=32（含对齐），32B × 2048 = 64KB 恰好
// 合法（2730 行即超限返回 306）。槽位占用 = batch + 延迟样本数。
constexpr uint32_t kRowCap = 2048;

struct Args {
    uint64_t rows = 200000;
    uint64_t ops_rows = 0;  // 0 = rows/5
    uint32_t batch = 512;
    uint32_t ping_samples = 200;
    uint32_t write_samples = 200;
    uint32_t sub_samples = 100;
    uint32_t page_rows = 256;
    bool no_sub = false;
    bool keep = false;
    bool ci = false;
    bool relaxed = false;  // 显式放宽 CI 阈值（Debug/覆盖率构建由 CMake 传 --relaxed）
    bool external = false;
    bool rows_set = false;
    bool ops_rows_set = false;
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    std::string user = "bench";
    std::string password = "bench-pw";
    std::string salt = "bench-salt";
    std::string table = "bench_net";
    std::string image;
};

bool ParseArgs(int argc, char** argv, Args* a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        // CMake `$<CONFIG:Debug>` 空展开会传入空参数（如 `--ci ""`），忽略之。
        if (k.empty()) continue;
        auto next = [&](std::string* out) -> bool {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        auto num = [&](uint64_t* out) -> bool {
            std::string v;
            if (!next(&v)) return false;
            char* end = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            if (end == nullptr || *end != '\0') return false;
            *out = n;
            return true;
        };
        std::string v;
        uint64_t n = 0;
        if (k == "--rows" && num(&n)) {
            a->rows = n;
            a->rows_set = true;
        } else if (k == "--ops-rows" && num(&n)) {
            a->ops_rows = n;
            a->ops_rows_set = true;
        } else if (k == "--batch" && num(&n)) {
            a->batch = static_cast<uint32_t>(n);
        } else if (k == "--ping-samples" && num(&n)) {
            a->ping_samples = static_cast<uint32_t>(n);
        } else if (k == "--write-samples" && num(&n)) {
            a->write_samples = static_cast<uint32_t>(n);
        } else if (k == "--sub-samples" && num(&n)) {
            a->sub_samples = static_cast<uint32_t>(n);
        } else if (k == "--page-rows" && num(&n)) {
            a->page_rows = static_cast<uint32_t>(n);
        } else if (k == "--no-sub") {
            a->no_sub = true;
        } else if (k == "--keep") {
            a->keep = true;
        } else if (k == "--ci") {
            a->ci = true;
        } else if (k == "--relaxed") {
            a->relaxed = true;
        } else if (k == "--host" && next(&v)) {
            a->host = v;
            a->external = true;
        } else if (k == "--port" && num(&n)) {
            a->port = static_cast<uint16_t>(n);
            a->external = true;
        } else if (k == "--user" && next(&v)) {
            a->user = v;
        } else if (k == "--password" && next(&v)) {
            a->password = v;
        } else if (k == "--salt" && next(&v)) {
            a->salt = v;
        } else if (k == "--table" && next(&v)) {
            a->table = v;
        } else if (k == "--image" && next(&v)) {
            a->image = v;
        } else if (k == "--help" || k == "-h") {
            std::printf(
                "用法：bench_net [--rows N] [--batch K] [--ops-rows M] [--ping-samples S]\n"
                "        [--write-samples S] [--sub-samples S] [--page-rows P] [--no-sub]\n"
                "        [--table T] [--image FILE] [--keep] [--ci] [--relaxed]\n"
                "        [--host H --port P --user U --password PW --salt S]\n");
            return false;
        } else {
            std::fprintf(stderr, "未知参数：%s\n", k.c_str());
            return false;
        }
    }
    if (a->batch == 0 || a->batch > 2048) {
        std::fprintf(stderr, "batch 须在 1..2048（row_cap 预算）\n");
        return false;
    }
    if (a->external && a->port == 0) {
        std::fprintf(stderr, "--host/--port 外部模式须给端口\n");
        return false;
    }
    if (static_cast<uint64_t>(a->batch) * 2 + a->write_samples + a->sub_samples + 32 > kRowCap) {
        std::fprintf(stderr, "键位预算超限：2×batch+样本+32 须 ≤ %u（当前 %llu）\n", kRowCap,
                     static_cast<unsigned long long>(a->batch) * 2 + a->write_samples +
                         a->sub_samples + 32);
        return false;
    }
    if (a->rows < a->batch) a->rows = a->batch;
    if (a->ops_rows == 0) a->ops_rows = a->rows / 5;
    return true;
}

/// 环境变量非空判定（MSVC 下用 `_dupenv_s` 避开 getenv 的 C4996；
/// 同 `src/infra/failpoint.cpp` 口径）。
bool EnvNonEmpty(const char* name) {
#if defined(_MSC_VER)
    char* buf = nullptr;
    size_t len = 0;
    if (::_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return false;
    const bool nonempty = buf[0] != '\0';
    std::free(buf);
    return nonempty;
#else
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0';
#endif
}

/// 消毒构建探测：CI 里 ASan/TSan/UBSan 作业设置对应环境变量；此类构建
/// 慢 2~10×，性能断言自动放宽（仅防"完全失效"）。Debug/覆盖率（-O0）
/// 构建经 `--relaxed` 显式放宽（CMake `$<CONFIG:Debug>` 传递）。
bool SanitizerBuild() {
    const char* names[] = {"ASAN_OPTIONS", "TSAN_OPTIONS", "UBSAN_OPTIONS", "MSAN_OPTIONS"};
    for (const char* n : names) {
        if (EnvNonEmpty(n)) return true;
    }
    return false;
}

double Pct(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(idx, sorted.size() - 1)];
}

double MaxOf(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return *std::max_element(v.begin(), v.end());
}

void PrintLat(const char* name, std::vector<double>& samples) {
    if (samples.empty()) {
        std::printf("BENCHNET %s n=0\n", name);
        return;
    }
    std::sort(samples.begin(), samples.end());
    std::printf("BENCHNET %s p50=%.1f p99=%.1f max=%.1f (n=%zu)\n", name, Pct(samples, 0.50),
                Pct(samples, 0.99), MaxOf(samples), samples.size());
}

/// 订阅交付收集器（写入串行 → last_id 单调判据）。
struct SubLat {
    std::mutex mu;
    std::condition_variable cv;
    int64_t last_id = 0;
    uint64_t count = 0;

    void On(const rtdb::ChangeEvent& ev) {
        std::lock_guard<std::mutex> lk(mu);
        ++count;
        if (!ev.row.empty()) last_id = ev.row[0].as_int();
        cv.notify_all();
    }

    bool WaitId(int64_t id, uint32_t budget_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(budget_ms),
                           [&] { return last_id >= id; });
    }
};

int Fail(const char* what) {
    std::printf("BENCHNET RESULT FAIL (%s)\n", what);
    std::fflush(stdout);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!ParseArgs(argc, argv, &a)) return 2;
    if (a.ci) {
        // CI 保守规模：吞吐量级判据不依赖大样本（仅在未显式指定时缩小）。
        if (!a.rows_set && a.rows == 200000) a.rows = 50000;
        if (!a.ops_rows_set && a.ops_rows == 40000) a.ops_rows = 10000;
        if (a.ping_samples == 200) a.ping_samples = 100;
        if (a.write_samples == 200) a.write_samples = 100;
        if (a.sub_samples == 100) a.sub_samples = 50;
    }
    const bool relaxed = a.relaxed || SanitizerBuild();
    if (a.image.empty()) a.image = "rtdb_bench_net.img";

    std::printf("BENCHNET config rows=%llu ops_rows=%llu batch=%u table=%s mode=%s%s\n",
                static_cast<unsigned long long>(a.rows),
                static_cast<unsigned long long>(a.ops_rows), a.batch, a.table.c_str(),
                a.external ? "external" : "inproc", relaxed ? " relaxed=1" : "");

    // ---------------- 服务端（进程内）或外部端点 ----------------
    rtdb::server::Server srv;
    bool srv_started = false;
    if (!a.external) {
        std::error_code ec;
        fs::remove(a.image, ec);  // 基准自持映像：每次重建
        fs::remove(a.image + ".snap", ec);
        rtdb::server::Config cfg;
        cfg.instance = "bench_net";
        cfg.path = a.image;
        cfg.initial_size_mb = 128;
        cfg.fsync = rtdb::FsyncPolicy::Never;
        cfg.auth_salt = a.salt;
        cfg.max_sessions = 16;
        rtdb::server::UserEntry u;
        u.name = a.user;
        u.digest = rtdb::server::HashPassword(cfg.auth_salt, a.password);
        cfg.users.push_back(u);
        const rtdb::Err se = srv.Start(cfg);
        if (!rtdb::IsOk(se)) {
            std::fprintf(stderr, "服务端启动失败：%s（映像 %s）\n", rtdb::ErrorMessage(se),
                         a.image.c_str());
            return 2;
        }
        srv_started = true;
        a.host = "127.0.0.1";
        a.port = srv.port();
    }

    rtdb::ClientOptions co;
    co.host = a.host;
    co.port = a.port;
    co.user = a.user;
    co.password = a.password;
    co.auth_salt = a.salt;
    co.connect_timeout_ms = 3000;
    co.request_timeout_ms = 10000;  // 大批应答留裕量

    auto cr = rtdb::Client::Connect(co);
    if (!cr.IsOk()) {
        std::fprintf(stderr, "客户端连接失败：%s\n", rtdb::ErrorMessage(cr.Error()));
        if (srv_started) srv.Stop();
        return 2;
    }
    std::unique_ptr<rtdb::Client> c = cr.TakeValue();

    // ---------------- 建表（可复用） ----------------
    rtdb::TableDef def;
    def.name = a.table;
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"v1", rtdb::DataType::kInt64, 0, true, false});
    def.columns.push_back({"v2", rtdb::DataType::kInt64, 0, true, false});
    def.row_cap = kRowCap;
    auto ct = c->CreateTable(def);
    if (!ct.IsOk() && ct.Error() != rtdb::Err::kTableExists) {
        std::fprintf(stderr, "建表失败：%s\n", rtdb::ErrorMessage(ct.Error()));
        if (srv_started) srv.Stop();
        return 2;
    }

    // ---------------- 1) 列式块写吞吐 ----------------
    std::vector<int64_t> ids(a.batch), v1(a.batch), v2(a.batch);
    for (uint32_t i = 0; i < a.batch; ++i) {
        ids[i] = i;
        v1[i] = static_cast<int64_t>(i) * 2;
        v2[i] = static_cast<int64_t>(i) * 3;
    }
    rtdb::RecordBlock blk(a.table, a.batch);
    blk.AppendColumn("id", ids).AppendColumn("v1", v1).AppendColumn("v2", v2);
    if (!blk.IsValid()) {
        std::fprintf(stderr, "块构建失败：%s\n", rtdb::ErrorMessage(blk.Error()));
        if (srv_started) srv.Stop();
        return 2;
    }

    const uint64_t block_iters = a.rows / a.batch;
    for (uint64_t i = 0; i < 10 && i < block_iters; ++i) {  // 预热
        auto r = c->UpsertBatch(blk.View());
        if (!r.IsOk()) {
            std::fprintf(stderr, "块写预热失败：%s\n", rtdb::ErrorMessage(r.Error()));
            return Fail("block warmup");
        }
    }
    double block_rps = 0.0;
    {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < block_iters; ++i) {
            auto r = c->UpsertBatch(blk.View());
            if (!r.IsOk()) {
                std::fprintf(stderr, "块写失败：%s\n", rtdb::ErrorMessage(r.Error()));
                return Fail("block upsert");
            }
        }
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        const uint64_t rows = block_iters * a.batch;
        block_rps = secs > 0 ? static_cast<double>(rows) / secs : 0.0;
        std::printf("BENCHNET block rows_per_sec=%.0f req_per_sec=%.0f rows=%llu secs=%.4f\n",
                    block_rps, secs > 0 ? static_cast<double>(block_iters) / secs : 0.0,
                    static_cast<unsigned long long>(rows), secs);
    }

    // ---------------- 2) ops 批写吞吐 ----------------
    // 独立键区间（块键 + batch 偏移）避开与块阶段的主键冲突；首轮 insert
    // 建行，后续轮转 update 覆盖写（同键复用不耗槽位）。
    std::vector<rtdb::ClientBatchOp> ops(a.batch);
    for (uint32_t i = 0; i < a.batch; ++i) {
        ops[i].type = rtdb::ClientBatchOp::Type::kInsert;
        ops[i].table = a.table;
        ops[i].row = {rtdb::ColumnValue::Int(static_cast<int64_t>(a.batch) + ids[i]),
                      rtdb::ColumnValue::Int(v1[i]), rtdb::ColumnValue::Int(v2[i])};
    }
    const uint64_t ops_iters = a.ops_rows / a.batch;
    double ops_rps = 0.0;
    {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < ops_iters; ++i) {
            auto r = c->ExecuteBatch(ops);
            if (!r.IsOk()) {
                std::fprintf(stderr, "ops 批写失败：%s\n", rtdb::ErrorMessage(r.Error()));
                return Fail("ops batch");
            }
            if (i == 0) {
                for (auto& op : ops) op.type = rtdb::ClientBatchOp::Type::kUpdate;
            }
        }
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        const uint64_t rows = ops_iters * a.batch;
        ops_rps = secs > 0 ? static_cast<double>(rows) / secs : 0.0;
        std::printf("BENCHNET ops rows_per_sec=%.0f req_per_sec=%.0f rows=%llu secs=%.4f\n",
                    ops_rps, secs > 0 ? static_cast<double>(ops_iters) / secs : 0.0,
                    static_cast<unsigned long long>(rows), secs);
    }

    // ---------------- 3) PING 往返延迟 ----------------
    // CI 共享 runner 偶发调度/IO 抖动（一次 8~40ms 离群即把 p99 拉高）：
    // --ci 模式测 3 轮取 p99 最优，避免单次噪声误报；真实回归会在 3 轮都
    // 超阈值时仍失败。非 CI / 本机运行保持单轮口径。
    // 注意：不在阶段间插入固定 sleep——组提交 per_second flush 周期约 1s，
    // 固定等待反而容易让计时窗口与后台回写重叠（本机实测会抬高 p50）。
    bool lat_failed = false;
    std::string lat_fail_name;
    int64_t lat_id = static_cast<int64_t>(a.batch) * 2 + 10;
    auto measure_ping = [&]() {
        std::vector<double> v;
        v.reserve(a.ping_samples);
        for (uint32_t i = 0; i < a.ping_samples; ++i) {
            const auto t0 = Clock::now();
            auto r = c->Ping();
            const auto dt = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            if (!r.IsOk()) {
                lat_failed = true;
                lat_fail_name = "ping";
                break;
            }
            v.push_back(dt);
        }
        return v;
    };
    auto measure_write = [&]() {
        std::vector<double> v;
        v.reserve(a.write_samples);
        for (uint32_t i = 0; i < a.write_samples; ++i, ++lat_id) {
            rtdb::ClientBatchOp op;
            op.type = rtdb::ClientBatchOp::Type::kInsert;
            op.table = a.table;
            op.row = {rtdb::ColumnValue::Int(lat_id), rtdb::ColumnValue::Int(lat_id),
                      rtdb::ColumnValue::Int(lat_id)};
            const auto t0 = Clock::now();
            auto r = c->ExecuteBatch({op});
            const auto dt = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            if (!r.IsOk()) {
                lat_failed = true;
                lat_fail_name = "write1";
                break;
            }
            v.push_back(dt);
        }
        return v;
    };
    auto sorted_p99 = [](std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        return Pct(v, 0.99);
    };

    std::vector<double> ping_us = measure_ping();
    if (lat_failed) return Fail(lat_fail_name.c_str());
    std::vector<double> write_us = measure_write();
    if (lat_failed) return Fail(lat_fail_name.c_str());

    std::vector<double> ping_rounds{sorted_p99(ping_us)};
    std::vector<double> write_rounds{sorted_p99(write_us)};
    if (a.ci) {
        for (int round = 0; round < 2 && !lat_failed; ++round) {
            std::vector<double> p = measure_ping();
            if (lat_failed) break;
            std::vector<double> w = measure_write();
            if (lat_failed) break;
            const double pp = sorted_p99(p);
            const double wp = sorted_p99(w);
            ping_rounds.push_back(pp);
            write_rounds.push_back(wp);
            if (pp < ping_rounds.front()) ping_us = std::move(p);
            if (wp < write_rounds.front()) write_us = std::move(w);
        }
        if (lat_failed) return Fail(lat_fail_name.c_str());
        std::printf("BENCHNET ci latency rounds ping_p99=[");
        for (size_t i = 0; i < ping_rounds.size(); ++i) {
            std::printf("%s%.1f", i == 0 ? "" : ",", ping_rounds[i]);
        }
        std::printf("] write_p99=[");
        for (size_t i = 0; i < write_rounds.size(); ++i) {
            std::printf("%s%.1f", i == 0 ? "" : ",", write_rounds[i]);
        }
        std::printf("] chosen=best\n");
    }
    PrintLat("ping_us", ping_us);
    PrintLat("write1_us", write_us);

    // ---------------- 5) 订阅交付延迟 ----------------
    double sub_p99_us = 0.0;
    SubLat sublat;
    std::unique_ptr<rtdb::Subscription> sub;
    if (!a.no_sub && a.sub_samples > 0) {
        rtdb::SubscriptionOptions so;
        so.tables = {a.table};
        so.delta_wait_ms = 1000;
        so.on_change = [&sublat](const rtdb::ChangeEvent& ev) { sublat.On(ev); };
        auto sr = c->Subscribe(so);
        if (!sr.IsOk()) {
            std::fprintf(stderr, "订阅失败：%s\n", rtdb::ErrorMessage(sr.Error()));
            return Fail("subscribe");
        }
        sub = sr.TakeValue();
        if (!sub->WaitReady(5000)) {
            std::fprintf(stderr, "订阅未就绪：%s\n", sub->last_error().c_str());
            return Fail("subscribe ready");
        }
        std::vector<double> sub_us;
        sub_us.reserve(a.sub_samples);
        for (uint32_t i = 0; i < a.sub_samples; ++i, ++lat_id) {
            rtdb::ClientBatchOp op;
            op.type = rtdb::ClientBatchOp::Type::kInsert;
            op.table = a.table;
            op.row = {rtdb::ColumnValue::Int(lat_id), rtdb::ColumnValue::Int(lat_id),
                      rtdb::ColumnValue::Int(lat_id)};
            const auto t0 = Clock::now();
            auto r = c->ExecuteBatch({op});
            if (!r.IsOk()) return Fail("sub write");
            if (!sublat.WaitId(lat_id, 5000)) return Fail("sub deliver timeout");
            const auto dt = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            sub_us.push_back(dt);
        }
        std::sort(sub_us.begin(), sub_us.end());
        sub_p99_us = Pct(sub_us, 0.99);
        PrintLat("sub_us", sub_us);
        std::printf("BENCHNET sub_delivered n=%llu p99=%.1f\n",
                    static_cast<unsigned long long>(sublat.count), sub_p99_us);
    } else {
        std::printf("BENCHNET sub_us n=0 (skipped)\n");
    }

    // ---------------- 6) 分页扫描吞吐（信息项） ----------------
    {
        uint64_t rows = 0;
        const auto t0 = Clock::now();
        auto r = c->ScanPaged(a.table, a.page_rows, [&rows](const rtdb::ClientScanResult& page) {
            rows += page.rows.size();
            return true;
        });
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        if (!r.IsOk()) {
            std::fprintf(stderr, "扫描失败：%s\n", rtdb::ErrorMessage(r.Error()));
            return Fail("scan");
        }
        std::printf("BENCHNET scan rows=%llu secs=%.4f rows_per_sec=%.0f\n",
                    static_cast<unsigned long long>(rows), secs,
                    secs > 0 ? static_cast<double>(rows) / secs : 0.0);
    }

    // ---------------- 收尾 ----------------
    if (sub != nullptr) sub->Close();
    c->Close();
    if (srv_started) {
        const rtdb::server::Server::Stats st = srv.stats();
        std::printf(
            "BENCHNET server accepted=%llu auth_ok=%llu requests=%llu payload_errors=%llu "
            "frame_errors=%llu\n",
            static_cast<unsigned long long>(st.accepted),
            static_cast<unsigned long long>(st.auth_ok),
            static_cast<unsigned long long>(st.requests),
            static_cast<unsigned long long>(st.payload_errors),
            static_cast<unsigned long long>(st.frame_errors));
        srv.Stop();
    }
    if (!a.external && !a.keep) {
        std::error_code ec;
        fs::remove(a.image, ec);
        fs::remove(a.image + ".snap", ec);
    }

    // ---------------- CI 阈值（保守下限；消毒构建放宽） ----------------
    // CI runner 与本地口径区分：
    //  - 本机 `--ci`：P99 ≤ 5ms（docs/14 §6.2 正式基线口径）；
    //  - GitHub Actions 共享 runner：调度/IO 抖动可产生 8~40ms 离群，P99 门槛
    //    提至 20ms（仍远高于本机 ~0.13ms，只防"完全失效"级回归），并配合
    //    上面"3 轮取最优 p99"吸收单次噪声；
    //  - 消毒/Debug（relaxed）：100ms。
    int rc = 0;
    const bool ci_host = EnvNonEmpty("GITHUB_ACTIONS") || EnvNonEmpty("CI");
    const double min_rps = relaxed ? 5000.0 : 50000.0;
    const double max_p99_us = (relaxed ? 100.0 : (ci_host ? 20.0 : 5.0)) * 1000.0;
    if (a.ci) {
        const double write_p99 = write_us.empty() ? 0.0 : Pct(write_us, 0.99);
        const double ping_p99 = ping_us.empty() ? 0.0 : Pct(ping_us, 0.99);
        if (block_rps < min_rps) {
            std::fprintf(stderr, "CI 失败：block rows/s=%.0f < %.0f\n", block_rps, min_rps);
            rc = 1;
        }
        if (write_p99 > max_p99_us) {
            std::fprintf(stderr, "CI 失败：write1 p99=%.0fus > %.0fus\n", write_p99, max_p99_us);
            rc = 1;
        }
        if (ping_p99 > max_p99_us) {
            std::fprintf(stderr, "CI 失败：ping p99=%.0fus > %.0fus\n", ping_p99, max_p99_us);
            rc = 1;
        }
        std::printf(
            "BENCHNET ci thresholds rows_per_sec>=%.0f p99_us<=%.0f relaxed=%d host_ci=%d\n",
            min_rps, max_p99_us, relaxed ? 1 : 0, ci_host ? 1 : 0);
    }
    std::printf("BENCHNET RESULT %s\n", rc == 0 ? "PASS" : "FAIL");
    std::fflush(stdout);
    return rc;
}
