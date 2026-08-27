#ifndef RTDB_SRC_INFRA_SLAB_ALLOCATOR_HPP_
#define RTDB_SRC_INFRA_SLAB_ALLOCATOR_HPP_

#include <cstdint>
#include <memory>

#include "rtdb/api_macro.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"

namespace rtdb {
namespace infra {

inline constexpr int kSlabClassCount = 12;               ///< 32B<<i 至 64KB 共 12 级
inline constexpr uint32_t kBlockHdrMagic = 0x4C424442u;  ///< "BDBL"

/// 控制块：固定位于 kAllocCtlOffset，create 时一次性落盘。
/// 所有分配元数据自描述（块头带 class_idx），释放无需外部尺寸。
struct shm_AllocCtl {
    uint64_t magic;        ///< "RTDBALC1" 常量
    uint32_t class_count;  ///< = kSlabClassCount
    uint32_t reserved;
    uint64_t data_base;                    ///< 首个可分配偏移（=kDataStartOffset）
    uint64_t data_end;                     ///< 末尾（创建时依映射大小定格）
    uint64_t bump;                         ///< 未切分空闲空间的起点
    uint64_t carved_bytes;                 ///< 已从 bump 切出的真实字节数（精确账本）
    uint64_t free_heads[kSlabClassCount];  ///< 各级空闲链头（用户载荷 off）
};
static_assert(sizeof(shm_AllocCtl) <= 512, "控制块必须容身 512B 分区");

/// 共享内存 Slab 分配器。
///
/// 块物理布局：[16B 头 | 用户区]，头 = {u32 class_idx, u32 magic,
/// u64 真实跨度}，Allocate 返回用户区偏移（自然对齐）；Deallocate
/// 复原头部自愈尺寸并按真实跨度计账。
/// 对齐保证：用户区落在 max(级尺寸,8) 边界上。
/// 线程/进程安全约定：调用方必须持有 ShmRootMutex。
/// 容量上限：单次 ≤64KB；更大块属 M1 变长池。
class RTDB_API SlabShmAllocator {
public:
    /// 附着/初始化。create_new=true 时清零控制块并圈定 data 区；
    /// 否则校验 magic/class_count/data_end 与当前映射的兼容性。
    static Result<std::unique_ptr<SlabShmAllocator>> Attach(void* base_addr, uint64_t map_size,
                                                            bool create_new);

    ~SlabShmAllocator() = default;

    /// 成功返回用户区偏移。失败码：
    ///   kUnsupported        尺寸超过最大级(64KB)
    ///   kOutOfMemory        空间不足
    Result<uint64_t> Allocate(uint64_t size) noexcept;

    /// 归还块（自描述头部定级）；损坏头部返回 kInternal。
    Err Deallocate(uint64_t user_off) noexcept;

    bool OwnsOffset(uint64_t user_off) const noexcept;

    uint64_t capacity_bytes() const noexcept { return ctl_->data_end - ctl_->data_base; }
    uint64_t used_bytes() const noexcept;  ///< 含块头的粗估占用

    void* OffsetToPtr(uint64_t off) const noexcept { return static_cast<char*>(base_) + off; }

private:
    SlabShmAllocator() = default;
    static constexpr uint32_t SizeToClass(uint64_t size) noexcept {
        // 32<<i 各级；ceil 到最小容纳级。
        uint32_t c = 0;
        while ((32ull << c) < size && c < kSlabClassCount - 1) ++c;
        return c;
    }
    static constexpr uint64_t ClassSize(uint32_t c) noexcept { return 32ull << c; }

    void* base_ = nullptr;
    shm_AllocCtl* ctl_ = nullptr;
};

}  // namespace infra
}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_SLAB_ALLOCATOR_HPP_
