#include "storage/table_store.hpp"

#include <cstring>
#include <utility>

namespace rtdb {
namespace storage {

namespace {

/// 记录内 null 位图偏移：逐列按 PlanRowLayout 的同一规则累加而来。
uint32_t ComputeNullmapOffset(const shm_TableDescriptor* desc) noexcept {
    uint32_t off = 0;
    for (uint32_t i = 0; i < desc->col_count; ++i) {
        const shm_ColDef& c = desc->cols[i];
        const DataType t = static_cast<DataType>(c.type);
        const uint32_t align = AlignOf(t);
        off = static_cast<uint32_t>((off + align - 1) / align * align);
        off += c.len != 0 ? c.len : (FixedSizeOf(t) > 0 ? FixedSizeOf(t) : 8);
    }
    return off;
}

/// 整数值按列宽写入（1/2/4/8 字节，本机序）。
void StoreInteger(uint8_t* dst, uint32_t width, int64_t v) noexcept {
    const uint32_t n = width < 8 ? width : 8;
    std::memcpy(dst, &v, n);
}

/// 读取整数值并按列宽做符号扩展。
int64_t LoadInteger(const uint8_t* src, uint32_t width) noexcept {
    if (width >= 8) {
        int64_t v = 0;
        std::memcpy(&v, src, 8);
        return v;
    }
    int64_t v = 0;
    std::memcpy(&v, src, width);  // dst 零初始化，高位为 0
    const uint32_t bits = width * 8;
    if (bits < 64 && (v & (1LL << (bits - 1))) != 0) v |= ~((1LL << bits) - 1);
    return v;
}

}  // namespace

bool IsSupportedPrimaryKeyType(DataType t) noexcept {
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

void ReleaseRegions(const shm_TableDescriptor* desc, infra::SlabShmAllocator* alloc) noexcept {
    if (desc == nullptr || alloc == nullptr) return;
    if (desc->var_pool_head != 0) alloc->Deallocate(desc->var_pool_head);
    if (desc->index_head_off[0] != 0) alloc->Deallocate(desc->index_head_off[0]);
    if (desc->tombmap_off != 0) alloc->Deallocate(desc->tombmap_off);
    if (desc->ver_array_off != 0) alloc->Deallocate(desc->ver_array_off);
    if (desc->data_seg_head_off != 0) alloc->Deallocate(desc->data_seg_head_off);
}

Err TableStore::Format(const TableDef& def, shm_TableDescriptor* desc, void* base,
                       infra::SlabShmAllocator* alloc) noexcept {
    (void)def;
    if (desc == nullptr || base == nullptr || alloc == nullptr) return Err::kInvalidArgument;

    // 主键列约束（M1）：整数类且 NOT NULL。
    const shm_ColDef& pk = desc->cols[0];
    if (!IsSupportedPrimaryKeyType(static_cast<DataType>(pk.type)) || pk.nullable != 0)
        return Err::kUnsupported;
    if (desc->rec_fixed_len == 0 || desc->row_cap == 0) return Err::kInvalidArgument;
    if (desc->rec_fixed_len > kMaxRowFixedBytes) return Err::kLimitExceeded;

    const uint64_t data_bytes = static_cast<uint64_t>(desc->rec_fixed_len) * desc->row_cap;
    if (data_bytes > kMaxRowDataBytes) return Err::kLimitExceeded;

    // 依次分配，失败回滚已分配块。
    uint64_t data_off = 0;
    uint64_t ver_off = 0;
    uint64_t tomb_off = 0;
    uint64_t pool_off = 0;
    uint64_t slot_off = 0;

    const auto release_all = [&]() noexcept {
        if (slot_off != 0) alloc->Deallocate(slot_off);
        if (pool_off != 0) alloc->Deallocate(pool_off);
        if (tomb_off != 0) alloc->Deallocate(tomb_off);
        if (ver_off != 0) alloc->Deallocate(ver_off);
        if (data_off != 0) alloc->Deallocate(data_off);
    };

    Result<uint64_t> r = alloc->Allocate(data_bytes);
    if (!r.IsOk()) return r.Error();
    data_off = r.Value();
    std::memset(alloc->OffsetToPtr(data_off), 0, static_cast<size_t>(data_bytes));

    const uint64_t ver_bytes = static_cast<uint64_t>(desc->row_cap) * 8;
    r = alloc->Allocate(ver_bytes);
    if (!r.IsOk()) {
        release_all();
        return r.Error();
    }
    ver_off = r.Value();
    std::memset(alloc->OffsetToPtr(ver_off), 0, static_cast<size_t>(ver_bytes));

    const uint64_t tomb_bytes = (static_cast<uint64_t>(desc->row_cap) + 7) / 8;
    r = alloc->Allocate(tomb_bytes);
    if (!r.IsOk()) {
        release_all();
        return r.Error();
    }
    tomb_off = r.Value();
    std::memset(alloc->OffsetToPtr(tomb_off), 0, static_cast<size_t>(tomb_bytes));

    // 变长池：仅当表含变长列时布局。
    bool has_var = false;
    for (uint32_t i = 0; i < desc->col_count; ++i) {
        const DataType t = static_cast<DataType>(desc->cols[i].type);
        if (t == DataType::kUtf8Var || t == DataType::kBytesVar) has_var = true;
    }
    if (has_var) {
        r = alloc->Allocate(kVarPoolRegionBytes);
        if (!r.IsOk()) {
            release_all();
            return r.Error();
        }
        pool_off = r.Value();
        auto pool_res = MVarLenPool::Attach(alloc->OffsetToPtr(pool_off), kVarPoolRegionBytes,
                                            /*create_new=*/true);
        if (!pool_res.IsOk()) {
            release_all();
            return pool_res.Error();
        }
    }

    // 主键索引锚点槽。
    r = alloc->Allocate(static_cast<uint64_t>(sizeof(PrimaryIndexTree::Slot)));
    if (!r.IsOk()) {
        release_all();
        return r.Error();
    }
    slot_off = r.Value();
    std::memset(alloc->OffsetToPtr(slot_off), 0, sizeof(PrimaryIndexTree::Slot));

    desc->data_seg_head_off = data_off;
    desc->ver_array_off = ver_off;
    desc->tombmap_off = tomb_off;
    desc->var_pool_head = pool_off;
    desc->var_pool_bytes = has_var ? kVarPoolRegionBytes : 0;
    desc->index_head_off[0] = slot_off;
    return Err::kOk;
}

Result<std::unique_ptr<TableStore>> TableStore::Attach(void* base, infra::SlabShmAllocator* alloc,
                                                       uint64_t desc_off) noexcept {
    if (base == nullptr || desc_off == 0)
        return Result<std::unique_ptr<TableStore>>::Fail(Err::kInvalidArgument);

    auto* desc = reinterpret_cast<shm_TableDescriptor*>(static_cast<char*>(base) + desc_off);
    if (desc->magic != kTableDescMagic)
        return Result<std::unique_ptr<TableStore>>::Fail(Err::kCorruption);
    if (desc->cols[0].type == 0 || desc->rec_fixed_len == 0 || desc->row_cap == 0)
        return Result<std::unique_ptr<TableStore>>::Fail(Err::kCorruption);

    auto store = std::unique_ptr<TableStore>(new TableStore());
    store->base_ = base;
    store->alloc_ = alloc;  // 可为 nullptr：只读角色挂载（变更操作将拒绝）
    store->desc_off_ = desc_off;
    store->desc_ = desc;
    store->index_slot_off_ = desc->index_head_off[0];
    store->nullmap_offset_ = ComputeNullmapOffset(desc);

    // 可写角色可校验各分区归属；只读角色无分配器，仅做非零检查。
    const bool regions_present = desc->data_seg_head_off != 0 && desc->ver_array_off != 0 &&
                                 desc->tombmap_off != 0 && store->index_slot_off_ != 0;
    if (!regions_present) return Result<std::unique_ptr<TableStore>>::Fail(Err::kCorruption);
    if (alloc != nullptr) {
        if (!alloc->OwnsOffset(desc->data_seg_head_off) ||
            !alloc->OwnsOffset(desc->ver_array_off) || !alloc->OwnsOffset(desc->tombmap_off) ||
            !alloc->OwnsOffset(store->index_slot_off_)) {
            return Result<std::unique_ptr<TableStore>>::Fail(Err::kCorruption);
        }
    }

    if (desc->var_pool_head != 0) {
        if (desc->var_pool_bytes == 0)
            return Result<std::unique_ptr<TableStore>>::Fail(Err::kCorruption);
        if (alloc != nullptr && !alloc->OwnsOffset(desc->var_pool_head))
            return Result<std::unique_ptr<TableStore>>::Fail(Err::kCorruption);
        auto pool_res = MVarLenPool::Attach(static_cast<char*>(base) + desc->var_pool_head,
                                            desc->var_pool_bytes, /*create_new=*/false);
        if (!pool_res.IsOk()) return Result<std::unique_ptr<TableStore>>::Fail(pool_res.Error());
        store->var_pool_ = pool_res.TakeValue();
        store->var_pool_base_ = static_cast<uint8_t*>(base) + desc->var_pool_head;
    }

    auto* slot = reinterpret_cast<PrimaryIndexTree::Slot*>(static_cast<char*>(base) +
                                                           store->index_slot_off_);
    store->index_.reset(new PrimaryIndexTree(base, alloc, slot));
    return Result<std::unique_ptr<TableStore>>::Ok(std::move(store));
}

uint8_t* TableStore::RowPtr(uint64_t row_no) const noexcept {
    return static_cast<uint8_t*>(base_) + desc_->data_seg_head_off + row_no * desc_->rec_fixed_len;
}

uint64_t* TableStore::VersionSlot(uint64_t row_no) const noexcept {
    return reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(base_) + desc_->ver_array_off +
                                       row_no * 8);
}

bool TableStore::IsTombstoned(uint64_t row_no) const noexcept {
    const auto* bits = static_cast<const uint8_t*>(base_) + desc_->tombmap_off;
    return ((bits[row_no / 8] >> (row_no % 8)) & 0x1u) != 0;
}

void TableStore::SetTombstone(uint64_t row_no, bool dead) const noexcept {
    auto* bits = static_cast<uint8_t*>(base_) + desc_->tombmap_off;
    if (dead) {
        bits[row_no / 8] = static_cast<uint8_t>(bits[row_no / 8] | (1u << (row_no % 8)));
    } else {
        bits[row_no / 8] = static_cast<uint8_t>(bits[row_no / 8] & ~(1u << (row_no % 8)));
    }
}

bool TableStore::EncodeKey(const ColumnValue& key, char out[8]) noexcept {
    if (key.is_null()) return false;
    switch (key.type()) {
        case DataType::kInt8:
        case DataType::kInt16:
        case DataType::kInt32:
        case DataType::kInt64:
        case DataType::kBool:
        case DataType::kUnixTsNs:
            infra::EncodeOrderedInt64(key.as_int(), out);
            return true;
        default:
            return false;
    }
}

Err TableStore::BuildRecord(const std::vector<ColumnValue>& row, std::vector<uint8_t>* rec,
                            std::vector<uint64_t>* allocated_var_offs) const noexcept {
    rec->assign(desc_->rec_fixed_len, 0);
    allocated_var_offs->clear();
    uint8_t* buf = rec->data();

    for (uint32_t i = 0; i < desc_->col_count; ++i) {
        const shm_ColDef& c = desc_->cols[i];
        const DataType t = static_cast<DataType>(c.type);
        const bool is_var = (t == DataType::kUtf8Var || t == DataType::kBytesVar);
        if (row[i].is_null()) {
            if (c.nullable == 0) return Err::kInvalidArgument;  // NOT NULL 列不可为空
            buf[nullmap_offset_ + i / 8] =
                static_cast<uint8_t>(buf[nullmap_offset_ + i / 8] | (1u << (i % 8)));
            continue;
        }

        if (is_var) {
            const std::string& s = row[i].as_bytes();
            uint32_t off = 0;
            const uint32_t len = static_cast<uint32_t>(s.size());
            if (len != 0) {
                if (var_pool_ == nullptr) return Err::kInternal;
                Result<uint64_t> ar = var_pool_->Allocate(len);
                if (!ar.IsOk()) {
                    for (uint64_t prev : *allocated_var_offs) var_pool_->Deallocate(prev);
                    allocated_var_offs->clear();
                    return ar.Error();
                }
                off = static_cast<uint32_t>(ar.Value());
                std::memcpy(var_pool_base_ + off, s.data(), len);
                allocated_var_offs->push_back(ar.Value());
            }
            std::memcpy(buf + c.offset, &off, 4);
            std::memcpy(buf + c.offset + 4, &len, 4);
            continue;
        }

        // 定长列。
        if (t == DataType::kFloat32) {
            const float f = static_cast<float>(row[i].as_real());
            std::memcpy(buf + c.offset, &f, 4);
        } else if (t == DataType::kFloat64) {
            const double d = row[i].as_real();
            std::memcpy(buf + c.offset, &d, 8);
        } else if (c.len > 8) {
            // 宽定长字段：按字节拷贝并零填充（如 16B 二进制标识）。
            const std::string& s = row[i].as_bytes();
            const size_t n = s.size() < c.len ? s.size() : c.len;
            if (n != 0) std::memcpy(buf + c.offset, s.data(), n);
        } else {
            StoreInteger(buf + c.offset, c.len, row[i].as_int());
        }
    }
    return Err::kOk;
}

void TableStore::ReleaseRecordVars(const uint8_t* rec) const noexcept {
    if (var_pool_ == nullptr) return;
    for (uint32_t i = 0; i < desc_->col_count; ++i) {
        const shm_ColDef& c = desc_->cols[i];
        const DataType t = static_cast<DataType>(c.type);
        if (t != DataType::kUtf8Var && t != DataType::kBytesVar) continue;
        if (((rec[nullmap_offset_ + i / 8] >> (i % 8)) & 0x1u) != 0) continue;
        uint32_t off = 0;
        std::memcpy(&off, rec + c.offset, 4);
        if (off != 0) var_pool_->Deallocate(off);
    }
}

std::vector<ColumnValue> TableStore::DecodeRecord(const uint8_t* rec) const noexcept {
    std::vector<ColumnValue> out;
    out.reserve(desc_->col_count);
    for (uint32_t i = 0; i < desc_->col_count; ++i) {
        const shm_ColDef& c = desc_->cols[i];
        const DataType t = static_cast<DataType>(c.type);
        const bool is_null_bit = ((rec[nullmap_offset_ + i / 8] >> (i % 8)) & 0x1u) != 0;
        if (is_null_bit) {
            out.push_back(ColumnValue::Null());
            continue;
        }
        if (t == DataType::kUtf8Var || t == DataType::kBytesVar) {
            uint32_t off = 0;
            uint32_t len = 0;
            std::memcpy(&off, rec + c.offset, 4);
            std::memcpy(&len, rec + c.offset + 4, 4);
            std::string s;
            if (off != 0 && len != 0 && var_pool_base_ != nullptr)
                s.assign(reinterpret_cast<const char*>(var_pool_base_ + off), len);
            out.push_back(t == DataType::kUtf8Var ? ColumnValue::Text(std::move(s))
                                                  : ColumnValue::Bytes(std::move(s)));
            continue;
        }
        if (t == DataType::kFloat32) {
            float f = 0;
            std::memcpy(&f, rec + c.offset, 4);
            out.push_back(ColumnValue::Real(static_cast<double>(f)));
        } else if (t == DataType::kFloat64) {
            double d = 0;
            std::memcpy(&d, rec + c.offset, 8);
            out.push_back(ColumnValue::Real(d));
        } else if (t == DataType::kBool) {
            out.push_back(ColumnValue::Bool(LoadInteger(rec + c.offset, c.len) != 0));
        } else if (c.len > 8) {
            std::string s(reinterpret_cast<const char*>(rec + c.offset), c.len);
            out.push_back(ColumnValue::Bytes(std::move(s)));
        } else {
            out.push_back(ColumnValue::IntOf(t, LoadInteger(rec + c.offset, c.len)));
        }
    }
    return out;
}

Err TableStore::Insert(const std::vector<ColumnValue>& row, uint64_t* out_row_no) noexcept {
    if (alloc_ == nullptr) return Err::kReadOnlyHandle;
    if (row.size() != desc_->col_count) return Err::kInvalidArgument;
    char key[8];
    if (!EncodeKey(row[0], key)) return Err::kInvalidArgument;
    uint64_t existing = 0;
    if (index_->Find(key, &existing)) return Err::kAlreadyExists;

    const uint64_t row_no = desc_->row_count;
    if (row_no >= desc_->row_cap) return Err::kOutOfMemory;  // M1：行槽不复用

    std::vector<uint8_t> rec;
    std::vector<uint64_t> vars;
    const Err build = BuildRecord(row, &rec, &vars);
    if (!IsOk(build)) return build;

    // 写入协议：置奇数版本 → 写记录 → 置偶数版本 → 发布索引键。
    uint64_t* ver = VersionSlot(row_no);
    *ver = 1;
    std::memcpy(RowPtr(row_no), rec.data(), desc_->rec_fixed_len);
    SetTombstone(row_no, false);
    *ver = 2;

    const Err ins = index_->Insert(key, row_no);
    if (!IsOk(ins)) {
        for (uint64_t v : vars) var_pool_->Deallocate(v);
        std::memset(RowPtr(row_no), 0, desc_->rec_fixed_len);
        *ver = 0;
        return ins;
    }
    desc_->row_count++;
    desc_->committed_row_count++;
    if (out_row_no != nullptr) *out_row_no = row_no;
    return Err::kOk;
}

Result<std::vector<ColumnValue>> TableStore::Get(const ColumnValue& key) const noexcept {
    char k[8];
    if (!EncodeKey(key, k)) return Result<std::vector<ColumnValue>>::Fail(Err::kInvalidArgument);
    uint64_t row_no = 0;
    if (!index_->Find(k, &row_no)) return Result<std::vector<ColumnValue>>::Fail(Err::kNotFound);
    if (row_no >= desc_->row_cap) return Result<std::vector<ColumnValue>>::Fail(Err::kCorruption);

    // 免锁乐观读：版本一致且为偶数才采纳。
    std::vector<uint8_t> rec;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t* ver = VersionSlot(row_no);
        const uint64_t v1 = *ver;
        if ((v1 & 1u) != 0) continue;  // 写入中，重试
        if (IsTombstoned(row_no)) return Result<std::vector<ColumnValue>>::Fail(Err::kNotFound);
        rec.assign(RowPtr(row_no), RowPtr(row_no) + desc_->rec_fixed_len);
        std::vector<ColumnValue> values = DecodeRecord(rec.data());
        const uint64_t v2 = *ver;
        if (v1 == v2 && (v2 & 1u) == 0)
            return Result<std::vector<ColumnValue>>::Ok(std::move(values));
    }
    return Result<std::vector<ColumnValue>>::Fail(Err::kTimeout);
}

Err TableStore::Update(const ColumnValue& key, const std::vector<ColumnValue>& row) noexcept {
    if (alloc_ == nullptr) return Err::kReadOnlyHandle;
    if (row.size() != desc_->col_count) return Err::kInvalidArgument;
    char k[8];
    char new_k[8];
    if (!EncodeKey(key, k) || !EncodeKey(row[0], new_k)) return Err::kInvalidArgument;
    if (std::memcmp(k, new_k, 8) != 0) return Err::kInvalidArgument;  // 主键不可变更

    uint64_t row_no = 0;
    if (!index_->Find(k, &row_no)) return Err::kNotFound;
    if (row_no >= desc_->row_cap) return Err::kCorruption;
    if (IsTombstoned(row_no)) return Err::kNotFound;

    std::vector<uint8_t> rec;
    std::vector<uint64_t> vars;
    const Err build = BuildRecord(row, &rec, &vars);
    if (!IsOk(build)) return build;

    // 旧记录副本：用于版本发布后再回收其变长块（读者按版本复核丢弃半更新数据）。
    std::vector<uint8_t> old_rec(RowPtr(row_no), RowPtr(row_no) + desc_->rec_fixed_len);

    uint64_t* ver = VersionSlot(row_no);
    *ver = *ver + 1;  // 奇数：写入中
    std::memcpy(RowPtr(row_no), rec.data(), desc_->rec_fixed_len);
    *ver = *ver + 1;  // 偶数：稳定
    ReleaseRecordVars(old_rec.data());
    return Err::kOk;
}

Err TableStore::Remove(const ColumnValue& key) noexcept {
    if (alloc_ == nullptr) return Err::kReadOnlyHandle;
    char k[8];
    if (!EncodeKey(key, k)) return Err::kInvalidArgument;
    uint64_t row_no = 0;
    if (!index_->Find(k, &row_no)) return Err::kNotFound;
    if (row_no >= desc_->row_cap) return Err::kCorruption;
    if (IsTombstoned(row_no)) return Err::kNotFound;

    std::vector<uint8_t> old_rec(RowPtr(row_no), RowPtr(row_no) + desc_->rec_fixed_len);

    uint64_t* ver = VersionSlot(row_no);
    *ver = *ver + 1;
    SetTombstone(row_no, true);
    *ver = *ver + 1;

    const Err err = index_->Remove(k);
    if (!IsOk(err)) return err;
    if (desc_->committed_row_count > 0) desc_->committed_row_count--;
    ReleaseRecordVars(old_rec.data());
    return Err::kOk;
}

Result<bool> TableStore::Contains(const ColumnValue& key) const noexcept {
    char k[8];
    if (!EncodeKey(key, k)) return Result<bool>::Fail(Err::kInvalidArgument);
    uint64_t row_no = 0;
    if (!index_->Find(k, &row_no)) return Result<bool>::Ok(false);
    if (row_no >= desc_->row_cap) return Result<bool>::Fail(Err::kCorruption);
    return Result<bool>::Ok(!IsTombstoned(row_no));
}

Err TableStore::Validate(uint64_t* out_index_entries) const noexcept {
    if (out_index_entries != nullptr) *out_index_entries = 0;

    uint64_t visited = 0;
    const Err tree_err = index_->Validate(&visited);
    if (!IsOk(tree_err)) return tree_err;

    uint64_t tombstones = 0;
    for (uint64_t row_no = 0; row_no < desc_->row_count; ++row_no) {
        if (IsTombstoned(row_no)) tombstones++;
        if ((*VersionSlot(row_no) & 1u) != 0) return Err::kCorruption;  // 稳定态应为偶数
    }
    const uint64_t live = desc_->row_count - tombstones;
    if (desc_->committed_row_count != live) return Err::kCorruption;
    if (index_->Size() != live) return Err::kCorruption;
    if (out_index_entries != nullptr) *out_index_entries = visited;
    return Err::kOk;
}

}  // namespace storage
}  // namespace rtdb
