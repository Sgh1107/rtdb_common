#include "storage/alter.hpp"

#include <cstring>
#include <vector>

#include "infra/failpoint.hpp"

namespace rtdb {
namespace storage {

namespace {

/// 列名相等比较（定长缓冲，含结尾 0）。
bool SameColName(const char* a, const char* b) noexcept {
    return std::strncmp(a, b, kMaxColNameBytes) == 0;
}

}  // namespace

void EncodeExtDefault(const ColumnDef& col, uint64_t* out_raw8) noexcept {
    if (out_raw8 == nullptr) return;
    *out_raw8 = 0;
    if (!col.has_default || col.default_value.is_null) return;
    const DataType t = col.type;
    if (t == DataType::kFloat32) {
        const float f = static_cast<float>(col.default_value.real_value);
        std::memcpy(out_raw8, &f, sizeof(f));
    } else if (t == DataType::kFloat64) {
        const double d = col.default_value.real_value;
        std::memcpy(out_raw8, &d, sizeof(d));
    } else {
        const int64_t v = col.default_value.int_value;
        std::memcpy(out_raw8, &v, sizeof(v));
    }
}

Err ValidateExtColumn(const ColumnDef& col, const shm_TableDescriptor* desc,
                      void* base) noexcept {
    if (desc == nullptr) return Err::kInvalidArgument;
    if (col.name.empty() || col.name.size() >= kMaxColNameBytes) return Err::kInvalidArgument;

    const DataType t = col.type;
    const uint32_t fixed = FixedSizeOf(t);
    // M1 边界：仅定长 ≤8B 标量（变长列/宽字段扩展归 M2）。
    if (fixed == 0 || fixed > kMaxExtSlotBytes) return Err::kUnsupported;
    if (col.len != 0 && (col.len < fixed || col.len > kMaxExtSlotBytes)) return Err::kUnsupported;

    // 默认值约束：无默认值或默认 NULL 的列必须可空（存量行惰性读默认）。
    if (!col.has_default && !col.nullable) return Err::kInvalidArgument;
    if (col.has_default && col.default_value.is_null && !col.nullable)
        return Err::kInvalidArgument;

    // 列名唯一性：内联列 + 既有扩展列（顺带校验扩展链自洽）。
    for (uint32_t i = 0; i < desc->col_count; ++i) {
        if (SameColName(desc->cols[i].name, col.name.c_str())) return Err::kInvalidArgument;
    }
    std::vector<shm_ExtColRegion*> existing;
    const Err werr = WalkExtColumns(base, desc, &existing);
    if (!IsOk(werr)) return werr;
    for (const shm_ExtColRegion* r : existing) {
        if (SameColName(r->name, col.name.c_str())) return Err::kInvalidArgument;
    }

    if (desc->col_count + desc->ext_col_count >= kMaxInitCols + kMaxExtCols)
        return Err::kLimitExceeded;
    return Err::kOk;
}

Err AppendExtColumn(shm_TableDescriptor* desc, void* base, infra::SlabShmAllocator* alloc,
                    const ColumnDef& col) noexcept {
    if (desc == nullptr || base == nullptr || alloc == nullptr) return Err::kInvalidArgument;
    const Err verr = ValidateExtColumn(col, desc, base);
    if (!IsOk(verr)) return verr;

    const DataType t = col.type;
    const uint32_t slot_bytes = col.len != 0 ? col.len : FixedSizeOf(t);
    const uint64_t region_bytes = ExtColRegionBytes(desc->row_cap, slot_bytes);

    // P2-1 预检：区域须能作为单个 Slab 块分配（最大级容量 infra::kMaxAllocBytes，
    // 否则分配器以 kUnsupported 失败、归因含混）。此处提前以 kLimitExceeded
    // 拒绝（提示 row_cap 过大导致扩展列区域超过单块上限）。
    if (region_bytes > infra::kMaxAllocBytes) return Err::kLimitExceeded;

    const Result<uint64_t> r = alloc->Allocate(region_bytes);
    if (!r.IsOk()) return r.Error();
    const uint64_t off = r.Value();
    auto* region = reinterpret_cast<shm_ExtColRegion*>(static_cast<char*>(base) + off);
    std::memset(region, 0, static_cast<size_t>(region_bytes));

    region->magic = kExtColMagic;
    region->next_off = 0;  // 先不挂链：中断留孤儿区（不可达、无害）
    region->slot_bytes = slot_bytes;
    region->bitmap_bytes = static_cast<uint32_t>((static_cast<uint64_t>(desc->row_cap) + 7) / 8);
    region->row_cap = desc->row_cap;
    region->col = desc->col_count + desc->ext_col_count;
    region->layout_ver = desc->data_layout_ver + 1;
    region->type = static_cast<uint8_t>(t);
    region->nullable = col.nullable ? 1 : 0;
    region->has_default = col.has_default ? 1 : 0;
    region->default_is_null = (col.has_default && col.default_value.is_null) ? 1 : 0;
    EncodeExtDefault(col, &region->default_value);
    std::memcpy(region->name, col.name.c_str(), col.name.size() + 1);  // 含结尾 0

    // 注入点：区域已写好、尚未挂链（崩溃 → 表 schema 不变，docs/06 §3）。
    failpoint::CrashPoint("alter.before_link");

    // 挂链（头插）+ 计数递增 + 布局版本递增。
    region->next_off = desc->ext_col_off;
    desc->ext_col_off = off;
    desc->ext_col_count++;
    desc->data_layout_ver++;
    return Err::kOk;
}

Err WalkExtColumns(void* base, const shm_TableDescriptor* desc,
                   std::vector<shm_ExtColRegion*>* out_regions) noexcept {
    if (out_regions == nullptr) return Err::kInvalidArgument;
    out_regions->clear();
    if (desc == nullptr) return Err::kInvalidArgument;
    if (desc->ext_col_count == 0) {
        if (desc->ext_col_off != 0) return Err::kCorruption;  // 计数与链不自洽
        return Err::kOk;
    }
    if (base == nullptr) return Err::kInvalidArgument;
    if (desc->ext_col_off == 0) return Err::kCorruption;  // 有计数无链

    // 顺链收集（头 = 最新），深度以计数截断（更深节点为孤儿区）。
    std::vector<shm_ExtColRegion*> chain;
    chain.reserve(desc->ext_col_count);
    uint64_t off = desc->ext_col_off;
    for (uint32_t depth = 0; depth < desc->ext_col_count; ++depth) {
        auto* region = reinterpret_cast<shm_ExtColRegion*>(static_cast<char*>(base) + off);
        if (region->magic != kExtColMagic) return Err::kCorruption;
        if (region->row_cap != desc->row_cap) return Err::kCorruption;
        if (region->slot_bytes == 0 || region->slot_bytes > kMaxExtSlotBytes)
            return Err::kCorruption;
        const uint64_t expect_bitmap =
            (static_cast<uint64_t>(region->row_cap) + 7) / 8;
        if (region->bitmap_bytes != expect_bitmap) return Err::kCorruption;
        chain.push_back(region);
        off = region->next_off;
        if (off == 0) {
            if (depth + 1 != desc->ext_col_count) return Err::kCorruption;  // 链短于计数
            break;
        }
    }
    // 反转为添加序（最旧在前）；同时校验全局列序号连续。
    out_regions->assign(chain.rbegin(), chain.rend());
    for (uint32_t i = 0; i < out_regions->size(); ++i) {
        if ((*out_regions)[i]->col != desc->col_count + i) return Err::kCorruption;
    }
    return Err::kOk;
}

}  // namespace storage
}  // namespace rtdb
