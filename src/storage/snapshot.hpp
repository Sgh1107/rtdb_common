#ifndef RTDB_SRC_STORAGE_SNAPSHOT_HPP_
#define RTDB_SRC_STORAGE_SNAPSHOT_HPP_

/// \file 冷快照（docs/03 §3.2/§3.3，docs/08 §3-W5）。
///
/// M1 语义：**停写（根锁）→ 全映像 flush → checkpoint_seq 推进到
/// committed_seq → 环可回收时丢弃保留记录 → 拷贝映像副本 → 清零副本
/// volatile 状态（根锁持有字/写者心跳）**。重启恢复
/// = 映像数据页（≤ checkpoint 已落盘部分）+ 重放 (checkpoint_seq, 尾]，
/// 由 Engine::Open 既有恢复路径完成（docs/08 §3-W4），快照本身不改变
/// 活跃映像的数据语义。
///
/// 边界（M1）：
///  - 快照文件为 `<映像路径>.snap` 的整体拷贝（停写时刻的一致性点），
///    非增量/多版本管理（归 M2）；
///  - Windows 下活动映射中的文件不做 rename（共享删除权限未开启），
///    以 copy 达成等价语义；
///  - 机器断电的页乱序残差同 W4（doublewrite 归 M2）。

#include <string>

#include "infra/shm_region.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"

namespace rtdb {
namespace storage {

class WalRing;
class GroupCommit;

/// \brief 执行冷快照（调用方须已持根互斥锁）。
///
/// 步骤：全映像 flush → checkpoint_seq 单调推进到 committed_seq →
/// 环满足不变量（head_seq ≤ checkpoint_seq + 1）时丢弃保留记录 →
/// 拷贝映像到 `<image_path>.snap` → 清零副本 volatile 状态（根锁
/// 持有字/写者心跳，使副本可独立挂载）。
///
/// \param region 共享内存区（写侧角色）
/// \param ring WAL 环（可为 nullptr：无环映像仅推进 checkpoint）
/// \param image_path 映像文件路径（快照路径的派生基准）
/// \param out_snapshot_path 可选输出：快照文件路径
/// \return kOk / kIoError（flush/拷贝/副本补写失败）
Err CreateColdSnapshot(ShmRegion* region, WalRing* ring, const std::string& image_path,
                       std::string* out_snapshot_path = nullptr) noexcept;

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_SNAPSHOT_HPP_
