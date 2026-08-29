#ifndef RTDB_SRC_STORAGE_VAR_POOL_HPP_
#define RTDB_SRC_STORAGE_VAR_POOL_HPP_

/// \file 变长池 MVarLenPool（docs/03 §2.1，docs/08 §3-W1）。
///
/// 为变长列（string/blob）提供映像内任意尺寸的分配：物理块采用
/// 边界标记（header/footer）+ 空闲双链，分配策略**最佳适配**，
/// 释放时与物理相邻空闲块**双向合并**， oversized 块**分裂**回收尾段。
///
/// 与 SlabShmAllocator 的分工：slab 面向 ≤64KB 固定级（索引节点/描述符），
/// 本池面向变长字节串（可跨越任意尺寸，受池容量约束）。
///
/// 物理块布局（pool_off 为块相对池基址的偏移，8 对齐）：
/// ```
/// B+0        u64 total_bytes   头（含头/载荷/尾三段）
/// B+8        u32 magic         分配态标记（空闲态中毒化值，双重释放检出）
/// B+12       u32 alloc_flag    1=占用 0=空闲
/// B+16..     payload           空闲块载荷前 16B 复用为 {prev_free, next_free}
/// B+total-8  u64 total_bytes   尾标记（向后合并的回跳依据）
/// ```
/// 线程/进程安全约定：调用方必须持有 ShmRootMutex（同 SlabShmAllocator）。

#include <cstdint>
#include <memory>

#include "rtdb/api_macro.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"

namespace rtdb {
namespace storage {

/// 池控制块：固定位于池区起始（64B），Attach(create_new) 时初始化。
struct shm_VarPoolCtl {
    uint64_t magic;            ///< "RTDBVPL1"
    uint64_t total_bytes;      ///< 池区总容量（含控制块）
    uint64_t first_block_off;  ///< 首物理块偏移（= kVarPoolCtlBytes）
    uint64_t free_head;        ///< 空闲双链头（0 = 无空闲块）
    uint64_t alloc_count;      ///< 活跃分配块数
    uint64_t alloc_bytes;      ///< 活跃分配的块总字节数（含头尾标记）
    uint64_t free_count;       ///< 空闲块数
    uint64_t reserved;
};
static_assert(sizeof(shm_VarPoolCtl) == 64, "池控制块必须恰好 64 字节");

class RTDB_API MVarLenPool {
public:
    /// 附着/初始化。create_new=true 时将 [ctl 后, 区尾] 整段 carving 为单个
    /// 空闲块；否则校验 magic/容量与当前区一致性（等价进程重启后复附着）。
    static Result<std::unique_ptr<MVarLenPool>> Attach(void* base_addr, uint64_t region_bytes,
                                                       bool create_new);

    /// 分配 n 字节（8 对齐返回）。策略：空闲双链全扫最佳适配；
    /// 命中块超出需求且余量 ≥ 最小块时分裂，尾段回空闲链。
    /// 失败码：kInvalidArgument（n 超池容量）/ kOutOfMemory（无适配空闲块）。
    Result<uint64_t> Allocate(uint64_t n) noexcept;

    /// 归还：校验分配态标记（中毒化防双重释放），与物理前驱/后继空闲块
    /// 双向合并后回挂空闲链。
    Err Deallocate(uint64_t pool_off) noexcept;

    bool OwnsOffset(uint64_t pool_off) const noexcept;

    // ---- 统计（碎片率观测，docs/06 §2-5）----
    uint64_t TotalUsableBytes() const noexcept;  ///< 池容量 - 控制块
    uint64_t UsedBytes() const noexcept;         ///< 活跃块总字节（含头尾）
    uint64_t FreeBytes() const noexcept;
    /// 空闲链中最大连续块（决定大对象能否再分配）。
    uint64_t LargestFreeBytes() const noexcept;
    /// 碎片率 = 1 - 最大空闲块/总空闲；0 = 完全无碎片。
    double FragmentationRatio() const noexcept;

    /// 遍历物理块校验结构不变量（头尾互证、总和闭合、标记合法）。
    /// 供测试与诊断；发现任一违例返回 kCorruption。
    Err ValidateWalk(uint64_t* block_count) const noexcept;

private:
    MVarLenPool() = default;

    static constexpr uint32_t kMagicAlloc = 0x564C5041u;  ///< "VLPA"
    static constexpr uint32_t kMagicFree = 0x564C5046u;   ///< "VLPF"（中毒化）
    /// 最小物理块：头 16 + 空闲载荷 16（双链指针）+ 尾 8。
    static constexpr uint64_t kMinBlockBytes = 40;

    shm_VarPoolCtl* ctl_ = nullptr;
    void* base_ = nullptr;

    uint64_t RegionEnd() const noexcept;
    /// 从空闲链摘除节点（O(1)，双链）。
    void UnlinkFree(uint64_t block_off) noexcept;
    /// 头插空闲链。
    void LinkFree(uint64_t block_off) noexcept;
};

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_VAR_POOL_HPP_
