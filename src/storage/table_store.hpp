#ifndef RTDB_SRC_STORAGE_TABLE_STORE_HPP_
#define RTDB_SRC_STORAGE_TABLE_STORE_HPP_

/// \file
/// \brief 表行存储 TableStore（docs/03 §3.1，docs/08 §3-W3）。
///
/// 物理结构（全部经 Slab 分配、偏移寻址）：
/// ```
/// [行数据区 rec_fixed_len × row_cap]  ← 行寻址 O(1)：base + row_no × stride
/// [行版本数组 u64 × row_cap]         ← 偶数=稳定，奇数=写入中（乐观读判据）
/// [墓碑位图 bit × row_cap]           ← 1 = 已删除
/// [变长池区（有变长列时）]            ← MVarLenPool 承载 string/blob 内容
/// [主键索引 Slot]                    ← SharedBTree<8, row_no> 锚点
/// ```
/// 行记录布局：`[列0..列N-1][null 位图]`（见 `PlanRowLayout`）。
///
/// 并发约定：
///  - 变更（Insert/Update/Remove）必须在 `ShmRootMutex` 临界区内调用；
///  - 读取（Get）为**免锁乐观读**：读版本 → 拷贝数据 → 复核版本，不一致重试；
///    写入方先置奇数版本、改数据、再置偶数版本，读者最终复核保证不会采纳
///    半更新数据；变长内容即使被回收/复用，也会因版本不符被丢弃重读。
///
/// M1 边界：删除置墓碑，删除后的槽位由后续插入复用（P0-3；高水位
/// `row_count` 不回落，槽位版本号跨生命周期单调递增以保 WAL 重放判重）；
/// 主键限整数类定长类型；单表行数据区受 `kMaxRowDataBytes` 约束
/// （段目录化列 M2）。

#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "infra/shared_btree.hpp"
#include "infra/slab_allocator.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"
#include "rtdb/table_def.hpp"
#include "storage/schema.hpp"
#include "storage/sec_index.hpp"
#include "storage/var_pool.hpp"

namespace rtdb {
namespace storage {

/// 主键索引：key = `EncodeOrderedInt64(主键值)`，payload = 行槽号。
using PrimaryIndexTree = infra::SharedBTree<8, uint64_t>;

/// \brief 单行变更的暂存产物（WAL 先行协议，docs/08 §2-D2，§3-W4）。
///
/// 写路径顺序：Stage*（校验 + 构建，含变长块分配与 insert 行槽保留）
/// → WAL 追加 → Apply*（数据页/索引变更）→ 提交标记。
/// 批量（Engine::ExecuteBatch）先对全部操作 Stage，再整体 WAL 化后依次
/// Apply——COMMIT 记录先于任何数据页变更落环，恢复时"无 COMMIT 整批作废"
/// 或"有 COMMIT 整批重放"，两段皆原子（docs/08 §2-D3）。
///
/// 生命周期约定（A2 修复后的完整不变式）：
///  - Stage 成功后 `allocated_vars` 中的变长块归暂存产物所有；
///  - WAL 追加成功 → `wal_written` 置位：记录已入环，重放可能按记录内
///    偏移重新物化行，此后本进程**不得**回收这些变长块（回收会让重放
///    引用已空闲块，随后被再分配 = 静默损坏）；泄漏有界，随 compact 收敛
///    （与 ReplayUpsert 对旧记录变长块的处理同策略）；
///  - ApplyStaged 成功 → 块所有权随记录转移进行数据，`allocated_vars` 清空，
///    此后**不得**再 ReleaseStaged；
///  - ApplyStaged 在**发布点之后**失败（行数据/主键已发布，`published` 置位）
///    → 数据不可回滚，变长块引用仍在行记录中：`ReleaseStaged` 必须跳过块
///    回收与行槽回退（P0-2：原实现释放被已发布行引用的块 + 行槽回退导致
///    槽位复用，重放幂等跳过又无法修复）；
///  - ApplyStaged 失败且未发布 → 数据页已由 Apply 内部回滚，行槽保留权
///    仍在暂存产物；调用方必须 ReleaseStaged 收尾（块回收与否见 wal_written）；
///  - ReleaseStaged 对未发布的 insert 会回退行槽保留（desc_->row_count--），
///    批量中止须按暂存逆序调用（LIFO）。
struct StagedRowOp {
    enum class Kind : uint8_t { kInsert = 1, kUpdate = 2, kDelete = 3 };

    Kind kind = Kind::kInsert;
    uint64_t row_no = 0;        ///< insert=保留的行槽号；update/delete=目标行槽号
    uint64_t row_ver = 0;       ///< 应用完成后的行版本号（偶数；重放判重依据）
    std::vector<uint8_t> rec;   ///< insert/update：主记录（rec_fixed_len 字节）+
                                 ///< 扩展列后缀（每列 [status:1][slot_bytes 值]，
                                 ///< 按添加序；W5-b。无扩展列时即主记录）
    std::vector<uint8_t> old_rec;  ///< update/delete：旧记录副本（应用成功后回收其变长块）
    std::vector<uint64_t> allocated_vars;  ///< 暂存期持有的变长块偏移
    char key[8];                ///< 主键有序编码（WAL 载荷 / 索引操作共用）
    /// 本操作的 WAL 记录已成功入环（由引擎在 WalAppendRowOp 成功后置位）。
    bool wal_written = false;
    /// 行数据/主键索引已发布（不可回滚）：ApplyStaged 内部在发布点置位。
    bool published = false;
    /// 插入复用了墓碑槽位（P0-3）：ReleaseStaged 不得回退 row_count。
    bool slot_reused = false;
};

/// \brief 单表行存储：格式化物理区 + 行级 CRUD + WAL 暂存/应用/重放。
#if defined(_MSC_VER)
#pragma warning(push)
// C4251: 内部持有 std::unique_ptr 成员（不参与 DLL 接口实体化）。
#pragma warning(disable : 4251)
#endif
class RTDB_API TableStore {
public:
    /// \brief 为新建表格式化物理区（行数据/版本数组/墓碑位图/变长池/主键槽）。
    ///
    /// 调用方（Catalog）已完成描述符元数据填充；本函数只负责物理区分配与
    /// 描述符中相应偏移字段的写入。任一步失败会释放此前已分配的块，
    /// 保证不留半格式化状态。
    ///
    /// \param def 建表定义（列 0 为主键，须为整数类且 NOT NULL）
    /// \param desc 描述符（列定义与元数据已填充；原地写入区域偏移）
    /// \param base 映像基址
    /// \param alloc 分配器（须非空）
    /// \return kOk / kLimitExceeded / kUnsupported / kOutOfMemory
    static Err Format(const TableDef& def, shm_TableDescriptor* desc, void* base,
                      infra::SlabShmAllocator* alloc) noexcept;

    /// \brief 复附着既有表（校验描述符与物理区一致性）。
    static Result<std::unique_ptr<TableStore>> Attach(void* base, infra::SlabShmAllocator* alloc,
                                                      uint64_t desc_off) noexcept;

    /// \brief 插入一行；主键冲突返回 kAlreadyExists。
    /// \param row 列值序列（长度须等于表列数；列 0 为主键且不可为空）
    /// \param out_row_no 可选输出：分配到的行槽号
    Err Insert(const std::vector<ColumnValue>& row, uint64_t* out_row_no = nullptr) noexcept;

    /// \brief 按主键读取整行（免锁乐观读）。
    /// \return 列值序列；行不存在或已删除返回 kNotFound
    Result<std::vector<ColumnValue>> Get(const ColumnValue& key) const noexcept;

    /// \brief 主键范围扫描（W5-c；物化返回，游标 API 归 M2）。
    ///
    /// 沿主键索引叶链顺序推进，逐行免锁乐观读（与 Get 同协议）。
    /// 语义边界（M1）：
    ///  - `lo`/`hi` 为 Null（is_null）表示该侧无界；两端默认闭区间；
    ///  - 结果为**近似视图**：并发插入可能缺席、并发更新中的行重试
    ///    若干次后跳过、并发删除的行直接缺席——不承诺快照隔离；
    ///  - 行按主键升序返回；旧句柄的列截断由调用方（Engine）处理。
    /// \param lo 下界（nullptr = 无界）
    /// \param hi 上界（nullptr = 无界）
    /// \return 行序列（每行 = 当前全量 schema 的列值）
    Result<std::vector<std::vector<ColumnValue>>> Scan(const ColumnValue* lo, bool lo_incl,
                                                       const ColumnValue* hi,
                                                       bool hi_incl) const noexcept;

    /// \brief 有上限的范围扫描（M4：分页 O(N²) 修复）。
    ///
    /// 与 `Scan` 同边界语义，但最多返回 `max_rows` 行；`*out_more` 表示
    /// 区间内仍有未读取的行（保守判定：截断点处游标仍有效）。分页调用方
    /// 以"上一页末行主键（开区间）"为下一页下界，单页成本 = O(log N +
    /// page_rows)，而不是重扫剩余全表。
    Result<std::vector<std::vector<ColumnValue>>> ScanLimited(const ColumnValue* lo, bool lo_incl,
                                                              const ColumnValue* hi, bool hi_incl,
                                                              uint64_t max_rows,
                                                              bool* out_more) const noexcept;

    /// \brief 按主键整行覆盖更新（主键不可变更）。
    Err Update(const ColumnValue& key, const std::vector<ColumnValue>& row) noexcept;

    /// \brief 按主键删除（置墓碑 + 摘除索引键）。
    Err Remove(const ColumnValue& key) noexcept;

    /// \brief 按主键判定行是否存在。
    Result<bool> Contains(const ColumnValue& key) const noexcept;

    // ------------------ WAL 先行协议（docs/08 §2-D2，W4）------------------
    /// \brief 暂存插入：校验 + 构建记录 + 分配变长块 + 保留行槽（row_count++）。
    /// 主键冲突返回 kAlreadyExists；行槽耗尽返回 kOutOfMemory。
    Err StageInsert(const std::vector<ColumnValue>& row, StagedRowOp* out) noexcept;

    /// \brief 暂存整行覆盖更新（主键不可变更，row[0] 须等于原主键）。
    Err StageUpdate(const ColumnValue& key, const std::vector<ColumnValue>& row,
                    StagedRowOp* out) noexcept;

    /// \brief 暂存删除（目标须存在且未墓碑化）。
    Err StageRemove(const ColumnValue& key, StagedRowOp* out) noexcept;

    /// \brief 应用暂存变更（数据页 + 索引 + 计数账本）。
    ///
    /// 失败语义：insert 的数据页变更内部回滚（行槽内容清零、版本归零），
    /// 变长块/行槽保留权留给调用方 ReleaseStaged；update/delete 应用中途
    /// 失败（仅索引操作可失败）由调用方毒化引擎（WAL 已先行，重启重放收敛）。
    /// 内部注入点：`wal.apply.mid`（数据已发布、索引未发布）。
    Err ApplyStaged(StagedRowOp* st) noexcept;

    /// \brief 释放暂存产物（变长块归还 + insert 行槽保留回退）。
    /// 批量中止须按暂存逆序调用；Apply 成功后不得调用。
    void ReleaseStaged(StagedRowOp* st) noexcept;

    // ------------------ 恢复重放（docs/08 §3-W4）------------------
    /// \brief 幂等重放一条 Insert/Update 记录（row_no + row_ver 判重）。
    ///
    /// 索引已发布 → 视为已应用（kOk）；版本落后 → 整行重写并补发布索引；
    /// 版本已到位但索引缺失（应用中断）→ 仅补发布索引。
    /// \param rec 整行记录；rec_len < rec_fixed_len 返回 kCorruption
    Err ReplayUpsert(uint64_t row_no, uint64_t row_ver, const uint8_t* rec,
                     uint32_t rec_len) noexcept;

    /// \brief 幂等重放一条 Delete 记录（key8 = 主键有序编码 8B）。
    Err ReplayDelete(uint64_t row_no, uint64_t row_ver, const uint8_t* key8) noexcept;

    /// \brief 重放收尾：committed_row_count = 索引规模（live rows）。
    /// row_count 已在 ReplayUpsert 中按 row_no+1 单调修正。
    Err RebuildCommittedCountAfterReplay() noexcept;

    // ------------------ 二级索引（M1-W5-d，docs/08 §3-W5）------------------
    /// \brief 创建二级索引（调用方须持根锁且为可写角色）。
    ///
    /// 顺序：校验定义 → 分配 Slot 锚点与元数据块 → 注入点
    /// `secidx.before_link` → populate 存量行 → 挂链 + 计数递增。
    /// populate 先于 link：读者要么看不到索引，要么看到全量（无半截视图）；
    /// 中断在挂链前留不可达孤儿块（无害，M2 compact 收敛）。
    /// 唯一索引对存量数据校验冲突，违例返回 kAlreadyExists。
    Err CreateSecondaryIndex(const SecondaryIndexDef& idx) noexcept;

    /// \brief 按二级索引等值查询（免锁近似读，同 Scan 语义边界）。
    ///
    /// 同值多行按 row_no 升序返回；索引不存在返回 kNotFound；空值键
    /// 返回 kInvalidArgument（NULL 不入索引）。
    Result<std::vector<std::vector<ColumnValue>>> GetBySecondary(const std::string& index_name,
                                                                 const ColumnValue& key) const noexcept;

    /// \brief 全量重建全部二级索引（写侧 Open 重放后自愈；持根锁）。
    ///
    /// 就地清空各索引树后按主键序遍历重灌（当前行状态含 ext 惰性默认值）；
    /// 旧节点不可达，待 M2 compact 回收。
    Err RebuildSecondaryIndexes() noexcept;

    /// \brief 已提交可见行数。
    uint64_t live_rows() const noexcept { return desc_->committed_row_count; }

    /// \brief 已占用行槽数（含墓碑）。
    uint64_t used_rows() const noexcept { return desc_->row_count; }

    /// \brief 增量解码：WAL insert/update 记录 → 行值（订阅/网络增量，
    /// docs/13 §3-W2）。
    ///
    /// 记录 = 内联列固定区 + 扩展列后缀；输出 = 内联列 + 后缀携带的
    /// 扩展列值（该次写入未提供的扩展列不产出——惰性默认值语义由
    /// 消费方按 schema 补充）。撕裂/畸形输入返回空序列（调用方跳过）。
    ///
    /// 变长列（utf8/bytes）的字节块存于表内池（记录仅含池引用），覆盖写
    /// 会回收旧块——仅当事件仍为行当前版本（row_no/row_ver 相同且未
    /// 墓碑）时解码交付真实值，否则交付 NULL（消费方按需按主键重取）。
    /// 调用方须持根锁（与写路径互斥，保证版本判定与池读取原子）。
    std::vector<ColumnValue> DecodeWalRow(const uint8_t* rec, uint32_t rec_len, uint64_t row_no,
                                          uint64_t row_ver) const noexcept;

    /// \brief 增量解码：WAL delete 载荷（8B 有序主键编码）→ 主键值。
    /// 载荷非法返回 Null。
    ColumnValue DecodeWalKey(const uint8_t* key8) const noexcept;

    /// \brief 结构不变量校验（测试与诊断）：
    /// 索引规模 + 墓碑数 == 已用槽数，且版本数组均为稳定偶数。
    Err Validate(uint64_t* out_index_entries = nullptr) const noexcept;

    /// \brief 描述符指针（调用方持根锁时可经此变更 schema，如在线加列）。
    shm_TableDescriptor* descriptor() noexcept { return desc_; }
    const shm_TableDescriptor* descriptor() const noexcept { return desc_; }

private:
    TableStore() = default;

    /// \brief 行槽在映像内的基址。
    uint8_t* RowPtr(uint64_t row_no) const noexcept;
    /// \brief 行版本槽。
    uint64_t* VersionSlot(uint64_t row_no) const noexcept;

    /// 行是否为指定版本且未墓碑（订阅解码的池引用有效性门，M3-W2）。
    bool IsCurrentRowVersion(uint64_t row_no, uint64_t row_ver) const noexcept;
    /// \brief 墓碑位判定。
    bool IsTombstoned(uint64_t row_no) const noexcept;
    void SetTombstone(uint64_t row_no, bool dead) const noexcept;
    /// \brief 主键编码；不可编码（类型不符/空值）返回 false。
    static bool EncodeKey(const ColumnValue& key, char out[8]) noexcept;
    /// \brief 组装记录缓冲（含变长块分配与扩展列后缀；失败时释放已分配块）。
    Err BuildRecord(const std::vector<ColumnValue>& row, std::vector<uint8_t>* rec,
                    std::vector<uint64_t>* allocated_var_offs) const noexcept;
    /// \brief 应用扩展列后缀（W5-b）：按添加序写各 ext 区槽值与 set/null 位。
    /// 后缀列数多于链长时余部忽略（加列先于记录落盘的崩溃残差，M1 边界）。
    Err ApplyExtSuffix(const uint8_t* suffix, size_t suffix_len, uint64_t row_no) noexcept;
    /// \brief 清零指定行的扩展列 set/null 位（插入回滚用，防槽位复用残留）。
    void ClearExtBits(uint64_t row_no) const noexcept;
    /// \brief 释放记录中的变长块。
    void ReleaseRecordVars(const uint8_t* rec) const noexcept;
    /// \brief 从记录解码列值。
    std::vector<ColumnValue> DecodeRecord(const uint8_t* rec) const noexcept;
    /// \brief 免锁乐观读一行（主记录 + ext 槽同一版本窗口拷贝）。
    /// 墓碑返回 kNotFound；重试耗尽返回 kTimeout（并发更新中）。
    Err ReadRow(uint64_t row_no, std::vector<ColumnValue>* out) const noexcept;

    /// \brief 查找可复用的墓碑槽位（P0-3；扫描起点按上次命中推进）。
    ///
    /// 仅在根锁内调用：`staged_slots_` 记录本进程当前暂存中的槽位，
    /// 防止同一批内两行复用同一槽位（暂存期槽位仍为墓碑态）。
    bool FindFreeSlot(uint64_t* out) noexcept;

    // ------------------ 二级索引私有助手（W5-d）------------------
    /// 单索引的键条目（与 sec_meta_/sec_trees_ 平行对齐；present=false
    /// 表示该行此列值不出现在索引中——NULL/不可编码）。
    struct SecEntry {
        std::array<char, 16> key{};
        bool present = false;
    };

    bool HaveSecIdx() const noexcept { return !sec_meta_.empty(); }

    /// \brief 按当前行状态（主记录 + ext 槽/惰性默认值）取被索引列值。
    Err ColumnValueAt(uint32_t global_col, uint64_t row_no, ColumnValue* out) const noexcept;

    /// \brief 取一行在各二级索引下的键条目（创建序，与 sec_trees_ 平行）。
    Err SecKeysOfRow(uint64_t row_no, std::vector<SecEntry>* out) const noexcept;

    /// \brief 向各二级索引插入键条目（kAlreadyExists 视为幂等 OK：
    /// 同键同行号即正确状态；其余错误上抛由调用方毒化）。
    Err InsertSecKeys(const std::vector<SecEntry>& keys) noexcept;

    /// \brief 从各二级索引摘除键条目（键不存在不影响结构）。
    void RemoveSecKeys(const std::vector<SecEntry>& keys) noexcept;

    /// \brief 唯一索引冲突预检（暂存阶段，零副作用；docs/08 §3-W5）。
    /// \param row 写入行（列序号 ≥ row.size() 的列不参与：旧句柄未提供，
    ///            惰性默认值不校验——M1 边界）
    /// \param self_row_no 更新场景的目标行号（排除自身当前条目）
    /// \param has_self 是否为更新（true = 存在自身条目需排除）
    Err CheckSecUnique(const std::vector<ColumnValue>& row, uint64_t self_row_no,
                       bool has_self) const noexcept;

    /// \brief 向（新）索引树灌入全部存活行的键（唯一索引带冲突校验）。
    Err PopulateSecondaryIndex(shm_SecIdxMeta* meta, SecIndexTree* tree) noexcept;

    void* base_ = nullptr;
    infra::SlabShmAllocator* alloc_ = nullptr;
    uint64_t desc_off_ = 0;
    shm_TableDescriptor* desc_ = nullptr;
    uint64_t index_slot_off_ = 0;
    uint8_t* var_pool_base_ = nullptr;
    uint32_t nullmap_offset_ = 0;
    std::unique_ptr<MVarLenPool> var_pool_;
    std::unique_ptr<PrimaryIndexTree> index_;
    /// 二级索引元数据（创建序）与树实例（平行；W5-d）。
    std::vector<shm_SecIdxMeta*> sec_meta_;
    std::vector<std::unique_ptr<SecIndexTree>> sec_trees_;
    /// 本进程已暂存（尚未应用/释放）的插入槽位（P0-3 复用互斥）。
    std::unordered_set<uint64_t> staged_slots_;
    /// 墓碑槽位扫描起点（避免每次插入都从 0 扫描）。
    uint32_t free_scan_hint_ = 0;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// \brief 主键列类型是否受支持（M1：整数类定长类型，且须 NOT NULL）。
RTDB_API bool IsSupportedPrimaryKeyType(DataType t) noexcept;

/// \brief 释放表物理区（建表回滚用；不触碰描述符本身）。
///
/// 依次归还变长池区、主键索引锚点、墓碑位图、版本数组、行数据区。
/// 仅在表内无存活数据（或表将被整体废弃）时调用。
RTDB_API void ReleaseRegions(const shm_TableDescriptor* desc,
                             infra::SlabShmAllocator* alloc) noexcept;

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_TABLE_STORE_HPP_
