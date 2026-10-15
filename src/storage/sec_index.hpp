#ifndef RTDB_SRC_STORAGE_SEC_INDEX_HPP_
#define RTDB_SRC_STORAGE_SEC_INDEX_HPP_

/// \file 二级索引（M1-W5-d，docs/03 §2.2/§3.1，docs/08 §3-W5）。
///
/// 语义（M1 框架）：
///  - 每索引一块 `shm_SecIdxMeta`（72B），经 `next_off` 链成倒序创建链，
///    表描述符 `reserved[0]` 指向链头、`reserved[1]` 计数（0 = 无二级索引）；
///  - 键布局：`[有序值 8B][有序 row_no 8B]`（16B 定长，memcmp 有序）；
///    同值条目按 row_no 升序连续，范围游标即可取同值多行；载荷 = row_no
///    （键内已含，取用便利）；
///  - 唯一索引：同布局 + 写路径（暂存阶段）前缀冲突校验 + 建索引时存量
///    数据校验；NULL 不入索引（SQL 语义，唯一性不约束 NULL）；
///  - 延迟重建：WAL 不记二级索引变更；写侧 Open 在重放收敛后对含索引的
///    活跃表全量重建自愈（崩溃/应用中断不留悬挂键，docs/08 §3-W5）；
///  - 变更必须在 `ShmRootMutex` 临界区内执行；读取（GetBySecondary）走
///    与主键扫描同款的近似视图语义（并发更新可能短暂缺席或按旧值命中）。

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "infra/shared_btree.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/err.hpp"
#include "rtdb/table_def.hpp"
#include "storage/schema.hpp"

namespace rtdb {
namespace storage {

/// 二级索引树：16B 键（8B 有序值 + 8B 有序 row_no），载荷 = row_no。
using SecIndexTree = infra::SharedBTree<16, uint64_t>;

/// 描述符 reserved[0]：二级索引链头偏移（0 = 无二级索引）。
inline uint64_t SecIdxHeadOff(const shm_TableDescriptor* desc) noexcept {
    return desc->reserved[0];
}

/// 描述符 reserved[1]：二级索引数（遍历深度；链上更深节点为孤儿区）。
inline uint64_t SecIdxCount(const shm_TableDescriptor* desc) noexcept {
    return desc->reserved[1];
}

/// \brief 列值 → 8B 有序索引键（整数类/浮点类/布尔/时间戳）。
///
/// 空值或不可有序编码（变长/NaN）返回 false——该行不出现在索引中
/// （NULL 语义；NaN 在写路径暂存阶段已被拒绝，此处为防御性兜底）。
bool EncodeSecValue(const ColumnValue& v, char out[8]) noexcept;

/// \brief 被索引列类型是否可有序编码（定长标量；变长列归 M2）。
bool SecEncodableType(DataType t) noexcept;

/// \brief 组装 16B 二级索引键（值前缀 + 有序 row_no 后缀）。
inline void MakeSecKey(const char value8[8], uint64_t row_no, char out[16]) noexcept {
    std::memcpy(out, value8, 8);
    infra::EncodeOrderedInt64(static_cast<int64_t>(row_no), out + 8);
}

/// \brief 校验二级索引定义（名称/列序号/类型可编码/重名/数量上限）。
/// \param idx 索引定义
/// \param desc 表描述符（当前 schema）
/// \param base 映像基址（遍历既有索引链与扩展链；ext_col_count>0 时须非空）
Err ValidateSecondaryIndexDef(const SecondaryIndexDef& idx, const shm_TableDescriptor* desc,
                              void* base) noexcept;

/// \brief 按创建序（最旧在前）遍历二级索引链。
///
/// 以 `reserved[1]` 计数为遍历深度（链上更深节点为崩溃残留孤儿区，
/// 不可达）；逐块校验 magic/树锚点/列序号/名称终止，违例返回 kCorruption；
/// 链短于计数同样判 kCorruption。
Err WalkSecondaryIndexes(void* base, const shm_TableDescriptor* desc,
                         std::vector<shm_SecIdxMeta*>* out_metas) noexcept;

/// \brief 按索引名定位元数据（不存在返回 nullptr）。
const shm_SecIdxMeta* FindSecMetaByName(void* base, const shm_TableDescriptor* desc,
                                        const std::string& name) noexcept;

/// \brief 前缀探测：索引中是否存在指定值的任何条目（唯一性校验用）。
bool SecPrefixExists(const SecIndexTree& tree, const char value8[8]) noexcept;

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_SEC_INDEX_HPP_
