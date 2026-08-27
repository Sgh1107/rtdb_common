/// SharedBTree 全路径单测（宿主堆缓冲模拟映射区）：
/// 插入/点查/唯一约束、范围游标开闭区间、惰性删除后结构不变量、
/// 大规模分裂压力、重附着持久一致、字符串键派生树。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "infra/shared_btree.hpp"

namespace {

namespace infra = rtdb::infra;

template <uint64_t RegionBytes>
struct HostRegion {
  HostRegion() : mem(static_cast<char*>(::operator new(
                     RegionBytes, std::align_val_t(4096)))) {
    std::memset(mem, 0, RegionBytes);
  }
  ~HostRegion() { ::operator delete(mem, std::align_val_t(4096)); }
  char* mem;
};

using IntTree = infra::SharedBTree<8, uint64_t>;

void EncodeKey(int64_t v, void* out) { infra::EncodeOrderedInt64(v, static_cast<char*>(out)); }
int64_t DecodeKey(const void* k) { return infra::DecodeOrderedInt64(static_cast<const char*>(k)); }

/// 4MB 空间 + 已附着的分配器 + 一棵活树。
struct BTreeEnv {
  static constexpr uint64_t kBytes = 4ull << 20;

  BTreeEnv() : region(),
               alloc(
                   infra::SlabShmAllocator::Attach(region.mem, kBytes, true)
                       .TakeValue()) {}

  IntTree MakeTree(IntTree::Slot* slot) const noexcept {
    return IntTree(region.mem, alloc.get(), slot);
  }
  std::unique_ptr<infra::SlabShmAllocator> Reattach() {
    return infra::SlabShmAllocator::Attach(region.mem, kBytes, false)
        .TakeValue();
  }

  HostRegion<kBytes> region;
  std::unique_ptr<infra::SlabShmAllocator> alloc;
};

// ---------------------------------------------------------------------------
// 基本插入/点查/唯一性
// ---------------------------------------------------------------------------
TEST(SharedBTreeInt, ModelDiffProbe) {
  // 模型对照：找出首个导致树内容偏离 std::map 的插入。
  constexpr int N = 512;
  BTreeEnv env;
  IntTree::Slot slot{0, 0};
  auto tree = env.MakeTree(&slot);
  std::map<int, int> ref;

  std::vector<int> idx(N);
  for (int i = 0; i < N; ++i) idx[i] = i;
  std::mt19937_64 rng(42);
  std::shuffle(idx.begin(), idx.end(), rng);

  char kb[8], hb[8];
  EncodeKey(0, kb);
  EncodeKey(N * 3 + 9, hb);

  for (int step = 0; step < N; ++step) {
    const int k = idx[step];
    char enc[8];
    EncodeKey(k, enc);
    const rtdb::Err rc = tree.Insert(enc, static_cast<uint64_t>(k));
    ASSERT_EQ(rc, rtdb::Err::kOk);
    ref[k] = k;

    // 一致性快查：总数一致 + 任取3个既有键可寻。
    if (tree.Size() != ref.size()) {
      std::printf("[model] SIZE DIVERGED at step=%d key=%d tree=%llu\n",
                  step, k,
                  static_cast<unsigned long long>(tree.Size()));
      FAIL();
    }
    uint64_t out = 0;
    const int probes[] = {ref.begin()->first, k};
    for (int p : probes) {
      char pk[8];
      EncodeKey(p, pk);
      if (!tree.Find(pk, &out)) {
        std::printf(
            "[model] FIND MISS at step=%d probed=%d lastKey=%d\n", step, p,
            k);
        FAIL();
      }
    }
    (void)kb;
    (void)hb;
  }
  SUCCEED();
}

TEST(SharedBTreeInt, InsertFindUniqueConstraint) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  constexpr int N = 512;
  BTreeEnv env;
  IntTree::Slot slot{0, 0};
  auto tree = env.MakeTree(&slot);

  std::vector<int> idx(N);
  for (int i = 0; i < N; ++i) idx[i] = i;
  std::mt19937_64 rng(42);
  std::shuffle(idx.begin(), idx.end(), rng);

  for (int done : idx) {
    char k[8];
    EncodeKey(done, k);
    ASSERT_EQ(tree.Insert(k, static_cast<uint64_t>(done) * 10), rtdb::Err::kOk)
        << "insert " << done;
    std::printf("[prog] preinsert=%d\n", done);
    {
      char pb[8];
      EncodeKey(done, pb);
      std::string tr;
      tree.DebugFindTrace(pb, &tr);   // 插入前路径回放（应未命中）
      const uint64_t pv = static_cast<uint64_t>(done) * 10;
      const rtdb::Err ie = tree.Insert(pb, pv);
      std::printf("[prog] inserted=%d err=%d trace=%s\n", done,
                  static_cast<int>(ie), tr.c_str());
    }
  }
  std::printf("[prog] insert-phase-done\n");
  EXPECT_EQ(tree.Size(), static_cast<uint64_t>(N));

  uint64_t out = 0;
  for (int i = 0; i < N; ++i) {
    char k[8];
    EncodeKey(i, k);
    ASSERT_TRUE(tree.Find(k, &out));
    EXPECT_EQ(out, static_cast<uint64_t>(i) * 10);
  }
  for (int i = N; i < N + 32; ++i) {  // 不存在的键
    char k[8];
    EncodeKey(i, k);
    EXPECT_FALSE(tree.Find(k, &out));
  }
  char dup_k[8];
  EncodeKey(N / 2, dup_k);
  EXPECT_EQ(tree.Insert(dup_k, 1u), rtdb::Err::kAlreadyExists);

  uint64_t visited = 0;
  EXPECT_EQ(tree.Validate(&visited), rtdb::Err::kOk);
}

// ---------------------------------------------------------------------------
// 范围游标：边界开闭组合与全序
// ---------------------------------------------------------------------------
TEST(SharedBTreeInt, CursorBoundCombinations) {
  constexpr int N = 1024;
  BTreeEnv env;
  IntTree::Slot slot{0, 0};
  auto tree = env.MakeTree(&slot);
  for (int i = 0; i < N; ++i) {
    char k[8];
    EncodeKey(i, k);
    ASSERT_EQ(tree.Insert(k, static_cast<uint64_t>(i)), rtdb::Err::kOk);
  }

  auto scan = [&](int lo, bool li, int hi, bool hinc) {
    char kb[8], hb[8];
    EncodeKey(lo, kb);
    EncodeKey(hi, hb);
    std::vector<int64_t> got;
    for (auto c = tree.SeekRange(kb, li, hb, hinc); c.Valid(); c.Next()) {
      char k[8];
      c.Key(k);
      got.push_back(DecodeKey(k));
    }
    return got;
  };

  // 全表扫描应升序完备。
  auto full = scan(0, true, N - 1, true);
  ASSERT_EQ(full.size(), static_cast<size_t>(N));
  for (int i = 0; i < N; ++i) EXPECT_EQ(full[i], i);

  // [200,800] 含两端。
  auto closed = scan(200, true, 800, true);
  ASSERT_EQ(closed.size(), 601u);
  EXPECT_EQ(closed.front(), 200);
  EXPECT_EQ(closed.back(), 800);

  // (200,800) 双开。
  auto open = scan(200, false, 800, false);
  ASSERT_EQ(open.size(), 599u);
  EXPECT_EQ(open.front(), 201);
  EXPECT_EQ(open.back(), 799);

  // 单元素闭闭区间。
  auto single = scan(517, true, 517, true);
  ASSERT_EQ(single.size(), 1u);
  EXPECT_EQ(single[0], 517);

  // 非法区间立即失效。
  EXPECT_TRUE(scan(700, true, 300, true).empty());
  EXPECT_TRUE(scan(517, false, 517, false).empty());

  uint64_t visited = 0;
  EXPECT_EQ(tree.Validate(&visited), rtdb::Err::kOk);
}

// ---------------------------------------------------------------------------
// 惰性删除：点查/计数/迭代/结构校验全保持一致
// ---------------------------------------------------------------------------
TEST(SharedBTreeInt, LazyRemoveKeepsInvariants) {
  constexpr int N = 1000;
  BTreeEnv env;
  IntTree::Slot slot{0, 0};
  auto tree = env.MakeTree(&slot);
  for (int i = 0; i < N; ++i) {
    char k[8];
    EncodeKey(i, k);
    ASSERT_EQ(tree.Insert(k, static_cast<uint64_t>(i)), rtdb::Err::kOk);
  }

  for (int i = 0; i < N; i += 2) {
    char k[8];
    EncodeKey(i, k);
    ASSERT_EQ(tree.Remove(k), rtdb::Err::kOk);
  }
  char k0[8];
  EncodeKey(0, k0);
  EXPECT_EQ(tree.Remove(k0), rtdb::Err::kNotFound);  // 不存在键

  EXPECT_EQ(tree.Size(), static_cast<uint64_t>(N / 2));

  uint64_t v = 0;
  for (int i = 1; i < N; i += 2) {
    char k[8];
    EncodeKey(i, k);
    ASSERT_TRUE(tree.Find(k, &v));
    EXPECT_EQ(v, static_cast<uint64_t>(i));
  }
  for (int i = 0; i < N; i += 2) {
    char k[8];
    EncodeKey(i, k);
    EXPECT_FALSE(tree.Find(k, &v));
  }

  // 全表游标：恰为奇数升序。
  char kb[8], hb[8];
  EncodeKey(0, kb);
  EncodeKey(N - 1, hb);
  int count = 0;
  for (auto c = tree.SeekRange(kb, true, hb, true); c.Valid(); c.Next()) {
    char cur[8];
    c.Key(cur);
    EXPECT_EQ(DecodeKey(cur), 2 * count + 1);
    ++count;
  }
  EXPECT_EQ(count, N / 2);

  uint64_t visited = 0;
  EXPECT_EQ(tree.Validate(&visited), rtdb::Err::kOk);
}

// ---------------------------------------------------------------------------
// 大规模随机分裂压力（多级树高、根升级、跨叶游标）
// ---------------------------------------------------------------------------
TEST(SharedBTreeInt, StressSplitsAndValidation) {
  constexpr int N = 30000;
  BTreeEnv env;
  IntTree::Slot slot{0, 0};
  auto tree = env.MakeTree(&slot);

  std::vector<int> idx(N);
  for (int i = 0; i < N; ++i) idx[i] = i;
  std::mt19937_64 rng(2026);
  std::shuffle(idx.begin(), idx.end(), rng);

  for (int n = 0; n < N; ++n) {
    char k[8];
    EncodeKey(idx[n], k);
    ASSERT_EQ(tree.Insert(k, static_cast<uint64_t>(idx[n])), rtdb::Err::kOk)
        << "at " << n;
    if ((n % 7500) == 7499) {  // 周期性结构校验
      uint64_t visited = 0;
      ASSERT_EQ(tree.Validate(&visited), rtdb::Err::kOk) << "after " << n + 1;
      EXPECT_EQ(tree.Size(), static_cast<uint64_t>(n + 1));
    }
  }
  uint64_t visited = 0;
  EXPECT_EQ(tree.Validate(&visited), rtdb::Err::kOk);
  EXPECT_GT(visited, static_cast<uint64_t>(N / 4));  // 节点数远超单叶

  std::mt19937_64 spot(7);
  uint64_t out = 0;
  for (int t = 0; t < 2000; ++t) {
    const int key = static_cast<int>(spot() % N);
    char k[8];
    EncodeKey(key, k);
    ASSERT_TRUE(tree.Find(k, &out));
    EXPECT_EQ(out, static_cast<uint64_t>(key));
  }
}

// ---------------------------------------------------------------------------
// 重附着持久一致（等价进程重启后共享内存内容原样可用）
// ---------------------------------------------------------------------------
TEST(SharedBTreeInt, ReattachSeesSameContent) {
  constexpr int N = 400;
  BTreeEnv env;
  IntTree::Slot slot{0, 0};
  {
    auto tree = env.MakeTree(&slot);
    for (int i = 0; i < N; ++i) {
      char k[8];
      EncodeKey(i * 3, k);  // 跳跃键，覆盖非满叶场景
      ASSERT_EQ(tree.Insert(k, static_cast<uint64_t>(i)), rtdb::Err::kOk);
    }
    uint64_t visited = 0;
    const rtdb::Err vrc = tree.Validate(&visited);
    std::printf("[diag] before-reopen validate=%d visited=%llu size=%llu\n",
                static_cast<int>(vrc),
                static_cast<unsigned long long>(visited),
                static_cast<unsigned long long>(tree.Size()));
  }

  // 模拟重启：分配器重新走校验路径附着。
  auto alloc2 = env.Reattach();
  IntTree::Slot slot2 = slot;
  IntTree tree2(env.region.mem, alloc2.get(), &slot2);

  {
    uint64_t visited = 0;
    const rtdb::Err vrc2 = tree2.Validate(&visited);
    std::printf("[diag] after-reopen validate=%d visited=%llu size=%llu\n",
                static_cast<int>(vrc2),
                static_cast<unsigned long long>(visited),
                static_cast<unsigned long long>(tree2.Size()));
  }

  EXPECT_EQ(tree2.Size(), static_cast<uint64_t>(N));
  std::vector<int> missing;
  uint64_t out = 0;
  for (int i = 0; i < N; ++i) {
    char k[8];
    EncodeKey(i * 3, k);
    if (!tree2.Find(k, &out)) missing.push_back(i);
  }
  if (!missing.empty()) {
    std::string trace;
    char probe[8];
    EncodeKey(missing.front() * 3, probe);
    tree2.DebugFindTrace(probe, &trace);
    std::printf("[diag] missing=%zu first={idx=%d key=%d} trace=%s\n",
                missing.size(), missing.front(), missing.front() * 3,
                trace.c_str());
    // 游标全表计数对照
    int seen = 0;
    char kb2[8], hb2[8];
    EncodeKey(0, kb2);
    EncodeKey(N * 3 - 1, hb2);
    for (auto c = tree2.SeekRange(kb2, true, hb2, true); c.Valid(); c.Next()) {
      ++seen;
    }
    std::printf("[diag] cursor-full-scan count=%d (expect %d)\n", seen, N);
  }
  ASSERT_TRUE(missing.empty());
  uint64_t visited = 0;
  EXPECT_EQ(tree2.Validate(&visited), rtdb::Err::kOk);
}

// ---------------------------------------------------------------------------
// 字符串键派生树（StringBTree 布局）冒烟：补零定长 → memcmp 即字典序
// ---------------------------------------------------------------------------
TEST(SharedBTreeString, LexicographicOrderSmoke) {
  using StrTree = infra::SharedBTree<64, uint64_t>;
  BTreeEnv env;
  StrTree::Slot slot{0, 0};
  StrTree tree(env.region.mem, env.alloc.get(), &slot);

  constexpr int N = 128;
  std::vector<std::string> keys;
  for (int i = 0; i < N; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key_%04d", i);
    keys.emplace_back(buf);
  }
  auto order = keys;
  std::mt19937_64 rng(9);
  std::shuffle(order.begin(), order.end(), rng);

  for (const auto& s : order) {
    char k[64]{};
    std::memcpy(k, s.data(), s.size());
    ASSERT_EQ(tree.Insert(k, s.size()), rtdb::Err::kOk);
  }

  // 右上界设为全 0xFF（补零键中为"最大"），扫描应完整覆盖字典序。
  char kb[64] = {};
  char hb[64];
  std::memset(hb, 0xFF, sizeof(hb));
  std::snprintf(kb, sizeof(kb), "%s", keys.front().c_str());
  size_t cnt = 0;
  const std::string* expect = keys.data();
  for (auto c = tree.SeekRange(kb, true, hb, true); c.Valid(); c.Next()) {
    char cur[64];
    std::memset(cur, 0, sizeof(cur));
    c.Key(cur);
    EXPECT_STREQ(cur, expect->c_str());
    EXPECT_EQ(c.Value(), expect->size());
    ++expect;
    ++cnt;
  }
  EXPECT_EQ(cnt, static_cast<size_t>(N));
}

}  // namespace

