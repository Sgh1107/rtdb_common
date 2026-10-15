/// gtest 主入口 + 跨进程崩溃演示的"子进程占位"模式。
///
/// 用法一：rtdb_unit_tests --rtdb-child-crash-lock <image_path>
///   子进程：打开实例 → 持有根锁 → quick_exit(9) 硬退出（不经过任何
///   析构/解锁路径，等价 kill -9 的持有态残留），供父进程验证恢复。
///
/// 用法二：rtdb_unit_tests --rtdb-child-wal-crash <image_path>
///   W4 WAL 崩溃注入子进程（docs/06 §3 注入矩阵）：执行脚本化写负载
///   （建表 → 5 行单条插入 → 6 行原子批），在 RTDB_FAILPOINTS 指定的
///   注入点硬退出（failpoint kCrash 模式，quick_exit(9)）；未命中则
///   正常返回 0。父进程负责设置/清除环境变量并重开映像验证恢复语义。
///
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "infra/failpoint.hpp"
#include "rtdb/engine.hpp"
#include "tests/unit/spawn_self.hpp"

namespace {

namespace failpoint = rtdb::failpoint;
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

/// WAL 崩溃注入子进程：脚本化负载（建表 "t"：id i64 主键 + val i64）。
///
/// 注入时序：先解除注入完成建表（DDL 不属于崩溃场景），再经
/// ResetFromEnv 恢复规格注入，只作用于行写负载。
int RunWalCrashChild(const std::string& file_path) {
    failpoint::ClearAll();  // 触发 env 解析后整体解除，保 DDL 阶段无注入
    rtdb::Options opts;
    opts.instance_name = "wal_crash_child";
    opts.file_path = file_path;
    opts.initial_size_bytes = 2 * 1024 * 1024;
    auto res = rtdb::Engine::Open(opts);
    if (!res.IsOk()) return 3;
    auto eng = res.TakeValue();

    rtdb::TableDef def;
    def.name = "t";
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
    auto t = eng->CreateTable(def);
    if (!t.IsOk()) return 4;
    const rtdb::TableHandle table = t.Value();

    failpoint::ResetFromEnv();  // 恢复注入，只作用于以下行写负载

    for (int64_t i = 1; i <= 5; ++i) {
        auto ins =
            eng->Insert(table, {rtdb::ColumnValue::Int(i), rtdb::ColumnValue::Int(i * 10)});
        if (!ins.IsOk()) return 5;
    }

    std::vector<rtdb::Engine::BatchOp> batch;
    for (int64_t i = 100; i <= 105; ++i) {
        rtdb::Engine::BatchOp op;
        op.type = rtdb::Engine::BatchOp::Type::kInsert;
        op.table = table;
        op.row = {rtdb::ColumnValue::Int(i), rtdb::ColumnValue::Int(i)};
        batch.push_back(std::move(op));
    }
    if (!eng->ExecuteBatch(batch).IsOk()) return 6;
    return 0;  // 注入点未命中，正常完成
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == rtdb_test::kCrashLockFlag)
        return RunCrashLockChild(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == rtdb_test::kWalCrashFlag)
        return RunWalCrashChild(argv[2]);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
