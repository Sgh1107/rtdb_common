#include "infra/slab_allocator.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "infra/layout.hpp"

namespace rtdb {
namespace infra {

using namespace layout;  // 分区常量

namespace {

inline constexpr uint64_t kAllocMagic = 0x52544442414C4331ULL;  // "RTDBALC1"
inline constexpr uint32_t kHdrBytes = 16;

/// 位于用户区前 16 字节：{class, magic, 真实跨度}。
/// 跨度含首切时的对齐间隙，使回收计账精确到字节。
struct BlockHeader {
  uint32_t class_idx;
  uint32_t magic;
  uint64_t span;
};

constexpr uint64_t AlignUp64(uint64_t v, uint64_t a) noexcept {
  return (v + a - 1) / a * a;
}

constexpr uint64_t AlignDown64(uint64_t v, uint64_t a) noexcept {
  return v / a * a;
}

}  // namespace

Result<std::unique_ptr<SlabShmAllocator>> SlabShmAllocator::Attach(
    void* base_addr, uint64_t map_size, bool create_new) {
  if (base_addr == nullptr || map_size < kDataStartOffset)
    return Result<std::unique_ptr<SlabShmAllocator>>::Fail(
        Err::kInvalidArgument);

  auto* ctl = reinterpret_cast<shm_AllocCtl*>(static_cast<char*>(base_addr) +
                                              kAllocCtlOffset);

  if (!create_new && ctl->magic != kAllocMagic)
    return Result<std::unique_ptr<SlabShmAllocator>>::Fail(Err::kBadSuperBlock);
  if (!create_new && ctl->class_count != kSlabClassCount)
    return Result<std::unique_ptr<SlabShmAllocator>>::Fail(
        Err::kIncompatibleLayout);
  if (!create_new && AlignDown64(map_size, 4096) < ctl->data_end)
    return Result<std::unique_ptr<SlabShmAllocator>>::Fail(
        Err::kIncompatibleLayout);  // 本次映射比历史容量小

  auto alloc = std::unique_ptr<SlabShmAllocator>(new SlabShmAllocator());
  alloc->base_ = base_addr;
  alloc->ctl_ = ctl;

  if (create_new) {
    std::memset(ctl, 0, sizeof(*ctl));
    ctl->magic = kAllocMagic;
    ctl->class_count = kSlabClassCount;
    ctl->data_base = kDataStartOffset;
    ctl->data_end = AlignUp64(map_size, 4096);
    ctl->bump = ctl->data_base;
    ctl->carved_bytes = 0;
  }
  return Result<std::unique_ptr<SlabShmAllocator>>::Ok(std::move(alloc));
}

Result<uint64_t> SlabShmAllocator::Allocate(uint64_t size) noexcept {
  if (size == 0 || size > ClassSize(kSlabClassCount - 1))
    return Result<uint64_t>::Fail(Err::kUnsupported);
  const uint32_t cls = SizeToClass(size);
  const uint64_t cls_size = ClassSize(cls);

  uint64_t user_off = 0;
  if (ctl_->free_heads[cls] != 0) {
    // 头弹空闲链：next 复用用户区首 8 字节（最小级 32B，安全）；
    // 头部三元组沿用首切写入值，无需改写。
    user_off = ctl_->free_heads[cls];
    const uint64_t* cell =
        static_cast<const uint64_t*>(OffsetToPtr(user_off));
    ctl_->free_heads[cls] = *cell;
  } else {
    // 无空闲则从 bump 线性切分：用户区落在级尺寸边界上，
    // 前置头部空间不破坏用户对齐；真实跨度（含间隙）入账。
    const uint64_t off =
        AlignUp64(ctl_->bump + kHdrBytes, cls_size > 8 ? cls_size : 8);
    if (off + cls_size > ctl_->data_end)
      return Result<uint64_t>::Fail(Err::kOutOfMemory);
    const uint64_t span = off + cls_size - ctl_->bump;
    ctl_->bump = off + cls_size;
    ctl_->carved_bytes += span;

    auto* hdr = static_cast<BlockHeader*>(OffsetToPtr(off - kHdrBytes));
    hdr->span = span;
    user_off = off;
    if (std::getenv("RTDB_BTREE_DEBUG") != nullptr)
      std::printf("[alloc] user=%llu span=%llu\n",
                  static_cast<unsigned long long>(user_off),
                  static_cast<unsigned long long>(span));
  }

  auto* hdr = static_cast<BlockHeader*>(OffsetToPtr(user_off - kHdrBytes));
  hdr->class_idx = cls;
  hdr->magic = kBlockHdrMagic;  // 复用路径在此处"解毒"
  return Result<uint64_t>::Ok(user_off);
}

Err SlabShmAllocator::Deallocate(uint64_t user_off) noexcept {
  if (!OwnsOffset(user_off)) return Err::kInvalidArgument;
  auto* hdr = static_cast<BlockHeader*>(OffsetToPtr(user_off - kHdrBytes));
  if (hdr->magic != kBlockHdrMagic || hdr->class_idx >= kSlabClassCount)
    return Err::kInternal;  // 双重释放或越界写破坏了头部
  if (hdr->span == 0 || hdr->span > ctl_->carved_bytes)
    return Err::kInternal;  // 非法跨度同样视为损坏

  // 先回冲账本并中毒块头，再头插链表。
  hdr->magic = kBlockHdrMagic ^ 0x5A5A5A5Au;
  ctl_->carved_bytes -= hdr->span;
  *static_cast<uint64_t*>(OffsetToPtr(user_off)) =
      ctl_->free_heads[hdr->class_idx];
  ctl_->free_heads[hdr->class_idx] = user_off;
  return Err::kOk;
}

bool SlabShmAllocator::OwnsOffset(uint64_t user_off) const noexcept {
  return user_off >= ctl_->data_base + kHdrBytes &&
         user_off + 8 <= ctl_->data_end && (user_off % 8) == 0;
}

uint64_t SlabShmAllocator::used_bytes() const noexcept {
  // 活跃字节 = 切出总量 - 已回收回冲量（精确，无估算）。
  return ctl_->carved_bytes;
}

}  // namespace infra
}  // namespace rtdb
