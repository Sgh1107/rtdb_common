#ifndef RTDB_SRC_STORAGE_CATALOG_HPP_
#define RTDB_SRC_STORAGE_CATALOG_HPP_

/// \file
/// \brief 表目录 Catalog（docs/03 §4.1，docs/08 §2-D4）。
///
/// 目录由两棵共享内存 B+ 树与一个目录根块构成：
///  - **表名树**：key = 表名补零定长 64B，payload = `table_id`；
///  - **表 id 树**：key = `EncodeOrderedInt64(table_id)`，payload = 描述符偏移；
///  - **目录根块**：记录 `next_table_id` / `table_count` 与两棵树的双字槽，
///    其偏移持久化在 SuperBlock 的 `catalog_desc_off`（0 = 目录尚未布局）。
///
/// 描述符（`shm_TableDescriptor`）经 Slab 分配器按需分配，不做固定数组，
/// 避免「最大表数 × 1280B」的静态占用。
///
/// 并发约定：所有变更必须在 `ShmRootMutex` 临界区内执行；只读查询
/// （`OpenTable` / `FindById` / `Validate`）可在无锁的 Reader 角色下使用。
/// 目录事务顺序：先完整写入描述符，再挂入索引树——保证读者顺树而下时
/// 不会观察到未初始化/半写入的描述符。

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "infra/layout.hpp"
#include "infra/shared_btree.hpp"
#include "infra/slab_allocator.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"
#include "rtdb/table_def.hpp"
#include "storage/schema.hpp"

namespace rtdb {
namespace storage {

/// 表名索引树：key = 表名补零定长 64B，payload = 表 id。
using CatalogNameTree = infra::SharedBTree<kMaxTableNameBytes, uint64_t>;

/// 表 id 索引树：key = `EncodeOrderedInt64(table_id)`，payload = 描述符偏移。
using CatalogIdTree = infra::SharedBTree<8, uint64_t>;

/// 目录根块魔数（"RTDBCAT1"）。
inline constexpr uint64_t kCatalogMagic = 0x5254444243415431ULL;

/// \brief 目录根块（128B 定长，标准布局，驻留映像）。
///
/// 生命周期：映像创建时从 Slab 分配并初始化，偏移写入 SuperBlock；
/// 之后各进程复附着同一根块，树实例按需重新构造（B+ 树自身无状态）。
struct shm_CatalogRoot {
    uint64_t magic;                   ///< 固定 kCatalogMagic
    uint64_t next_table_id;           ///< 下一个待分配表 id（从 1 起，单调不回收）
    uint64_t table_count;             ///< 活跃表数（墓碑表不计入）
    uint64_t reserved0;               ///< 预留（对齐/未来扩展）
    CatalogNameTree::Slot name_slot;  ///< 表名树锚点
    CatalogIdTree::Slot id_slot;      ///< 表 id 树锚点
    uint64_t reserved[8];             ///< 预留（未来权限/二级目录锚点）
};
static_assert(sizeof(shm_CatalogRoot) == 128, "目录根块必须恰好 128 字节");
static_assert(std::is_standard_layout<shm_CatalogRoot>::value, "目录根块必须是标准布局");

/// \brief 表目录：建表/删表/按名或 id 定位表描述符。
#if defined(_MSC_VER)
#pragma warning(push)
// C4251: 内部持有 std::unique_ptr 成员（不参与 DLL 接口实体化）；
// 与 Engine 的 PImpl 处理一致，属预期内安全用法。
#pragma warning(disable : 4251)
#endif
class RTDB_API Catalog {
public:
    /// \brief 附着或初始化目录。
    ///
    /// `create_new == true` 时从 `alloc` 分配目录根块并写入 SuperBlock
    /// （同时刷新 layout CRC）；否则读取 `SuperBlock::catalog_desc_off`
    /// 校验魔数后复附着。
    ///
    /// \param base_addr 映像基址
    /// \param alloc 分配器；Reader 角色传 nullptr（目录只读可用）
    /// \param sb SuperBlock（读写角色可写；create_new 时会写 catalog_desc_off）
    /// \param create_new true = 初始化新目录
    /// \return 目录实例；SuperBlock 未布局目录且 create_new=false 时返回
    ///         一个 `available() == false` 的实例（兼容 M0 期映像）
    static Result<std::unique_ptr<Catalog>> Attach(void* base_addr, infra::SlabShmAllocator* alloc,
                                                   layout::shm_SuperBlock* sb, bool create_new);

    /// \brief 目录是否已布局并就绪。
    bool available() const noexcept { return root_ != nullptr; }

    /// \brief 建立新表（需持根锁，且为可写角色）。
    ///
    /// 依次校验：表名合法性 → 列定义合法性 → 上限约束 → 重名冲突；
    /// 分配并填充描述符后挂入两棵树。任一步失败完整回滚（不泄漏描述符、
    /// 不推进 id 计数），失败码见 `Err`：
    /// `kInvalidArgument` / `kLimitExceeded` / `kTableExists` /
    /// `kReadOnlyHandle` / `kUnsupported` / `kOutOfMemory`。
    ///
    /// \param def 建表定义
    /// \return 描述符在映像内的偏移
    Result<uint64_t> CreateTable(const TableDef& def) noexcept;

    /// \brief 删除表：置描述符墓碑态并摘除表名索引（id 索引保留以维持
    /// 物理目录完整，空间回收归后续 compact）。需持根锁且为可写角色。
    ///
    /// \param name 表名
    /// \return kOk / kTableNotFound / kReadOnlyHandle / kUnsupported
    Err DropTable(std::string_view name) noexcept;

    /// \brief 按表名定位描述符偏移（只读可用）。
    /// \return 描述符偏移；不存在或已删除返回 kTableNotFound
    Result<uint64_t> OpenTable(std::string_view name) const noexcept;

    /// \brief 按表 id 定位描述符偏移（只读可用）。
    /// \return 描述符偏移；不存在返回 kTableNotFound
    Result<uint64_t> FindById(uint64_t table_id) const noexcept;

    /// \brief 目录内活跃表数。
    uint64_t table_count() const noexcept { return root_ != nullptr ? root_->table_count : 0; }

    /// \brief 下一个待分配表 id（1 起）。
    uint64_t next_table_id() const noexcept { return root_ != nullptr ? root_->next_table_id : 0; }

    /// \brief 映像内描述符指针（调用方保证偏移有效）。
    const shm_TableDescriptor* Descriptor(uint64_t desc_off) const noexcept;

    /// \brief 结构不变量校验（测试与诊断用）：
    /// 两棵树的计数关系、描述符魔数/id/名字自洽、状态分布一致。
    /// \param out_active_tables 可选输出：活跃表数
    /// \return kOk 或 kCorruption
    Err Validate(uint64_t* out_active_tables = nullptr) const noexcept;

    /// \brief 遍历目录内全部表（含墓碑表；按表 id 升序）。
    ///
    /// 供开卷类操作（如二级索引延迟重建，M1-W5-d）枚举活跃表；
    /// 墓碑表由调用方按 `state` 过滤（物理区待 compact 回收）。
    /// 回调同步调用，`desc` 指针仅在回调执行期内有效。
    /// \param fn void(uint64_t desc_off, const shm_TableDescriptor* desc)
    /// \return kOk / kUnsupported（目录未布局）/ kInvalidArgument / kCorruption
    Err ForEachTable(const std::function<void(uint64_t, const shm_TableDescriptor*)>& fn) const noexcept;

private:
    Catalog() = default;

    void* base_ = nullptr;
    infra::SlabShmAllocator* alloc_ = nullptr;
    layout::shm_SuperBlock* sb_ = nullptr;
    shm_CatalogRoot* root_ = nullptr;
    std::unique_ptr<CatalogNameTree> name_tree_;
    std::unique_ptr<CatalogIdTree> id_tree_;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// \brief 表名 → 定长键编码（补零）。调用方须先通过 `IsValidTableName`。
/// \param name 表名
/// \param out 输出缓冲，长度恰为 kMaxTableNameBytes
void EncodeTableNameKey(std::string_view name, char* out) noexcept;

/// \brief 表名合法性：非空、不含 NUL、长度 ≤ kMaxTableNameBytes-1。
bool IsValidTableName(std::string_view name) noexcept;

/// \brief 列名合法性：非空、不含 NUL、长度 ≤ kMaxColNameBytes-1。
bool IsValidColumnName(std::string_view name) noexcept;

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_CATALOG_HPP_
