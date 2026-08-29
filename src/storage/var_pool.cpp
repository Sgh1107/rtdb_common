#include "storage/var_pool.hpp"

#include <cstring>
#include <memory>

namespace rtdb {
namespace storage {

namespace {

inline constexpr uint64_t kPoolMagic = 0x5254444256504C31ULL;  // "RTDBVPL1"
inline constexpr uint64_t kCtlBytes = 64;

/// 物理块头：{块总字节, 标记, 占用标志}；尾标记为 u64 镜像 total。
struct BlockHdr {
    uint64_t total;
    uint32_t magic;
    uint32_t alloc_flag;
};
static_assert(sizeof(BlockHdr) == 16, "块头必须 16 字节");

/// 空闲块载荷前 16B 复用为空闲双链节点。
struct FreeNode {
    uint64_t prev_free;  ///< 0 = 链首无前驱（块偏移恒 ≥ kCtlBytes，无歧义）
    uint64_t next_free;  ///< 0 = 链尾
};
static_assert(sizeof(FreeNode) == 16, "空闲节点必须 16 字节");

inline uint64_t AlignUp8(uint64_t v) noexcept { return (v + 7) / 8 * 8; }

inline BlockHdr* HdrAt(void* base, uint64_t off) noexcept {
    return reinterpret_cast<BlockHdr*>(static_cast<char*>(base) + off);
}
inline uint64_t* FooterAt(void* base, uint64_t block_off, uint64_t total) noexcept {
    return reinterpret_cast<uint64_t*>(static_cast<char*>(base) + block_off + total - 8);
}
inline FreeNode* NodeAt(void* base, uint64_t block_off) noexcept {
    return reinterpret_cast<FreeNode*>(static_cast<char*>(base) + block_off + 16);
}

}  // namespace

Result<std::unique_ptr<MVarLenPool>> MVarLenPool::Attach(void* base_addr, uint64_t region_bytes,
                                                         bool create_new) {
    if (base_addr == nullptr || region_bytes < kCtlBytes + kMinBlockBytes)
        return Result<std::unique_ptr<MVarLenPool>>::Fail(Err::kInvalidArgument);

    auto* ctl = reinterpret_cast<shm_VarPoolCtl*>(base_addr);
    if (!create_new) {
        if (ctl->magic != kPoolMagic)
            return Result<std::unique_ptr<MVarLenPool>>::Fail(Err::kCorruption);
        if (ctl->total_bytes != region_bytes)
            return Result<std::unique_ptr<MVarLenPool>>::Fail(Err::kIncompatibleLayout);
    }

    auto pool = std::unique_ptr<MVarLenPool>(new MVarLenPool());
    pool->base_ = base_addr;
    pool->ctl_ = ctl;

    if (create_new) {
        std::memset(ctl, 0, sizeof(*ctl));
        ctl->magic = kPoolMagic;
        ctl->total_bytes = region_bytes;
        ctl->first_block_off = kCtlBytes;
        ctl->free_head = 0;
        // 整段 carving 为单个空闲块。
        const uint64_t first_total = region_bytes - kCtlBytes;
        auto* h = HdrAt(base_addr, kCtlBytes);
        h->total = first_total;
        h->magic = kMagicFree;
        h->alloc_flag = 0;
        *FooterAt(base_addr, kCtlBytes, first_total) = first_total;
        ctl->free_head = kCtlBytes;
        ctl->free_count = 1;
    }
    return Result<std::unique_ptr<MVarLenPool>>::Ok(std::move(pool));
}

void MVarLenPool::UnlinkFree(uint64_t block_off) noexcept {
    auto* node = NodeAt(base_, block_off);
    const uint64_t prev = node->prev_free;
    const uint64_t next = node->next_free;
    if (prev != 0)
        NodeAt(base_, prev)->next_free = next;
    else
        ctl_->free_head = next;
    if (next != 0) NodeAt(base_, next)->prev_free = prev;
    ctl_->free_count--;
}

void MVarLenPool::LinkFree(uint64_t block_off) noexcept {
    auto* node = NodeAt(base_, block_off);
    node->prev_free = 0;
    node->next_free = ctl_->free_head;
    if (ctl_->free_head != 0) NodeAt(base_, ctl_->free_head)->prev_free = block_off;
    ctl_->free_head = block_off;
    ctl_->free_count++;
}

Result<uint64_t> MVarLenPool::Allocate(uint64_t n) noexcept {
    if (n == 0 || n > ctl_->total_bytes) return Result<uint64_t>::Fail(Err::kInvalidArgument);
    const uint64_t needed_total = 16 + AlignUp8(n) + 8;

    // 最佳适配：全链扫描，取能容纳需求的最小块。
    uint64_t best_off = 0;
    uint64_t best_total = 0;
    for (uint64_t off = ctl_->free_head; off != 0;) {
        const BlockHdr* h = HdrAt(base_, off);
        if (h->total >= needed_total && (best_off == 0 || h->total < best_total)) {
            best_off = off;
            best_total = h->total;
            if (best_total == needed_total) break;  // 恰好匹配，提前收工
        }
        off = NodeAt(base_, off)->next_free;
    }
    if (best_off == 0) return Result<uint64_t>::Fail(Err::kOutOfMemory);

    UnlinkFree(best_off);

    // 分裂：余量足够成块时，把尾段切回空闲链。
    uint64_t used_total = best_total;
    if (best_total - needed_total >= kMinBlockBytes) {
        used_total = needed_total;
        const uint64_t rest_off = best_off + needed_total;
        const uint64_t rest_total = best_total - needed_total;
        auto* rh = HdrAt(base_, rest_off);
        rh->total = rest_total;
        rh->magic = kMagicFree;
        rh->alloc_flag = 0;
        *FooterAt(base_, rest_off, rest_total) = rest_total;
        LinkFree(rest_off);
    }

    auto* h = HdrAt(base_, best_off);
    h->total = used_total;
    h->magic = kMagicAlloc;
    h->alloc_flag = 1;
    *FooterAt(base_, best_off, used_total) = used_total;
    ctl_->alloc_count++;
    ctl_->alloc_bytes += used_total;
    return Result<uint64_t>::Ok(best_off + 16);  // 载荷偏移
}

Err MVarLenPool::Deallocate(uint64_t pool_off) noexcept {
    if (!OwnsOffset(pool_off)) return Err::kInvalidArgument;
    const uint64_t block_off = pool_off - 16;
    BlockHdr* h = HdrAt(base_, block_off);
    if (h->magic != kMagicAlloc || h->alloc_flag != 1) return Err::kInternal;  // 双重释放/越界写
    if (block_off + h->total > ctl_->total_bytes) return Err::kCorruption;

    ctl_->alloc_count--;
    ctl_->alloc_bytes -= h->total;
    h->magic = kMagicFree;  // 先中毒：后续合并/再分配前的窗口期内可被检出
    h->alloc_flag = 0;

    // 向后合并：物理后继是空闲块则吸收（从空闲链摘除后继）。
    uint64_t total = h->total;
    const uint64_t next_off = block_off + total;
    if (next_off < RegionEnd()) {
        const BlockHdr* nh = HdrAt(base_, next_off);
        if (nh->magic == kMagicFree && nh->alloc_flag == 0) {
            UnlinkFree(next_off);
            total += nh->total;
        }
    }

    // 向前合并：物理前驱是空闲块则并入前驱（前驱节点已在链上，原地扩账）。
    if (block_off > ctl_->first_block_off) {
        const uint64_t prev_total =
            *reinterpret_cast<const uint64_t*>(static_cast<const char*>(base_) + block_off - 8);
        const uint64_t prev_off = block_off - prev_total;
        if (prev_off >= ctl_->first_block_off) {
            BlockHdr* ph = HdrAt(base_, prev_off);
            if (ph->magic == kMagicFree && ph->alloc_flag == 0) {
                ph->total = prev_total + total;  // 节点位置不变，仅扩账
                *FooterAt(base_, prev_off, ph->total) = ph->total;
                // 当前块不再入链；标记区域维持"空闲但不在链"的瞬态无害
                // （其头部已并入前驱载荷，此后不可达）。
                return Err::kOk;
            }
        }
    }

    h->total = total;
    *FooterAt(base_, block_off, total) = total;
    LinkFree(block_off);
    return Err::kOk;
}

bool MVarLenPool::OwnsOffset(uint64_t pool_off) const noexcept {
    return pool_off >= ctl_->first_block_off + 16 && pool_off < ctl_->total_bytes &&
           (pool_off % 8) == 0;
}

uint64_t MVarLenPool::TotalUsableBytes() const noexcept { return ctl_->total_bytes - kCtlBytes; }

uint64_t MVarLenPool::UsedBytes() const noexcept { return ctl_->alloc_bytes; }

uint64_t MVarLenPool::FreeBytes() const noexcept { return TotalUsableBytes() - ctl_->alloc_bytes; }

uint64_t MVarLenPool::LargestFreeBytes() const noexcept {
    uint64_t largest = 0;
    for (uint64_t off = ctl_->free_head; off != 0;) {
        const BlockHdr* h = HdrAt(base_, off);
        if (h->total > largest) largest = h->total;
        off = NodeAt(base_, off)->next_free;
    }
    return largest;
}

double MVarLenPool::FragmentationRatio() const noexcept {
    const uint64_t free_bytes = FreeBytes();
    if (free_bytes == 0) return 0.0;
    return 1.0 - static_cast<double>(LargestFreeBytes()) / static_cast<double>(free_bytes);
}

Err MVarLenPool::ValidateWalk(uint64_t* block_count) const noexcept {
    if (block_count != nullptr) *block_count = 0;
    uint64_t free_seen = 0;
    uint64_t off = ctl_->first_block_off;
    while (off < ctl_->total_bytes) {
        const BlockHdr* h = HdrAt(base_, off);
        if (h->total < kMinBlockBytes || off + h->total > ctl_->total_bytes)
            return Err::kCorruption;
        const bool is_alloc = (h->magic == kMagicAlloc);
        const bool is_free = (h->magic == kMagicFree);
        if (!is_alloc && !is_free) return Err::kCorruption;
        if (is_alloc != (h->alloc_flag == 1)) return Err::kCorruption;
        if (*FooterAt(base_, off, h->total) != h->total) return Err::kCorruption;
        if (is_free) free_seen++;
        if (block_count != nullptr) (*block_count)++;
        off += h->total;
    }
    if (off != ctl_->total_bytes) return Err::kCorruption;
    if (free_seen != ctl_->free_count) return Err::kCorruption;

    // 空闲双链一致性：逐节点校验 prev/next 互指且 magic 均为空闲态。
    uint64_t linked = 0;
    uint64_t prev = 0;
    for (uint64_t off2 = ctl_->free_head; off2 != 0;) {
        const BlockHdr* h = HdrAt(base_, off2);
        if (h->magic != kMagicFree) return Err::kCorruption;
        const FreeNode* n = NodeAt(base_, off2);
        if (n->prev_free != prev) return Err::kCorruption;
        prev = off2;
        off2 = n->next_free;
        linked++;
    }
    if (linked != ctl_->free_count) return Err::kCorruption;
    return Err::kOk;
}

uint64_t MVarLenPool::RegionEnd() const noexcept { return ctl_->total_bytes; }

}  // namespace storage
}  // namespace rtdb
