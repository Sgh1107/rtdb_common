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
/// 用法三：rtdb_unit_tests --rtdb-child-w5-snap <image_path>
///   W5 冷快照崩溃注入子进程（docs/06 §3 注入矩阵 snapshot 中点）：
///   执行脚本化负载（建表 → 3 行插入 → CreateSnapshot），在
///   RTDB_FAILPOINTS 指定的注入点（snapshot.before_copy）硬退出。
///
/// 用法四：rtdb_unit_tests --rtdb-child-w5-alter <image_path>
///   W5 在线加列崩溃注入子进程（docs/06 §3 注入矩阵 alter 中点）：
///   执行脚本化负载（建表 → 3 行插入 → AlterTableAddColumn），在
///   RTDB_FAILPOINTS 指定的注入点（alter.before_link）硬退出。
///
/// 用法五：rtdb_unit_tests --rtdb-child-w5-secidx <image_path>
///   W5 二级索引崩溃注入子进程（docs/06 §3 注入矩阵 secidx 中点）：
///   执行脚本化负载（建表 → 3 行插入 → CreateSecondaryIndex），在
///   RTDB_FAILPOINTS 指定的注入点（secidx.before_link）硬退出。
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

/// W5 冷快照崩溃注入子进程：脚本化负载（建表 "t"：id i64 主键 + val i64，
/// 3 行插入，随后 CreateSnapshot）。注入时序同 WAL 子进程：先解除注入
/// 完成建表与行写，再经 ResetFromEnv 恢复规格，只作用于快照阶段。
int RunW5SnapChild(const std::string& file_path) {
    failpoint::ClearAll();  // 触发 env 解析后整体解除，保前置阶段无注入
    rtdb::Options opts;
    opts.instance_name = "w5_snap_child";
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
    for (int64_t i = 1; i <= 3; ++i) {
        auto ins =
            eng->Insert(table, {rtdb::ColumnValue::Int(i), rtdb::ColumnValue::Int(i * 10)});
        if (!ins.IsOk()) return 5;
    }

    failpoint::ResetFromEnv();  // 恢复注入，只作用于快照阶段
    auto snap = eng->CreateSnapshot();
    if (!snap.IsOk()) return 6;  // 注入点未命中却失败 → 子进程异常
    return 0;
}

/// W5 在线加列崩溃注入子进程：脚本化负载（建表 "t"：id i64 主键 + val i64，
/// 3 行插入，随后 AlterTableAddColumn 追加 tag(i64, 默认 42)）。
/// 注入时序同 WAL 子进程：先解除注入完成前置阶段，再经 ResetFromEnv
/// 恢复规格，只作用于加列阶段。
int RunW5AlterChild(const std::string& file_path) {
    failpoint::ClearAll();  // 触发 env 解析后整体解除，保前置阶段无注入
    rtdb::Options opts;
    opts.instance_name = "w5_alter_child";
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
    for (int64_t i = 1; i <= 3; ++i) {
        auto ins =
            eng->Insert(table, {rtdb::ColumnValue::Int(i), rtdb::ColumnValue::Int(i * 10)});
        if (!ins.IsOk()) return 5;
    }

    failpoint::ResetFromEnv();  // 恢复注入，只作用于加列阶段
    rtdb::ColumnDef col;
    col.name = "tag";
    col.type = rtdb::DataType::kInt64;
    col.nullable = true;
    col.has_default = true;
    col.default_value.is_null = false;
    col.default_value.int_value = 42;
    auto add = eng->AlterTableAddColumn(table, col);
    if (!add.IsOk()) return 6;  // 注入点未命中却失败 → 子进程异常
    return 0;
}

/// W5 二级索引崩溃注入子进程：脚本化负载（建表 "t"：id i64 主键 + val i64，
/// 3 行插入，随后在 val 列创建非唯一二级索引 "idx_val"）。
/// 注入时序同 WAL 子进程：先解除注入完成前置阶段，再经 ResetFromEnv
/// 恢复规格，只作用于建索引阶段。
int RunW5SecIdxChild(const std::string& file_path) {
    failpoint::ClearAll();  // 触发 env 解析后整体解除，保前置阶段无注入
    rtdb::Options opts;
    opts.instance_name = "w5_secidx_child";
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
    for (int64_t i = 1; i <= 3; ++i) {
        auto ins =
            eng->Insert(table, {rtdb::ColumnValue::Int(i), rtdb::ColumnValue::Int(i * 10)});
        if (!ins.IsOk()) return 5;
    }

    failpoint::ResetFromEnv();  // 恢复注入，只作用于建索引阶段
    rtdb::SecondaryIndexDef idx;
    idx.name = "idx_val";
    idx.column_index = 1;
    idx.unique = false;
    auto cr = eng->CreateSecondaryIndex(table, idx);
    if (!cr.IsOk()) return 6;  // 注入点未命中却失败 → 子进程异常
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == rtdb_test::kCrashLockFlag)
        return RunCrashLockChild(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == rtdb_test::kWalCrashFlag)
        return RunWalCrashChild(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == rtdb_test::kW5SnapFlag)
        return RunW5SnapChild(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == rtdb_test::kW5AlterFlag)
        return RunW5AlterChild(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == rtdb_test::kW5SecIdxFlag)
        return RunW5SecIdxChild(argv[2]);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
