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
/// M1 边界：行槽不复用（删除仅置墓碑，槽位回收归 compact）；主键限整数类
/// 定长类型；单表行数据区受 `kMaxRowDataBytes` 约束（段目录化列 M2）。

#include <cstdint>
#include <memory>
#include <vector>

#include "infra/shared_btree.hpp"
#include "infra/slab_allocator.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"
#include "rtdb/table_def.hpp"
#include "storage/schema.hpp"
#include "storage/var_pool.hpp"

namespace rtdb {
namespace storage {

/// 主键索引：key = `EncodeOrderedInt64(主键值)`，payload = 行槽号。
using PrimaryIndexTree = infra::SharedBTree<8, uint64_t>;

/// \brief 单表行存储：格式化物理区 + 行级 CRUD。
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

    /// \brief 按主键整行覆盖更新（主键不可变更）。
    Err Update(const ColumnValue& key, const std::vector<ColumnValue>& row) noexcept;

    /// \brief 按主键删除（置墓碑 + 摘除索引键）。
    Err Remove(const ColumnValue& key) noexcept;

    /// \brief 按主键判定行是否存在。
    Result<bool> Contains(const ColumnValue& key) const noexcept;

    /// \brief 已提交可见行数。
    uint64_t live_rows() const noexcept { return desc_->committed_row_count; }

    /// \brief 已占用行槽数（含墓碑）。
    uint64_t used_rows() const noexcept { return desc_->row_count; }

    /// \brief 结构不变量校验（测试与诊断）：
    /// 索引规模 + 墓碑数 == 已用槽数，且版本数组均为稳定偶数。
    Err Validate(uint64_t* out_index_entries = nullptr) const noexcept;

    /// \brief 描述符只读指针。
    const shm_TableDescriptor* descriptor() const noexcept { return desc_; }

private:
    TableStore() = default;

    /// \brief 行槽在映像内的基址。
    uint8_t* RowPtr(uint64_t row_no) const noexcept;
    /// \brief 行版本槽。
    uint64_t* VersionSlot(uint64_t row_no) const noexcept;
    /// \brief 墓碑位判定。
    bool IsTombstoned(uint64_t row_no) const noexcept;
    void SetTombstone(uint64_t row_no, bool dead) const noexcept;
    /// \brief 主键编码；不可编码（类型不符/空值）返回 false。
    static bool EncodeKey(const ColumnValue& key, char out[8]) noexcept;
    /// \brief 组装记录缓冲（含变长块分配）；失败时释放已分配块。
    Err BuildRecord(const std::vector<ColumnValue>& row, std::vector<uint8_t>* rec,
                    std::vector<uint64_t>* allocated_var_offs) const noexcept;
    /// \brief 释放记录中的变长块。
    void ReleaseRecordVars(const uint8_t* rec) const noexcept;
    /// \brief 从记录解码列值。
    std::vector<ColumnValue> DecodeRecord(const uint8_t* rec) const noexcept;

    void* base_ = nullptr;
    infra::SlabShmAllocator* alloc_ = nullptr;
    uint64_t desc_off_ = 0;
    shm_TableDescriptor* desc_ = nullptr;
    uint64_t index_slot_off_ = 0;
    uint8_t* var_pool_base_ = nullptr;
    uint32_t nullmap_offset_ = 0;
    std::unique_ptr<MVarLenPool> var_pool_;
    std::unique_ptr<PrimaryIndexTree> index_;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// \brief 主键列类型是否受支持（M1：整数类定长类型，且须 NOT NULL）。
bool IsSupportedPrimaryKeyType(DataType t) noexcept;

/// \brief 释放表物理区（建表回滚用；不触碰描述符本身）。
///
/// 依次归还变长池区、主键索引锚点、墓碑位图、版本数组、行数据区。
/// 仅在表内无存活数据（或表将被整体废弃）时调用。
void ReleaseRegions(const shm_TableDescriptor* desc, infra::SlabShmAllocator* alloc) noexcept;

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_TABLE_STORE_HPP_
