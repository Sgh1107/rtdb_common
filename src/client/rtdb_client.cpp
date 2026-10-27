/// C++ 远程客户端实现（docs/13 §5-W4）：请求面（自动重连/会话恢复）+
/// 订阅泵（回调/last-acked 续传/溢出重同步）。
///
/// 线程模型：
///  - `Client`：全部请求经 `Impl::mu` 串行化；订阅各自独立连接/线程，
///    与请求面互不阻塞；
///  - `Subscription`：泵线程独占一条连接；`Close()` 置停止标志并 join
///    （join 上界 = 泵单次阻塞窗口：长轮询 ≤ delta_wait_ms、连接 ≤ 3s），
///    在回调线程内调用 `Close()` 则不 join（泵自行退出，shared_ptr 保活）。

#include "rtdb/rtdb_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include "infra/sha256.hpp"
#include "net/codec.hpp"
#include "net/value_codec.hpp"
#include "net/wire.hpp"
#include "platform/net.hpp"
#include "rtdb/proto_v1.h"

namespace rtdb {

namespace {

/// 整数类主键判据（SCAN 边界编码只支持整数类，见 W3 协议布局）。
bool IsIntegerClass(DataType t) noexcept {
    switch (t) {
        case DataType::kInt8:
        case DataType::kInt16:
        case DataType::kInt32:
        case DataType::kInt64:
        case DataType::kBool:
        case DataType::kUnixTsNs:
            return true;
        default:
            return false;
    }
}

/// 单次帧交换（请求 → 应答；不做重连）。返回业务错误码；连接级异常
/// 统一为 `kNetworkError`（流可能停在半帧，调用方必须丢弃连接）。
Err RawExchange(platform::TcpSocket& sock, uint32_t session, uint16_t func,
                const std::vector<uint8_t>& body, std::vector<uint8_t>* out,
                std::string* err) {
    net::FrameHeader h;
    h.ver = net::kProtoVersion;
    h.flags = 0;
    h.session = session;
    h.func = func;
    h.payload_len = static_cast<uint32_t>(body.size());
    if (!IsOk(net::WriteFrame(sock, h, body.empty() ? nullptr : body.data()))) {
        if (err != nullptr) *err = "发送失败（连接已断开）";
        return Err::kNetworkError;
    }
    net::FrameHeader rh;
    std::vector<uint8_t> resp;
    if (!IsOk(net::ReadFrame(sock, net::kDefaultMaxPayloadBytes, &rh, &resp))) {
        if (err != nullptr) *err = "接收失败（连接已断开）";
        return Err::kNetworkError;
    }
    if (rh.flags != net::kFlagResp || rh.func != func || rh.session != session) {
        if (err != nullptr) *err = "应答帧头不匹配";
        return Err::kNetworkError;
    }
    if (resp.size() < 2) {
        if (err != nullptr) *err = "应答前导截断";
        return Err::kNetworkError;
    }
    const uint16_t raw =
        static_cast<uint16_t>(resp[0] | (static_cast<uint16_t>(resp[1]) << 8));
    const Err be = static_cast<Err>(static_cast<int16_t>(raw));
    size_t off = 2;
    if (be != Err::kOk) {
        if (resp.size() < 4) {
            if (err != nullptr) *err = "应答前导截断";
            return Err::kNetworkError;
        }
        const uint16_t ml =
            static_cast<uint16_t>(resp[2] | (static_cast<uint16_t>(resp[3]) << 8));
        off = 4 + static_cast<size_t>(ml);
        if (resp.size() < off) {
            if (err != nullptr) *err = "应答前导截断";
            return Err::kNetworkError;
        }
        if (err != nullptr) {
            err->assign(reinterpret_cast<const char*>(resp.data() + 4), ml);
        }
    } else if (err != nullptr) {
        err->clear();
    }
    if (out != nullptr) {
        out->assign(resp.begin() + static_cast<std::ptrdiff_t>(off), resp.end());
    }
    return be;
}

/// 变更事件解码（与 W3 §8 布局一致；成功返回 true）。
bool ReadChange(net::Reader& r, ChangeEvent* ev) noexcept {
    const uint8_t kind = r.U8();
    if (!r.ok()) return false;
    switch (kind) {
        case RTDB_CHG_INSERT:
            ev->kind = ChangeEvent::Kind::kInsert;
            break;
        case RTDB_CHG_UPDATE:
            ev->kind = ChangeEvent::Kind::kUpdate;
            break;
        case RTDB_CHG_DELETE:
            ev->kind = ChangeEvent::Kind::kDelete;
            break;
        default:
            return false;
    }
    ev->table_id = r.I64();
    if (!r.Str(&ev->table)) return false;
    ev->seq = r.U64();
    ev->ts_ns = r.U64();
    ev->row_no = r.U64();
    ev->row_ver = r.U64();
    if (ev->kind == ChangeEvent::Kind::kDelete) {
        ev->key = net::ReadValueGeneric(r);
    } else {
        const uint32_t n = r.U32();
        ev->row.reserve(n);
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            ev->row.push_back(net::ReadValueGeneric(r));
        }
    }
    return r.ok();
}

/// 结果头 → TableSchema（不读行）。
Result<TableSchema> ReadSchema(net::Reader& r) noexcept {
    auto hdr = net::ReadResultHeader(r);
    if (!hdr.IsOk()) return Result<TableSchema>::Fail(hdr.Error());
    TableSchema s;
    s.name = hdr.Value().table;
    s.col_names = hdr.Value().col_names;
    s.col_types = hdr.Value().col_types;
    return Result<TableSchema>::Ok(std::move(s));
}

/// 线格式结果头（schema + 行数；扫描路径用）。
struct WireHeader {
    TableSchema schema;
    uint32_t n_rows = 0;
};

Result<WireHeader> ReadWireHeader(net::Reader& r) noexcept {
    auto hdr = net::ReadResultHeader(r);
    if (!hdr.IsOk()) return Result<WireHeader>::Fail(hdr.Error());
    WireHeader h;
    h.schema.name = hdr.Value().table;
    h.schema.col_names = hdr.Value().col_names;
    h.schema.col_types = hdr.Value().col_types;
    h.n_rows = hdr.Value().n_rows;
    return Result<WireHeader>::Ok(std::move(h));
}

/// 逐行解码（n 行）。
bool ReadRows(net::Reader& r, const std::vector<DataType>& types, uint32_t n,
              std::vector<std::vector<ColumnValue>>* rows) noexcept {
    rows->clear();
    rows->reserve(n);
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        rows->push_back(net::ReadRow(r, types));
    }
    return r.ok();
}

}  // namespace

// ============================================================
// Client::Impl
// ============================================================

struct Client::Impl {
    ClientOptions opts;
    mutable std::mutex mu;          ///< 请求面串行化（含连接状态）
    platform::TcpSocket sock;
    uint32_t session = 0;
    bool alive = false;             ///< 最近一次交换后连接可用
    std::string last_err;

    void DropLocked() noexcept {
        sock.Close();
        session = 0;
        alive = false;
    }

    Err AuthLocked() {
        std::vector<uint8_t> body;
        net::Writer w(&body);
        w.Str(opts.user);
        const auto d = infra::Sha256Concat(opts.auth_salt, opts.password);
        w.Var(std::string(reinterpret_cast<const char*>(d.data()), d.size()));
        std::vector<uint8_t> out;
        const Err e = RawExchange(sock, 0, RTDB_PF_AUTH, body, &out, &last_err);
        if (e != Err::kOk) return e;
        net::Reader r(out.data(), out.size());
        const uint32_t sess = r.U32();
        (void)r.U32();  // ttl_ms
        if (!r.Finish() || sess == 0) {
            last_err = "AUTH 应答异常";
            return Err::kProtocolError;
        }
        session = sess;
        alive = true;
        return Err::kOk;
    }

    Err ConnectLocked() {
        DropLocked();
        (void)platform::NetStartup();
        auto s = platform::TcpSocket::Connect(opts.host, opts.port, opts.connect_timeout_ms);
        if (!s.IsOk()) {
            last_err = "连接失败";
            return s.Error();
        }
        sock = s.TakeValue();
        sock.SetTimeout(opts.request_timeout_ms);
        return AuthLocked();
    }

    enum class Retry { kRead, kWrite };

    /// 请求-应答（含重连/会话恢复策略；见文件头与 rtdb_client.hpp）。
    Err Request(uint16_t func, const std::vector<uint8_t>& body, Retry retry,
                std::vector<uint8_t>* out) {
        std::lock_guard<std::mutex> lk(mu);
        if (!alive) {
            const Err ce = ConnectLocked();
            if (ce != Err::kOk) return ce;
        }
        Err e = RawExchange(sock, session, func, body, out, &last_err);
        if (e == Err::kNetworkError) {
            DropLocked();
            if (retry == Retry::kRead) {
                const Err ce = ConnectLocked();
                if (ce != Err::kOk) return ce;
                e = RawExchange(sock, session, func, body, out, &last_err);
                if (e == Err::kNetworkError) {
                    DropLocked();
                    return e;
                }
            } else {
                return e;  // 写类不自动重发（避免重复写）
            }
        }
        if (e == Err::kTokenExpired) {
            // 会话失效（TTL/服务端重启）：请求必未执行，重新 AUTH 后重发一次。
            const Err ce = ConnectLocked();
            if (ce != Err::kOk) return ce;
            e = RawExchange(sock, session, func, body, out, &last_err);
            if (e == Err::kNetworkError) {
                DropLocked();
                return e;
            }
        }
        alive = (e != Err::kNetworkError);
        return e;
    }
};

// ============================================================
// Subscription::Impl
// ============================================================

struct Subscription::Impl {
    Subscription* owner = nullptr;
    ClientOptions opts;
    SubscriptionOptions sub_opts;

    mutable std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    bool ready = false;
    uint64_t position = 0;       ///< 续传点（最后交付 seq）
    bool has_position = false;   ///< 首连后置真（之后重连不覆盖）
    uint64_t reset_epoch = 0;    ///< ResetTo 计数（溢出挂起等待判据）
    std::string last_err;

    platform::TcpSocket sock;  ///< 泵私有
    uint32_t session = 0;
    uint32_t sub_id = 0;
    std::thread pump;

    bool IsStop() {
        std::lock_guard<std::mutex> lk(mu);
        return stop;
    }

    void SetReady(bool v) {
        {
            std::lock_guard<std::mutex> lk(mu);
            ready = v;
        }
        cv.notify_all();
    }

    void SetErr(std::string s) {
        std::lock_guard<std::mutex> lk(mu);
        last_err = std::move(s);
    }

    void DropConn() noexcept {
        sock.Close();
        session = 0;
        sub_id = 0;
        SetReady(false);
    }

    void NotifyState(bool connected, uint32_t attempt) noexcept {
        if (!sub_opts.on_state) return;
        try {
            sub_opts.on_state(connected, attempt);
        } catch (...) {
        }
    }

    void SleepBackoff(uint32_t attempt) {
        uint32_t ms = sub_opts.reconnect_min_ms;
        for (uint32_t i = 1; i < attempt && ms < sub_opts.reconnect_max_ms; ++i) {
            ms = (ms < sub_opts.reconnect_max_ms / 2) ? ms * 2 : sub_opts.reconnect_max_ms;
        }
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stop; });
    }

    /// 建立连接 + AUTH + SUBSCRIBE（幂等失败清理）。
    bool ConnectSubscribe() {
        DropConn();
        if (IsStop()) return false;

        (void)platform::NetStartup();
        auto s = platform::TcpSocket::Connect(opts.host, opts.port, opts.connect_timeout_ms);
        if (!s.IsOk()) {
            SetErr("连接失败");
            return false;
        }
        sock = s.TakeValue();
        sock.SetTimeout(opts.request_timeout_ms);

        {
            std::vector<uint8_t> body;
            net::Writer w(&body);
            w.Str(opts.user);
            const auto d = infra::Sha256Concat(opts.auth_salt, opts.password);
            w.Var(std::string(reinterpret_cast<const char*>(d.data()), d.size()));
            std::vector<uint8_t> out;
            std::string err;
            const Err e = RawExchange(sock, 0, RTDB_PF_AUTH, body, &out, &err);
            if (e != Err::kOk) {
                SetErr(err.empty() ? "AUTH 失败" : err);
                return false;
            }
            net::Reader r(out.data(), out.size());
            const uint32_t sess = r.U32();
            (void)r.U32();
            if (!r.Finish() || sess == 0) {
                SetErr("AUTH 应答异常");
                return false;
            }
            session = sess;
        }
        {
            std::vector<uint8_t> body;
            net::Writer w(&body);
            w.U32(static_cast<uint32_t>(sub_opts.tables.size()));
            for (const std::string& t : sub_opts.tables) w.Str(t);
            std::vector<uint8_t> out;
            std::string err;
            const Err e = RawExchange(sock, session, RTDB_PF_SUBSCRIBE, body, &out, &err);
            if (e != Err::kOk) {
                SetErr(err.empty() ? "SUBSCRIBE 失败" : err);
                return false;
            }
            net::Reader r(out.data(), out.size());
            const uint32_t sid = r.U32();
            const int64_t start_seq = r.I64();
            if (!r.Finish() || sid == 0) {
                SetErr("SUBSCRIBE 应答异常");
                return false;
            }
            sub_id = sid;
            std::lock_guard<std::mutex> lk(mu);
            if (!has_position) {  // 首连：以服务端订阅位置为起点
                position = start_seq < 0 ? 0 : static_cast<uint64_t>(start_seq);
                has_position = true;
            }
        }
        return true;
    }

    /// 单次 FETCH_DELTA（长轮询）→ 回调交付。返回 kOk = 成功（无论批空否）。
    Err PumpOnce() {
        uint64_t from = 0;
        uint64_t epoch_before = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            from = position;
            epoch_before = reset_epoch;
        }

        std::vector<uint8_t> body;
        net::Writer w(&body);
        w.U32(sub_id);
        w.U64(from);
        w.U32(sub_opts.delta_wait_ms);
        w.U32(sub_opts.delta_max_bytes);

        std::vector<uint8_t> out;
        std::string err;
        const Err e = RawExchange(sock, session, RTDB_PF_FETCH_DELTA, body, &out, &err);
        if (e != Err::kOk) {
            SetErr(err.empty() ? "FETCH_DELTA 失败" : err);
            return e;
        }

        net::Reader r(out.data(), out.size());
        (void)r.I64();  // last_seq（客户端以逐事件 seq 推进续传点）
        const uint32_t n = r.U32();
        if (!r.ok()) return Err::kProtocolError;
        std::vector<ChangeEvent> evs;
        evs.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            ChangeEvent ev;
            if (!ReadChange(r, &ev)) return Err::kProtocolError;
            evs.push_back(std::move(ev));
        }
        if (!r.Finish()) return Err::kProtocolError;

        for (const ChangeEvent& ev : evs) {
            if (sub_opts.on_change) {
                bool ok = true;
                try {
                    sub_opts.on_change(ev);
                } catch (...) {
                    // 回调异常：不推进本事件（下一轮重投；服务端允许重复）
                    ok = false;
                    SetErr("on_change 回调异常（事件将重投）");
                }
                if (!ok) break;
            }
            std::lock_guard<std::mutex> lk(mu);
            if (reset_epoch == epoch_before && ev.seq > position) position = ev.seq;
        }

        if (!evs.empty()) {  // ACK（诊断账本；失败忽略）
            uint64_t ack_seq;
            {
                std::lock_guard<std::mutex> lk(mu);
                ack_seq = position;
            }
            std::vector<uint8_t> ab;
            net::Writer aw(&ab);
            aw.U32(sub_id);
            aw.U64(ack_seq);
            (void)RawExchange(sock, session, RTDB_PF_ACK, ab, nullptr, nullptr);
        }
        return Err::kOk;
    }

    /// 溢出重同步：回调 + 挂起等待 ResetTo（或 stop；不空转）。
    void HandleOverflow() {
        uint64_t epoch_before = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            epoch_before = reset_epoch;
        }
        SetErr("订阅游标越窗（需整表重取后 ResetTo）");
        if (sub_opts.on_overflow) {
            try {
                sub_opts.on_overflow(*owner);
            } catch (...) {
                SetErr("on_overflow 回调异常");
            }
        }
        std::unique_lock<std::mutex> lk(mu);
        while (!stop && reset_epoch == epoch_before) {
            cv.wait_for(lk, std::chrono::milliseconds(100));
        }
    }

    void Run() {
        uint32_t attempt = 0;
        bool linked = false;  // 连接 + AUTH + SUBSCRIBE 是否已建立
        while (!IsStop()) {
            if (!linked) {
                if (!ConnectSubscribe()) {
                    ++attempt;
                    NotifyState(false, attempt);
                    SleepBackoff(attempt);
                    continue;
                }
                attempt = 0;
                linked = true;
                SetReady(true);
                NotifyState(true, 0);
            }

            const Err e = PumpOnce();
            if (e == Err::kOk) continue;
            if (e == Err::kCursorOverflow) {
                HandleOverflow();  // 连接仍好：等重同步后继续（不复连）
                continue;
            }
            DropConn();
            linked = false;
            ++attempt;
            NotifyState(false, attempt);
            SleepBackoff(attempt);
        }
        DropConn();
    }
};

// ============================================================
// Subscription 门面
// ============================================================

Subscription::Subscription() noexcept = default;

Subscription::~Subscription() { Close(); }

Subscription::Subscription(Subscription&& other) noexcept
    : impl_(std::move(other.impl_)) {
    if (impl_ != nullptr) impl_->owner = this;
}

Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        Close();
        impl_ = std::move(other.impl_);
        if (impl_ != nullptr) impl_->owner = this;
    }
    return *this;
}

void Subscription::Close() noexcept {
    if (impl_ == nullptr) return;
    Impl& im = *impl_;
    {
        std::lock_guard<std::mutex> lk(im.mu);
        im.stop = true;
    }
    im.cv.notify_all();
    // 泵单次阻塞窗口有界（长轮询 ≤ delta_wait、连接 ≤ connect_timeout），
    // join 等待上界即该窗口；回调线程内调用不 join（泵自行退出，
    // shared_ptr 保活 Impl）。
    if (im.pump.joinable() && im.pump.get_id() != std::this_thread::get_id()) {
        im.pump.join();
    }
}

bool Subscription::connected() const noexcept {
    if (impl_ == nullptr) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->ready;
}

uint64_t Subscription::position() const noexcept {
    if (impl_ == nullptr) return 0;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->position;
}

void Subscription::ResetTo(uint64_t seq) noexcept {
    if (impl_ == nullptr) return;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->position = seq;
        impl_->has_position = true;
        ++impl_->reset_epoch;
    }
    impl_->cv.notify_all();
}

bool Subscription::WaitReady(uint32_t timeout_ms) const noexcept {
    if (impl_ == nullptr) return false;
    Impl& im = *impl_;
    std::unique_lock<std::mutex> lk(im.mu);
    im.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                   [&im] { return im.ready || im.stop; });
    return im.ready;
}

std::string Subscription::last_error() const {
    if (impl_ == nullptr) return {};
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_err;
}

// ============================================================
// Client 门面
// ============================================================

Client::Client() noexcept = default;

Client::~Client() { Close(); }

Result<std::unique_ptr<Client>> Client::Connect(const ClientOptions& opts) noexcept {
    if (opts.port == 0 || opts.user.empty()) {
        return Result<std::unique_ptr<Client>>::Fail(Err::kInvalidArgument);
    }
    std::unique_ptr<Client> c(new Client());
    c->impl_ = std::make_unique<Impl>();
    c->impl_->opts = opts;
    std::lock_guard<std::mutex> lk(c->impl_->mu);
    const Err e = c->impl_->ConnectLocked();
    if (e != Err::kOk) return Result<std::unique_ptr<Client>>::Fail(e);
    return Result<std::unique_ptr<Client>>::Ok(std::move(c));
}

bool Client::connected() const noexcept {
    if (impl_ == nullptr) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->alive;
}

uint32_t Client::session() const noexcept {
    if (impl_ == nullptr) return 0;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->session;
}

void Client::Close() noexcept {
    if (impl_ == nullptr) return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->DropLocked();
}

std::string Client::last_error() const {
    if (impl_ == nullptr) return {};
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_err;
}

namespace {

/// STATS key → 稳定指标名（口径见 C4；未知 key 保留 unknown_<id> 前向兼容）。
std::string StatsKeyName(uint16_t key) {
    switch (key) {
        case RTDB_STAT_UPTIME_MS:
            return "uptime_ms";
        case RTDB_STAT_ACCEPTED:
            return "accepted";
        case RTDB_STAT_REJECTED_FULL:
            return "rejected_full";
        case RTDB_STAT_AUTH_OK:
            return "auth_ok";
        case RTDB_STAT_AUTH_FAIL:
            return "auth_fail";
        case RTDB_STAT_REQUESTS:
            return "requests";
        case RTDB_STAT_FRAME_ERRORS:
            return "frame_errors";
        case RTDB_STAT_PAYLOAD_ERRORS:
            return "payload_errors";
        case RTDB_STAT_AUTHZ_DENIED:
            return "authz_denied";
        case RTDB_STAT_SESSIONS_CURRENT:
            return "sessions_current";
        case RTDB_STAT_SESSIONS_TOTAL:
            return "sessions_total";
        case RTDB_STAT_HANDSHAKE_TIMEOUTS:
            return "handshake_timeouts";
        case RTDB_STAT_IDLE_TIMEOUTS:
            return "idle_timeouts";
        case RTDB_STAT_AUTH_LIMIT_CLOSED:
            return "auth_limit_closed";
        case RTDB_STAT_ADMIN_REQUESTS:
            return "admin_requests";
        case RTDB_STAT_COMMITTED_SEQ:
            return "committed_seq";
        case RTDB_STAT_CHECKPOINT_SEQ:
            return "checkpoint_seq";
        case RTDB_STAT_WRITER_HEARTBEAT_NS:
            return "writer_heartbeat_ns";
        case RTDB_STAT_SESSION_TTL_MS:
            return "session_ttl_ms";
        default:
            return "unknown_" + std::to_string(static_cast<unsigned>(key));
    }
}

}  // namespace

uint64_t AdminStats::Get(std::string_view key) const {
    const auto it = values.find(std::string(key));
    return it == values.end() ? 0 : it->second;
}

bool AdminStats::Has(std::string_view key) const {
    return values.find(std::string(key)) != values.end();
}

Result<PingInfo> Client::Ping() {
    std::vector<uint8_t> out;
    const Err e = impl_->Request(RTDB_PF_PING, {}, Impl::Retry::kRead, &out);
    if (e != Err::kOk) return Result<PingInfo>::Fail(e);
    net::Reader r(out.data(), out.size());
    PingInfo p;
    p.committed_seq = r.I64();
    p.writer_heartbeat_ns = r.U64();
    p.checkpoint_seq = r.I64();
    p.session_ttl_ms = r.U32();
    if (!r.Finish()) return Result<PingInfo>::Fail(Err::kProtocolError);
    return Result<PingInfo>::Ok(p);
}

Result<AdminStats> Client::GetServerStats() {
    std::vector<uint8_t> body;
    {
        net::Writer w(&body);
        w.U8(RTDB_ADMIN_STATS);
    }
    std::vector<uint8_t> out;
    const Err e = impl_->Request(RTDB_PF_ADMIN, body, Impl::Retry::kRead, &out);
    if (e != Err::kOk) return Result<AdminStats>::Fail(e);
    net::Reader r(out.data(), out.size());
    const uint16_t ver = r.U16();
    const uint32_t n = r.U32();
    if (!r.ok() || ver != RTDB_STATS_VERSION) return Result<AdminStats>::Fail(Err::kProtocolError);
    AdminStats st;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        const uint16_t key = r.U16();
        const uint64_t val = r.U64();
        if (!r.ok()) return Result<AdminStats>::Fail(Err::kProtocolError);
        st.values[StatsKeyName(key)] = val;
    }
    if (!r.Finish()) return Result<AdminStats>::Fail(Err::kProtocolError);
    return Result<AdminStats>::Ok(std::move(st));
}

Result<AuthzInfo> Client::GetAuthzInfo() {
    std::vector<uint8_t> out;
    const Err e = impl_->Request(RTDB_PF_AUTHZ_ME, {}, Impl::Retry::kRead, &out);
    if (e != Err::kOk) return Result<AuthzInfo>::Fail(e);
    net::Reader r(out.data(), out.size());
    AuthzInfo info;
    if (!r.Str(&info.user)) return Result<AuthzInfo>::Fail(Err::kProtocolError);
    const uint8_t flags = r.U8();
    if (!r.ok()) return Result<AuthzInfo>::Fail(Err::kProtocolError);
    info.admin = (flags & RTDB_AUTHZ_FLAG_ADMIN) != 0;
    info.ddl = (flags & RTDB_AUTHZ_FLAG_DDL) != 0;
    const uint32_t n_r = r.U32();
    if (!r.ok() || n_r > 4096) return Result<AuthzInfo>::Fail(Err::kProtocolError);
    for (uint32_t i = 0; i < n_r; ++i) {
        std::string t;
        if (!r.Str(&t)) return Result<AuthzInfo>::Fail(Err::kProtocolError);
        info.read_tables.push_back(std::move(t));
    }
    const uint32_t n_w = r.U32();
    if (!r.ok() || n_w > 4096) return Result<AuthzInfo>::Fail(Err::kProtocolError);
    for (uint32_t i = 0; i < n_w; ++i) {
        std::string t;
        if (!r.Str(&t)) return Result<AuthzInfo>::Fail(Err::kProtocolError);
        info.write_tables.push_back(std::move(t));
    }
    if (!r.Finish()) return Result<AuthzInfo>::Fail(Err::kProtocolError);
    return Result<AuthzInfo>::Ok(std::move(info));
}

Result<CreateTableInfo> Client::CreateTable(const TableDef& def) {
    std::vector<uint8_t> body;
    net::Writer w(&body);
    w.U8(RTDB_DDL_CREATE_TABLE);
    net::WriteTableDef(w, def);
    std::vector<uint8_t> out;
    const Err e = impl_->Request(RTDB_PF_DDL, body, Impl::Retry::kWrite, &out);
    if (e != Err::kOk) return Result<CreateTableInfo>::Fail(e);
    net::Reader r(out.data(), out.size());
    CreateTableInfo info;
    info.table_id = r.I64();
    info.visible_columns = r.U32();
    info.layout_version = r.U32();
    if (!r.Finish()) return Result<CreateTableInfo>::Fail(Err::kProtocolError);
    return Result<CreateTableInfo>::Ok(info);
}

Result<void> Client::DropTable(std::string_view name) {
    std::vector<uint8_t> body;
    net::Writer w(&body);
    w.U8(RTDB_DDL_DROP_TABLE);
    w.Str(name);
    const Err e = impl_->Request(RTDB_PF_DDL, body, Impl::Retry::kWrite, nullptr);
    if (e != Err::kOk) return Result<void>::Fail(e);
    return Result<void>::Ok();
}

Result<TableSchema> Client::DescribeTable(std::string_view name) {
    std::vector<uint8_t> body;
    net::Writer w(&body);
    w.U8(RTDB_DDL_DESCRIBE_TABLE);
    w.Str(name);
    std::vector<uint8_t> out;
    const Err e = impl_->Request(RTDB_PF_DDL, body, Impl::Retry::kRead, &out);
    if (e != Err::kOk) return Result<TableSchema>::Fail(e);
    net::Reader r(out.data(), out.size());
    const int64_t table_id = r.I64();
    const uint32_t layout = r.U32();
    if (!r.ok()) return Result<TableSchema>::Fail(Err::kProtocolError);
    auto schema = ReadSchema(r);
    if (!schema.IsOk()) return schema;
    if (!r.Finish()) return Result<TableSchema>::Fail(Err::kProtocolError);
    schema.Value().table_id = table_id;
    schema.Value().layout_version = layout;
    return schema;
}

Result<int64_t> Client::ExecuteBatch(const std::vector<ClientBatchOp>& ops) {
    std::vector<uint8_t> body;
    net::Writer w(&body);
    w.U8(RTDB_BATCH_MODE_OPS);
    w.U32(static_cast<uint32_t>(ops.size()));
    for (const ClientBatchOp& op : ops) {
        w.U8(static_cast<uint8_t>(op.type));
        w.Str(op.table);
        for (const ColumnValue& v : op.row) net::WriteValue(w, v);
    }
    std::vector<uint8_t> out;
    const Err e = impl_->Request(RTDB_PF_EXEC_BATCH, body, Impl::Retry::kWrite, &out);
    if (e != Err::kOk) return Result<int64_t>::Fail(e);
    net::Reader r(out.data(), out.size());
    const int64_t committed = r.I64();
    if (!r.Finish()) return Result<int64_t>::Fail(Err::kProtocolError);
    return Result<int64_t>::Ok(committed);
}

Result<int64_t> Client::UpsertBatch(const RecordBlockView& block) {
    std::vector<uint8_t> body;
    net::Writer w(&body);
    w.U8(RTDB_BATCH_MODE_BLOCK);
    net::WriteBlock(w, block);
    std::vector<uint8_t> out;
    const Err e = impl_->Request(RTDB_PF_EXEC_BATCH, body, Impl::Retry::kWrite, &out);
    if (e != Err::kOk) return Result<int64_t>::Fail(e);
    net::Reader r(out.data(), out.size());
    const int64_t committed = r.I64();
    if (!r.Finish()) return Result<int64_t>::Fail(Err::kProtocolError);
    return Result<int64_t>::Ok(committed);
}

Result<ClientScanResult> Client::Scan(std::string_view table, const Engine::KeyRange& range) {
    std::vector<uint8_t> body;
    net::Writer w(&body);
    w.Str(table);
    for (int side = 0; side < 2; ++side) {
        const ColumnValue& v = (side == 0) ? range.lo : range.hi;
        if (v.is_null()) {
            w.U8(0);
            continue;
        }
        if (!IsIntegerClass(v.type())) {
            return Result<ClientScanResult>::Fail(Err::kInvalidArgument);
        }
        w.U8(1);
        w.U8(RTDB_VAL_I64);
        w.I64(v.as_int());
    }
    std::vector<uint8_t> out;
    const Err e = impl_->Request(RTDB_PF_SCAN, body, Impl::Retry::kRead, &out);
    if (e != Err::kOk) return Result<ClientScanResult>::Fail(e);
    net::Reader r(out.data(), out.size());
    auto hdr = ReadWireHeader(r);
    if (!hdr.IsOk()) return Result<ClientScanResult>::Fail(hdr.Error());
    ClientScanResult res;
    res.schema = std::move(hdr.Value().schema);
    if (!ReadRows(r, res.schema.col_types, hdr.Value().n_rows, &res.rows)) {
        return Result<ClientScanResult>::Fail(Err::kProtocolError);
    }
    if (!r.Finish()) return Result<ClientScanResult>::Fail(Err::kProtocolError);
    return Result<ClientScanResult>::Ok(std::move(res));
}

Result<uint64_t> Client::ScanPaged(std::string_view table, uint32_t page_rows,
                                   const std::function<bool(const ClientScanResult&)>& on_page) {
    if (page_rows == 0 || page_rows > 100000) {
        return Result<uint64_t>::Fail(Err::kInvalidArgument);
    }
    auto ClosePage = [this](uint64_t page_id) {
        std::vector<uint8_t> cb;
        net::Writer cw(&cb);
        cw.U8(RTDB_SP_CLOSE);
        cw.U64(page_id);
        // 尽力关闭（失败由会话回收兜底）
        (void)impl_->Request(RTDB_PF_SCAN_PAGE, cb, Impl::Retry::kRead, nullptr);
    };

    // OPEN
    std::vector<uint8_t> body;
    net::Writer w(&body);
    w.U8(RTDB_SP_OPEN);
    w.Str(table);
    w.U32(page_rows);
    std::vector<uint8_t> out;
    Err e = impl_->Request(RTDB_PF_SCAN_PAGE, body, Impl::Retry::kRead, &out);
    if (e != Err::kOk) return Result<uint64_t>::Fail(e);

    net::Reader r(out.data(), out.size());
    const uint64_t page_id = r.U64();
    uint8_t done = r.U8();
    if (!r.ok()) return Result<uint64_t>::Fail(Err::kProtocolError);
    ClientScanResult page;
    auto hdr = ReadWireHeader(r);
    if (!hdr.IsOk()) {
        ClosePage(page_id);
        return Result<uint64_t>::Fail(hdr.Error());
    }
    page.schema = std::move(hdr.Value().schema);
    if (!ReadRows(r, page.schema.col_types, hdr.Value().n_rows, &page.rows) || !r.Finish()) {
        ClosePage(page_id);
        return Result<uint64_t>::Fail(Err::kProtocolError);
    }
    uint64_t total = page.rows.size();
    if (on_page && !on_page(page)) {
        ClosePage(page_id);
        return Result<uint64_t>::Ok(total);
    }

    // FETCH 循环（逐页；页面行数由服务端保证 ≤ page_rows）
    while (!done) {
        std::vector<uint8_t> fb;
        net::Writer fw(&fb);
        fw.U8(RTDB_SP_FETCH);
        fw.U64(page_id);
        out.clear();
        e = impl_->Request(RTDB_PF_SCAN_PAGE, fb, Impl::Retry::kRead, &out);
        if (e != Err::kOk) return Result<uint64_t>::Fail(e);
        net::Reader fr(out.data(), out.size());
        done = fr.U8();
        const uint32_t n = fr.U32();
        if (!fr.ok()) return Result<uint64_t>::Fail(Err::kProtocolError);
        page.rows.clear();
        if (!ReadRows(fr, page.schema.col_types, n, &page.rows) || !fr.Finish()) {
            return Result<uint64_t>::Fail(Err::kProtocolError);
        }
        total += page.rows.size();
        if (on_page && !on_page(page)) break;
    }
    ClosePage(page_id);
    return Result<uint64_t>::Ok(total);
}

Result<std::unique_ptr<Subscription>> Client::Subscribe(const SubscriptionOptions& opts) {
    if (impl_ == nullptr) return Result<std::unique_ptr<Subscription>>::Fail(Err::kInvalidArgument);
    std::unique_ptr<Subscription> sub(new Subscription());
    sub->impl_ = std::make_shared<Subscription::Impl>();
    Subscription::Impl& si = *sub->impl_;
    si.owner = sub.get();
    si.opts = impl_->opts;
    si.sub_opts = opts;
    // 泵线程持有 Impl 的 shared_ptr（Close 在回调内调用时保活）。
    std::shared_ptr<Subscription::Impl> keep = sub->impl_;
    si.pump = std::thread([keep] { keep->Run(); });
    return Result<std::unique_ptr<Subscription>>::Ok(std::move(sub));
}

}  // namespace rtdb
