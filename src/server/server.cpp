#include "server/server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "infra/sha256.hpp"
#include "net/codec.hpp"
#include "net/value_codec.hpp"
#include "net/wire.hpp"
#include "platform/net.hpp"
#include "rtdb/change_cursor.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/proto_v1.h"
#include "rtdb/table_def.hpp"
#include "server/authz.hpp"
#include "server/perf.hpp"

namespace rtdb {
namespace server {

namespace {

/// 会话接收轮询超时（毫秒）：Stop 收敛预算与空闲唤醒开销的折中
/// （docs/13 §5-W3；全连接线程模型，M3 不引入事件循环）。
constexpr uint32_t kSessionPollMs = 250;

/// FETCH_DELTA 长轮询的等待切片上限（毫秒）：保全 stop 响应性，
/// 订阅语义不变（总预算内循环等待首批变更）。
constexpr uint32_t kDeltaWaitSliceMs = 250;

/// 单会话 SCAN_PAGE 分页句柄上限。
constexpr size_t kMaxPagesPerSession = 16;

/// 单次 SUBSCRIBE 的表名数量上限。
constexpr uint32_t kMaxSubscribeTables = 64;

/// EXEC_BATCH ops 模式单批操作数上限（载荷上限内的保守界）。
constexpr uint32_t kMaxBatchOps = 100000;

/// SCAN_PAGE 单页行数上限。
constexpr uint32_t kMaxPageRows = 100000;

/// 会话接收轮询口径的单调时钟（纳秒；TTL 判定用，不受墙钟回拨影响）。
uint64_t SteadyNowNs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// 会话状态（连接私有；会话模型单线程使用）。
struct SessionState {
    /// SCAN_PAGE 分页句柄：持有表句柄 + 该句柄 schema + 续扫游标。
    struct Page {
        TableHandle handle;
        std::vector<ColumnDef> cols;
        ColumnValue next_lo = ColumnValue::Null();  ///< 续扫下界（开区间起点）
        bool has_lo = false;
        uint32_t page_rows = 0;
    };

    uint32_t id = 0;                 ///< AUTH 发放的会话号（帧内回显校验）
    uint64_t last_active_ns = 0;     ///< TTL 滚动基准（单调时钟）
    uint32_t sub_id = 0;             ///< 当前订阅号（0 = 无订阅）
    uint32_t next_sub_id = 0;
    uint64_t last_acked_seq = 0;     ///< 客户端 ACK 水位（诊断账本）
    uint64_t next_page_id = 0;
    uint32_t auth_failures = 0;      ///< 本连接累计 AUTH 失败次数（P1-7 限速）
    /// AUTH 成功绑定的用户（指向 Impl::cfg.users 元素；Start 后冻结，连接
    /// 生命周期内有效）。未认证/旧会话重认证失败时保持原值。
    const UserEntry* user = nullptr;
    std::unique_ptr<ChangeCursor> cursor;
    std::map<uint64_t, Page> pages;
};

/// 变更事件 → 线编码（docs/13 §5-W3 增量体布局）：
/// `[u8 op][i64 table_id][Str table][u64 seq][u64 ts_ns][u64 row_no]
///  [u64 row_ver]` + insert/update:`[u32 nvals][自描述值...]` /
/// delete:`[自描述键值]`。
void EncodeChange(net::Writer& w, const ChangeEvent& ev) noexcept {
    uint8_t kind = RTDB_CHG_INSERT;
    if (ev.kind == ChangeEvent::Kind::kUpdate) {
        kind = RTDB_CHG_UPDATE;
    } else if (ev.kind == ChangeEvent::Kind::kDelete) {
        kind = RTDB_CHG_DELETE;
    }
    w.U8(kind);
    w.I64(ev.table_id);
    w.Str(ev.table);
    w.U64(ev.seq);
    w.U64(ev.ts_ns);
    w.U64(ev.row_no);
    w.U64(ev.row_ver);
    if (ev.kind == ChangeEvent::Kind::kDelete) {
        net::WriteValue(w, ev.key);
        return;
    }
    w.U32(static_cast<uint32_t>(ev.row.size()));
    for (const ColumnValue& v : ev.row) net::WriteValue(w, v);
}

}  // namespace

// ============================================================
// 服务端实现
// ============================================================

struct Server::Impl {
    Config cfg;
    std::unique_ptr<Engine> engine;
    platform::TcpListener listener;
    std::atomic<bool> stop{false};
    std::atomic<uint16_t> bound_port{0};
    std::thread accept_thread;
    std::thread heartbeat_thread;
    bool running = false;

    std::mutex err_mu;
    std::string last_error;

    /// 线程/会话登记（Stop 等待全部线程收敛；会话数另计上限）。
    std::mutex conn_mu;
    std::condition_variable conn_cv;
    uint32_t live_threads = 0;
    uint32_t live_sessions = 0;

    std::mutex rng_mu;
    std::mt19937_64 rng;

    /// M4 PerfCounters：进程内原子计数（docs/16 D3），热路径无锁。
    PerfCounters perf;
    /// 进程运行起点（STATS uptime 口径）。
    uint64_t start_ns = 0;

    Impl() {
        std::random_device rd;
        const uint64_t seed =
            (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd()) ^
            SteadyNowNs();
        rng.seed(seed);
    }

    void SetError(std::string msg) {
        std::lock_guard<std::mutex> lk(err_mu);
        last_error = std::move(msg);
    }

    uint32_t NextSessionId() {
        std::lock_guard<std::mutex> lk(rng_mu);
        uint32_t id = static_cast<uint32_t>(rng() & 0x7FFFFFFFu);
        return id != 0 ? id : 1u;
    }

    /// 会话/拒绝线程退出登记（is_session 决定是否归还会话名额）。
    ///
    /// 通知必须在 conn_mu 临界区内完成：这是"线程已完成对 Impl 的访问"与
    /// Stop 观察 live_threads == 0 之间的唯一同步点。若 notify 放在解锁之后，
    /// Stop 的谓词检查（或虚假唤醒）可能在解锁与 notify 之间先观察到 0 并
    /// 析构 Impl/conn_cv，随后本线程再对已析构条件变量广播 → UB（TSan:
    /// pthread_cond_destroy vs pthread_cond_broadcast）。临界区内 notify 使
    /// 解锁严格早于 Stop 的加锁，构成 happens-before；解锁后本线程不再触碰
    /// Impl 任何成员，故 Stop 在观察到 0 之后析构是安全的。
    void ConnReleased(bool is_session) {
        std::lock_guard<std::mutex> lk(conn_mu);
        if (live_threads > 0) --live_threads;
        if (is_session && live_sessions > 0) --live_sessions;
        if (is_session) perf.sessions_current.fetch_sub(1, std::memory_order_relaxed);
        conn_cv.notify_all();
    }

    // ---- M4 Authz 权限门（docs/16 D2）----
    /// 权限拒绝统一出口：审计计数 + kPermissionDenied（503）。
    Err DenyAuthz() noexcept {
        perf.authz_denied.fetch_add(1, std::memory_order_relaxed);
        return Err::kPermissionDenied;
    }
    Err RequireRead(const SessionState& st, std::string_view table) noexcept {
        if (st.user == nullptr) return Err::kTokenExpired;
        return CanRead(*st.user, table) ? Err::kOk : DenyAuthz();
    }
    Err RequireWrite(const SessionState& st, std::string_view table) noexcept {
        if (st.user == nullptr) return Err::kTokenExpired;
        return CanWrite(*st.user, table) ? Err::kOk : DenyAuthz();
    }
    Err RequireDdl(const SessionState& st) noexcept {
        if (st.user == nullptr) return Err::kTokenExpired;
        return CanDdl(*st.user) ? Err::kOk : DenyAuthz();
    }
    Err RequireAdmin(const SessionState& st) noexcept {
        if (st.user == nullptr) return Err::kTokenExpired;
        return CanAdmin(*st.user) ? Err::kOk : DenyAuthz();
    }

    // ---- 表元数据 ----
    /// 按名打开表并取句柄可见列的 schema 快照。
    Err DescribeByName(const std::string& name, TableHandle* th,
                       std::vector<ColumnDef>* cols) {
        auto t = engine->OpenTable(name);
        if (!t.IsOk()) return t.Error();
        auto c = engine->DescribeTable(t.Value());
        if (!c.IsOk()) return c.Error();
        *th = t.TakeValue();
        *cols = c.TakeValue();
        return Err::kOk;
    }

    // ---- 线程体 ----
    void AcceptLoop();
    void HeartbeatLoop();
    void SessionLoop(platform::TcpSocket sock);
    void RejectLoop(platform::TcpSocket sock);

    // ---- 请求分派 ----
    /// AUTHZ 摘要编码（AUTHZ_ME/AUTHZ_SHOW 共用，docs/16 D3 布局）：
    /// `[Str user][u8 flags][u32 n_r][Str]*[u32 n_w][Str]*`。
    void EncodeAuthz(const UserEntry& user, net::Writer& w) const;

    Err Dispatch(SessionState& st, const net::FrameHeader& h, const uint8_t* payload,
                 size_t len, std::vector<uint8_t>* body);
    Err DoAuth(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err DoPing(net::Reader& r, std::vector<uint8_t>* body);
    Err DoDdl(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err DoExecBatch(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err DoScan(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err DoScanPage(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err FetchPageSlice(SessionState::Page& p, std::vector<std::vector<ColumnValue>>* rows,
                       bool* done);
    Err DoSubscribe(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err DoFetchDelta(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err DoAck(SessionState& st, net::Reader& r);
    Err DoUnregister(SessionState& st, net::Reader& r);
    /// M4 管理面（仅 admin）：STATS / AUTHZ_SHOW（docs/16 D3）。
    Err DoAdmin(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    /// M4 自身权限查询（任意已认证用户）：不吃 admin 门。
    Err DoAuthzMe(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
};

void Server::Impl::AcceptLoop() {
    while (!stop.load()) {
        auto acc = listener.Accept(100);
        if (!acc.IsOk()) {
            if (acc.Error() == Err::kTimeout) continue;
            if (stop.load()) break;
            SetError(std::string("accept: ") + ErrorMessage(acc.Error()));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        platform::TcpSocket sock = acc.TakeValue();

        bool over = false;
        {
            std::lock_guard<std::mutex> lk(conn_mu);
            over = live_sessions >= cfg.max_sessions;
            ++live_threads;
            if (!over) ++live_sessions;
        }
        if (over) {
            perf.rejected_full.fetch_add(1, std::memory_order_relaxed);
            std::thread(&Server::Impl::RejectLoop, this, std::move(sock)).detach();
        } else {
            perf.accepted.fetch_add(1, std::memory_order_relaxed);
            perf.sessions_current.fetch_add(1, std::memory_order_relaxed);
            perf.sessions_total.fetch_add(1, std::memory_order_relaxed);
            std::thread(&Server::Impl::SessionLoop, this, std::move(sock)).detach();
        }
    }
}

void Server::Impl::HeartbeatLoop() {
    while (!stop.load()) {
        if (engine != nullptr) engine->RefreshWriterHeartbeat();
        const uint32_t step_ms = 25;
        for (uint32_t waited = 0; waited < cfg.heartbeat_ms && !stop.load();
             waited += step_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        }
    }
}

void Server::Impl::SessionLoop(platform::TcpSocket sock) {
    // 生命周期契约：Guard 必须最先声明（故最后析构）——ConnReleased 之后本线程
    // 不得再触碰 Impl 任何成员；Stop 观察到 live_threads == 0 即可能析构 Impl。
    struct Guard {
        Impl* im;
        ~Guard() { im->ConnReleased(true); }
    } guard{this};

    sock.SetTimeout(kSessionPollMs);
    SessionState st;
    const uint64_t connected_ns = SteadyNowNs();
    uint64_t last_rx_ns = connected_ns;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> body;
    std::vector<uint8_t> resp;

    for (;;) {
        if (stop.load(std::memory_order_relaxed)) break;

        // P1-7：握手/空闲超时。每连接一个线程，静默连接若不设上限会
        // 长期占用 max_sessions 名额（线程栈 + 会话槽），构成低成本 DoS。
        const uint64_t now = SteadyNowNs();
        if (st.id == 0 && cfg.handshake_timeout_ms != 0 &&
            now - connected_ns >
                static_cast<uint64_t>(cfg.handshake_timeout_ms) * 1000000ull) {
            perf.handshake_timeouts.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (cfg.idle_timeout_ms != 0 &&
            now - last_rx_ns > static_cast<uint64_t>(cfg.idle_timeout_ms) * 1000000ull) {
            perf.idle_timeouts.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        net::FrameHeader h;
        payload.clear();
        const Err fe = net::ReadFrame(sock, cfg.max_payload_bytes, &h, &payload);
        if (fe == Err::kTimeout) continue;
        if (!IsOk(fe)) {
            perf.frame_errors.fetch_add(1, std::memory_order_relaxed);  // 帧级违例/EOF
            break;
        }
        last_rx_ns = SteadyNowNs();
        if (h.flags != 0) {
            perf.frame_errors.fetch_add(1, std::memory_order_relaxed);  // 请求帧携带应答位
            break;
        }

        perf.requests.fetch_add(1, std::memory_order_relaxed);
        body.clear();
        const Err re = Dispatch(st, h, payload.data(), payload.size(), &body);
        if (!IsOk(re)) perf.payload_errors.fetch_add(1, std::memory_order_relaxed);
        bool close_after = false;
        uint64_t auth_delay_ms = 0;
        if (re == Err::kAuthFailed) {
            // P1-7：失败计数 + 线性退避，抬高在线暴力猜解成本；达到上限
            // 先回完本次 501 再断连（客户端需重新建连）。
            if (st.auth_failures >= cfg.max_auth_failures) {
                perf.auth_limit_closed.fetch_add(1, std::memory_order_relaxed);
                close_after = true;
            } else {
                uint64_t delay = static_cast<uint64_t>(cfg.auth_backoff_ms) * st.auth_failures;
                if (delay > 2000) delay = 2000;
                auth_delay_ms = delay;
            }
        }

        resp.clear();
        {
            net::Writer w(&resp);
            net::WriteResponsePrologue(
                w, re,
                IsOk(re) ? std::string_view{} : std::string_view(ErrorMessage(re)));
        }
        resp.insert(resp.end(), body.begin(), body.end());
        if (resp.size() > cfg.max_payload_bytes) {
            // 防御：应答超帧上限（处理器应已自限）→ 以 kLimitExceeded 提示改用分页。
            resp.clear();
            net::Writer w(&resp);
            net::WriteResponsePrologue(w, Err::kLimitExceeded,
                                       std::string_view(ErrorMessage(Err::kLimitExceeded)));
        }

        net::FrameHeader rh;
        rh.ver = net::kProtoVersion;
        rh.flags = net::kFlagResp;
        rh.session = h.session;
        rh.func = h.func;
        rh.payload_len = static_cast<uint32_t>(resp.size());
        if (!IsOk(net::WriteFrame(sock, rh, resp.data()))) break;
        if (close_after) break;
        // 退避在应答之后：客户端已收到本次 501，重试间隔被拉长。
        if (auth_delay_ms != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(auth_delay_ms));
    }
    sock.Close();
}

void Server::Impl::RejectLoop(platform::TcpSocket sock) {
    // 生命周期契约同 SessionLoop：Guard 最先声明/最后析构，之后不再访问 Impl。
    struct Guard {
        Impl* im;
        ~Guard() { im->ConnReleased(false); }
    } guard{this};

    // 限时读一帧（尽力回 404 明示原因）后关闭；静默客户端 3s 后放弃。
    sock.SetTimeout(3000);
    net::FrameHeader h;
    std::vector<uint8_t> payload;
    if (IsOk(net::ReadFrame(sock, cfg.max_payload_bytes, &h, &payload))) {
        std::vector<uint8_t> resp;
        {
            net::Writer w(&resp);
            net::WriteResponsePrologue(w, Err::kTooManySessions,
                                       std::string_view(ErrorMessage(Err::kTooManySessions)));
        }
        net::FrameHeader rh;
        rh.ver = net::kProtoVersion;
        rh.flags = net::kFlagResp;
        rh.session = h.session;
        rh.func = h.func;
        rh.payload_len = static_cast<uint32_t>(resp.size());
        (void)net::WriteFrame(sock, rh, resp.data());
    }
    sock.Close();
}

Err Server::Impl::Dispatch(SessionState& st, const net::FrameHeader& h,
                           const uint8_t* payload, size_t len,
                           std::vector<uint8_t>* body) {
    net::Reader r(payload, len);

    if (h.func == RTDB_PF_AUTH) return DoAuth(st, r, body);

    // 会话门：帧会话号须与连接绑定的发放号一致，且未超 TTL（滚动续期）。
    if (st.id == 0 || h.session != st.id) return Err::kTokenExpired;
    const uint64_t now = SteadyNowNs();
    if (now - st.last_active_ns >
        static_cast<uint64_t>(cfg.session_ttl_ms) * 1000000ull) {
        st.id = 0;  // 过期即失效：客户端须重新 AUTH
        st.cursor.reset();
        st.sub_id = 0;
        st.pages.clear();
        return Err::kTokenExpired;
    }
    st.last_active_ns = now;

    switch (h.func) {
        case RTDB_PF_PING:
            return DoPing(r, body);
        case RTDB_PF_DDL:
            return DoDdl(st, r, body);
        case RTDB_PF_EXEC_BATCH:
            return DoExecBatch(st, r, body);
        case RTDB_PF_SCAN:
            return DoScan(st, r, body);
        case RTDB_PF_SCAN_PAGE:
            return DoScanPage(st, r, body);
        case RTDB_PF_SUBSCRIBE:
            return DoSubscribe(st, r, body);
        case RTDB_PF_FETCH_DELTA:
            return DoFetchDelta(st, r, body);
        case RTDB_PF_ACK:
            return DoAck(st, r);
        case RTDB_PF_UNREGISTER:
            return DoUnregister(st, r);
        case RTDB_PF_ADMIN:
            return DoAdmin(st, r, body);
        case RTDB_PF_AUTHZ_ME:
            return DoAuthzMe(st, r, body);
        default:
            return Err::kNotImplemented;  // 未知功能号：400，不断连
    }
}

Err Server::Impl::DoAuth(SessionState& st, net::Reader& r, std::vector<uint8_t>* body) {
    std::string user;
    if (!r.Str(&user)) return Err::kProtocolError;
    std::string digest;
    if (!r.Var(&digest)) return Err::kProtocolError;
    if (!r.Finish()) return Err::kProtocolError;
    if (digest.size() != infra::kSha256DigestBytes) return Err::kProtocolError;

    const UserEntry* found = nullptr;
    for (const UserEntry& u : cfg.users) {
        if (u.name == user) {
            found = &u;
            break;
        }
    }
    // 等时比较：用户不存在也走一次定长比较（弱化存在性时序差）。
    static const std::array<uint8_t, infra::kSha256DigestBytes> kZeroDigest{};
    const uint8_t* expect = found != nullptr ? found->digest.data() : kZeroDigest.data();
    const bool ok = infra::ConstantTimeEqual(
        expect, reinterpret_cast<const uint8_t*>(digest.data()), infra::kSha256DigestBytes);
    if (!ok) {
        perf.auth_fail.fetch_add(1, std::memory_order_relaxed);
        if (st.auth_failures < UINT32_MAX) ++st.auth_failures;
        return Err::kAuthFailed;  // 501：凭据错误（连接保留，可重试）
    }

    perf.auth_ok.fetch_add(1, std::memory_order_relaxed);
    st.auth_failures = 0;
    st.id = NextSessionId();
    st.last_active_ns = SteadyNowNs();
    st.user = found;  // M4：权限随 AUTH 绑定到会话（cfg.users 生命周期内稳定）
    st.cursor.reset();  // 重新认证：旧订阅/分页随身份重建
    st.sub_id = 0;
    st.pages.clear();

    net::Writer w(body);
    w.U32(st.id);
    w.U32(cfg.session_ttl_ms);
    return Err::kOk;
}

Err Server::Impl::DoPing(net::Reader& r, std::vector<uint8_t>* body) {
    if (!r.Finish()) return Err::kProtocolError;
    net::Writer w(body);
    w.I64(engine->CommittedSeq());
    w.U64(static_cast<uint64_t>(engine->WriterHeartbeatNs()));
    w.I64(engine->CheckpointSeq());
    w.U32(cfg.session_ttl_ms);
    return Err::kOk;
}

Err Server::Impl::DoDdl(SessionState& st, net::Reader& r, std::vector<uint8_t>* body) {
    const uint8_t sub = r.U8();
    if (!r.ok()) return Err::kProtocolError;

    if (sub == RTDB_DDL_CREATE_TABLE) {
        auto def = net::ReadTableDef(r);
        if (!def.IsOk()) return def.Error();
        if (!r.Finish()) return Err::kProtocolError;
        const Err pe = RequireDdl(st);
        if (!IsOk(pe)) return pe;
        auto t = engine->CreateTable(def.Value().def);
        if (!t.IsOk()) return t.Error();
        const TableHandle& th = t.Value();
        net::Writer w(body);
        w.I64(th.table_id());
        w.U32(th.visible_column_count());
        w.U32(th.layout_version());
        return Err::kOk;
    }
    if (sub == RTDB_DDL_DROP_TABLE) {
        std::string name;
        if (!r.Str(&name)) return Err::kProtocolError;
        if (!r.Finish()) return Err::kProtocolError;
        const Err pe = RequireDdl(st);
        if (!IsOk(pe)) return pe;
        // Result<void>::Error() 的契约是 !IsOk()（Debug/ASan 下断言），不可对
        // 成功结果调用；此处显式判成功，语义与 Release（err_ == kOk）一致。
        const auto drop_res = engine->DropTable(name);
        return drop_res.IsOk() ? Err::kOk : drop_res.Error();
    }
    if (sub == RTDB_DDL_DESCRIBE_TABLE) {
        std::string name;
        if (!r.Str(&name)) return Err::kProtocolError;
        if (!r.Finish()) return Err::kProtocolError;
        // 描述表 = 元数据读：读权限或 DDL 权限均可（docs/16 D2）。
        if (st.user == nullptr) return Err::kTokenExpired;
        if (!CanRead(*st.user, name) && !CanDdl(*st.user)) return DenyAuthz();
        TableHandle th;
        std::vector<ColumnDef> cols;
        const Err e = DescribeByName(name, &th, &cols);
        if (!IsOk(e)) return e;
        net::Writer w(body);
        w.I64(th.table_id());
        w.U32(th.layout_version());
        net::WriteResultHeader(w, name, Span<const ColumnDef>(cols.data(), cols.size()), 0);
        return Err::kOk;
    }
    return Err::kProtocolError;  // 未知子操作
}

Err Server::Impl::DoExecBatch(SessionState& st, net::Reader& r,
                              std::vector<uint8_t>* body) {
    const uint8_t mode = r.U8();
    if (!r.ok()) return Err::kProtocolError;

    if (mode == RTDB_BATCH_MODE_OPS) {
        const uint32_t n = r.U32();
        if (!r.ok()) return Err::kProtocolError;
        if (n > kMaxBatchOps) return Err::kLimitExceeded;

        struct TableCache {
            TableHandle handle;
            std::vector<DataType> types;
        };
        std::unordered_map<std::string, TableCache> cache;
        std::vector<Engine::BatchOp> ops;
        ops.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t op_type = r.U8();
            std::string table;
            if (!r.Str(&table)) return Err::kProtocolError;
            // M4 Authz：写类批操作逐表校验写权限（批内任意表被拒 → 整批零副作用）。
            const Err pe = RequireWrite(st, table);
            if (!IsOk(pe)) return pe;

            auto it = cache.find(table);
            if (it == cache.end()) {
                TableHandle th;
                std::vector<ColumnDef> cols;
                const Err e = DescribeByName(table, &th, &cols);
                if (!IsOk(e)) return e;
                TableCache tc;
                tc.handle = th;
                tc.types.reserve(cols.size());
                for (const ColumnDef& cd : cols) tc.types.push_back(cd.type);
                it = cache.emplace(std::move(table), std::move(tc)).first;
            }

            Engine::BatchOp op;
            op.table = it->second.handle;
            if (op_type == static_cast<uint8_t>(Engine::BatchOp::Type::kInsert) ||
                op_type == static_cast<uint8_t>(Engine::BatchOp::Type::kUpdate)) {
                op.type = static_cast<Engine::BatchOp::Type>(op_type);
                op.row = net::ReadRow(r, it->second.types);
                if (!r.ok()) return Err::kProtocolError;
            } else if (op_type == static_cast<uint8_t>(Engine::BatchOp::Type::kDelete)) {
                op.type = Engine::BatchOp::Type::kDelete;
                op.row.push_back(net::ReadValueTyped(r, it->second.types[0]));
                if (!r.ok()) return Err::kProtocolError;
            } else {
                return Err::kProtocolError;
            }
            ops.push_back(std::move(op));
        }
        if (!r.Finish()) return Err::kProtocolError;
        if (ops.empty()) return Err::kInvalidArgument;

        const auto exec_res = engine->ExecuteBatch(ops);
        if (!exec_res.IsOk()) return exec_res.Error();
        net::Writer w(body);
        w.I64(engine->CommittedSeq());
        return Err::kOk;
    }

    if (mode == RTDB_BATCH_MODE_BLOCK) {
        auto blk = net::ReadBlock(r);
        if (!blk.IsOk()) return blk.Error();
        if (!r.Finish()) return Err::kProtocolError;
        // M4 Authz：块写按目标表校验写权限（docs/16 D2）。
        const Err pe = RequireWrite(st, blk.Value().View().table_name);
        if (!IsOk(pe)) return pe;
        const auto upsert_res = engine->UpsertBatch(blk.Value().View());
        if (!upsert_res.IsOk()) return upsert_res.Error();
        net::Writer w(body);
        w.I64(engine->CommittedSeq());
        return Err::kOk;
    }
    return Err::kProtocolError;  // 未知模式
}

Err Server::Impl::DoScan(SessionState& st, net::Reader& r,
                         std::vector<uint8_t>* body) {
    std::string table;
    if (!r.Str(&table)) return Err::kProtocolError;
    // M4 Authz：表数据读需要表级读权限（admin 或 r 白名单）。
    const Err pe = RequireRead(st, table);
    if (!IsOk(pe)) return pe;

    Engine::KeyRange range;
    for (int side = 0; side < 2; ++side) {
        const uint8_t present = r.U8();
        if (!r.ok()) return Err::kProtocolError;
        if (present == 0) continue;
        const uint8_t tag = r.U8();
        if (!r.ok()) return Err::kProtocolError;
        ColumnValue v = ColumnValue::Null();
        if (tag == RTDB_VAL_NULL) {
            v = ColumnValue::Null();
        } else if (tag == RTDB_VAL_I64) {
            v = ColumnValue::Int(r.I64());
        } else {
            return Err::kProtocolError;
        }
        if (side == 0) {
            range.lo = v;
        } else {
            range.hi = v;
        }
    }
    if (!r.Finish()) return Err::kProtocolError;

    TableHandle th;
    std::vector<ColumnDef> cols;
    const Err de = DescribeByName(table, &th, &cols);
    if (!IsOk(de)) return de;

    auto rows = engine->Scan(th, range);
    if (!rows.IsOk()) return rows.Error();

    net::Writer w(body);
    net::WriteResultHeader(w, table, Span<const ColumnDef>(cols.data(), cols.size()),
                           static_cast<uint32_t>(rows.Value().size()));
    for (const std::vector<ColumnValue>& row : rows.Value()) net::WriteRow(w, row);
    return Err::kOk;
}

Err Server::Impl::FetchPageSlice(SessionState::Page& p,
                                 std::vector<std::vector<ColumnValue>>* rows, bool* done) {
    Engine::KeyRange range;
    if (p.has_lo) {
        range.lo = p.next_lo;
        range.lo_inclusive = false;
    }
    bool more = false;
    auto res = engine->ScanLimited(p.handle, range, p.page_rows, &more);
    if (!res.IsOk()) return res.Error();

    const std::vector<std::vector<ColumnValue>>& all = res.Value();
    rows->assign(all.begin(), all.end());
    *done = !more;
    if (!rows->empty() && !(*rows)[rows->size() - 1].empty()) {
        p.next_lo = (*rows)[rows->size() - 1][0];  // 主键 = 列 0（建表约束）
        p.has_lo = true;
    }
    return Err::kOk;
}

Err Server::Impl::DoScanPage(SessionState& st, net::Reader& r, std::vector<uint8_t>* body) {
    const uint8_t sub = r.U8();
    if (!r.ok()) return Err::kProtocolError;

    if (sub == RTDB_SP_OPEN) {
        std::string table;
        if (!r.Str(&table)) return Err::kProtocolError;
        const uint32_t page_rows = r.U32();
        if (!r.Finish()) return Err::kProtocolError;
        // M4 Authz：分页扫描同样按表级读权限（FETCH/CLOSE 由会话内页句柄
        // 保证与 OPEN 时的授权一致，不再重复校验）。
        const Err pe = RequireRead(st, table);
        if (!IsOk(pe)) return pe;
        if (page_rows == 0 || page_rows > kMaxPageRows) return Err::kInvalidArgument;
        if (st.pages.size() >= kMaxPagesPerSession) return Err::kLimitExceeded;

        TableHandle th;
        std::vector<ColumnDef> cols;
        const Err de = DescribeByName(table, &th, &cols);
        if (!IsOk(de)) return de;

        const uint64_t page_id = ++st.next_page_id;
        SessionState::Page& p = st.pages[page_id];
        p.handle = th;
        p.cols = std::move(cols);
        p.page_rows = page_rows;

        std::vector<std::vector<ColumnValue>> rows;
        bool done = false;
        const Err fe = FetchPageSlice(p, &rows, &done);
        if (!IsOk(fe)) {
            st.pages.erase(page_id);
            return fe;
        }

        net::Writer w(body);
        w.U64(page_id);
        w.U8(done ? 1 : 0);
        net::WriteResultHeader(w, table, Span<const ColumnDef>(p.cols.data(), p.cols.size()),
                               static_cast<uint32_t>(rows.size()));
        for (const std::vector<ColumnValue>& row : rows) net::WriteRow(w, row);
        return Err::kOk;
    }

    if (sub == RTDB_SP_FETCH) {
        const uint64_t page_id = r.U64();
        if (!r.Finish()) return Err::kProtocolError;
        auto it = st.pages.find(page_id);
        if (it == st.pages.end()) return Err::kNotFound;  // 未知/已关闭分页句柄
        std::vector<std::vector<ColumnValue>> rows;
        bool done = false;
        const Err fe = FetchPageSlice(it->second, &rows, &done);
        if (!IsOk(fe)) return fe;
        net::Writer w(body);
        w.U8(done ? 1 : 0);
        w.U32(static_cast<uint32_t>(rows.size()));
        for (const std::vector<ColumnValue>& row : rows) net::WriteRow(w, row);
        return Err::kOk;
    }

    if (sub == RTDB_SP_CLOSE) {
        const uint64_t page_id = r.U64();
        if (!r.Finish()) return Err::kProtocolError;
        st.pages.erase(page_id);
        return Err::kOk;  // 幂等
    }
    return Err::kProtocolError;  // 未知子操作
}

Err Server::Impl::DoSubscribe(SessionState& st, net::Reader& r, std::vector<uint8_t>* body) {
    const uint32_t n = r.U32();
    if (!r.ok() || n > kMaxSubscribeTables) return Err::kProtocolError;
    std::vector<std::string> names;
    names.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        std::string s;
        if (!r.Str(&s)) return Err::kProtocolError;
        names.push_back(std::move(s));
    }
    if (!r.Finish()) return Err::kProtocolError;

    // M4 Authz：订阅要求对全部目标表有读权限（空列表 = 全部活跃表，要求
    // admin；否则按表逐个校验，任一拒绝即整次订阅失败）。
    if (names.empty()) {
        const Err pe = RequireAdmin(st);
        if (!IsOk(pe)) return pe;
    } else {
        for (const std::string& name : names) {
            const Err pe = RequireRead(st, name);
            if (!IsOk(pe)) return pe;
        }
    }

    auto cur = engine->Subscribe(names);
    if (!cur.IsOk()) return cur.Error();
    st.cursor = std::make_unique<ChangeCursor>(cur.TakeValue());
    st.sub_id = ++st.next_sub_id;
    if (st.sub_id == 0) st.sub_id = ++st.next_sub_id;  // 防回绕归零（近乎不可能）
    st.last_acked_seq = 0;

    net::Writer w(body);
    w.U32(st.sub_id);
    w.I64(static_cast<int64_t>(st.cursor->Position()));
    return Err::kOk;
}

Err Server::Impl::DoFetchDelta(SessionState& st, net::Reader& r,
                               std::vector<uint8_t>* body) {
    const uint32_t sub = r.U32();
    const uint64_t from = r.U64();
    uint32_t timeout_ms = r.U32();
    uint32_t max_bytes = r.U32();
    if (!r.Finish()) return Err::kProtocolError;
    if (st.cursor == nullptr || sub == 0 || sub != st.sub_id) return Err::kProtocolError;

    if (timeout_ms > cfg.delta_max_timeout_ms) timeout_ms = cfg.delta_max_timeout_ms;
    if (max_bytes < 64) max_bytes = 64;
    if (max_bytes > cfg.max_delta_bytes) max_bytes = cfg.max_delta_bytes;

    // 参考上界：客户端不能从"未来"拉取（越界一律夹到 committed）。
    const int64_t committed = engine->CommittedSeq();
    uint64_t start = from;
    if (committed >= 0 && start > static_cast<uint64_t>(committed)) {
        start = static_cast<uint64_t>(committed);
    }
    st.cursor->ResetTo(start);

    // 长轮询：总预算 = timeout_ms，内部按 ≤250ms 切片以保全 stop 响应性
    // （语义 = 预算内等待首批变更；W2 游标本身支持无变更等待）。
    ChangeBatch batch;
    bool have = false;
    if (timeout_ms == 0) {
        auto res = st.cursor->Next(0);
        if (!res.IsOk()) return res.Error();
        batch = res.TakeValue();
        have = !batch.events.empty();
    } else {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline || stop.load()) break;
            const uint64_t remain_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            uint32_t slice = kDeltaWaitSliceMs;
            if (remain_ms == 0) {
                slice = 1;
            } else if (remain_ms < slice) {
                slice = static_cast<uint32_t>(remain_ms);
            }
            auto res = st.cursor->Next(slice);
            if (!res.IsOk()) return res.Error();  // 含 kCursorOverflow（402）
            if (!res.Value().events.empty()) {
                batch = res.TakeValue();
                have = true;
                break;
            }
        }
    }

    // 编码：max_bytes 截断（至少 1 条），last_seq = 最后一条被编码变更的
    // seq（客户端据此续拉，被截断的变更不丢失）。单条变更超帧上限 → 306。
    std::vector<uint8_t> chg;
    uint32_t n_out = 0;
    uint64_t last_seq = start;
    if (have) {
        for (const ChangeEvent& ev : batch.events) {
            std::vector<uint8_t> one;
            net::Writer ow(&one);
            EncodeChange(ow, ev);
            if (n_out == 0 && one.size() + 64 > cfg.max_payload_bytes) {
                return Err::kLimitExceeded;
            }
            if (n_out >= 1 && chg.size() + one.size() > max_bytes) break;
            chg.insert(chg.end(), one.begin(), one.end());
            ++n_out;
            last_seq = ev.seq;
        }
    }

    net::Writer w(body);
    w.I64(static_cast<int64_t>(last_seq));
    w.U32(n_out);
    body->insert(body->end(), chg.begin(), chg.end());
    return Err::kOk;
}

Err Server::Impl::DoAck(SessionState& st, net::Reader& r) {
    const uint32_t sub = r.U32();
    const uint64_t seq = r.U64();
    if (!r.Finish()) return Err::kProtocolError;
    if (st.cursor == nullptr || sub != st.sub_id) return Err::kProtocolError;
    st.last_acked_seq = seq;  // 诊断账本：M3 不影响交付（重拉由 from_seq 驱动）
    return Err::kOk;
}

Err Server::Impl::DoUnregister(SessionState& st, net::Reader& r) {
    uint32_t sub = 0;
    if (!r.Finish()) {  // 允许空载荷（清空当前订阅）或显式 sub_id
        sub = r.U32();
        if (!r.Finish()) return Err::kProtocolError;
    }
    if (st.cursor == nullptr) return Err::kProtocolError;
    if (sub != 0 && sub != st.sub_id) return Err::kProtocolError;
    st.cursor.reset();
    st.sub_id = 0;
    return Err::kOk;
}

// ---- M4 管理面 / 权限摘要（docs/16 D2/D3）----

void Server::Impl::EncodeAuthz(const UserEntry& user, net::Writer& w) const {
    w.Str(user.name);
    uint8_t flags = 0;
    if (user.perms.admin) flags |= RTDB_AUTHZ_FLAG_ADMIN;
    if (user.perms.ddl) flags |= RTDB_AUTHZ_FLAG_DDL;
    w.U8(flags);
    w.U32(static_cast<uint32_t>(user.perms.read_tables.size()));
    for (const std::string& t : user.perms.read_tables) w.Str(t);
    w.U32(static_cast<uint32_t>(user.perms.write_tables.size()));
    for (const std::string& t : user.perms.write_tables) w.Str(t);
}

Err Server::Impl::DoAdmin(SessionState& st, net::Reader& r, std::vector<uint8_t>* body) {
    const uint8_t sub = r.U8();
    if (!r.ok()) return Err::kProtocolError;
    const Err pe = RequireAdmin(st);
    if (!IsOk(pe)) return pe;
    perf.admin_requests.fetch_add(1, std::memory_order_relaxed);

    if (sub == RTDB_ADMIN_STATS) {
        if (!r.Finish()) return Err::kProtocolError;
        const uint64_t uptime_ms =
            start_ns == 0 ? 0 : (SteadyNowNs() - start_ns) / 1000000ull;
        struct KV {
            uint16_t key;
            uint64_t value;
        };
        const std::array<KV, 19> kv{{
            {RTDB_STAT_UPTIME_MS, uptime_ms},
            {RTDB_STAT_ACCEPTED, perf.accepted.load()},
            {RTDB_STAT_REJECTED_FULL, perf.rejected_full.load()},
            {RTDB_STAT_AUTH_OK, perf.auth_ok.load()},
            {RTDB_STAT_AUTH_FAIL, perf.auth_fail.load()},
            {RTDB_STAT_REQUESTS, perf.requests.load()},
            {RTDB_STAT_FRAME_ERRORS, perf.frame_errors.load()},
            {RTDB_STAT_PAYLOAD_ERRORS, perf.payload_errors.load()},
            {RTDB_STAT_AUTHZ_DENIED, perf.authz_denied.load()},
            {RTDB_STAT_SESSIONS_CURRENT, perf.sessions_current.load()},
            {RTDB_STAT_SESSIONS_TOTAL, perf.sessions_total.load()},
            {RTDB_STAT_HANDSHAKE_TIMEOUTS, perf.handshake_timeouts.load()},
            {RTDB_STAT_IDLE_TIMEOUTS, perf.idle_timeouts.load()},
            {RTDB_STAT_AUTH_LIMIT_CLOSED, perf.auth_limit_closed.load()},
            {RTDB_STAT_ADMIN_REQUESTS, perf.admin_requests.load()},
            {RTDB_STAT_COMMITTED_SEQ, static_cast<uint64_t>(engine->CommittedSeq())},
            {RTDB_STAT_CHECKPOINT_SEQ, static_cast<uint64_t>(engine->CheckpointSeq())},
            {RTDB_STAT_WRITER_HEARTBEAT_NS,
             static_cast<uint64_t>(engine->WriterHeartbeatNs())},
            {RTDB_STAT_SESSION_TTL_MS, static_cast<uint64_t>(cfg.session_ttl_ms)},
        }};
        net::Writer w(body);
        w.U16(RTDB_STATS_VERSION);
        w.U32(static_cast<uint32_t>(kv.size()));
        for (const KV& e : kv) {
            w.U16(e.key);
            w.U64(e.value);
        }
        return Err::kOk;
    }

    if (sub == RTDB_ADMIN_AUTHZ_SHOW) {
        if (!r.Finish()) return Err::kProtocolError;
        net::Writer w(body);
        w.U32(static_cast<uint32_t>(cfg.users.size()));
        for (const UserEntry& u : cfg.users) EncodeAuthz(u, w);
        return Err::kOk;
    }
    return Err::kProtocolError;  // 未知管理子操作
}

Err Server::Impl::DoAuthzMe(SessionState& st, net::Reader& r,
                            std::vector<uint8_t>* body) {
    if (st.user == nullptr) return Err::kTokenExpired;
    if (!r.Finish()) return Err::kProtocolError;
    perf.admin_requests.fetch_add(1, std::memory_order_relaxed);
    net::Writer w(body);
    EncodeAuthz(*st.user, w);
    return Err::kOk;
}

// ============================================================
// Server 门面
// ============================================================

Server::Server() noexcept = default;

Server::~Server() {
    // Stop 的契约：要么排空全部会话线程（可安全析构 Impl），要么整体泄漏 Impl
    // （impl_ 置空，残留线程仍持有有效内存）。因此此处销毁 impl_ 不会与
    // 已分离会话线程的 conn_mu/conn_cv 访问竞争。
    if (impl_ != nullptr && impl_->running) Stop(5000);
}

Err Server::Start(const Config& cfg) {
    if (impl_ == nullptr) impl_ = std::make_unique<Impl>();
    Impl& im = *impl_;
    if (im.running) return Err::kInvalidArgument;
    if (cfg.path.empty()) return Err::kInvalidArgument;

    im.cfg = cfg;
    // 协议 v1 硬上限：帧载荷 ≤ 4 MiB（客户端解码侧的对称约束）。
    if (im.cfg.max_payload_bytes > net::kDefaultMaxPayloadBytes) {
        im.cfg.max_payload_bytes = net::kDefaultMaxPayloadBytes;
    }
    if (im.cfg.max_delta_bytes > im.cfg.max_payload_bytes) {
        im.cfg.max_delta_bytes = im.cfg.max_payload_bytes;
    }

    Options o;
    o.instance = im.cfg.instance;
    o.path = im.cfg.path;
    o.initial_size_bytes = im.cfg.initial_size_mb << 20;
    o.wal_fsync = im.cfg.fsync;
    o.wal_ring_bytes = static_cast<uint64_t>(im.cfg.wal_ring_kb) << 10;
    o.role = Role::Standalone;  // D7：rtdbd 单进程写者（主备仲裁归 M4）
    auto eng = Engine::Open(o);
    if (!eng.IsOk()) {
        im.SetError(std::string("engine open: ") + ErrorMessage(eng.Error()));
        return eng.Error();
    }
    im.engine = eng.TakeValue();

    auto lis = platform::TcpListener::Listen(im.cfg.listen_host, im.cfg.listen_port,
                                             static_cast<int>(im.cfg.max_sessions));
    if (!lis.IsOk()) {
        im.SetError(std::string("listen: ") + ErrorMessage(lis.Error()));
        im.engine.reset();
        return lis.Error();
    }
    im.listener = lis.TakeValue();
    im.bound_port.store(im.listener.port());
    im.engine->RefreshWriterHeartbeat();

    im.stop.store(false);
    im.start_ns = SteadyNowNs();
    im.running = true;
    im.accept_thread = std::thread(&Server::Impl::AcceptLoop, &im);
    im.heartbeat_thread = std::thread(&Server::Impl::HeartbeatLoop, &im);
    return Err::kOk;
}

void Server::Stop(uint32_t timeout_ms) {
    if (impl_ == nullptr || !impl_->running) return;
    Impl& im = *impl_;
    im.stop.store(true);

    // 先收敛接受/心跳线程：此后不再登记新的会话线程，live_threads 只减不增。
    if (im.accept_thread.joinable()) im.accept_thread.join();
    if (im.heartbeat_thread.joinable()) im.heartbeat_thread.join();
    im.listener.Close();

    // 排空判据必须在 conn_mu 内读取：会话线程的退出登记与 notify 在同一临界区
    // （见 ConnReleased），故"观察到 live_threads == 0"意味着所有会话/拒绝线程
    // 对 Impl 的访问均已完成，并经 conn_mu 的解锁→加锁与本线程建立 happens-before。
    bool drained = false;
    {
        std::unique_lock<std::mutex> lk(im.conn_mu);
        drained = im.conn_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                      [&im] { return im.live_threads == 0; });
    }

    im.running = false;
    im.bound_port.store(0);

    if (drained) {
        if (im.cfg.snapshot_on_exit && im.engine != nullptr) {
            auto s = im.engine->CreateSnapshot();
            if (!s.IsOk()) {
                im.SetError(std::string("snapshot_on_exit: ") + ErrorMessage(s.Error()));
            }
        }
        im.engine.reset();  // 关引擎：最终 flush（docs/13 §3-D7）
    } else {
        // 极端情形：会话线程未在预算内收敛（对端卡死/平台异常）。此时既不能
        // join（可能无界阻塞）也不能析构 Impl：残留线程仍会访问 conn_mu/conn_cv
        // 与（经会话状态的）引擎。整体泄漏 Impl（含引擎，同原"进程退出统一回收"
        // 口径）以保安全；泄漏后本 Server 不再可用（impl_ == nullptr，API 退化
        // 为未运行态），进程退出由操作系统统一回收。
        im.SetError("stop timeout: live sessions remain; Impl leaked to avoid UAF");
        (void)impl_.release();
    }
}

bool Server::running() const noexcept { return impl_ != nullptr && impl_->running; }

uint16_t Server::port() const noexcept {
    return impl_ != nullptr ? impl_->bound_port.load() : 0;
}

Engine* Server::engine() noexcept { return impl_ != nullptr ? impl_->engine.get() : nullptr; }

Server::Stats Server::stats() const noexcept {
    Stats s;
    if (impl_ == nullptr) return s;
    s.accepted = impl_->perf.accepted.load();
    s.rejected_full = impl_->perf.rejected_full.load();
    s.auth_ok = impl_->perf.auth_ok.load();
    s.auth_fail = impl_->perf.auth_fail.load();
    s.requests = impl_->perf.requests.load();
    s.frame_errors = impl_->perf.frame_errors.load();
    s.payload_errors = impl_->perf.payload_errors.load();
    s.authz_denied = impl_->perf.authz_denied.load();
    s.sessions_current = impl_->perf.sessions_current.load();
    s.sessions_total = impl_->perf.sessions_total.load();
    s.handshake_timeouts = impl_->perf.handshake_timeouts.load();
    s.idle_timeouts = impl_->perf.idle_timeouts.load();
    s.auth_limit_closed = impl_->perf.auth_limit_closed.load();
    s.admin_requests = impl_->perf.admin_requests.load();
    return s;
}

std::string Server::last_error() const {
    if (impl_ == nullptr) return {};
    std::lock_guard<std::mutex> lk(impl_->err_mu);
    return impl_->last_error;
}

}  // namespace server
}  // namespace rtdb
