#include "storage/sec_index.hpp"

#include <cstring>

#include "storage/alter.hpp"

namespace rtdb {
namespace storage {

bool SecEncodableType(DataType t) noexcept {
    switch (t) {
        case DataType::kInt8:
        case DataType::kInt16:
        case DataType::kInt32:
        case DataType::kInt64:
        case DataType::kUnixTsNs:
        case DataType::kBool:
        case DataType::kFloat32:
        case DataType::kFloat64:
            return true;
        default:
            return false;  // 变长/其他：M1 二级索引不支持
    }
}

bool EncodeSecValue(const ColumnValue& v, char out[8]) noexcept {
    if (v.is_null()) return false;
    switch (v.type()) {
        case DataType::kInt8:
        case DataType::kInt16:
        case DataType::kInt32:
        case DataType::kInt64:
        case DataType::kUnixTsNs:
            infra::EncodeOrderedInt64(v.as_int(), out);
            return true;
        case DataType::kBool:
            infra::EncodeOrderedInt64(v.as_int() != 0 ? 1 : 0, out);
            return true;
        case DataType::kFloat32:
        case DataType::kFloat64: {
            double d = v.as_real();
            if (d != d) return false;  // NaN 不可有序编码（写路径已拒，此处兜底）
            if (d == 0.0) d = 0.0;     // 归一化 -0.0（与 +0.0 排序等价）
            uint64_t u = 0;
            std::memcpy(&u, &d, sizeof(u));
            // IEEE754 有序变换：负数整体翻转、正数仅置符号位；大端存储后
            // memcmp 序即数值序（docs/03 §2.2）。
            if ((u & (1ull << 63)) != 0) {
                u = ~u;
            } else {
                u |= (1ull << 63);
            }
            for (int i = 7; i >= 0; --i) {
                out[i] = static_cast<char>(u & 0xFFu);
                u >>= 8;
            }
            return true;
        }
        default:
            return false;
    }
}

Err ValidateSecondaryIndexDef(const SecondaryIndexDef& idx, const shm_TableDescriptor* desc,
                              void* base) noexcept {
    if (desc == nullptr) return Err::kInvalidArgument;
    if (idx.name.empty() || idx.name.size() >= kMaxColNameBytes) return Err::kInvalidArgument;
    if (idx.name.find('\0') != std::string::npos) return Err::kInvalidArgument;

    // 列序号须落在当前 schema（内联列 + 扩展列）。
    const uint64_t total = static_cast<uint64_t>(desc->col_count) + desc->ext_col_count;
    if (idx.column_index >= total) return Err::kInvalidArgument;

    // 被索引列类型须可有序编码（定长标量；变长归 M2）。
    DataType t = DataType::kInt64;
    if (idx.column_index < desc->col_count) {
        t = static_cast<DataType>(desc->cols[idx.column_index].type);
    } else {
        std::vector<shm_ExtColRegion*> regions;
        const Err werr = WalkExtColumns(base, desc, &regions);
        if (!IsOk(werr)) return werr;
        t = static_cast<DataType>(regions[idx.column_index - desc->col_count]->type);
    }
    if (!SecEncodableType(t)) return Err::kUnsupported;

    // 重名冲突（顺带校验既有索引链自洽）。
    std::vector<shm_SecIdxMeta*> existing;
    const Err werr = WalkSecondaryIndexes(base, desc, &existing);
    if (!IsOk(werr)) return werr;
    for (const shm_SecIdxMeta* m : existing) {
        if (std::strncmp(m->name, idx.name.c_str(), kMaxColNameBytes) == 0)
            return Err::kAlreadyExists;
    }

    // 每表索引数上限（与 SuperBlock.max_indexes_per_table 一致）。
    if (SecIdxCount(desc) >= kMaxSecIdxPerTable) return Err::kLimitExceeded;
    return Err::kOk;
}

Err WalkSecondaryIndexes(void* base, const shm_TableDescriptor* desc,
                         std::vector<shm_SecIdxMeta*>* out_metas) noexcept {
    if (out_metas == nullptr) return Err::kInvalidArgument;
    out_metas->clear();
    if (desc == nullptr) return Err::kInvalidArgument;

    const uint64_t count = SecIdxCount(desc);
    if (count == 0) {
        if (SecIdxHeadOff(desc) != 0) return Err::kCorruption;  // 计数与链不自洽
        return Err::kOk;
    }
    if (base == nullptr) return Err::kInvalidArgument;
    if (SecIdxHeadOff(desc) == 0) return Err::kCorruption;  // 有计数无链
    const uint64_t total_cols = static_cast<uint64_t>(desc->col_count) + desc->ext_col_count;

    // 顺链收集（头 = 最新），深度以计数截断（更深节点为孤儿区）。
    std::vector<shm_SecIdxMeta*> chain;
    chain.reserve(static_cast<size_t>(count));
    uint64_t off = SecIdxHeadOff(desc);
    for (uint64_t depth = 0; depth < count; ++depth) {
        auto* meta = reinterpret_cast<shm_SecIdxMeta*>(static_cast<char*>(base) + off);
        if (meta->magic != kSecIdxMagic) return Err::kCorruption;
        if (meta->tree_slot_off == 0) return Err::kCorruption;
        if (meta->col >= total_cols) return Err::kCorruption;
        if (meta->name[kMaxColNameBytes - 1] != '\0') return Err::kCorruption;
        chain.push_back(meta);
        off = meta->next_off;
        if (off == 0) {
            if (depth + 1 != count) return Err::kCorruption;  // 链短于计数
            break;
        }
    }
    // 反转为创建序（最旧在前）。
    out_metas->assign(chain.rbegin(), chain.rend());
    return Err::kOk;
}

const shm_SecIdxMeta* FindSecMetaByName(void* base, const shm_TableDescriptor* desc,
                                        const std::string& name) noexcept {
    std::vector<shm_SecIdxMeta*> metas;
    if (!IsOk(WalkSecondaryIndexes(base, desc, &metas))) return nullptr;
    for (const shm_SecIdxMeta* m : metas) {
        if (name.size() < kMaxColNameBytes &&
            std::strncmp(m->name, name.c_str(), kMaxColNameBytes) == 0) {
            return m;
        }
    }
    return nullptr;
}

bool SecPrefixExists(const SecIndexTree& tree, const char value8[8]) noexcept {
    char lo[16];
    char hi[16];
    std::memcpy(lo, value8, 8);
    infra::EncodeOrderedInt64(INT64_MIN, lo + 8);
    std::memcpy(hi, value8, 8);
    infra::EncodeOrderedInt64(INT64_MAX, hi + 8);
    auto cur = tree.SeekRange(lo, true, hi, true);
    return cur.Valid();
}

}  // namespace storage
}  // namespace rtdb
