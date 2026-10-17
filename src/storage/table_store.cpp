#include "storage/table_store.hpp"

#include <cstring>
#include <utility>

#include "infra/failpoint.hpp"
#include "storage/alter.hpp"

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

/// 值类型（ColumnValue::type()）与 schema 列类型的可写入兼容矩阵（P1-4）：
///  - 整数族（含 bool/时间戳）内互写按值转换；
///  - 浮点仅入浮点列；
///  - 变长（utf8/bytes）仅入变长列。
/// 错配返回 false：调用方拒绝写入，杜绝 as_int()/as_bytes() 直读默认值
/// （Text 塞整数列 → 静默 0；整数塞变长列 → 静默空串）。
bool ValueTypeCompatible(DataType value_kind, DataType column) noexcept {
    switch (value_kind) {
        case DataType::kInt8:
        case DataType::kInt16:
        case DataType::kInt32:
        case DataType::kInt64:
        case DataType::kBool:
        case DataType::kUnixTsNs:
            return column == DataType::kInt8 || column == DataType::kInt16 ||
                   column == DataType::kInt32 || column == DataType::kInt64 ||
                   column == DataType::kBool || column == DataType::kUnixTsNs;
        case DataType::kFloat32:
        case DataType::kFloat64:
            return column == DataType::kFloat32 || column == DataType::kFloat64;
        case DataType::kUtf8Var:
        case DataType::kBytesVar:
            return column == DataType::kUtf8Var || column == DataType::kBytesVar;
        default:
            return false;
    }
}

/// 标量按类型+宽度写入（内联定长列与 ext 槽共用同一规则）。
void StoreScalar(uint8_t* dst, uint32_t width, DataType t, const ColumnValue& v) noexcept {
    if (t == DataType::kFloat32) {
        const float f = static_cast<float>(v.as_real());
        std::memcpy(dst, &f, 4);
    } else if (t == DataType::kFloat64) {
        const double d = v.as_real();
        std::memcpy(dst, &d, 8);
    } else {
        StoreInteger(dst, width, v.as_int());
    }
}

/// 标量按类型+宽度读出（ext 槽/default 共用）。
ColumnValue LoadScalar(const uint8_t* src, uint32_t width, DataType t) noexcept {
    if (t == DataType::kFloat32) {
        float f = 0;
        std::memcpy(&f, src, 4);
        return ColumnValue::Real(static_cast<double>(f));
    }
    if (t == DataType::kFloat64) {
        double d = 0;
        std::memcpy(&d, src, 8);
        return ColumnValue::Real(d);
    }
    if (t == DataType::kBool) return ColumnValue::Bool(LoadInteger(src, width) != 0);
    return ColumnValue::IntOf(t, LoadInteger(src, width));
}

/// ext 后缀每列的状态字节（槽值之前 1 字节）。
constexpr uint8_t kExtStatusValue = 1;  ///< 显式非空值
constexpr uint8_t kExtStatusNull = 2;   ///< 显式 NULL

/// 位图测试/设置（行号 → bit）。
bool BitTest(const uint8_t* bits, uint64_t row_no) noexcept {
    return ((bits[row_no / 8] >> (row_no % 8)) & 0x1u) != 0;
}
void BitSet(uint8_t* bits, uint64_t row_no, bool v) noexcept {
    if (v) {
        bits[row_no / 8] = static_cast<uint8_t>(bits[row_no / 8] | (1u << (row_no % 8)));
    } else {
        bits[row_no / 8] = static_cast<uint8_t>(bits[row_no / 8] & ~(1u << (row_no % 8)));
    }
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

    // 二级索引链（W5-d）：按创建序遍历元数据并逐个附着树实例
    // （只读角色无分配器，仅做非零校验；树可按槽只读使用）。
    std::vector<shm_SecIdxMeta*> sec_metas;
    const Err sec_walk = WalkSecondaryIndexes(base, desc, &sec_metas);
    if (!IsOk(sec_walk)) return Result<std::unique_ptr<TableStore>>::Fail(sec_walk);
    for (shm_SecIdxMeta* m : sec_metas) {
        if (alloc != nullptr && !alloc->OwnsOffset(m->tree_slot_off))
            return Result<std::unique_ptr<TableStore>>::Fail(Err::kCorruption);
        auto* sec_slot = reinterpret_cast<SecIndexTree::Slot*>(static_cast<char*>(base) +
                                                               m->tree_slot_off);
        store->sec_meta_.push_back(m);
        store->sec_trees_.emplace_back(new SecIndexTree(base, alloc, sec_slot));
    }
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

        // P1-4：值类型硬校验（此前按列型直读 as_int()/as_bytes()，错配
        // 静默取零值）。宽定长字段（len>8）按字节写入，要求变长类值。
        if (c.len > 8 && !is_var) {
            if (row[i].type() != DataType::kUtf8Var && row[i].type() != DataType::kBytesVar)
                return Err::kInvalidArgument;
        } else if (!ValueTypeCompatible(row[i].type(), t)) {
            return Err::kInvalidArgument;
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

    // 扩展列后缀（W5-b）：按添加序为调用方可见的每个扩展列追加
    // [status:1][slot_bytes 值]。句柄可见数 < 当前扩展数时，其后新增的
    // 列不出现于后缀——应用期不写其槽，读取走惰性默认值。
    if (row.size() > desc_->col_count) {
        std::vector<shm_ExtColRegion*> regions;
        const Err werr = WalkExtColumns(base_, desc_, &regions);
        if (!IsOk(werr)) return werr;
        const size_t provided = row.size() - desc_->col_count;
        if (provided > regions.size()) return Err::kInvalidArgument;  // 行宽超过当前 schema
        for (size_t j = 0; j < provided; ++j) {
            const shm_ExtColRegion* r = regions[j];
            const DataType et = static_cast<DataType>(r->type);
            const ColumnValue& v = row[desc_->col_count + j];
            if (v.is_null()) {
                if (r->nullable == 0) return Err::kInvalidArgument;  // NOT NULL 扩展列
                rec->push_back(kExtStatusNull);
                rec->resize(rec->size() + r->slot_bytes, 0);
                continue;
            }
            // P1-4：扩展列值类型同校验（StoreScalar 直读 as_real()/as_int()）。
            if (!ValueTypeCompatible(v.type(), et)) return Err::kInvalidArgument;
            rec->push_back(kExtStatusValue);
            const size_t at = rec->size();
            rec->resize(at + r->slot_bytes, 0);
            StoreScalar(rec->data() + at, r->slot_bytes, et, v);
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

// ------------------ 扩展列（W5-b）------------------

Err TableStore::ApplyExtSuffix(const uint8_t* suffix, size_t suffix_len,
                               uint64_t row_no) noexcept {
    if (suffix_len == 0) return Err::kOk;
    if (suffix == nullptr) return Err::kInvalidArgument;
    if (row_no >= desc_->row_cap) return Err::kCorruption;

    std::vector<shm_ExtColRegion*> regions;
    const Err werr = WalkExtColumns(base_, desc_, &regions);
    if (!IsOk(werr)) return werr;

    size_t pos = 0;
    for (size_t j = 0; j < regions.size() && pos < suffix_len; ++j) {
        shm_ExtColRegion* r = regions[j];
        const size_t need = 1 + r->slot_bytes;
        if (pos + need > suffix_len) return Err::kCorruption;  // 后缀撕裂
        const uint8_t status = suffix[pos];
        uint8_t* slot = ExtColSlots(r) + row_no * r->slot_bytes;
        if (status == kExtStatusValue) {
            std::memcpy(slot, suffix + pos + 1, r->slot_bytes);
            BitSet(ExtColSetBits(r), row_no, true);
            BitSet(ExtColNullBits(r), row_no, false);
        } else if (status == kExtStatusNull) {
            std::memset(slot, 0, r->slot_bytes);
            BitSet(ExtColSetBits(r), row_no, true);
            BitSet(ExtColNullBits(r), row_no, true);
        } else {
            return Err::kCorruption;  // 未知状态字节
        }
        pos += need;
    }
    return Err::kOk;
}

void TableStore::ClearExtBits(uint64_t row_no) const noexcept {
    if (row_no >= desc_->row_cap) return;
    std::vector<shm_ExtColRegion*> regions;
    if (!IsOk(WalkExtColumns(base_, desc_, &regions))) return;  // 链已损坏：无从清理
    for (shm_ExtColRegion* r : regions) {
        BitSet(ExtColSetBits(r), row_no, false);
        BitSet(ExtColNullBits(r), row_no, false);
        std::memset(ExtColSlots(r) + row_no * r->slot_bytes, 0, r->slot_bytes);
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

bool TableStore::IsCurrentRowVersion(uint64_t row_no, uint64_t row_ver) const noexcept {
    if (row_no >= desc_->row_cap) return false;
    const uint64_t* ver = VersionSlot(row_no);
    if (ver == nullptr || *ver != row_ver) return false;
    return !IsTombstoned(row_no);
}

std::vector<ColumnValue> TableStore::DecodeWalRow(const uint8_t* rec, uint32_t rec_len,
                                                  uint64_t row_no, uint64_t row_ver) const noexcept {
    if (rec == nullptr || rec_len < desc_->rec_fixed_len) return {};
    std::vector<ColumnValue> out = DecodeRecord(rec);
    // 变长列：池引用可能已被覆盖写回收（ReleaseRecordVars）——仅当前版本
    // 可安全按池引用解码，其余交付 NULL（防交付串行值，docs/13 §3-W2）。
    if (var_pool_ != nullptr && !IsCurrentRowVersion(row_no, row_ver)) {
        for (uint32_t i = 0; i < desc_->col_count && i < out.size(); ++i) {
            const DataType t = static_cast<DataType>(desc_->cols[i].type);
            if (t == DataType::kUtf8Var || t == DataType::kBytesVar)
                out[i] = ColumnValue::Null();
        }
    }
    const size_t suffix_len = rec_len - desc_->rec_fixed_len;
    if (suffix_len == 0) return out;

    std::vector<shm_ExtColRegion*> regions;
    if (!IsOk(WalkExtColumns(base_, desc_, &regions))) return out;  // 链损坏：交付内联前缀

    const uint8_t* suffix = rec + desc_->rec_fixed_len;
    size_t pos = 0;
    for (size_t j = 0; j < regions.size() && pos < suffix_len; ++j) {
        const shm_ExtColRegion* r = regions[j];
        const size_t need = 1 + static_cast<size_t>(r->slot_bytes);
        if (pos + need > suffix_len) break;  // 后缀撕裂：交付健全前缀
        const uint8_t status = suffix[pos];
        if (status == kExtStatusNull) {
            out.push_back(ColumnValue::Null());
        } else if (status == kExtStatusValue) {
            out.push_back(
                LoadScalar(suffix + pos + 1, r->slot_bytes, static_cast<DataType>(r->type)));
        } else {
            break;  // 未知状态：交付健全前缀
        }
        pos += need;
    }
    return out;
}

ColumnValue TableStore::DecodeWalKey(const uint8_t* key8) const noexcept {
    if (key8 == nullptr || desc_->col_count == 0) return ColumnValue::Null();
    const int64_t v = infra::DecodeOrderedInt64(reinterpret_cast<const char*>(key8));
    return ColumnValue::IntOf(static_cast<DataType>(desc_->cols[0].type), v);
}

// ------------------ WAL 先行协议：暂存（docs/08 §2-D2）------------------

Err TableStore::StageInsert(const std::vector<ColumnValue>& row, StagedRowOp* out) noexcept {
    if (alloc_ == nullptr) return Err::kReadOnlyHandle;
    if (out == nullptr) return Err::kInvalidArgument;
    // 行宽须落在 [内联列数, 内联+当前扩展数]：旧句柄可见列数少于当前
    // schema 时按前缀写入，其后扩展列走惰性默认值（W5-b）。
    if (row.size() < desc_->col_count ||
        row.size() > static_cast<size_t>(desc_->col_count) + desc_->ext_col_count)
        return Err::kInvalidArgument;
    if (!EncodeKey(row[0], out->key)) return Err::kInvalidArgument;
    uint64_t existing = 0;
    if (index_->Find(out->key, &existing)) return Err::kAlreadyExists;

    // 唯一二级索引冲突预检（W5-d，暂存阶段零副作用；批内两行同值互不
    // 可见——"批内操作互不依赖"的既定边界，docs/08 §2-D3）。
    const Err uerr = CheckSecUnique(row, 0, false);
    if (!IsOk(uerr)) return uerr;

    const uint64_t row_no = desc_->row_count;
    if (row_no >= desc_->row_cap) return Err::kOutOfMemory;  // M1：行槽不复用

    out->kind = StagedRowOp::Kind::kInsert;
    out->row_no = row_no;
    out->row_ver = 2;  // 首次发布：0 → 1(写入中) → 2(稳定)
    out->old_rec.clear();
    out->allocated_vars.clear();
    out->wal_written = false;
    out->published = false;
    const Err build = BuildRecord(row, &out->rec, &out->allocated_vars);
    if (!IsOk(build)) {
        out->rec.clear();
        out->allocated_vars.clear();
        return build;
    }
    desc_->row_count++;  // 行槽保留（应用失败由 ReleaseStaged 回退）
    return Err::kOk;
}

Err TableStore::StageUpdate(const ColumnValue& key, const std::vector<ColumnValue>& row,
                            StagedRowOp* out) noexcept {
    if (alloc_ == nullptr) return Err::kReadOnlyHandle;
    if (out == nullptr) return Err::kInvalidArgument;
    if (row.size() < desc_->col_count ||
        row.size() > static_cast<size_t>(desc_->col_count) + desc_->ext_col_count)
        return Err::kInvalidArgument;
    char k[8];
    if (!EncodeKey(key, k) || !EncodeKey(row[0], out->key)) return Err::kInvalidArgument;
    if (std::memcmp(k, out->key, 8) != 0) return Err::kInvalidArgument;  // 主键不可变更

    uint64_t row_no = 0;
    if (!index_->Find(k, &row_no)) return Err::kNotFound;
    if (row_no >= desc_->row_cap) return Err::kCorruption;
    if (IsTombstoned(row_no)) return Err::kNotFound;

    // 唯一二级索引冲突预检（W5-d）：排除本行当前条目（值未变时放行）。
    const Err uerr = CheckSecUnique(row, row_no, true);
    if (!IsOk(uerr)) return uerr;

    out->kind = StagedRowOp::Kind::kUpdate;
    out->row_no = row_no;
    out->row_ver = *VersionSlot(row_no) + 2;
    out->allocated_vars.clear();
    out->wal_written = false;
    out->published = false;
    const Err build = BuildRecord(row, &out->rec, &out->allocated_vars);
    if (!IsOk(build)) {
        out->rec.clear();
        out->allocated_vars.clear();
        return build;
    }
    // 旧记录副本：应用发布新版本后回收其变长块（读者按版本复核丢弃半更新数据）。
    out->old_rec.assign(RowPtr(row_no), RowPtr(row_no) + desc_->rec_fixed_len);
    return Err::kOk;
}

Err TableStore::StageRemove(const ColumnValue& key, StagedRowOp* out) noexcept {
    if (alloc_ == nullptr) return Err::kReadOnlyHandle;
    if (out == nullptr) return Err::kInvalidArgument;
    if (!EncodeKey(key, out->key)) return Err::kInvalidArgument;
    uint64_t row_no = 0;
    if (!index_->Find(out->key, &row_no)) return Err::kNotFound;
    if (row_no >= desc_->row_cap) return Err::kCorruption;
    if (IsTombstoned(row_no)) return Err::kNotFound;

    out->kind = StagedRowOp::Kind::kDelete;
    out->row_no = row_no;
    out->row_ver = *VersionSlot(row_no) + 2;
    out->rec.clear();
    out->allocated_vars.clear();
    out->wal_written = false;
    out->published = false;
    out->old_rec.assign(RowPtr(row_no), RowPtr(row_no) + desc_->rec_fixed_len);
    return Err::kOk;
}

// ------------------ WAL 先行协议：应用 ------------------

Err TableStore::ApplyStaged(StagedRowOp* st) noexcept {
    if (st == nullptr) return Err::kInvalidArgument;
    uint64_t* ver = VersionSlot(st->row_no);

    switch (st->kind) {
        case StagedRowOp::Kind::kInsert: {
            // 写入协议：置奇数版本 → 写记录 → 写扩展列槽 → 置偶数版本 →
            // 发布索引键（ext 先于版本翻转，崩溃恢复不出现半 ext 状态）。
            *ver = 1;
            std::memcpy(RowPtr(st->row_no), st->rec.data(), desc_->rec_fixed_len);
            if (st->rec.size() > desc_->rec_fixed_len) {
                const Err ee = ApplyExtSuffix(st->rec.data() + desc_->rec_fixed_len,
                                              st->rec.size() - desc_->rec_fixed_len, st->row_no);
                if (!IsOk(ee)) {
                    ClearExtBits(st->row_no);
                    std::memset(RowPtr(st->row_no), 0, desc_->rec_fixed_len);
                    *ver = 0;
                    return ee;
                }
            }
            SetTombstone(st->row_no, false);
            *ver = st->row_ver;
            // 注入点：行数据已发布、主键索引未发布（docs/06 §3 "索引更新一半"）。
            failpoint::CrashPoint("wal.apply.mid");
            if (failpoint::ErrorPoint("wal.index.insert")) {
                // 模拟索引分配失败：数据页回滚（行槽留待 ReleaseStaged 回退）；
                // 变长块因记录已入 WAL 由 ReleaseStaged 按 wal_written 保留
                //（重放会按记录内偏移重新物化行，回收即埋雷）。
                ClearExtBits(st->row_no);
                std::memset(RowPtr(st->row_no), 0, desc_->rec_fixed_len);
                *ver = 0;
                return Err::kOutOfMemory;
            }
            const Err ins = index_->Insert(st->key, st->row_no);
            if (!IsOk(ins)) {
                ClearExtBits(st->row_no);
                std::memset(RowPtr(st->row_no), 0, desc_->rec_fixed_len);
                *ver = 0;
                return ins;
            }
            // ---- 发布点已过（A2）：行数据 + 主键索引均已发布，不可回滚 ----
            // 此后任何失败上抛由调用方毒化（WAL 已先行，Open 重建自愈）；
            // 变长块引用已进入已发布行记录——先转移所有权（published + 清空
            // allocated_vars），ReleaseStaged 不得再回收，否则已发布行引用
            // 空闲块（P0-2），且重放幂等跳过无法修复。
            st->published = true;
            st->allocated_vars.clear();  // 变长块所有权转移至行数据
            // 二级索引维护（W5-d）：主键发布后按整行当前状态建键；键含
            // row_no，同值多行自然并存（非唯一索引的自然形态）。失败上抛
            // 由调用方毒化——WAL 已先行，Open 重建自愈。
            if (HaveSecIdx()) {
                // 注入点：发布后二级索引维护失败（A2 回归用例）。
                if (failpoint::ErrorPoint("wal.secidx.after_publish")) return Err::kOutOfMemory;
                std::vector<SecEntry> keys;
                const Err ke = SecKeysOfRow(st->row_no, &keys);
                if (!IsOk(ke)) return ke;
                const Err ie = InsertSecKeys(keys);
                if (!IsOk(ie)) return ie;
            }
            desc_->committed_row_count++;
            return Err::kOk;
        }
        case StagedRowOp::Kind::kUpdate: {
            // 旧二级索引键（W5-d）：写入前取当前行状态（ext 列同取）。
            std::vector<SecEntry> old_keys;
            if (HaveSecIdx()) {
                const Err ke = SecKeysOfRow(st->row_no, &old_keys);
                if (!IsOk(ke)) return ke;
            }
            *ver = st->row_ver - 1;  // 奇数：写入中
            std::memcpy(RowPtr(st->row_no), st->rec.data(), desc_->rec_fixed_len);
            if (st->rec.size() > desc_->rec_fixed_len) {
                const Err ee = ApplyExtSuffix(st->rec.data() + desc_->rec_fixed_len,
                                              st->rec.size() - desc_->rec_fixed_len, st->row_no);
                if (!IsOk(ee)) return ee;  // 版本仍为奇数：读者重试；调用方毒化
            }
            *ver = st->row_ver;  // 偶数：稳定
            // ---- 发布点已过（A2）：新行数据不可回滚 ----
            // 此后（二级索引维护）失败上抛，变长块所有权必须已转移：
            // ReleaseStaged 依据 published 拒绝回收（P0-2）。
            st->published = true;
            if (HaveSecIdx()) {
                // 新键按写入后的当前行状态取（旧句柄窄行写入时未提供的
                // ext 列保留旧值，天然正确）；先摘旧键再插新键——并发
                // 读者至多短暂缺席（近似视图），不会重复命中旧值+新值。
                std::vector<SecEntry> new_keys;
                const Err ke = SecKeysOfRow(st->row_no, &new_keys);
                if (!IsOk(ke)) return ke;
                RemoveSecKeys(old_keys);
                const Err ie = InsertSecKeys(new_keys);
                if (!IsOk(ie)) return ie;  // 调用方毒化；Open 重建自愈
            }
            ReleaseRecordVars(st->old_rec.data());
            st->allocated_vars.clear();  // 变长块所有权转移至新行数据（P1-1：
                                         // 批回滚"已应用项已清空"的前提对
                                         // update 同样成立）
            return Err::kOk;
        }
        case StagedRowOp::Kind::kDelete: {
            // 旧二级索引键（W5-d）：删除前取当前行状态（ext 列同取）。
            std::vector<SecEntry> old_keys;
            if (HaveSecIdx()) {
                const Err ke = SecKeysOfRow(st->row_no, &old_keys);
                if (!IsOk(ke)) return ke;
            }
            *ver = st->row_ver - 1;
            SetTombstone(st->row_no, true);
            *ver = st->row_ver;
            st->published = true;  // 墓碑已发布（删除无变长块，标志为语义一致性）
            const Err err = index_->Remove(st->key);
            if (!IsOk(err)) return err;  // 调用方毒化引擎（WAL 已先行）
            // 摘除二级索引键：读者按行号读行，墓碑即缺席（先于/后于主键
            // 删除均可，不影响查询正确性）。
            RemoveSecKeys(old_keys);
            if (desc_->committed_row_count > 0) desc_->committed_row_count--;
            ReleaseRecordVars(st->old_rec.data());
            return Err::kOk;
        }
        default:
            return Err::kInternal;
    }
}

void TableStore::ReleaseStaged(StagedRowOp* st) noexcept {
    if (st == nullptr) return;
    // A2 所有权不变式：变长块在以下两种情形下不可由本进程回收——
    //  ① wal_written：记录已入环，重放会按记录内偏移重新物化行；回收则
    //     重放把已空闲块写进行记录，随后被再分配 = 静默损坏（泄漏有界，
    //     随 compact 收敛，与 ReplayUpsert 对旧块的处理同策略）；
    //  ② published：行数据已发布且不可回滚，块仍被已发布行引用。
    if (!st->published && !st->wal_written) {
        for (uint64_t off : st->allocated_vars) {
            if (var_pool_ != nullptr) var_pool_->Deallocate(off);
        }
    }
    st->allocated_vars.clear();
    // insert 行槽保留回退：仅当该槽未被已发布行接管（!published）且是最后
    // 一次保留（LIFO）时严格成立；否则槽位作为"保留未用"遗留（不可达、
    // 无害），随 M2 compact 收敛。published 情形若回退行槽，后续插入会
    // 复用已发布行槽 → 键/数据错位（P0-2 的另一半）。
    if (!st->published && st->kind == StagedRowOp::Kind::kInsert && desc_->row_count > 0 &&
        st->row_no + 1 == desc_->row_count) {
        desc_->row_count--;
    }
    st->rec.clear();
    st->old_rec.clear();
}

// ------------------ 恢复重放（docs/08 §3-W4）------------------

Err TableStore::ReplayUpsert(uint64_t row_no, uint64_t row_ver, const uint8_t* rec,
                             uint32_t rec_len) noexcept {
    if (alloc_ == nullptr) return Err::kCorruption;  // 只读角色不参与重放
    if (rec == nullptr || rec_len < desc_->rec_fixed_len) return Err::kCorruption;
    if (row_no >= desc_->row_cap) return Err::kCorruption;

    // 索引键必须与写路径同构：记录内主键列按自然编码存储，而索引键为
    // EncodeOrderedInt64 有序编码（两者字节序不同）——先解码主键列再
    // EncodeKey，复刻 StageInsert 的键生成（docs/08 §3-W4 修复）。
    const std::vector<ColumnValue> row = DecodeRecord(rec);
    if (row.empty()) return Err::kCorruption;
    char key_buf[8];
    if (!EncodeKey(row[0], key_buf)) return Err::kCorruption;
    const uint8_t* key = reinterpret_cast<const uint8_t*>(key_buf);
    uint64_t existing = 0;
    const bool found = index_->Find(key, &existing);
    if (found) {
        if (existing == row_no) return Err::kOk;  // 已发布：幂等跳过
        return Err::kCorruption;                  // 键被其他槽持有：结构违例
    }

    uint64_t* ver = VersionSlot(row_no);
    if (*ver < row_ver) {
        // 数据未应用（或写入中断）：整行重写并清墓碑。
        std::memcpy(RowPtr(row_no), rec, desc_->rec_fixed_len);
        SetTombstone(row_no, false);
        // 扩展列后缀（W5-b）：与主记录同批重放；记录早于加列时无后缀，
        // 链短于后缀时余部忽略（加列先于记录落盘的崩溃残差，M1 边界）。
        if (rec_len > desc_->rec_fixed_len) {
            const Err ee = ApplyExtSuffix(rec + desc_->rec_fixed_len,
                                          rec_len - desc_->rec_fixed_len, row_no);
            if (!IsOk(ee)) return ee;
        }
        *ver = row_ver;
        // 旧记录变长块不回收：撕裂内容上执行 Deallocate 有池破坏风险；
        // 泄漏有界，随 M2 compact 收敛（docs/08 §3-W4 落地记录）。
    }
    // 版本已到位但索引缺失（应用中断于发布前）→ 仅补发布索引。
    const Err ins = index_->Insert(key, row_no);
    if (!IsOk(ins)) return ins;
    if (row_no + 1 > desc_->row_count) desc_->row_count = static_cast<uint32_t>(row_no + 1);
    return Err::kOk;
}

Err TableStore::ReplayDelete(uint64_t row_no, uint64_t row_ver, const uint8_t* key8) noexcept {
    if (alloc_ == nullptr) return Err::kCorruption;
    if (key8 == nullptr) return Err::kCorruption;
    if (row_no >= desc_->row_cap) return Err::kCorruption;

    uint64_t existing = 0;
    const bool have = index_->Find(key8, &existing);
    if (have && existing != row_no) return Err::kCorruption;

    uint64_t* ver = VersionSlot(row_no);
    if (*ver < row_ver) {
        SetTombstone(row_no, true);
        *ver = row_ver;
    }
    if (have) {
        const Err err = index_->Remove(key8);
        if (!IsOk(err)) return err;
    }
    return Err::kOk;
}

Err TableStore::RebuildCommittedCountAfterReplay() noexcept {
    desc_->committed_row_count = static_cast<uint32_t>(index_->Size());
    return Err::kOk;
}

// ------------------ 二级索引（M1-W5-d，docs/08 §3-W5）------------------

Err TableStore::CreateSecondaryIndex(const SecondaryIndexDef& idx) noexcept {
    if (alloc_ == nullptr) return Err::kReadOnlyHandle;
    const Err verr = ValidateSecondaryIndexDef(idx, desc_, base_);
    if (!IsOk(verr)) return verr;

    // 分配 Slot 锚点与元数据块（先完整写入，再挂链；失败完整回滚）。
    Result<uint64_t> slot_res = alloc_->Allocate(static_cast<uint64_t>(sizeof(SecIndexTree::Slot)));
    if (!slot_res.IsOk()) return slot_res.Error();
    const uint64_t slot_off = slot_res.Value();
    std::memset(alloc_->OffsetToPtr(slot_off), 0, sizeof(SecIndexTree::Slot));

    Result<uint64_t> meta_res = alloc_->Allocate(static_cast<uint64_t>(sizeof(shm_SecIdxMeta)));
    if (!meta_res.IsOk()) {
        alloc_->Deallocate(slot_off);
        return meta_res.Error();
    }
    const uint64_t meta_off = meta_res.Value();
    auto* meta = reinterpret_cast<shm_SecIdxMeta*>(alloc_->OffsetToPtr(meta_off));
    std::memset(meta, 0, sizeof(*meta));
    meta->magic = kSecIdxMagic;
    meta->next_off = 0;  // 先不挂链：中断留孤儿块（不可达、无害）
    meta->tree_slot_off = slot_off;
    meta->col = idx.column_index;
    meta->unique = idx.unique ? 1 : 0;
    meta->state = 0;
    std::memcpy(meta->name, idx.name.c_str(), idx.name.size() + 1);  // 含结尾 0

    // 注入点：元数据已写、尚未挂链（崩溃 → 表无此索引，docs/06 §3）。
    failpoint::CrashPoint("secidx.before_link");

    // populate 先于 link：读者要么看不到索引，要么看到全量（无半截视图）。
    // 唯一索引在此校验存量数据；违例回滚分配、表 schema 不变。
    SecIndexTree tree(base_, alloc_,
                      reinterpret_cast<SecIndexTree::Slot*>(alloc_->OffsetToPtr(slot_off)));
    const Err perr = PopulateSecondaryIndex(meta, &tree);
    if (!IsOk(perr)) {
        // 已灌入的树节点不回收（页回收归 M2 compact），仅归还锚点/元数据块。
        alloc_->Deallocate(meta_off);
        alloc_->Deallocate(slot_off);
        return perr;
    }

    // 挂链（头插）+ 计数递增 + 附着到本实例（此后本会话立即可查）。
    meta->next_off = SecIdxHeadOff(desc_);
    desc_->reserved[0] = meta_off;
    desc_->reserved[1] = SecIdxCount(desc_) + 1;
    sec_meta_.push_back(meta);
    sec_trees_.emplace_back(new SecIndexTree(
        base_, alloc_, reinterpret_cast<SecIndexTree::Slot*>(alloc_->OffsetToPtr(slot_off))));
    return Err::kOk;
}

Err TableStore::PopulateSecondaryIndex(shm_SecIdxMeta* meta, SecIndexTree* tree) noexcept {
    if (meta == nullptr || tree == nullptr) return Err::kInvalidArgument;
    // 主键序遍历（升序）；逐行取被索引列当前值建键。
    char lo8[8];
    char hi8[8];
    infra::EncodeOrderedInt64(INT64_MIN, lo8);
    infra::EncodeOrderedInt64(INT64_MAX, hi8);
    auto cur = index_->SeekRange(lo8, true, hi8, true);
    while (cur.Valid()) {
        const uint64_t row_no = cur.Value();
        // 墓碑行不在主键索引中（删除即摘键），此处为防御性双查；
        // 奇数版本行（崩溃残留的写入中态）跳过，由后续重写收敛。
        if (row_no < desc_->row_cap && !IsTombstoned(row_no) && (*VersionSlot(row_no) & 1u) == 0) {
            ColumnValue v = ColumnValue::Null();
            const Err ce = ColumnValueAt(meta->col, row_no, &v);
            if (!IsOk(ce)) return ce;
            char vk[8];
            if (EncodeSecValue(v, vk)) {
                if (meta->unique != 0 && SecPrefixExists(*tree, vk))
                    return Err::kAlreadyExists;  // 存量数据违反唯一约束
                SecEntry e;
                MakeSecKey(vk, row_no, e.key.data());
                e.present = true;
                const Err ie = tree->Insert(e.key.data(), row_no);
                if (!IsOk(ie)) return ie;
            }
        }
        cur.Next();
    }
    return Err::kOk;
}

Err TableStore::ColumnValueAt(uint32_t global_col, uint64_t row_no, ColumnValue* out) const noexcept {
    if (out == nullptr) return Err::kInvalidArgument;
    if (row_no >= desc_->row_cap) return Err::kCorruption;

    if (global_col < desc_->col_count) {
        // 内联列：直读主记录（调用方均持根锁或在写入协议内，无并发写）。
        const uint8_t* rec = RowPtr(row_no);
        if (((rec[nullmap_offset_ + global_col / 8] >> (global_col % 8)) & 0x1u) != 0) {
            *out = ColumnValue::Null();
            return Err::kOk;
        }
        const shm_ColDef& c = desc_->cols[global_col];
        const DataType t = static_cast<DataType>(c.type);
        if (t == DataType::kFloat32) {
            float f = 0;
            std::memcpy(&f, rec + c.offset, 4);
            *out = ColumnValue::Real(static_cast<double>(f));
        } else if (t == DataType::kFloat64) {
            double d = 0;
            std::memcpy(&d, rec + c.offset, 8);
            *out = ColumnValue::Real(d);
        } else if (t == DataType::kBool) {
            *out = ColumnValue::Bool(LoadInteger(rec + c.offset, c.len) != 0);
        } else if (FixedSizeOf(t) > 0) {
            *out = ColumnValue::IntOf(t, LoadInteger(rec + c.offset, c.len));
        } else {
            return Err::kUnsupported;  // 变长列：校验期已拒，此处兜底
        }
        return Err::kOk;
    }

    // 扩展列：槽值 / set / null 位（与 ReadRow 的 ext 解码同一规则）。
    const uint32_t j = global_col - desc_->col_count;
    std::vector<shm_ExtColRegion*> regions;
    const Err werr = WalkExtColumns(base_, desc_, &regions);
    if (!IsOk(werr)) return werr;
    if (j >= regions.size()) return Err::kCorruption;
    shm_ExtColRegion* r = regions[j];
    const DataType t = static_cast<DataType>(r->type);
    if (!BitTest(ExtColSetBits(r), row_no)) {
        if (r->has_default != 0 && r->default_is_null == 0) {
            *out = LoadScalar(reinterpret_cast<const uint8_t*>(&r->default_value), r->slot_bytes, t);
        } else {
            *out = ColumnValue::Null();
        }
        return Err::kOk;
    }
    if (BitTest(ExtColNullBits(r), row_no)) {
        *out = ColumnValue::Null();
        return Err::kOk;
    }
    *out = LoadScalar(ExtColSlots(r) + row_no * r->slot_bytes, r->slot_bytes, t);
    return Err::kOk;
}

Err TableStore::SecKeysOfRow(uint64_t row_no, std::vector<SecEntry>* out) const noexcept {
    if (out == nullptr) return Err::kInvalidArgument;
    out->clear();
    out->resize(sec_meta_.size());  // 与 sec_trees_ 平行对齐
    for (size_t i = 0; i < sec_meta_.size(); ++i) {
        ColumnValue v = ColumnValue::Null();
        const Err ce = ColumnValueAt(sec_meta_[i]->col, row_no, &v);
        if (!IsOk(ce)) return ce;
        char vk[8];
        if (!EncodeSecValue(v, vk)) continue;  // NULL/不可编码：该行不入此索引
        MakeSecKey(vk, row_no, (*out)[i].key.data());
        (*out)[i].present = true;
    }
    return Err::kOk;
}

Err TableStore::InsertSecKeys(const std::vector<SecEntry>& keys) noexcept {
    for (size_t i = 0; i < keys.size() && i < sec_trees_.size(); ++i) {
        if (!keys[i].present) continue;
        const uint64_t row_no =
            static_cast<uint64_t>(infra::DecodeOrderedInt64(keys[i].key.data() + 8));
        const Err ie = sec_trees_[i]->Insert(keys[i].key.data(), row_no);
        // 同键同行号已存在 = 状态本就正确（幂等）；其余错误上抛。
        if (!IsOk(ie) && ie != Err::kAlreadyExists) return ie;
    }
    return Err::kOk;
}

void TableStore::RemoveSecKeys(const std::vector<SecEntry>& keys) noexcept {
    for (size_t i = 0; i < keys.size() && i < sec_trees_.size(); ++i) {
        if (!keys[i].present) continue;
        // 键缺失不影响结构（读者按行号读行，墓碑/旧值即缺席）。
        (void)sec_trees_[i]->Remove(keys[i].key.data());
    }
}

Err TableStore::CheckSecUnique(const std::vector<ColumnValue>& row, uint64_t self_row_no,
                               bool has_self) const noexcept {
    for (size_t i = 0; i < sec_meta_.size(); ++i) {
        if (sec_meta_[i]->unique == 0) continue;
        const uint32_t col = sec_meta_[i]->col;
        // 写入行未提供该列（旧句柄窄行）：惰性默认值不参与唯一性校验
        // （M1 边界，docs/08 §3-W5）。
        if (col >= row.size()) continue;
        const ColumnValue& v = row[col];
        if (v.is_null()) continue;  // NULL 不入唯一索引（SQL 语义）
        if ((v.type() == DataType::kFloat32 || v.type() == DataType::kFloat64) &&
            v.as_real() != v.as_real()) {
            return Err::kInvalidArgument;  // NaN 不可有序编码：拒绝写入
        }
        char vk[8];
        if (!EncodeSecValue(v, vk)) continue;
        // 前缀探测：同值存在其他行 → 冲突（更新场景排除自身当前条目）。
        char lo[16];
        char hi[16];
        std::memcpy(lo, vk, 8);
        infra::EncodeOrderedInt64(INT64_MIN, lo + 8);
        std::memcpy(hi, vk, 8);
        infra::EncodeOrderedInt64(INT64_MAX, hi + 8);
        auto cur = sec_trees_[i]->SeekRange(lo, true, hi, true);
        while (cur.Valid()) {
            if (!has_self || cur.Value() != self_row_no) return Err::kAlreadyExists;
            cur.Next();
        }
    }
    return Err::kOk;
}

Result<std::vector<std::vector<ColumnValue>>> TableStore::GetBySecondary(
    const std::string& index_name, const ColumnValue& key) const noexcept {
    const shm_SecIdxMeta* meta = FindSecMetaByName(base_, desc_, index_name);
    if (meta == nullptr)
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(Err::kNotFound);
    char vk[8];
    if (!EncodeSecValue(key, vk))
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(Err::kInvalidArgument);

    const SecIndexTree* tree = nullptr;
    for (size_t i = 0; i < sec_meta_.size(); ++i) {
        if (sec_meta_[i] == meta) {
            tree = sec_trees_[i].get();
            break;
        }
    }
    if (tree == nullptr)
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(Err::kCorruption);

    // 同值区间 [值|MIN, 值|MAX]：同值多行按 row_no 升序连续。
    char lo[16];
    char hi[16];
    std::memcpy(lo, vk, 8);
    infra::EncodeOrderedInt64(INT64_MIN, lo + 8);
    std::memcpy(hi, vk, 8);
    infra::EncodeOrderedInt64(INT64_MAX, hi + 8);

    std::vector<std::vector<ColumnValue>> out;
    auto cur = tree->SeekRange(lo, true, hi, true);
    while (cur.Valid()) {
        std::vector<ColumnValue> row;
        const Err e = ReadRow(cur.Value(), &row);
        if (IsOk(e)) {
            out.push_back(std::move(row));
        } else if (e != Err::kNotFound && e != Err::kTimeout) {
            return Result<std::vector<std::vector<ColumnValue>>>::Fail(e);  // 结构错误上抛
        }
        // kNotFound/kTimeout：并发删除/更新中的行，缺席于本次近似查询（M1）。
        cur.Next();
    }
    return Result<std::vector<std::vector<ColumnValue>>>::Ok(std::move(out));
}

Err TableStore::RebuildSecondaryIndexes() noexcept {
    if (alloc_ == nullptr) return Err::kReadOnlyHandle;
    for (size_t i = 0; i < sec_meta_.size(); ++i) {
        SecIndexTree* tree = sec_trees_[i].get();
        // 就地清空（根置零）：各进程缓存的树实例按同一 Slot 绑定，不可
        // 更换锚点指针；旧节点不可达，待 M2 compact 回收。
        tree->Reset();
        const Err e = PopulateSecondaryIndex(sec_meta_[i], tree);
        if (!IsOk(e)) return e;
    }
    return Err::kOk;
}

// ------------------ 兼容封装：单步 CRUD（不经 WAL，直测/工具用）------------------

Err TableStore::Insert(const std::vector<ColumnValue>& row, uint64_t* out_row_no) noexcept {
    StagedRowOp st;
    const Err stage = StageInsert(row, &st);
    if (!IsOk(stage)) return stage;
    const Err apply = ApplyStaged(&st);
    if (!IsOk(apply)) {
        ReleaseStaged(&st);
        return apply;
    }
    if (out_row_no != nullptr) *out_row_no = st.row_no;
    return Err::kOk;
}

/// 免锁乐观读一行（主记录与 ext 槽在同一版本窗口内拷贝；W3 读协议，
/// W5-b 扩展列同窗口、W5-c 扫描复用）。
Err TableStore::ReadRow(uint64_t row_no, std::vector<ColumnValue>* out) const noexcept {
    if (out == nullptr) return Err::kInvalidArgument;
    if (row_no >= desc_->row_cap) return Err::kCorruption;

    // 扩展列链快照（添加序；无扩展列时零开销）。
    std::vector<shm_ExtColRegion*> regions;
    if (desc_->ext_col_count != 0) {
        const Err werr = WalkExtColumns(base_, desc_, &regions);
        if (!IsOk(werr)) return werr;
    }
    struct ExtCell {
        uint8_t set = 0;
        uint8_t is_null = 0;
        std::vector<uint8_t> value;
    };
    std::vector<ExtCell> cells(regions.size());
    for (size_t j = 0; j < regions.size(); ++j) cells[j].value.resize(regions[j]->slot_bytes);

    std::vector<uint8_t> rec;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t* ver = VersionSlot(row_no);
        const uint64_t v1 = *ver;
        if ((v1 & 1u) != 0) continue;  // 写入中，重试
        if (IsTombstoned(row_no)) return Err::kNotFound;
        rec.assign(RowPtr(row_no), RowPtr(row_no) + desc_->rec_fixed_len);
        for (size_t j = 0; j < regions.size(); ++j) {
            shm_ExtColRegion* r = regions[j];
            const uint8_t* slot = ExtColSlots(r) + row_no * r->slot_bytes;
            std::memcpy(cells[j].value.data(), slot, r->slot_bytes);
            cells[j].set = BitTest(ExtColSetBits(r), row_no) ? 1 : 0;
            cells[j].is_null = BitTest(ExtColNullBits(r), row_no) ? 1 : 0;
        }
        std::vector<ColumnValue> values = DecodeRecord(rec.data());
        const uint64_t v2 = *ver;
        if (v1 == v2 && (v2 & 1u) == 0) {
            // 扩展列解码（W5-b）：未设置 → 惰性默认值；设置且 null 位 → NULL。
            for (size_t j = 0; j < regions.size(); ++j) {
                const shm_ExtColRegion* r = regions[j];
                const DataType et = static_cast<DataType>(r->type);
                const ExtCell& c = cells[j];
                if (c.set == 0) {
                    if (r->has_default != 0 && r->default_is_null == 0) {
                        values.push_back(LoadScalar(
                            reinterpret_cast<const uint8_t*>(&r->default_value), r->slot_bytes, et));
                    } else {
                        values.push_back(ColumnValue::Null());
                    }
                } else if (c.is_null != 0) {
                    values.push_back(ColumnValue::Null());
                } else {
                    values.push_back(LoadScalar(c.value.data(), r->slot_bytes, et));
                }
            }
            *out = std::move(values);
            return Err::kOk;
        }
    }
    return Err::kTimeout;  // 并发更新中：调用方按语义跳过或上报
}

Result<std::vector<ColumnValue>> TableStore::Get(const ColumnValue& key) const noexcept {
    char k[8];
    if (!EncodeKey(key, k)) return Result<std::vector<ColumnValue>>::Fail(Err::kInvalidArgument);
    uint64_t row_no = 0;
    if (!index_->Find(k, &row_no)) return Result<std::vector<ColumnValue>>::Fail(Err::kNotFound);
    std::vector<ColumnValue> values;
    const Err e = ReadRow(row_no, &values);
    if (!IsOk(e)) return Result<std::vector<ColumnValue>>::Fail(e);
    return Result<std::vector<ColumnValue>>::Ok(std::move(values));
}

Result<std::vector<std::vector<ColumnValue>>> TableStore::Scan(const ColumnValue* lo, bool lo_incl,
                                                               const ColumnValue* hi,
                                                               bool hi_incl) const noexcept {
    // 边界编码：nullptr = 无界 → 有序编码极值（包含性强制为闭）。
    char lo8[8];
    char hi8[8];
    if (lo != nullptr) {
        if (!EncodeKey(*lo, lo8)) return Result<std::vector<std::vector<ColumnValue>>>::Fail(
                                           Err::kInvalidArgument);
    } else {
        infra::EncodeOrderedInt64(INT64_MIN, lo8);
        lo_incl = true;
    }
    if (hi != nullptr) {
        if (!EncodeKey(*hi, hi8)) return Result<std::vector<std::vector<ColumnValue>>>::Fail(
                                           Err::kInvalidArgument);
    } else {
        infra::EncodeOrderedInt64(INT64_MAX, hi8);
        hi_incl = true;
    }

    std::vector<std::vector<ColumnValue>> out;
    auto cur = index_->SeekRange(lo8, lo_incl, hi8, hi_incl);
    while (cur.Valid()) {
        const uint64_t row_no = cur.Value();
        std::vector<ColumnValue> row;
        const Err e = ReadRow(row_no, &row);
        if (IsOk(e)) {
            out.push_back(std::move(row));
        } else if (e != Err::kNotFound && e != Err::kTimeout) {
            return Result<std::vector<std::vector<ColumnValue>>>::Fail(e);  // 结构错误上抛
        }
        // kNotFound/kTimeout：并发删除/更新中的行，缺席于本次近似扫描（M1）。
        cur.Next();
    }
    return Result<std::vector<std::vector<ColumnValue>>>::Ok(std::move(out));
}

Err TableStore::Update(const ColumnValue& key, const std::vector<ColumnValue>& row) noexcept {
    StagedRowOp st;
    const Err stage = StageUpdate(key, row, &st);
    if (!IsOk(stage)) return stage;
    const Err apply = ApplyStaged(&st);
    if (!IsOk(apply)) {
        ReleaseStaged(&st);
        return apply;
    }
    return Err::kOk;
}

Err TableStore::Remove(const ColumnValue& key) noexcept {
    StagedRowOp st;
    const Err stage = StageRemove(key, &st);
    if (!IsOk(stage)) return stage;
    const Err apply = ApplyStaged(&st);
    if (!IsOk(apply)) {
        ReleaseStaged(&st);
        return apply;
    }
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
