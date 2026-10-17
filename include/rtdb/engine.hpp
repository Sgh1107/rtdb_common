#ifndef RTDB_ENGINE_HPP_
#define RTDB_ENGINE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rtdb/api_macro.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"
#include "rtdb/record_block.hpp"
#include "rtdb/result.hpp"
#include "rtdb/table_def.hpp"
#include "rtdb/table_handle.hpp"

namespace rtdb {

namespace infra {
class SlabShmAllocator;
class ShmRootMutex;
}  // namespace infra

#if defined(_MSC_VER)
#pragma warning(push)
// C4251: PImpl 成员不需要 DLL 接口 —— 所有成员函数均在 DLL 内实现，
// 客户端不会触碰 Impl 的实例化，属预期内安全用法。
#pragma warning(disable : 4251)
#endif

/// 实时库引擎门面（facade）。
///
/// 各阶段职责：
///   - M0：打开/创建文件映像共享内存区，校验 SuperBlock，维护
///     committed_seq / session_count 等全局状态字；
///   - M1-W2：表目录 —— `CreateTable` / `OpenTable` / `DropTable`，
///     目录持久化于映像内双 B+ 树；
///   - M1-W3：行 CRUD（主键寻址，免锁乐观读）；
///   - M1-W4：WAL 组提交 —— 映像内 WalRing、三档持久化语义、
///     `ExecuteBatch` 原子批、打开时崩溃恢复重放（docs/08 §3-W4）；
///   - M1-W5（当前）：冷快照 `CreateSnapshot`、在线加列、范围扫描、
///     二级索引框架（docs/08 §3-W5）。
///
/// 线程安全：同一实例可被多线程使用；写操作内部经根互斥锁串行化。
/// 不同进程通过同一映像文件挂载共享数据（docs/02 §2）。
class RTDB_API Engine {
public:
    Engine() noexcept;
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    /// 打开或创建实例。语义：
    ///  - 文件不存在 → 按 Options 创建映像并初始化 SuperBlock；
    ///  - 已存在     → 校验 magic/格式版本/机器字宽/layout_crc，
    ///                 任一不匹配返回对应错误码（kBadSuperBlock /
    ///                 kVersionMismatch / kIncompatibleLayout）；
    ///  - Reader 角色以只读映射打开；其他角色读写映射；
    ///  - 写侧角色打开时执行 WAL 恢复重放（docs/08 §3-W4）：重放
    ///    (checkpoint_seq, 尾] 后发布 committed_seq——Reader 应在
    ///    任一写侧完成打开后挂载。
    static Result<std::unique_ptr<Engine>> Open(const Options& opts);

    /// 全局提交序列号：所有已提交变更的最大单调序号。
    /// 订阅游标机制（M3）以此为基准。
    int64_t CommittedSeq() const noexcept;

    /// 持久化水位：已全映像 flush 覆盖到的 seq（三档语义账本，
    /// docs/03 §3.2）。恒 ≤ CommittedSeq()；Reader 恒为 -1。
    int64_t FlushedSeq() const noexcept;

    /// 重放下界（checkpoint）：已随快照/强刷落盘、且环内该点之前记录
    /// 已被回收的最大 seq（docs/03 §3.2）。恒 ≤ CommittedSeq()；
    /// Reader 可读（只读映射内的持久化状态字）。
    int64_t CheckpointSeq() const noexcept;

    /// 本映像被打开过的累计次数（每次成功 Open 递增一次）。
    /// 用作跨进程活性/持久化的最小可见证据。
    int64_t SessionCount() const noexcept;

    // ------------------ 表目录（M1-W2）------------------
    /// \brief 建立新表。
    ///
    /// 校验表名/列定义/上限，分配并填充描述符后挂入目录双 B+ 树；
    /// 任一步失败完整回滚。
    ///
    /// \param def 建表定义（表名非空、至少 1 列）
    /// \return 表句柄；失败码见 `Err`：
    ///         `kInvalidArgument` / `kLimitExceeded` / `kTableExists` /
    ///         `kReadOnlyHandle` / `kUnsupported` / `kOutOfMemory`
    Result<TableHandle> CreateTable(const TableDef& def);

    /// \brief 按名打开已存在的表。
    /// \param name 表名
    /// \return 表句柄；不存在或已删除返回 `kTableNotFound`
    Result<TableHandle> OpenTable(std::string_view name);

    /// \brief 删除表（置描述符墓碑态并摘除表名索引）。
    ///
    /// 物理数据区不随删表释放，待后续 compact 回收；表名可被新表复用。
    ///
    /// \param name 表名
    /// \return kOk；不存在返回 `kTableNotFound`；只读角色返回 `kReadOnlyHandle`
    Result<void> DropTable(std::string_view name);

    // ------------------ 行 CRUD（M1-W3）------------------
    /// \brief 插入一行。列 0 为主键（整数类、非空），主键冲突返回
    /// `kAlreadyExists`；行槽耗尽返回 `kOutOfMemory`。
    /// \param table 目标表句柄
    /// \param row 列值序列（长度须等于表列数，顺序与建表列序一致）
    /// \return 分配到的行槽号
    Result<uint64_t> Insert(const TableHandle& table, const std::vector<ColumnValue>& row);

    /// \brief upsert 一行（M2，docs/04 §3 便利面；语义同
    /// `TableHandle::Upsert`）：存在则按所给列合并覆盖，不存在则插入。
    /// \param row 按列名定位的部分行（须含主键列）
    /// \return 行槽号
    Result<uint64_t> Upsert(const TableHandle& table, const Row& row);

    // ------------------ 列式批量（M2-W2，docs/03 §2.3）------------------
    /// \brief 列式块原子 upsert（docs/03 §2.3 `UpsertBatch(RecordBlockView)`）。
    ///
    /// 语义（与单行 `Upsert` 的**合并**不同，块为**整行覆盖**）：
    ///  - 块内每行存在则整行覆盖（未提供列按 schema 填充：nullable →
    ///    NULL；ext 列默认值；非空缺列 kInvalidArgument），不存在则插入；
    ///  - 整块一个原子批（BATCH_BEGIN + N×op + BATCH_COMMIT）：任一行
    ///    校验失败整批零副作用（WAL 不写 BEGIN）；WAL/应用阶段失败 →
    ///    毒化引擎（kPoisoned），重启重放收敛；
    ///  - 块内主键不得重复（kInvalidArgument）；
    ///  - 列按列名映射到表 schema；未知列名/载荷类型与列类型不兼容 →
    ///    kInvalidArgument；块行数受表 row_cap 约束（kOutOfMemory）。
    ///
    /// \param block 列式视图（零拷贝；内存须在调用期间有效）
    /// \return 批提交序列号（committed_seq）
    Result<uint64_t> UpsertBatch(const RecordBlockView& block);

    /// \brief `RecordBlock` 便捷重载（docs/04 §3 `eng->UpsertBatch(blk)`）。
    Result<uint64_t> UpsertBatch(const RecordBlock& block) {
        return UpsertBatch(block.View());
    }

    /// \brief 按主键读取整行（免锁乐观读，返回自持有值）。
    ///
    /// 返回列数 = 句柄可见列数（建表列 + 句柄绑定时刻的扩展列）；
    /// 句柄发放后新增的列对本句柄不可见（docs/02 §3 要点 3）。
    /// \return 列值序列；行不存在或已删除返回 `kNotFound`
    Result<std::vector<ColumnValue>> GetByKey(const TableHandle& table, const ColumnValue& key);

    /// \brief 按主键整行覆盖更新（主键不可变更；值须置于 row[0]）。
    /// \param out_row_no 可选输出：目标行槽号（M2 upsert 合并路径用）
    /// \return kOk / kNotFound（行不存在）/ kInvalidArgument / kReadOnlyHandle
    Result<void> UpdateByKey(const TableHandle& table, const ColumnValue& key,
                             const std::vector<ColumnValue>& row, uint64_t* out_row_no = nullptr);

    /// \brief 按主键删除行（置墓碑并摘除索引键；行槽待 compact 回收）。
    /// \return kOk；行不存在返回 `kNotFound`
    Result<void> DeleteByKey(const TableHandle& table, const ColumnValue& key);

    /// \brief 按主键判定行是否存在。
    Result<bool> ContainsKey(const TableHandle& table, const ColumnValue& key);

    // ------------------ 范围扫描（M1-W5，docs/03 §4.2）------------------
    /// \brief 主键范围（`Scan` 入参）：`lo`/`hi` 为 Null（is_null）表示
    /// 该侧无界；两端默认闭区间（含端点）。
    struct KeyRange {
        ColumnValue lo = ColumnValue::Null();  ///< 下界（is_null = 无界）
        ColumnValue hi = ColumnValue::Null();  ///< 上界（is_null = 无界）
        bool lo_inclusive = true;              ///< 下界是否含端点
        bool hi_inclusive = true;              ///< 上界是否含端点
    };

    /// \brief 主键范围扫描（物化返回；游标/流式 API 归 M2）。
    ///
    /// 行按主键升序返回，每行列数 = 句柄可见列数（同 GetByKey 的截断
    /// 语义）。语义边界（M1）：
    ///  - 结果为**近似视图**：并发插入可能缺席、并发更新中的行重试若干
    ///    次后跳过、并发删除的行直接缺席——不承诺快照隔离；
    ///  - 只读角色（Reader）可扫描；
    ///  - 边界不可编码（非整数类主键/空值越界）返回 `kInvalidArgument`。
    Result<std::vector<std::vector<ColumnValue>>> Scan(const TableHandle& table,
                                                       const KeyRange& range);

    /// \brief 全表扫描（`Scan` 的无界便捷形式）。
    Result<std::vector<std::vector<ColumnValue>>> ScanAll(const TableHandle& table);

    // ------------------ 原子批量（M1-W4）------------------
    /// \brief 原子批量操作单元（`ExecuteBatch` 入参）。
    ///
    /// 语义边界（M1，docs/08 §2-D3）：
    ///  - 批内操作互不依赖：全部校验基于批前状态（不支持批内读己之写）；
    ///  - update 的 row[0] 须等于原主键；delete 的 row 恰含 1 个元素（主键）；
    ///  - 运行期任一操作校验失败 → 整批拒绝、零副作用（WAL 不写 BEGIN）；
    ///  - WAL 侧 BATCH_BEGIN + N×op + BATCH_COMMIT 三段式：COMMIT 先于
    ///    数据页变更落环，恢复时"无 COMMIT 整批作废 / 有 COMMIT 整批重放"。
    struct BatchOp {
        enum class Type : uint8_t { kInsert = 1, kUpdate = 2, kDelete = 3 };

        Type type = Type::kInsert;
        TableHandle table;             ///< 目标表
        std::vector<ColumnValue> row;  ///< insert/update=整行（row[0]=主键）；
                                       ///< delete=仅主键（1 个元素）
    };

    /// \brief 原子执行一批操作（组提交，一次落盘确认）。
    ///
    /// 全部操作校验通过后 WAL 化并依次应用；committed_seq 一次推进到
    /// BATCH_COMMIT 的 seq。任一环节失败整批失败：
    ///  - 校验失败（冲突/不存在/参数错）→ 零副作用返回原错误码；
    ///  - WAL/应用阶段失败 → 引擎毒化（kPoisoned），重启重放收敛
    ///    （docs/08 §2-D2）。
    Result<void> ExecuteBatch(const std::vector<BatchOp>& ops);

    // ------------------ 冷快照（M1-W5，docs/03 §3.2/§3.3）------------------
    /// \brief 执行冷快照：停写（根锁）→ 全映像 flush → checkpoint_seq
    /// 推进到 committed_seq → 环可回收时丢弃保留记录 → 拷贝映像副本。
    ///
    /// 返回快照文件路径（`<映像路径>.snap`）。重启闭环：映像内数据页
    /// 即 ≤ checkpoint 的已落盘状态，Open 时既有恢复路径重放
    /// (checkpoint_seq, 尾]（W4），快照不改变活跃映像的数据语义。
    /// 快照副本本身也是完整映像，可独立挂载恢复（一致性点拷贝）。
    ///
    /// 边界（M1）：副本为整体拷贝而非 rename（Windows 活动映射不做
    /// 重命名）；多版本/增量快照管理归 M2；机器断电的页乱序残差同
    /// W4（doublewrite 归 M2）。
    ///
    /// \return 快照文件路径；Reader 角色返回 `kReadOnlyHandle`；
    ///         flush/拷贝失败返回 `kIoError`
    Result<std::string> CreateSnapshot();

    // ------------------ 在线加列（M1-W5，docs/02 §3 要点 3）------------------
    /// \brief 在线追加一列（不停服、不重建行数据）。
    ///
    /// 新列进入 ext 扩展区（每列一块链式区域），主记录布局不变；
    /// `data_layout_ver` 递增，已发放句柄按绑定时刻的列数继续读写
    /// （旧句柄不崩、不见新列）。存量行惰性读默认值（不物化）：
    /// 未显式设置的行按 `ColumnDef.has_default`/`default_value` 读取，
    /// 无默认值仅 nullable 列允许（旧行读 NULL）。
    ///
    /// 边界（M1）：仅定长 ≤8B 标量列（变长/宽字段返回 kUnsupported）；
    /// 列名须全表唯一；总列数 ≤ 32（`kMaxInitCols + kMaxExtCols`）。
    /// 崩溃安全序：先写区域再挂链最后计数（中断留不可达孤儿区，
    /// 表 schema 不变）。加列本身不写 WAL（描述符事务顺序保证，
    /// 同 CreateTable 的观测语义）。
    ///
    /// \param table 目标表句柄（建表/打开时获得）
    /// \param col 新列定义
    /// \return kOk；失败码：`kInvalidArgument` / `kUnsupported` /
    ///         `kLimitExceeded` / `kReadOnlyHandle` / `kPoisoned` /
    ///         `kOutOfMemory` / `kTableNotFound`
    Result<void> AlterTableAddColumn(const TableHandle& table, const ColumnDef& col);

    // ------------------ 二级索引（M1-W5，docs/03 §2.2）------------------
    /// \brief 创建二级索引（持根锁；不影响在线读写，M1-W5-d 框架）。
    ///
    /// 键布局 `[有序值][有序 row_no]`（16B）；同值多行按 row_no 升序
    /// 连续，等值查询一次游标取全量。存量行建索引时全量灌入；唯一索引
    /// 对存量数据校验冲突（违例 `kAlreadyExists`）。
    ///
    /// 持久化与自愈：不写 WAL（同加列：描述符事务顺序即崩溃安全）；
    /// 索引变更本身由写侧 Open 的全量重建兜底——崩溃/应用中断不留
    /// 悬挂键（docs/08 §3-W5）。
    ///
    /// 边界（M1）：被索引列须为定长标量（整数类/浮点类/布尔/时间戳；
    /// 变长列 `kUnsupported`）；每表 ≤ `kMaxSecIdxPerTable` 个；NULL
    /// 不入索引（唯一性不约束 NULL）；唯一性校验仅覆盖写入行显式提供
    /// 的列（旧句柄窄行写入时未提供列走惰性默认值，不参与校验）。
    ///
    /// \param table 目标表句柄
    /// \param idx 索引定义（表内重名 `kAlreadyExists`）
    /// \return kOk；失败码：`kInvalidArgument` / `kUnsupported` /
    ///         `kLimitExceeded` / `kAlreadyExists` / `kReadOnlyHandle` /
    ///         `kPoisoned` / `kOutOfMemory`
    Result<void> CreateSecondaryIndex(const TableHandle& table, const SecondaryIndexDef& idx);

    /// \brief 按二级索引等值查询（免锁近似读，语义边界同 `Scan`）。
    ///
    /// 返回行按 row_no 升序，每行列数 = 句柄可见列数（同 GetByKey 截断）；
    /// 索引不存在返回 `kTableNotFound`；空值键返回 `kInvalidArgument`。
    /// Reader 角色可查询。
    Result<std::vector<std::vector<ColumnValue>>> GetBySecondary(const TableHandle& table,
                                                                 const std::string& index_name,
                                                                 const ColumnValue& key);

    // ------------------ 写侧全局锁与内存管理（M0 直通句柄）------------------
    /// 获取根互斥锁（保护分配器 freelist 与未来目录树的全局变更；
    /// 可恢复持有者崩溃后的残留死锁）。timeout_ms==0 表示纯尝试。
    Err LockRoot(uint32_t timeout_ms) noexcept;
    void UnlockRoot() noexcept;

    /// 共享内存 slab 分配器句柄。调用方负责持锁；Reader 恒为 nullptr。
    infra::SlabShmAllocator* RawAllocator() const noexcept;
    /// 根互斥锁对象本体（供高级用法），Reader 恒为 nullptr。
    infra::ShmRootMutex* RawRootMutex() const noexcept;

private:
    struct Impl;

    /// \brief 由描述符字段装配表句柄（内部；仅传递原始字段，不泄漏内部类型）。
    static TableHandle MakeHandle(int64_t table_id, const char* name, uint32_t layout_version,
                                  uint32_t col_count, uint32_t ext_col_count, uint32_t row_cap,
                                  uint64_t desc_off);

    std::unique_ptr<Impl> impl_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace rtdb

#endif  // RTDB_ENGINE_HPP_
