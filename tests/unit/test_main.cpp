/// gtest 主入口 + 跨进程死亡恢复演示的"子进程锁占位"模式。
///
/// 用法：rtdb_unit_tests --rtdb-child-crash-lock <image_path>
/// 子进程：打开实例 → 持有根锁 → quick_exit(9) 硬退出（不经过任何
/// 析构/解锁路径，等价 kill -9 的持有态残留），供父进程验证恢复。

#include <cstdio>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "rtdb/engine.hpp"
#include "tests/unit/spawn_self.hpp"

namespace {

int RunCrashLockChild(const std::string& file_path) {
    rtdb::Options opts;
    opts.instance_name = "crash_child";
    opts.file_path = file_path;
    opts.initial_size_bytes = 1024 * 1024;
    auto res = rtdb::Engine::Open(opts);
    if (!res.IsOk()) return 3;
    auto eng = res.TakeValue();
    if (!IsOk(eng->LockRoot(5000))) return 4;  // 必须拿到才算占位成功
    std::quick_exit(9);                        // 携锁死亡
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == rtdb_test::kCrashLockFlag)
        return RunCrashLockChild(argv[2]);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
