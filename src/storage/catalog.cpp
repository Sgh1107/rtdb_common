#include "storage/catalog.hpp"

#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include "infra/shm_region.hpp"
#include "storage/table_store.hpp"

namespace rtdb {
namespace storage {

namespace {

/// 当前 Unix 纳秒时间戳（建表时间记录用）。
int64_t NowUnixNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 列类型是否为本库已定义取值。
bool IsKnownType(DataType t) noexcept {
    switch (t) {
        case DataType::kInt8:
        case DataType::kInt16:
        case DataType::kInt32:
        case DataType::kInt64:
        case DataType::kFloat32:
        case DataType::kFloat64:
        case DataType::kBool:
        case DataType::kUtf8Var:
        case DataType::kBytesVar:
        case DataType::kUnixTsNs:
            return true;
        default:
            return false;
    }
}

/// 列类型是否为变长类型。
bool IsVarType(DataType t) noexcept { return t == DataType::kUtf8Var || t == DataType::kBytesVar; }

/// 定长名（char[N]）写入：零填充 + 截断保护。调用方须先校验长度。
void StoreFixedName(const std::string& name, char* dst, size_t cap) noexcept {
    std::memset(dst, 0, cap);
    const size_t n = name.size() < cap - 1 ? name.size() : cap - 1;
    std::memcpy(dst, name.data(), n);
}

}  // namespace

void EncodeTableNameKey(std::string_view name, char* out) noexcept {
    std::memset(out, 0, kMaxTableNameBytes);
    std::memcpy(out, name.data(), name.size());
}

bool IsValidTableName(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxTableNameBytes - 1) return false;
    return name.find('\0') == std::string_view::npos;
}

bool IsValidColumnName(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxColNameBytes - 1) return false;
    return name.find('\0') == std::string_view::npos;
}

Result<std::unique_ptr<Catalog>> Catalog::Attach(void* base_addr, infra::SlabShmAllocator* alloc,
                                                 layout::shm_SuperBlock* sb, bool create_new) {
    if (base_addr == nullptr || sb == nullptr)
        return Result<std::unique_ptr<Catalog>>::Fail(Err::kInvalidArgument);

    auto catalog = std::unique_ptr<Catalog>(new Catalog());
    catalog->base_ = base_addr;
    catalog->alloc_ = alloc;
    catalog->sb_ = sb;

    uint64_t root_off = sb->catalog_desc_off;
    if (create_new) {
        if (alloc == nullptr) return Result<std::unique_ptr<Catalog>>::Fail(Err::kReadOnlyHandle);
        Result<uint64_t> alloc_res =
            alloc->Allocate(static_cast<uint64_t>(sizeof(shm_CatalogRoot)));
        if (!alloc_res.IsOk()) return Result<std::unique_ptr<Catalog>>::Fail(alloc_res.Error());
        root_off = alloc_res.Value();

        auto* fresh = reinterpret_cast<shm_CatalogRoot*>(static_cast<char*>(base_addr) + root_off);
        std::memset(fresh, 0, sizeof(*fresh));
        fresh->magic = kCatalogMagic;
        fresh->next_table_id = 1;  // 表 id 从 1 起（0 保留为"无效"哨兵）
        fresh->table_count = 0;

        // catalog_desc_off 属稳定字段（参与 layout CRC），写入后须刷新校验和。
        sb->catalog_desc_off = root_off;
        layout::RefreshLayoutCrc(sb);
    }

    if (root_off != 0) {
        if (root_off + sizeof(shm_CatalogRoot) > sb->max_size_bytes)
            return Result<std::unique_ptr<Catalog>>::Fail(Err::kCorruption);
        auto* root = reinterpret_cast<shm_CatalogRoot*>(static_cast<char*>(base_addr) + root_off);
        if (root->magic != kCatalogMagic)
            return Result<std::unique_ptr<Catalog>>::Fail(Err::kCorruption);
        catalog->root_ = root;
        catalog->name_tree_.reset(new CatalogNameTree(base_addr, alloc, &root->name_slot));
        catalog->id_tree_.reset(new CatalogIdTree(base_addr, alloc, &root->id_slot));
    }
    return Result<std::unique_ptr<Catalog>>::Ok(std::move(catalog));
}

const shm_TableDescriptor* Catalog::Descriptor(uint64_t desc_off) const noexcept {
    if (desc_off == 0 || desc_off + sizeof(shm_TableDescriptor) > sb_->max_size_bytes)
        return nullptr;
    return reinterpret_cast<const shm_TableDescriptor*>(static_cast<const char*>(base_) + desc_off);
}

Result<uint64_t> Catalog::CreateTable(const TableDef& def) noexcept {
    if (root_ == nullptr) return Result<uint64_t>::Fail(Err::kUnsupported);
    if (alloc_ == nullptr) return Result<uint64_t>::Fail(Err::kReadOnlyHandle);

    // ---- 表名合法性 ----
    if (!IsValidTableName(def.name)) return Result<uint64_t>::Fail(Err::kInvalidArgument);

    // ---- 列定义合法性 ----
    const size_t col_count = def.columns.size();
    const uint64_t max_cols =
        sb_->max_cols_per_table < kMaxInitCols ? sb_->max_cols_per_table : kMaxInitCols;
    if (col_count == 0 || col_count > max_cols)
        return Result<uint64_t>::Fail(col_count == 0 ? Err::kInvalidArgument : Err::kLimitExceeded);
    for (size_t i = 0; i < col_count; ++i) {
        const ColumnDef& col = def.columns[i];
        if (!IsValidColumnName(col.name)) return Result<uint64_t>::Fail(Err::kInvalidArgument);
        if (!IsKnownType(col.type)) return Result<uint64_t>::Fail(Err::kInvalidArgument);
        if (IsVarType(col.type) && col.len != 0)
            return Result<uint64_t>::Fail(Err::kInvalidArgument);
        // 列名去重：重名列将使后续按名寻址产生歧义。
        for (size_t j = 0; j < i; ++j) {
            if (def.columns[j].name == col.name)
                return Result<uint64_t>::Fail(Err::kInvalidArgument);
        }
    }

    // ---- 上限约束 ----
    if (root_->table_count >= sb_->max_tables) return Result<uint64_t>::Fail(Err::kLimitExceeded);

    // ---- 主键约束（M1）：列 0 为整数类定长类型且 NOT NULL ----
    if (!IsSupportedPrimaryKeyType(def.columns[0].type))
        return Result<uint64_t>::Fail(Err::kUnsupported);
    if (def.columns[0].nullable) return Result<uint64_t>::Fail(Err::kInvalidArgument);

    // ---- 重名冲突 ----
    char name_key[kMaxTableNameBytes];
    EncodeTableNameKey(def.name, name_key);
    uint64_t existing_id = 0;
    if (name_tree_->Find(name_key, &existing_id)) return Result<uint64_t>::Fail(Err::kTableExists);

    // ---- 行布局计算 ----
    const uint32_t row_cap = def.row_cap == 0 ? kDefaultRowCap : def.row_cap;
    if (row_cap == 0) return Result<uint64_t>::Fail(Err::kInvalidArgument);

    std::unique_ptr<uint32_t[]> col_offsets(new uint32_t[col_count]);
    const RowLayoutPlan plan =
        PlanRowLayout(def.columns.data(), col_count, row_cap, col_offsets.get());
    if (!IsOk(plan.err)) return Result<uint64_t>::Fail(plan.err);

    // ---- 分配并填充描述符（先完整写入，再挂树）----
    Result<uint64_t> alloc_res =
        alloc_->Allocate(static_cast<uint64_t>(sizeof(shm_TableDescriptor)));
    if (!alloc_res.IsOk()) return Result<uint64_t>::Fail(alloc_res.Error());
    const uint64_t desc_off = alloc_res.Value();
    auto* desc = reinterpret_cast<shm_TableDescriptor*>(static_cast<char*>(base_) + desc_off);
    std::memset(desc, 0, sizeof(*desc));
    desc->magic = kTableDescMagic;
    desc->table_id = root_->next_table_id;
    desc->create_unix_ns = NowUnixNs();
    StoreFixedName(def.name, desc->name, sizeof(desc->name));
    desc->col_count = static_cast<uint32_t>(col_count);
    desc->ext_col_count = 0;
    desc->rec_fixed_len = plan.rec_fixed_len;
    desc->row_cap = row_cap;
    desc->row_count = 0;
    desc->committed_row_count = 0;
    desc->state = static_cast<uint32_t>(TableState::kActive);
    desc->data_layout_ver = 1;
    for (size_t i = 0; i < col_count; ++i) {
        shm_ColDef& sc = desc->cols[i];
        StoreFixedName(def.columns[i].name, sc.name, sizeof(sc.name));
        sc.offset = col_offsets[i];
        sc.len = IsVarType(def.columns[i].type) ? 0 : FixedSizeOf(def.columns[i].type);
        if (def.columns[i].len != 0) sc.len = def.columns[i].len;
        sc.default_bits_off = 0;
        sc.type = static_cast<uint8_t>(def.columns[i].type);
        sc.nullable = def.columns[i].nullable ? 1 : 0;
        sc.flags = 0;
    }

    // ---- 分配物理区（行数据/版本数组/墓碑位图/变长池/主键索引槽）----
    const Err fmt = TableStore::Format(def, desc, base_, alloc_);
    if (!IsOk(fmt)) {
        alloc_->Deallocate(desc_off);
        return Result<uint64_t>::Fail(fmt);
    }

    // ---- 挂入两棵索引树（失败即完整回滚）----
    Err err = name_tree_->Insert(name_key, desc->table_id);
    if (!IsOk(err)) {
        ReleaseRegions(desc, alloc_);
        alloc_->Deallocate(desc_off);
        return Result<uint64_t>::Fail(err);
    }
    char id_key[8];
    infra::EncodeOrderedInt64(static_cast<int64_t>(desc->table_id), id_key);
    err = id_tree_->Insert(id_key, desc_off);
    if (!IsOk(err)) {
        name_tree_->Remove(name_key);
        ReleaseRegions(desc, alloc_);
        alloc_->Deallocate(desc_off);
        return Result<uint64_t>::Fail(err);
    }

    root_->next_table_id++;
    root_->table_count++;
    return Result<uint64_t>::Ok(desc_off);
}

Err Catalog::DropTable(std::string_view name) noexcept {
    if (root_ == nullptr) return Err::kUnsupported;
    if (alloc_ == nullptr) return Err::kReadOnlyHandle;
    if (!IsValidTableName(name)) return Err::kInvalidArgument;

    char name_key[kMaxTableNameBytes];
    EncodeTableNameKey(name, name_key);
    uint64_t table_id = 0;
    if (!name_tree_->Find(name_key, &table_id)) return Err::kTableNotFound;

    char id_key[8];
    infra::EncodeOrderedInt64(static_cast<int64_t>(table_id), id_key);
    uint64_t desc_off = 0;
    if (!id_tree_->Find(id_key, &desc_off)) return Err::kCorruption;

    auto* desc = reinterpret_cast<shm_TableDescriptor*>(static_cast<char*>(base_) + desc_off);
    if (desc->magic != kTableDescMagic || desc->table_id != table_id) {
        // 目录与描述符不一致：此时按只读语义拒绝，避免在不确定状态上继续。
        return Err::kCorruption;
    }
    if (desc->state != static_cast<uint32_t>(TableState::kActive)) return Err::kTableNotFound;

    // 先置墓碑（已按名解析到本表的读者随即拒绝），再摘除表名索引。
    desc->state = static_cast<uint32_t>(TableState::kTombstone);
    const Err err = name_tree_->Remove(name_key);
    if (!IsOk(err)) {
        desc->state = static_cast<uint32_t>(TableState::kActive);  // 回滚
        return err;
    }
    if (root_->table_count > 0) root_->table_count--;
    // id 索引保留：物理目录完整（描述符仍可按 id 审计），空间回收归 compact。
    return Err::kOk;
}

Result<uint64_t> Catalog::FindById(uint64_t table_id) const noexcept {
    if (root_ == nullptr) return Result<uint64_t>::Fail(Err::kUnsupported);
    if (table_id == 0) return Result<uint64_t>::Fail(Err::kInvalidArgument);

    char id_key[8];
    infra::EncodeOrderedInt64(static_cast<int64_t>(table_id), id_key);
    uint64_t desc_off = 0;
    if (!id_tree_->Find(id_key, &desc_off)) return Result<uint64_t>::Fail(Err::kTableNotFound);
    const shm_TableDescriptor* desc = Descriptor(desc_off);
    if (desc == nullptr || desc->magic != kTableDescMagic || desc->table_id != table_id)
        return Result<uint64_t>::Fail(Err::kCorruption);
    return Result<uint64_t>::Ok(desc_off);
}

Result<uint64_t> Catalog::OpenTable(std::string_view name) const noexcept {
    if (root_ == nullptr) return Result<uint64_t>::Fail(Err::kUnsupported);
    if (!IsValidTableName(name)) return Result<uint64_t>::Fail(Err::kInvalidArgument);

    char name_key[kMaxTableNameBytes];
    EncodeTableNameKey(name, name_key);
    uint64_t table_id = 0;
    if (!name_tree_->Find(name_key, &table_id)) return Result<uint64_t>::Fail(Err::kTableNotFound);

    Result<uint64_t> desc_res = FindById(table_id);
    if (!desc_res.IsOk()) return Result<uint64_t>::Fail(desc_res.Error());
    const shm_TableDescriptor* desc = Descriptor(desc_res.Value());
    if (desc == nullptr) return Result<uint64_t>::Fail(Err::kCorruption);
    if (desc->state != static_cast<uint32_t>(TableState::kActive))
        return Result<uint64_t>::Fail(Err::kTableNotFound);
    return desc_res;
}

Err Catalog::Validate(uint64_t* out_active_tables) const noexcept {
    if (out_active_tables != nullptr) *out_active_tables = 0;
    if (root_ == nullptr) return Err::kOk;  // 未布局目录视为空目录

    uint64_t name_count = 0;
    uint64_t active_from_name = 0;
    uint64_t max_id = 0;

    // ---- 表名树遍历：每个名字必须解析到同名的活跃描述符 ----
    char lo[kMaxTableNameBytes] = {0};
    char hi[kMaxTableNameBytes];
    std::memset(hi, 0xFF, sizeof(hi));
    for (auto cur = name_tree_->SeekRange(lo, true, hi, true); cur.Valid(); cur.Next()) {
        char key[kMaxTableNameBytes];
        cur.Key(key);
        const uint64_t table_id = cur.Value();
        Result<uint64_t> desc_res = FindById(table_id);
        if (!desc_res.IsOk()) return Err::kCorruption;
        const shm_TableDescriptor* desc = Descriptor(desc_res.Value());
        if (desc == nullptr) return Err::kCorruption;
        if (std::memcmp(desc->name, key, kMaxTableNameBytes) != 0) return Err::kCorruption;
        if (desc->state != static_cast<uint32_t>(TableState::kActive)) return Err::kCorruption;
        if (table_id > max_id) max_id = table_id;
        name_count++;
        active_from_name++;
    }

    // ---- 表 id 树遍历：活跃/墓碑计数与描述符自洽 ----
    char id_lo[8];
    char id_hi[8];
    infra::EncodeOrderedInt64(std::numeric_limits<int64_t>::min(), id_lo);
    infra::EncodeOrderedInt64(std::numeric_limits<int64_t>::max(), id_hi);
    uint64_t active_from_id = 0;
    uint64_t tombstones = 0;
    for (auto cur = id_tree_->SeekRange(id_lo, true, id_hi, true); cur.Valid(); cur.Next()) {
        char key[8];
        cur.Key(key);
        const uint64_t table_id = static_cast<uint64_t>(infra::DecodeOrderedInt64(key));
        const uint64_t desc_off = cur.Value();
        const shm_TableDescriptor* desc = Descriptor(desc_off);
        if (desc == nullptr || desc->magic != kTableDescMagic) return Err::kCorruption;
        if (desc->table_id != table_id) return Err::kCorruption;
        if (desc->state == static_cast<uint32_t>(TableState::kActive)) {
            active_from_id++;
        } else if (desc->state == static_cast<uint32_t>(TableState::kTombstone)) {
            tombstones++;
        } else {
            return Err::kCorruption;
        }
        if (table_id > max_id) max_id = table_id;
    }

    if (name_count != active_from_name) return Err::kCorruption;
    if (active_from_name != active_from_id) return Err::kCorruption;
    if (root_->table_count != active_from_id) return Err::kCorruption;
    if (name_tree_->Size() != active_from_id) return Err::kCorruption;
    if (id_tree_->Size() != active_from_id + tombstones) return Err::kCorruption;
    if (root_->next_table_id <= max_id) return Err::kCorruption;

    if (out_active_tables != nullptr) *out_active_tables = active_from_id;
    return Err::kOk;
}

}  // namespace storage
}  // namespace rtdb
