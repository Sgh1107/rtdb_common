#include "storage/snapshot.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "infra/failpoint.hpp"
#include "infra/layout.hpp"
#include "storage/wal_ring.hpp"

namespace rtdb {
namespace storage {

namespace {

/// 单调推进 64 位水位字（CAS 环；只增不减，并发安全）。
void AdvanceMonotonic(std::atomic<uint64_t>* slot, uint64_t target) noexcept {
    uint64_t cur = slot->load(std::memory_order_relaxed);
    while (cur < target && !slot->compare_exchange_weak(cur, target, std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
    }
}

/// \brief 清零快照副本的 volatile 状态，使副本可独立挂载（含写角色）。
///
/// 冷快照在根锁持有态下抓取，整文件拷贝会把"锁持有字 + 主写者心跳"
/// 一并复制：副本若保留 `locked=1/owner=<抓取者 PID>`，后续挂载者将
/// 撞上根锁 5s 超时（抓取者往往仍然存活，死亡恢复不触发）。故对副本
/// 补写清零：
///  - 根互斥锁状态字（16B @ kRootMutexOffset，infra::shm_RootMutexState）；
///  - 主写者心跳（8B @ writer_heartbeat_ns，0 = 无活跃写者）。
/// 其余字段（水位/目录/环/分配器 freelist）均为持久状态，随拷贝保留
/// 即一致性点语义。仅动副本文件，活映像不受影响；副本写入失败判
/// kIoError（快照整体失败，活映像仍可用）。
Err NeutralizeVolatileStateForCopy(const std::string& snap_path) noexcept {
    static const uint8_t kZeros[16] = {0};
#if defined(_MSC_VER)
    std::FILE* f = nullptr;
    if (fopen_s(&f, snap_path.c_str(), "rb+") != 0) return Err::kIoError;
#else
    std::FILE* f = std::fopen(snap_path.c_str(), "rb+");
    if (f == nullptr) return Err::kIoError;
#endif

    bool ok = std::fseek(f, static_cast<long>(layout::kRootMutexOffset), SEEK_SET) == 0 &&
              std::fwrite(kZeros, 1, sizeof(kZeros), f) == sizeof(kZeros);
    ok = ok && std::fseek(f, static_cast<long>(offsetof(layout::shm_SuperBlock,
                                                        writer_heartbeat_ns)),
                          SEEK_SET) == 0 &&
         std::fwrite(kZeros, 1, sizeof(uint64_t), f) == sizeof(uint64_t);
    ok = ok && std::fflush(f) == 0;
    std::fclose(f);
    return ok ? Err::kOk : Err::kIoError;
}

}  // namespace

Err CreateColdSnapshot(ShmRegion* region, WalRing* ring, const std::string& image_path,
                       std::string* out_snapshot_path) noexcept {
    if (region == nullptr || image_path.empty()) return Err::kInvalidArgument;
    layout::shm_SuperBlock* sb = region->super_block();
    if (sb == nullptr) return Err::kInternal;

    // 1) 停写一致性点：全映像 flush（调用方已持根锁，无并发写者；
    //    组提交后台线程的 flush 与本步骤互不干扰——两者都不改数据）。
    Err fe = region->FlushAll();
    if (!IsOk(fe)) return fe;

    // 2) checkpoint_seq 单调推进到 committed_seq：重放下界前移，环内
    //    (checkpoint, committed] 之外的记录不再被重启恢复需要。
    //    flushed_seq 账本同步前移（断电窗口收缩到 checkpoint 之后）。
    const uint64_t committed = layout::AsAtomicU64(&sb->committed_seq)->load();
    AdvanceMonotonic(layout::AsAtomicU64(&sb->checkpoint_seq), committed);
    AdvanceMonotonic(layout::AsAtomicU64(&sb->flushed_seq), committed);
    const uint64_t checkpoint = layout::AsAtomicU64(&sb->checkpoint_seq)->load();

    // 3) 环回收：仅当全部已分配记录 seq ≤ checkpoint 时才可整体丢弃
    //    （head_seq = 下一个待分配 seq；head_seq ≤ checkpoint + 1 ⇔ 环内
    //    无一条记录的重放价值）。违例时保守保留（覆盖协议不变量仍成立：
    //    tail_seq ≤ checkpoint + 1）。
    if (ring != nullptr && ring->next_seq() <= checkpoint + 1) {
        ring->DropAllRetained();
    }

    // 头部（checkpoint/flushed 水位）回写，保证随后拷贝到的副本自洽。
    fe = region->Flush();
    if (!IsOk(fe)) return fe;

    // 4) 拷贝映像副本（停写时刻的一致性点）。Windows 活动映射中的文件
    //    不做 rename（共享删除权限未开启），copy 达成等价语义；POSIX
    //    亦统一 copy，两平台行为一致（rename 式快照管理器归 M2）。
    //    注入点：拷贝前硬退出（模拟拷贝中断——活映像不受影响，
    //    .snap 可能缺失/部分，docs/06 §3 混沌矩阵 snapshot 中点）。
    failpoint::CrashPoint("snapshot.before_copy");
    const std::string snap_path = image_path + ".snap";
    std::error_code ec;
    std::filesystem::copy_file(image_path, snap_path,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return Err::kIoError;

    // 副本完整性 sanity：字节数须与源一致（拷贝成功即应成立）。
    std::error_code ec_src;
    std::error_code ec_dst;
    const auto src_bytes = std::filesystem::file_size(image_path, ec_src);
    const auto dst_bytes = std::filesystem::file_size(snap_path, ec_dst);
    if (ec_src || ec_dst || src_bytes != dst_bytes) return Err::kIoError;

    // 5) 清零副本的 volatile 状态（根锁持有字/写者心跳），使副本可独立
    //    挂载恢复（抓取时持锁，直接拷贝会把"已锁"带进副本）。
    const Err ne = NeutralizeVolatileStateForCopy(snap_path);
    if (!IsOk(ne)) return ne;

    if (out_snapshot_path != nullptr) *out_snapshot_path = snap_path;
    return Err::kOk;
}

}  // namespace storage
}  // namespace rtdb
