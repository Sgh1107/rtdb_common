#ifndef RTDB_CHANGE_CURSOR_HPP_
#define RTDB_CHANGE_CURSOR_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rtdb/api_macro.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"

namespace rtdb {

/// 无变更时 `ChangeCursor::Next` 的等待预算：无限等待（直到有变更）。
inline constexpr uint32_t kCursorWaitForever = 0xFFFFFFFFu;

/// \brief 变更事件（订阅增量交付单元，docs/13 §3-D1）。
///
/// 事件来源 = WalRing 已提交记录（`committed_seq` 以内），因此：
///  - 事件按 seq 严格递增、逐条完整交付，允许重复、不允许缺漏；
///  - 原子批的边界由 COMMIT 记录保证——批内任一条可见时全批均已提交，
///    但单次 `Next` 允许按字节预算把同一批拆到多次交付（交付层拆分，
///    语义层原子）。
struct ChangeEvent {
    /// 变更类型（数值与协议 `RTDB_CHG_*` 对齐）。
    enum class Kind : uint8_t {
        kInsert = 1,
        kUpdate = 2,
        kDelete = 3,
    };

    Kind kind = Kind::kInsert;
    int64_t table_id = 0;
    std::string table;    ///< 表名（订阅过滤器/目录解析所得）
    uint64_t seq = 0;     ///< WAL 序号（严格递增）
    uint64_t ts_ns = 0;   ///< 写入时刻（Unix 纳秒）
    uint64_t row_no = 0;  ///< 行槽号
    uint64_t row_ver = 0; ///< 应用后的行版本

    /// insert/update：行值（内联列 + 该次写入携带的扩展列；扩展列可少于
    /// 当前 schema——未携带列由消费方按默认值语义补充）；
    /// delete：空（被删主键见 `key`）。
    std::vector<ColumnValue> row;

    /// delete：被删主键；insert/update 恒为 Null。
    ColumnValue key = ColumnValue::Null();
};

/// \brief 一次 `Next` 交付的增量批次。
struct ChangeBatch {
    std::vector<ChangeEvent> events;
    /// 本次推进后的游标位置（≥ 批内最后事件 seq；空批 = 原游标）。
    uint64_t last_seq = 0;

    bool empty() const noexcept { return events.empty(); }
};

/// \brief 增量订阅游标（`Engine::Subscribe` 产物，docs/04 §3）。
///
/// 语义（docs/13 §3-D1）：
///  - 游标初值 = 订阅时的 `committed_seq`（仅交付订阅之后的变更）；
///  - `Next` 在无变更时经平台等待原语（futex/WaitOnAddress）等待
///    `committed_seq` 变化，跨进程写者的提交同样唤醒；
///  - 游标落后于环回收窗口（`from + 1 < oldest_seq`）→ `kCursorOverflow`：
///    调用方须整表重取并以 `ResetTo(快照 seq)` 重置游标；
///  - 生命周期：游标引用的引擎资源不得早于游标析构；同一游标非线程安全
///    （会话模型单线程使用）。
#if defined(_MSC_VER)
#pragma warning(push)
// C4251: PImpl 成员不需要 DLL 接口 —— 与 Engine 同策略（engine.hpp）。
#pragma warning(disable : 4251)
#endif

class RTDB_API ChangeCursor {
public:
    ChangeCursor() noexcept;
    ~ChangeCursor();
    ChangeCursor(ChangeCursor&&) noexcept;
    ChangeCursor& operator=(ChangeCursor&&) noexcept;
    ChangeCursor(const ChangeCursor&) = delete;
    ChangeCursor& operator=(const ChangeCursor&) = delete;

    /// 有效性（默认构造 / 移动后的源对象为无效）。
    bool valid() const noexcept;

    /// 当前游标：下次交付的起点（开区间端点）。
    uint64_t Position() const noexcept;

    /// \brief 拉取增量。
    ///
    /// 有变更 → `kOk` 批（可能因字节预算分批，剩余由后续 `Next` 交付）；
    /// 无变更 → 等待至多 `timeout_ms`（0 = 立即返回空批；
    /// `kCursorWaitForever` = 无限等待）后返回空批。
    ///
    /// \return `kOk` / `kCursorOverflow`（须全量重取后 `ResetTo`）/
    ///         `kUnsupported`（引擎未挂载 WAL 环，如 Reader 主角色）/
    ///         `kTimeout`（根锁等待超时）/ `kCorrupted` 族（环结构违例）
    Result<ChangeBatch> Next(uint32_t timeout_ms);

    /// 重置游标（溢出重同步 / 手动跳转）。向后跳只允许落在环回收窗口内，
    /// 超出窗口的下一次 `Next` 返回 `kCursorOverflow`。
    void ResetTo(uint64_t seq) noexcept;

private:
    friend class Engine;
    struct Impl;
    explicit ChangeCursor(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace rtdb

#endif  // RTDB_CHANGE_CURSOR_HPP_
