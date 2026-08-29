/// MVarLenPool 单测：最佳适配、分裂、双向合并、双重释放检出、
/// 碎片率与回收率阈值（docs/06 §2-5）、复附着状态保持。
/// 宿主堆页对齐缓冲充当映射区替身。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "storage/var_pool.hpp"

namespace {

using rtdb::storage::MVarLenPool;
using Pool = std::unique_ptr<MVarLenPool>;

/// 页对齐宿主缓冲（池基址须 8 对齐，给 4096 保险）。
struct Region {
    explicit Region(uint64_t bytes)
        : size(bytes), mem(static_cast<char*>(::operator new(bytes, std::align_val_t(4096)))) {
        std::memset(mem, 0, bytes);
    }
    ~Region() { ::operator delete(mem, std::align_val_t(4096)); }
    uint64_t size;
    char* mem;
};

class VarPoolTest : public ::testing::Test {
protected:
    static constexpr uint64_t kRegionBytes = 1ull << 20;  // 1MB

    void SetUp() override {
        region_ = std::make_unique<Region>(kRegionBytes);
        auto res = MVarLenPool::Attach(region_->mem, kRegionBytes, /*create_new=*/true);
        ASSERT_TRUE(res.IsOk());
        pool_ = res.TakeValue();
    }

    /// 便捷分配并写入标记，返回载荷偏移。
    uint64_t AllocMarked(uint64_t n, uint8_t tag) {
        auto r = pool_->Allocate(n);
        EXPECT_TRUE(r.IsOk()) << "alloc " << n;
        if (!r.IsOk()) return 0;
        std::memset(region_->mem + r.Value(), tag, static_cast<size_t>(n));
        return r.Value();
    }

    std::unique_ptr<Region> region_;
    Pool pool_;
};

// ---------------------------------------------------------------------------
// 基础往返与数据完整性
// ---------------------------------------------------------------------------
TEST_F(VarPoolTest, AllocFreeRoundtripKeepsDataIsolated) {
    const uint64_t a = AllocMarked(100, 0xAA);
    const uint64_t b = AllocMarked(1000, 0xBB);
    EXPECT_NE(a, b);
    EXPECT_EQ(static_cast<uint8_t>(region_->mem[a]), 0xAA);
    EXPECT_EQ(static_cast<uint8_t>(region_->mem[b]), 0xBB);
    EXPECT_EQ(pool_->UsedBytes(), (16 + 104 + 8) + (16 + 1000 + 8));

    ASSERT_EQ(pool_->Deallocate(a), rtdb::Err::kOk);
    ASSERT_EQ(pool_->Deallocate(b), rtdb::Err::kOk);
    EXPECT_EQ(pool_->UsedBytes(), 0u);
    EXPECT_EQ(pool_->FreeBytes(), pool_->TotalUsableBytes());
    uint64_t blocks = 0;
    EXPECT_EQ(pool_->ValidateWalk(&blocks), rtdb::Err::kOk);
}

// ---------------------------------------------------------------------------
// 最佳适配：多个空洞中选最小可容纳者
// ---------------------------------------------------------------------------
TEST_F(VarPoolTest, BestFitSelectsSmallestSufficientHole) {
    const uint64_t a = AllocMarked(200, 1);           // total 224
    const uint64_t b = AllocMarked(200, 2);           // total 224（占位，保持两洞分离）
    const uint64_t c = AllocMarked(400, 3);           // total 424
    ASSERT_EQ(pool_->Deallocate(a), rtdb::Err::kOk);  // 洞1 = 224
    ASSERT_EQ(pool_->Deallocate(c), rtdb::Err::kOk);  // 洞2 = 424

    auto r = pool_->Allocate(100);  // needed_total = 128：224 与 424 都装得下
    ASSERT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), a);  // 必须选最小的洞1，而非 424 的大洞
    EXPECT_EQ(pool_->Deallocate(b), rtdb::Err::kOk);
    EXPECT_EQ(pool_->Deallocate(r.Value()), rtdb::Err::kOk);
}

// ---------------------------------------------------------------------------
// 分裂：大块切分后尾段回空闲链，顺序分配物理连续
// ---------------------------------------------------------------------------
TEST_F(VarPoolTest, SplitCarvesTailBackToFreeList) {
    const uint64_t first = AllocMarked(64, 7);  // total 88，自首块切出
    EXPECT_EQ(first, 64 + 16);                  // 控制块 64 + 块头 16
    // 尾段回到空闲链：顺序分配紧随其后（块边界 64+88，载荷再 +16）。
    const uint64_t second = AllocMarked(64, 8);
    EXPECT_EQ(second, 64 + 88 + 16);
    uint64_t blocks = 0;
    EXPECT_EQ(pool_->ValidateWalk(&blocks), rtdb::Err::kOk);
    EXPECT_EQ(blocks, 3u);  // 已用 2 块 + 尾部余量块
}

// ---------------------------------------------------------------------------
// 合并：两侧空闲块吸收，中间释放后还原为单一连续大块
// ---------------------------------------------------------------------------
TEST_F(VarPoolTest, CoalesceMergesBothSides) {
    const uint64_t x = AllocMarked(64, 1);
    const uint64_t y = AllocMarked(64, 2);
    const uint64_t z = AllocMarked(64, 3);  // 每块 total 88
    ASSERT_EQ(pool_->Deallocate(x), rtdb::Err::kOk);
    ASSERT_EQ(pool_->Deallocate(z), rtdb::Err::kOk);
    ASSERT_EQ(pool_->Deallocate(y), rtdb::Err::kOk);
    // X+Y+Z 连同尾部余量全部并回：整池回归单一空闲块。
    EXPECT_EQ(pool_->LargestFreeBytes(), pool_->TotalUsableBytes());
    EXPECT_EQ(pool_->UsedBytes(), 0u);
    uint64_t blocks = 0;
    EXPECT_EQ(pool_->ValidateWalk(&blocks), rtdb::Err::kOk);

    // 合并后的大洞可一次性满足原三块总和的请求，且落位在 X 处。
    auto r = pool_->Allocate(192);
    ASSERT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), x);
}

// ---------------------------------------------------------------------------
// 双重释放 / 越界偏移拒绝
// ---------------------------------------------------------------------------
TEST_F(VarPoolTest, DoubleFreeAndBadOffsetRejected) {
    const uint64_t a = AllocMarked(32, 9);
    ASSERT_EQ(pool_->Deallocate(a), rtdb::Err::kOk);
    EXPECT_EQ(pool_->Deallocate(a), rtdb::Err::kInternal);
    // 明确非法偏移：0、控制块内、未 8 对齐 → kInvalidArgument；
    // 落在区域内但非块头处 → 头部 magic 校验拦截为 kInternal（毒化语义）。
    EXPECT_EQ(pool_->Deallocate(0), rtdb::Err::kInvalidArgument);
    EXPECT_EQ(pool_->Deallocate(16), rtdb::Err::kInvalidArgument);
    EXPECT_EQ(pool_->Deallocate(80 + 4), rtdb::Err::kInvalidArgument);
    EXPECT_EQ(pool_->Deallocate(kRegionBytes - 8), rtdb::Err::kInternal);
}

// ---------------------------------------------------------------------------
// 耗尽与全量回收（docs/06 §2-5：回收率阈值）
// ---------------------------------------------------------------------------
TEST_F(VarPoolTest, ExhaustionThenFullRecovery) {
    // 4KB 小池内反复分配直至耗尽。
    Region small(4096);
    auto p = MVarLenPool::Attach(small.mem, 4096, true).TakeValue();
    const uint64_t usable = p->TotalUsableBytes();
    uint64_t allocated = 0;
    for (;;) {
        auto r = p->Allocate(64);  // total 88/块
        if (!r.IsOk()) {
            EXPECT_EQ(r.Error(), rtdb::Err::kOutOfMemory);
            break;
        }
        allocated += 88;
        EXPECT_LE(allocated, usable);
    }
    EXPECT_GT(allocated, 0u);
    // 全量回收：最大空闲块必须精确回到整池（零碎片），大请求恢复可分配。
    for (uint64_t off = 64 + 16; allocated > 0; off += 88) {
        ASSERT_EQ(p->Deallocate(off), rtdb::Err::kOk);
        allocated -= 88;
    }
    EXPECT_EQ(p->LargestFreeBytes(), usable);
    EXPECT_DOUBLE_EQ(p->FragmentationRatio(), 0.0);
    EXPECT_TRUE(p->Allocate(usable - 24).IsOk());  // 满容量单块请求
}

// ---------------------------------------------------------------------------
// 碎片率：棋盘式释放抬升碎片率，顺序回收到 0
// ---------------------------------------------------------------------------
TEST_F(VarPoolTest, FragmentationRatioRisesAndFallsToZero) {
    // 精确切满：usable = 8 块 × 120（载荷 96）。
    Region small(1024);
    auto p = MVarLenPool::Attach(small.mem, 1024, true).TakeValue();
    const uint64_t usable = p->TotalUsableBytes();
    ASSERT_EQ(usable % 120, 0u) << "region size must tile 8x120";
    std::vector<uint64_t> offs;
    for (int i = 0; i < 8; ++i) {
        auto r = p->Allocate(96);
        ASSERT_TRUE(r.IsOk()) << i;
        EXPECT_EQ(r.Value(), 64 + 16 + static_cast<uint64_t>(i) * 120);
        offs.push_back(r.Value());
    }
    EXPECT_EQ(p->FreeBytes(), 0u);
    EXPECT_DOUBLE_EQ(p->FragmentationRatio(), 0.0);

    // 棋盘释放 4/8 块：free=480，最大洞=120 → 碎片率 0.75。
    for (int i : {0, 2, 4, 6}) ASSERT_EQ(p->Deallocate(offs[i]), rtdb::Err::kOk);
    EXPECT_DOUBLE_EQ(p->FragmentationRatio(), 0.75);
    uint64_t blocks = 0;
    EXPECT_EQ(p->ValidateWalk(&blocks), rtdb::Err::kOk);

    // 顺序回收剩余块 → 全并回单块，碎片率归零。
    for (int i : {1, 3, 5, 7}) ASSERT_EQ(p->Deallocate(offs[i]), rtdb::Err::kOk);
    EXPECT_EQ(p->LargestFreeBytes(), usable);
    EXPECT_DOUBLE_EQ(p->FragmentationRatio(), 0.0);
}

// ---------------------------------------------------------------------------
// 复附着：状态跨"重启"保持（等价崩溃恢复路径）
// ---------------------------------------------------------------------------
TEST_F(VarPoolTest, ReattachPreservesState) {
    const uint64_t a = AllocMarked(128, 0x5A);
    const uint64_t before_used = pool_->UsedBytes();
    auto r2 = MVarLenPool::Attach(region_->mem, kRegionBytes, /*create_new=*/false);
    ASSERT_TRUE(r2.IsOk());
    Pool pool2 = r2.TakeValue();
    EXPECT_EQ(pool2->UsedBytes(), before_used);
    uint64_t blocks = 0;
    EXPECT_EQ(pool2->ValidateWalk(&blocks), rtdb::Err::kOk);
    EXPECT_EQ(pool2->Deallocate(a), rtdb::Err::kOk);

    // 错误路径：magic 损坏 / 容量不符。
    Region other(4096);
    EXPECT_FALSE(MVarLenPool::Attach(other.mem, 4096, false).IsOk());
    EXPECT_EQ(MVarLenPool::Attach(region_->mem, 2048, false).Error(),
              rtdb::Err::kIncompatibleLayout);
}

}  // namespace
