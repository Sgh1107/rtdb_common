#ifndef RTDB_RTDB_CLIENT_HPP_
#define RTDB_RTDB_CLIENT_HPP_

/// \file
/// \brief C++ 远程客户端面（docs/13 §5-W4）：连接/自动重连、请求便捷面
/// （DDL/批量/扫描/分页扫描）、订阅泵（回调 + last-acked 续传 + 溢出重取）。
///
/// 模型：
///  - `Client` = 一条请求连接，内部串行化（多线程可共享调用；请求按序
///    执行，单次请求不重入）；
///  - `Subscription` = 独立一条连接 + 一个泵线程（FETCH_DELTA 长轮询），
///    与请求面互不阻塞；变更回调发生在泵线程（回调内可安全使用 Client
///    的只读请求，不要做长耗时阻塞）。
///
/// 请求重试边界（docs/13 §5-W4 定稿）：
///  - 读类请求（PING/描述/扫描）遇网络错误自动重连并**重发一次**；
///  - 写类请求（建表/删表/EXEC_BATCH/块写）**不自动重发**——"已提交但
///    应答丢失"时重发会产生重复写，由调用方按幂等语义决定是否重试；
///  - 会话失效（`kTokenExpired`）在任何请求上都会自动重新 AUTH 后重发
///    一次（该请求必未执行，无重复风险）。
///
/// 订阅续传（docs/04 §5 降级路径）：
///  - 泵以 `Subscription::position()`（最后交付 seq）为续传点；断连/会话
///    过期 → 指数退避重连 + AUTH/SUBSCRIBE 后从续传点 FETCH_DELTA（服务端
///    允许重复、不允许缺漏，故续传只会重复交付、不会丢事件）；
///  - 游标越窗（`kCursorOverflow`，环回收后落后过深）→ 触发
///    `on_overflow`：调用方整表重取（如 `ScanPaged`），再以"重取时
///    `Ping().committed_seq`"调用 `ResetTo`，泵被唤醒后从新位置续跑。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rtdb/api_macro.hpp"
#include "rtdb/change_cursor.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/engine.hpp"
#include "rtdb/err.hpp"
#include "rtdb/record_block.hpp"
#include "rtdb/result.hpp"
#include "rtdb/table_def.hpp"

namespace rtdb {

/// 客户端连接参数（AUTH 摘要 = SHA-256(auth_salt || password)，与服务端
/// 配置的盐一致；docs/13 §8-W3）。
struct ClientOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 0;  ///< 必填（90xx 保留给本地嵌入，无默认端口）
    std::string user;
    std::string password;
    std::string auth_salt;

    uint32_t connect_timeout_ms = 3000;
    uint32_t request_timeout_ms = 5000;
};

/// PING 应答（服务端活性/进度面；docs/13 §8-W3 PING 布局）。
struct PingInfo {
    int64_t committed_seq = -1;        ///< 全局提交序号（-1 = 未知）
    uint64_t writer_heartbeat_ns = 0;  ///< 写者心跳（Unix 纳秒回环）
    int64_t checkpoint_seq = -1;       ///< 检查点水位
    uint32_t session_ttl_ms = 0;       ///< 会话 TTL（毫秒）
};

/// 表 schema 快照（DESCRIBE/扫描结果自描述面；列可空性与默认值不在
/// 线格式内——M3 只交付名称/类型）。
struct TableSchema {
    std::string name;
    std::vector<std::string> col_names;
    std::vector<DataType> col_types;
    int64_t table_id = 0;         ///< DESCRIBE 提供；SCAN 结果为 0
    uint32_t layout_version = 0;  ///< DESCRIBE 提供；SCAN 结果为 0
};

/// 建表应答（DESCRIBE 可另取完整 schema）。
struct CreateTableInfo {
    int64_t table_id = 0;
    uint32_t visible_columns = 0;
    uint32_t layout_version = 0;
};

/// 扫描结果（自持有；行值 = 句柄可见列序，同 `Engine::Scan`）。
struct ClientScanResult {
    TableSchema schema;
    std::vector<std::vector<ColumnValue>> rows;
};

/// 客户端批操作单元（按表名寻址；语义 = `Engine::BatchOp`：批内互不
/// 依赖，基于批前状态）。
struct ClientBatchOp {
    enum class Type : uint8_t { kInsert = 1, kUpdate = 2, kDelete = 3 };
    Type type = Type::kInsert;
    std::string table;
    /// insert/update：整行（可见列序，update 的 row[0] = 原主键）；
    /// delete：单元素主键。
    std::vector<ColumnValue> row;
};

class Subscription;
struct SubscriptionOptions;

#if defined(_MSC_VER)
#pragma warning(push)
// C4251: PImpl 成员（unique_ptr/shared_ptr<未导出 Impl>）不参与 DLL 接口
// 实体化 —— 与 Engine/ChangeCursor 同策略（engine.hpp/change_cursor.hpp）。
#pragma warning(disable : 4251)
#endif

/// \brief 远程客户端（请求面）。生命周期 = 一条 TCP 连接 + 会话；
/// 断连后由下一次请求自动重建（读类重发一次，见文件头约定）。
class RTDB_CLIENT_API Client {
public:
    /// 连接 + AUTH。失败：kNetworkError/kTimeout（连不上）、kAuthFailed
    /// （口令错）、kProtocolError（对端异常）。
    static Result<std::unique_ptr<Client>> Connect(const ClientOptions& opts) noexcept;

    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// 最近一次请求后连接是否可用（false = 已断；下次请求会尝试重连）。
    bool connected() const noexcept;

    /// 当前会话号（0 = 未认证/已失效）。
    uint32_t session() const noexcept;

    /// 服务端活性/进度探询（读类；自动重连重发）。
    Result<PingInfo> Ping();

    /// 建表（写类）。
    Result<CreateTableInfo> CreateTable(const TableDef& def);

    /// 删表（写类）。
    Result<void> DropTable(std::string_view name);

    /// 表 schema 快照（读类）。
    Result<TableSchema> DescribeTable(std::string_view name);

    /// 原子批（写类；ops 模式）。成功返回 committed_seq。
    Result<int64_t> ExecuteBatch(const std::vector<ClientBatchOp>& ops);

    /// 列式块 upsert（写类；块模式，语义 = `Engine::UpsertBatch`）。
    Result<int64_t> UpsertBatch(const RecordBlockView& block);

    /// 单次 SCAN（读类）。区间语义同 `Engine::KeyRange`（整数类主键、
    /// 闭区间、空值 = 无界）。结果超帧上限返回 `kLimitExceeded`，应改用
    /// `ScanPaged` 分页。
    Result<ClientScanResult> Scan(std::string_view table,
                                  const Engine::KeyRange& range = Engine::KeyRange());

    /// SCAN_PAGE 逐页扫描（读类）：`on_page` 返回 false 提前停止（发送
    /// CLOSE）；返回总行数。页面持有在服务端会话内，异常路径由会话结束
    /// 回收。
    Result<uint64_t> ScanPaged(std::string_view table, uint32_t page_rows,
                               const std::function<bool(const ClientScanResult&)>& on_page);

    /// 订阅（独立连接 + 泵线程；见 `SubscriptionOptions`）。
    Result<std::unique_ptr<Subscription>> Subscribe(const SubscriptionOptions& opts);

    /// 关闭请求连接（幂等；订阅各自独立，不受影响）。
    void Close() noexcept;

    /// 最近一次错误描述（诊断文本；进程内自持有）。
    std::string last_error() const;

private:
    Client() noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 订阅泵参数。
struct SubscriptionOptions {
    /// 过滤表名（空 = 全部活跃表；表名不存在时泵按退避重试直至建表）。
    std::vector<std::string> tables;

    /// 变更回调（泵线程；按 seq 严格递增交付，允许重复）。
    std::function<void(const ChangeEvent&)> on_change;
    /// 溢出回调（泵线程）：调用方应整表重取并 `ResetTo(...)`；回调返回
    /// 且未 `ResetTo` 时泵挂起等待（不空转、不丢事件）。
    std::function<void(Subscription&)> on_overflow;
    /// 连接状态回调（泵线程；attempt = 当前连续失败次数，0 = 就绪）。
    std::function<void(bool connected, uint32_t attempt)> on_state;

    uint32_t delta_wait_ms = 1000;        ///< 单次 FETCH_DELTA 长轮询预算
    uint32_t delta_max_bytes = 1u << 20;  ///< 单批字节预算（服务端截断）
    uint32_t reconnect_min_ms = 200;      ///< 重连退避下限
    uint32_t reconnect_max_ms = 5000;     ///< 重连退避上限
};

/// \brief 订阅句柄（move-only）：独立连接 + 泵线程。
///
/// 线程契约：回调在泵线程执行；`position()/connected()/ResetTo()` 可从
/// 任意线程调用。不要在回调内销毁本对象（`Close` 会等待泵退出；在回调
/// 内调用 `Close` 只置停止标志，泵在本轮返回后自行退出）。
class RTDB_CLIENT_API Subscription {
public:
    Subscription() noexcept;
    ~Subscription();
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    /// 终止泵并断开（幂等；析构自动调用）。
    void Close() noexcept;

    /// 泵是否就绪（AUTH + SUBSCRIBE 成功且连接存活）。
    bool connected() const noexcept;

    /// 最后交付的 seq（= 续传起点；首连前为 0，订阅成功时为服务端
    /// 订阅位置 start_seq）。
    uint64_t position() const noexcept;

    /// 溢出重同步：重置续传点并唤醒泵（在 `on_overflow` 内或之后调用）。
    void ResetTo(uint64_t seq) noexcept;

    /// 等待首次就绪（测试/演示用；返回是否已就绪）。
    bool WaitReady(uint32_t timeout_ms) const noexcept;

    /// 最近一次错误描述（诊断文本）。
    std::string last_error() const;

private:
    friend class Client;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace rtdb

#endif  // RTDB_RTDB_CLIENT_HPP_
