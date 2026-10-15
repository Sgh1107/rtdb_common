#ifndef RTDB_SRC_STORAGE_ALTER_HPP_
#define RTDB_SRC_STORAGE_ALTER_HPP_

/// \file 在线加列（docs/02 §3 要点 3，docs/03 §3.1，docs/08 §2-D5，§3-W5）。
///
/// 语义（M1）：
///  - 列定义只追加不重排：新列进入 ext 扩展区（每列一块链式区域，
///    `shm_ExtColRegion`），主记录布局不变，旧 `TableHandle` 按绑定
///    时刻的列数继续读（`data_layout_ver` 递增仅作观测）；
///  - 存量行惰性读默认值（不物化）：未显式设置的行按区域头默认值
///    （或 NULL）读取；此后 Update/Insert 显式写槽；
///  - 仅支持定长 ≤8B 标量列（`kMaxExtSlotBytes`）；变长/宽字段扩展
///    列归 M2（kUnsupported）；
///  - 崩溃安全序：先写区域 → （注入点 alter.before_link）→ 挂链 →
///    `ext_col_count++` / `data_layout_ver++`。中断在挂链前留下不可达
///    孤儿区（无害，M2 compact 收敛），表 schema 不变。

#include <cstdint>
#include <vector>

#include "infra/slab_allocator.hpp"
#include "rtdb/err.hpp"
#include "rtdb/table_def.hpp"
#include "storage/schema.hpp"

namespace rtdb {
namespace storage {

/// \brief 校验扩展列定义（类型/宽度/可空性/默认值/列名唯一性/总数上限）。
/// \param col 待添加列定义
/// \param desc 表描述符（含既有列与扩展链）
/// \param base 映像基址（遍历既有扩展链做列名唯一性校验；ext_col_count>0 时须非空）
Err ValidateExtColumn(const ColumnDef& col, const shm_TableDescriptor* desc,
                      void* base) noexcept;

/// \brief 追加一个扩展列（分配区域 → 填充 → 挂链 → 计数/版本递增）。
///
/// 调用方须持根互斥锁且 desc 归属可写映像。失败不留半挂接状态：
/// 分配失败直接返回错误码；注入点前中断留孤儿区（不可达）。
///
/// \param desc 表描述符（原地更新 ext_col_off/ext_col_count/data_layout_ver）
/// \param base 映像基址
/// \param alloc 分配器（须非空）
/// \param col 列定义（经 ValidateExtColumn 校验）
Err AppendExtColumn(shm_TableDescriptor* desc, void* base, infra::SlabShmAllocator* alloc,
                    const ColumnDef& col) noexcept;

/// \brief 按添加序（最旧在前）遍历扩展列链。
///
/// 以 `desc->ext_col_count` 为遍历深度（链上更深节点为崩溃残留孤儿区，
/// 不可达）；逐区校验 magic/row_cap/slot_bytes/bitmap_bytes，违例返回
/// kCorruption。链长不足计数同样判 kCorruption。
Err WalkExtColumns(void* base, const shm_TableDescriptor* desc,
                   std::vector<shm_ExtColRegion*>* out_regions) noexcept;

/// \brief 扩展列槽值的类型编码（与内联定长列同一规则）。
void EncodeExtDefault(const ColumnDef& col, uint64_t* out_raw8) noexcept;

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_ALTER_HPP_
