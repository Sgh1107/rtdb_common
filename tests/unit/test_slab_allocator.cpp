/// SlabShmAllocator 全路径单测：宿主堆缓冲充当"映射区"替身。
/// 覆盖：附着/重附、多级分配对齐、空闲复用、耗尽、越界拒绝、
/// 双重释放检出、占用统计一致性。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <vector>

#include "infra/slab_allocator.hpp"
#include "rtdb/result.hpp"

namespace {

using rtdb::infra::SlabShmAllocator;
using R = rtdb::Result<uint64_t>;

/// 页对齐宿主缓冲。
struct HeapRegion {
  explicit HeapRegion(uint64_t bytes)
      : size_(bytes),
        mem_(static_cast<char*>(
            ::operator new(bytes, std::align_val_t(4096)))) {
    std::memset(mem_, 0, bytes);
  }
  ~HeapRegion() { ::operator delete(mem_, std::align_val_t(4096)); }

  uint64_t size_;
  char* mem_;
};

class SlabTest : public ::testing::Test {
 protected:
  static constexpr uint64_t kRegionBytes = 1ull << 20;  // 1MB
  using AllocResult = rtdb::Result<std::unique_ptr<SlabShmAllocator>>;

  void SetUp() override { region_ = std::make_unique<HeapRegion>(kRegionBytes); }

  AllocResult Attach(bool create) {
    return SlabShmAllocator::Attach(region_->mem_, region_->size_, create);
  }

  std::unique_ptr<HeapRegion> region_;
};

TEST_F(SlabTest, AttachCreateAndReattachValidates) {
  auto a1 = Attach(true);
  ASSERT_TRUE(a1.IsOk());
  const uint64_t cap1 = a1.Value()->capacity_bytes();

  auto a2 = Attach(false);  // 复附着须走校验路径成功
  ASSERT_TRUE(a2.IsOk());
  EXPECT_EQ(a2.Value()->capacity_bytes(), cap1);
}

TEST_F(SlabTest, AllocFreeLifoReuse) {
  auto a = Attach(true);
  ASSERT_TRUE(a.IsOk());

  auto r1 = a.Value()->Allocate(4096);
  auto r2 = a.Value()->Allocate(4096);
  auto r3 = a.Value()->Allocate(4096);
  ASSERT_TRUE(r1.IsOk() && r2.IsOk() && r3.IsOk());
  EXPECT_NE(r1.Value(), r2.Value());
  EXPECT_NE(r2.Value(), r3.Value());
  // 页级分配用户区直接按页对齐（块头占前置 8 字节）。
  EXPECT_EQ(r1.Value() % 4096, 0u);

  // 空闲块中毒后再次分配必须取"最后归还者"（头插链）。
  EXPECT_EQ(a.Value()->Deallocate(r2.Value()), rtdb::Err::kOk);
  auto r4 = a.Value()->Allocate(4096);
  ASSERT_TRUE(r4.IsOk());
  EXPECT_EQ(r4.Value(), r2.Value());
}

TEST_F(SlabTest, RejectZeroAndOversize) {
  auto a = Attach(true);
  ASSERT_TRUE(a.IsOk());
  EXPECT_FALSE(a.Value()->Allocate(0).IsOk());
  EXPECT_EQ(a.Value()->Allocate(64ull * 1024 + 1).Error(),
            rtdb::Err::kUnsupported);
}

TEST_F(SlabTest, ExhaustionReturnsOutOfMemory) {
  auto a = Attach(true);
  ASSERT_TRUE(a.IsOk());

  int allocated = 0;
  for (; allocated < 4096; ++allocated) {
    auto r = a.Value()->Allocate(64 * 1024);  // 最大级，快速吃满
    if (!r.IsOk()) {
      EXPECT_EQ(r.Error(), rtdb::Err::kOutOfMemory);
      break;
    }
  }
  EXPECT_LT(allocated, 4096);  // 1MB 必然先耗尽
}

TEST_F(SlabTest, DoubleFreeDetectedViaHeaderPoisoning) {
  auto a = Attach(true);
  ASSERT_TRUE(a.IsOk());

  auto r = a.Value()->Allocate(128);
  ASSERT_TRUE(r.IsOk());
  EXPECT_EQ(a.Value()->Deallocate(r.Value()), rtdb::Err::kOk);
  EXPECT_EQ(a.Value()->Deallocate(r.Value()), rtdb::Err::kInternal);
}

TEST_F(SlabTest, MultiClassDistinctAlignedOffsets) {
  auto a = Attach(true);
  ASSERT_TRUE(a.IsOk());

  std::set<uint64_t> seen;
  for (uint64_t sz : {32u, 64u, 256u, 1024u, 8192u}) {
    auto r = a.Value()->Allocate(sz);
    ASSERT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value() % 32, 0u);       // 最小级对齐
    EXPECT_EQ(seen.count(r.Value()), 0u);
    seen.insert(r.Value());
    // 可写且互不重叠的粗验证：各块写标记后回读。
    *reinterpret_cast<uint64_t*>(a.Value()->OffsetToPtr(r.Value())) =
        r.Value();
  }
  for (auto off : seen)
    EXPECT_EQ(*reinterpret_cast<const uint64_t*>(a.Value()->OffsetToPtr(off)),
              off);
}

TEST_F(SlabTest, UsedBytesAccountsFreelist) {
  auto a = Attach(true);
  ASSERT_TRUE(a.IsOk());
  EXPECT_EQ(a.Value()->used_bytes(), 0u);

  auto r = a.Value()->Allocate(4096);
  ASSERT_TRUE(r.IsOk());
  EXPECT_GT(a.Value()->used_bytes(), 0u);

  EXPECT_EQ(a.Value()->Deallocate(r.Value()), rtdb::Err::kOk);
  EXPECT_EQ(a.Value()->used_bytes(), 0u);  // 全回收归零
}

}  // namespace
