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

    std::atomic<uint64_t> stat_accepted{0};
    std::atomic<uint64_t> stat_rejected_full{0};
    std::atomic<uint64_t> stat_auth_ok{0};
    std::atomic<uint64_t> stat_auth_fail{0};
    std::atomic<uint64_t> stat_requests{0};
    std::atomic<uint64_t> stat_frame_errors{0};
    std::atomic<uint64_t> stat_payload_errors{0};

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
    void ConnReleased(bool is_session) {
        {
            std::lock_guard<std::mutex> lk(conn_mu);
            if (live_threads > 0) --live_threads;
            if (is_session && live_sessions > 0) --live_sessions;
        }
        conn_cv.notify_all();
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
    Err Dispatch(SessionState& st, const net::FrameHeader& h, const uint8_t* payload,
                 size_t len, std::vector<uint8_t>* body);
    Err DoAuth(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err DoPing(net::Reader& r, std::vector<uint8_t>* body);
    Err DoDdl(net::Reader& r, std::vector<uint8_t>* body);
    Err DoExecBatch(net::Reader& r, std::vector<uint8_t>* body);
    Err DoScan(net::Reader& r, std::vector<uint8_t>* body);
    Err DoScanPage(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err FetchPageSlice(SessionState::Page& p, std::vector<std::vector<ColumnValue>>* rows,
                       bool* done);
    Err DoSubscribe(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err DoFetchDelta(SessionState& st, net::Reader& r, std::vector<uint8_t>* body);
    Err DoAck(SessionState& st, net::Reader& r);
    Err DoUnregister(SessionState& st, net::Reader& r);
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
            stat_rejected_full++;
            std::thread(&Server::Impl::RejectLoop, this, std::move(sock)).detach();
        } else {
            stat_accepted++;
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
    struct Guard {
        Impl* im;
        ~Guard() { im->ConnReleased(true); }
    } guard{this};

    sock.SetTimeout(kSessionPollMs);
    SessionState st;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> body;
    std::vector<uint8_t> resp;

    for (;;) {
        if (stop.load(std::memory_order_relaxed)) break;

        net::FrameHeader h;
        payload.clear();
        const Err fe = net::ReadFrame(sock, cfg.max_payload_bytes, &h, &payload);
        if (fe == Err::kTimeout) continue;
        if (!IsOk(fe)) {
            stat_frame_errors++;  // 帧级违例/EOF：流不可再信任，关闭连接
            break;
        }
        if (h.flags != 0) {
            stat_frame_errors++;  // 请求帧不得携带应答位
            break;
        }

        stat_requests++;
        body.clear();
        const Err re = Dispatch(st, h, payload.data(), payload.size(), &body);
        if (!IsOk(re)) stat_payload_errors++;

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
    }
    sock.Close();
}

void Server::Impl::RejectLoop(platform::TcpSocket sock) {
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
            return DoDdl(r, body);
        case RTDB_PF_EXEC_BATCH:
            return DoExecBatch(r, body);
        case RTDB_PF_SCAN:
            return DoScan(r, body);
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
        stat_auth_fail++;
        return Err::kAuthFailed;  // 501：凭据错误（连接保留，可重试）
    }

    stat_auth_ok++;
    st.id = NextSessionId();
    st.last_active_ns = SteadyNowNs();
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

Err Server::Impl::DoDdl(net::Reader& r, std::vector<uint8_t>* body) {
    const uint8_t sub = r.U8();
    if (!r.ok()) return Err::kProtocolError;

    if (sub == RTDB_DDL_CREATE_TABLE) {
        auto def = net::ReadTableDef(r);
        if (!def.IsOk()) return def.Error();
        if (!r.Finish()) return Err::kProtocolError;
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
        const Err e = engine->DropTable(name).Error();
        if (IsOk(e)) return Err::kOk;
        return e;
    }
    if (sub == RTDB_DDL_DESCRIBE_TABLE) {
        std::string name;
        if (!r.Str(&name)) return Err::kProtocolError;
        if (!r.Finish()) return Err::kProtocolError;
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

Err Server::Impl::DoExecBatch(net::Reader& r, std::vector<uint8_t>* body) {
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

        const Err e = engine->ExecuteBatch(ops).Error();
        if (!IsOk(e)) return e;
        net::Writer w(body);
        w.I64(engine->CommittedSeq());
        return Err::kOk;
    }

    if (mode == RTDB_BATCH_MODE_BLOCK) {
        auto blk = net::ReadBlock(r);
        if (!blk.IsOk()) return blk.Error();
        if (!r.Finish()) return Err::kProtocolError;
        const Err e = engine->UpsertBatch(blk.Value().View()).Error();
        if (!IsOk(e)) return e;
        net::Writer w(body);
        w.I64(engine->CommittedSeq());
        return Err::kOk;
    }
    return Err::kProtocolError;  // 未知模式
}

Err Server::Impl::DoScan(net::Reader& r, std::vector<uint8_t>* body) {
    std::string table;
    if (!r.Str(&table)) return Err::kProtocolError;

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
    auto res = engine->Scan(p.handle, range);
    if (!res.IsOk()) return res.Error();

    const std::vector<std::vector<ColumnValue>>& all = res.Value();
    const size_t take = all.size() < p.page_rows ? all.size() : p.page_rows;
    rows->assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(take));
    *done = all.size() <= p.page_rows;
    if (take > 0 && !(*rows)[take - 1].empty()) {
        p.next_lo = (*rows)[take - 1][0];  // 主键 = 列 0（建表约束）
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

// ============================================================
// Server 门面
// ============================================================

Server::Server() noexcept = default;

Server::~Server() {
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
    im.running = true;
    im.accept_thread = std::thread(&Server::Impl::AcceptLoop, &im);
    im.heartbeat_thread = std::thread(&Server::Impl::HeartbeatLoop, &im);
    return Err::kOk;
}

void Server::Stop(uint32_t timeout_ms) {
    if (impl_ == nullptr || !impl_->running) return;
    Impl& im = *impl_;
    im.stop.store(true);

    if (im.accept_thread.joinable()) im.accept_thread.join();
    if (im.heartbeat_thread.joinable()) im.heartbeat_thread.join();
    im.listener.Close();

    {
        std::unique_lock<std::mutex> lk(im.conn_mu);
        im.conn_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [&im] { return im.live_threads == 0; });
    }

    if (im.live_threads == 0) {
        if (im.cfg.snapshot_on_exit && im.engine != nullptr) {
            auto s = im.engine->CreateSnapshot();
            if (!s.IsOk()) {
                im.SetError(std::string("snapshot_on_exit: ") + ErrorMessage(s.Error()));
            }
        }
        im.engine.reset();  // 关引擎：最终 flush（docs/13 §3-D7）
    } else {
        // 极端情形：会话线程未在预算内收敛（对端卡死/平台异常）。
        // 保安全不析构引擎（会话线程仍可能持有引用），进程退出统一回收。
        im.SetError("stop timeout: live sessions remain; engine leaked to avoid UAF");
        (void)im.engine.release();
    }
    im.running = false;
    im.bound_port.store(0);
}

bool Server::running() const noexcept { return impl_ != nullptr && impl_->running; }

uint16_t Server::port() const noexcept {
    return impl_ != nullptr ? impl_->bound_port.load() : 0;
}

Engine* Server::engine() noexcept { return impl_ != nullptr ? impl_->engine.get() : nullptr; }

Server::Stats Server::stats() const noexcept {
    Stats s;
    if (impl_ == nullptr) return s;
    s.accepted = impl_->stat_accepted.load();
    s.rejected_full = impl_->stat_rejected_full.load();
    s.auth_ok = impl_->stat_auth_ok.load();
    s.auth_fail = impl_->stat_auth_fail.load();
    s.requests = impl_->stat_requests.load();
    s.frame_errors = impl_->stat_frame_errors.load();
    s.payload_errors = impl_->stat_payload_errors.load();
    return s;
}

std::string Server::last_error() const {
    if (impl_ == nullptr) return {};
    std::lock_guard<std::mutex> lk(impl_->err_mu);
    return impl_->last_error;
}

}  // namespace server
}  // namespace rtdb
