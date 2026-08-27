/// ShmRootMutex 全路径单测：
///   - 同进程互斥与 TryLock 语义；
///   - 多线程争用序列化正确性；
///   - 跨进程"持有者硬退出 → 新会话抢锁恢复"演示（M0 出口项）。

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "infra/shm_mutex.hpp"
#include "platform/platform_process.hpp"
#include "rtdb/engine.hpp"
#include "tests/unit/spawn_self.hpp"

namespace fs = std::filesystem;

namespace {

namespace infra = rtdb::infra;

/// 64B 缓冲充当共享内存锁状态字区。
class MutexEnv : public ::testing::Test {
 protected:
  void SetUp() override {
    std::memset(buf_, 0, sizeof(buf_));
    infra::ShmRootMutex::InitState(State());
    m_ = std::make_unique<infra::ShmRootMutex>(State());
  }

  auto* State() { return reinterpret_cast<infra::shm_RootMutexState*>(buf_); }

  alignas(16) unsigned char buf_[64]{};
  std::unique_ptr<infra::ShmRootMutex> m_;
};

}  // namespace

TEST_F(MutexEnv, SameProcessMutualExclusion) {
  ASSERT_EQ(m_->Lock(1000), rtdb::Err::kOk);
  EXPECT_TRUE(m_->IsLockedBySelf());

  // 第二把句柄：同一状态字上必然拿不到。
  infra::ShmRootMutex m2(State());
  EXPECT_FALSE(m2.TryLock());
  EXPECT_EQ(m2.Lock(1), rtdb::Err::kTimeout);

  m_->Unlock();

  EXPECT_TRUE(m2.TryLock());
  EXPECT_TRUE(m2.IsLockedBySelf());
  m2.Unlock();
}

TEST_F(MutexEnv, TryLockIsPureFastPath) {
  ASSERT_TRUE(m_->TryLock());
  // TryLock 不做死亡恢复也不阻塞，即便持有人是自己。
  infra::ShmRootMutex m2(State());
  EXPECT_FALSE(m2.TryLock());
  m_->Unlock();
  EXPECT_TRUE(m2.TryLock());  // 空闲即得
  m2.Unlock();
}

TEST_F(MutexEnv, ThreadedContentionSerializes) {
  constexpr int kThreads = 4;
  constexpr int kIters = 25000;

  uint64_t shared = 0;          // 非原子：靠锁保护
  std::atomic<int> depth{0};    // 并发重叠深度
  std::atomic<int> max_depth{0};
  std::atomic<int> lock_failures{0};

  std::vector<std::thread> pool;
  for (int t = 0; t < kThreads; ++t) {
    pool.emplace_back([&] {
      for (int i = 0; i < kIters; ++i) {
        if (m_->Lock(5000) != rtdb::Err::kOk) {
          lock_failures.fetch_add(1);
          continue;
        }
        // —— 临界区开始：若互斥被破坏，深度必然出现 >1 ——
        const int d = depth.fetch_add(1, std::memory_order_acq_rel) + 1;
        int cur = max_depth.load(std::memory_order_relaxed);
        while (d > cur &&
               !max_depth.compare_exchange_weak(cur, d,
                                                std::memory_order_relaxed)) {
        }
        shared += 1;
        depth.fetch_sub(1, std::memory_order_acq_rel);
        // —— 临界区结束 ——
        m_->Unlock();
      }
    });
  }
  for (auto& th : pool) th.join();

  EXPECT_EQ(lock_failures.load(), 0);
  EXPECT_EQ(max_depth.load(), 1);  // 锁语义：临界区永不并发
  EXPECT_EQ(shared, static_cast<uint64_t>(kThreads * kIters));
}

// ---------------------------------------------------------------------------
// 跨进程死亡恢复（M0 验收出口："两进程锁互斥+死亡恢复演示通过"）
// ---------------------------------------------------------------------------
TEST(ShmMutexDeathRecovery, RecoversAfterHolderHardKill) {
  namespace platform = rtdb::platform;
  const std::string exe = platform::HostExecutablePath();
  if (exe.empty()) GTEST_SKIP() << "当前平台无法定位自身可执行文件";

  const std::string img =
      (fs::temp_directory_path() /
       ("rtdb_crash_lock_" + std::to_string(platform::CurrentProcessId()) +
        ".rtdb"))
          .string();
  std::error_code ec;
  fs::remove(img, ec);

  rtdb::Options opts;
  opts.instance_name = "crash_recovery";
  opts.file_path = img;
  opts.initial_size_bytes = 1024 * 1024;

  {  // 先建好映像
    auto res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(res.IsOk());
  }

  // 子进程携锁 hard-exit(9)。
  const auto out = rtdb_test::SpawnSelfWithFlag(exe, img);
  ASSERT_TRUE(out.spawned);
  EXPECT_EQ(out.exit_code, 9);

  // 新会话挂载后必须能在短时间内恢复残锁（若不恢复将 kTimeout）。
  auto res = rtdb::Engine::Open(opts);
  ASSERT_TRUE(res.IsOk()) << "子进程异常退出后映像不可用";
  auto eng = res.TakeValue();
  auto* mutex = eng->RawRootMutex();
  ASSERT_NE(mutex, nullptr);

  EXPECT_EQ(mutex->Lock(4000), rtdb::Err::kOk);  // 死亡恢复路径
  EXPECT_TRUE(mutex->IsLockedBySelf());

  // 恢复者继续正常使用：加解锁闭环。
  eng->UnlockRoot();
  EXPECT_EQ(mutex->Lock(500), rtdb::Err::kOk);
  eng->UnlockRoot();

  fs::remove(img, ec);
}
