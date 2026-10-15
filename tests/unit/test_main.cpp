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
/// 用法六：rtdb_unit_tests --rtdb-child-chaos <image> <scenario> <fsync>
///   W6 混沌写负载子进程（scripts/chaos_m1.sh 驱动，docs/06 §3 矩阵）：
///   建表 + 播种 5 行 → 武装注入 → 按场景执行负载，在 RTDB_FAILPOINTS
///   指定注入点硬退出；未命中则完整完成返回 0。
///   scenario ∈ {insert, batch, alter, snapshot, secidx}；
///   fsync ∈ {always, per_second, never}。
///
/// 用法七：rtdb_unit_tests --rtdb-verify-chaos <image> <scenario>
///   W6 混沌验证子进程：重开映像校验 docs/06 §3 的 (a)(b)(c) 与 M1
///   出口标准（committed_seq 一致 / 无半截记录 / 数据零丢失 / 恢复
///   幂等 / 崩溃后可写），退出码 0 = 全部不变量成立。
///
/// 用法八：rtdb_unit_tests --rtdb-bench <image> <rows>
///   W6 内部 bench（docs/05 §4 量级确认，docs/08 §3-W6-b）：
///   单行 upsert P99（per_second/always 两档）、主键点查 P99、
///   原子批吞吐、范围扫描速率、WAL 重放速率（多轮重开累计）。
///   M1 边界：WAL 环为单块 64KB（slab 最大级），重放样本以环内容为限。

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

#if defined(__GNUC__) && !defined(__clang__)
extern "C" void __gcov_dump(void) __attribute__((weak));
#endif

/// 硬退出（kill -9 等价）：与 std::quick_exit 同为「不析构、不跑
/// atexit」，另在 gcov 插桩构建（MinGW）下先主动 __gcov_dump 再 _Exit，
/// 规避 libgcc 退出转储链触发的 UCRT fast-fail（详见 failpoint.cpp
/// detail::HardExit 注释）。weak 引用：普通构建下为 null，零影响。
[[noreturn]] void HardExitForTest(int code) noexcept {
#if defined(__GNUC__) && !defined(__clang__)
    if (__gcov_dump) __gcov_dump();
#endif
    std::fflush(nullptr);
    std::_Exit(code);
}

int RunCrashLockChild(const std::string& file_path) {
    rtdb::Options opts;
    opts.instance_name = "crash_child";
    opts.file_path = file_path;
    opts.initial_size_bytes = 1024 * 1024;
    auto res = rtdb::Engine::Open(opts);
    if (!res.IsOk()) return 3;
    auto eng = res.TakeValue();
    if (!IsOk(eng->LockRoot(5000))) return 4;  // 必须拿到才算占位成功
    HardExitForTest(9);                        // 携锁死亡
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

/// W6 混沌写负载子进程（scripts/chaos_m1.sh 驱动，docs/06 §3 注入矩阵）。
///
/// 注入时序：先 ClearAll 解除（保 Open/建表/播种无注入），完成播种后
/// ResetFromEnv 恢复规格，注入只作用于场景负载。播种的 5 行必然已提交，
/// 是验证侧"数据零丢失"的基准集。
int RunChaosWriteChild(const std::string& file_path, const std::string& scenario,
                       const std::string& fsync_mode) {
    failpoint::ClearAll();  // 触发 env 解析后整体解除，保前置阶段无注入
    rtdb::Options opts;
    opts.instance_name = "chaos_child";
    opts.file_path = file_path;
    opts.initial_size_bytes = 2 * 1024 * 1024;
    opts.role = rtdb::Role::Standalone;
    if (fsync_mode == "always") {
        opts.fsync = rtdb::FsyncPolicy::Always;
    } else if (fsync_mode == "never") {
        opts.fsync = rtdb::FsyncPolicy::Never;
    } else {
        opts.fsync = rtdb::FsyncPolicy::PerSecond;
    }
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
    constexpr int64_t kSeed = 5;
    for (int64_t i = 1; i <= kSeed; ++i) {
        auto ins =
            eng->Insert(table, {rtdb::ColumnValue::Int(i), rtdb::ColumnValue::Int(i * 10)});
        if (!ins.IsOk()) return 5;
    }

    failpoint::ResetFromEnv();  // 恢复规格注入，只作用于以下场景负载

    const auto row = [](int64_t i) {
        return std::vector<rtdb::ColumnValue>{rtdb::ColumnValue::Int(i),
                                              rtdb::ColumnValue::Int(i * 10)};
    };
    if (scenario == "insert") {
        for (int64_t i = kSeed + 1; i <= kSeed + 15; ++i) {
            if (!eng->Insert(table, row(i)).IsOk()) return 6;
        }
    } else if (scenario == "batch") {
        std::vector<rtdb::Engine::BatchOp> batch;
        for (int64_t i = kSeed + 1; i <= kSeed + 10; ++i) {
            rtdb::Engine::BatchOp op;
            op.type = rtdb::Engine::BatchOp::Type::kInsert;
            op.table = table;
            op.row = row(i);
            batch.push_back(std::move(op));
        }
        if (!eng->ExecuteBatch(batch).IsOk()) return 7;
    } else if (scenario == "alter") {
        rtdb::ColumnDef col;
        col.name = "tag";
        col.type = rtdb::DataType::kInt64;
        col.nullable = true;
        col.has_default = true;
        col.default_value.is_null = false;
        col.default_value.int_value = 42;
        if (!eng->AlterTableAddColumn(table, col).IsOk()) return 8;
        // 加列成功后继续写（混合路径：新句柄 + 3 列行）。
        auto nh = eng->OpenTable("t");
        if (!nh.IsOk()) return 9;
        for (int64_t i = kSeed + 1; i <= kSeed + 3; ++i) {
            if (!eng->Insert(nh.Value(), {rtdb::ColumnValue::Int(i),
                                          rtdb::ColumnValue::Int(i * 10),
                                          rtdb::ColumnValue::Int(i)})
                     .IsOk()) {
                return 10;
            }
        }
    } else if (scenario == "snapshot") {
        if (!eng->CreateSnapshot().IsOk()) return 11;
        for (int64_t i = kSeed + 1; i <= kSeed + 3; ++i) {
            if (!eng->Insert(table, row(i)).IsOk()) return 12;
        }
    } else if (scenario == "secidx") {
        rtdb::SecondaryIndexDef idx;
        idx.name = "idx_val";
        idx.column_index = 1;
        idx.unique = false;
        if (!eng->CreateSecondaryIndex(table, idx).IsOk()) return 13;
        for (int64_t i = kSeed + 1; i <= kSeed + 3; ++i) {
            if (!eng->Insert(table, row(i)).IsOk()) return 14;
        }
    } else {
        return 2;  // 未知场景
    }
    return 0;  // 注入点未命中，负载完整完成
}

/// W6 混沌验证子进程（docs/06 §3 (a)(b)(c) + M1 出口标准）。
///
/// 不变量（每轮崩溃后必须全部成立）：
///  (a) committed_seq 一致：≥ 播种规模（DDL + 5 行），且二次重开不变；
///  (b) 无半截记录：播种行逐行可读且值完整（val == id × 10）；
///  (c) 数据零丢失：可读 id 集合是 1..N 的连续前缀（顺序写无空洞）；
///  附加：schema 与场景自洽（alter ∈ {2,3} 列）、引擎崩溃后可写可读、
///       secidx 场景索引要么不存在要么可查、snapshot 场景副本（若存在）
///       必须是可独立挂载的一致性点映像。
int RunChaosVerifyChild(const std::string& file_path, const std::string& scenario) {
    constexpr int64_t kSeed = 5;
    rtdb::Options opts;
    opts.instance_name = "chaos_verify";
    opts.file_path = file_path;
    opts.initial_size_bytes = 2 * 1024 * 1024;
    opts.role = rtdb::Role::Standalone;
    opts.fsync = rtdb::FsyncPolicy::PerSecond;

    int failures = 0;
    const auto check = [&failures](bool ok, const char* what) {
        std::printf("CHAOS_CHECK %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };

    auto open = rtdb::Engine::Open(opts);
    if (!open.IsOk()) {
        std::printf("CHAOS_VERIFY_FAIL open: %s\n", rtdb::ErrorMessage(open.Error()));
        return 1;
    }
    auto eng = open.TakeValue();

    // (a) committed_seq 一致：不低于播种规模（建表 DDL + 5 行）。
    check(eng->CommittedSeq() >= 1 + kSeed, "committed_seq_at_least_seed");

    auto t_res = eng->OpenTable("t");
    if (!t_res.IsOk()) {
        std::printf("CHAOS_VERIFY_FAIL opentable: %s\n", rtdb::ErrorMessage(t_res.Error()));
        return 1;
    }
    const rtdb::TableHandle t = t_res.Value();

    // (b) 无半截记录：播种行逐行可读且值完整。
    bool seed_ok = true;
    for (int64_t i = 1; i <= kSeed; ++i) {
        auto got = eng->GetByKey(t, rtdb::ColumnValue::Int(i));
        if (!got.IsOk() || got.Value().size() < 2 || got.Value()[1].as_int() != i * 10) {
            seed_ok = false;
            std::printf("CHAOS_DETAIL seed_row_broken id=%lld\n", static_cast<long long>(i));
        }
    }
    check(seed_ok, "seed_rows_intact");

    // (c) 数据零丢失：可读 id 为 1..N 连续前缀，且无撕裂值。
    auto all = eng->ScanAll(t);
    if (!all.IsOk()) {
        std::printf("CHAOS_VERIFY_FAIL scan: %s\n", rtdb::ErrorMessage(all.Error()));
        return 1;
    }
    bool prefix_ok = true;
    bool values_ok = true;
    int64_t expect_id = 1;
    for (const auto& r : all.Value()) {
        const int64_t id = r[0].as_int();
        if (id != expect_id) {
            prefix_ok = false;
            std::printf("CHAOS_DETAIL id_gap id=%lld expect=%lld\n",
                        static_cast<long long>(id), static_cast<long long>(expect_id));
        }
        if (r.size() < 2 || r[1].as_int() != id * 10) {
            values_ok = false;
            std::printf("CHAOS_DETAIL torn_value id=%lld\n", static_cast<long long>(id));
        }
        ++expect_id;
    }
    check(prefix_ok, "id_prefix_no_holes");
    check(values_ok, "no_torn_values");

    // schema 与场景自洽（alter 场景：崩溃于挂链前 = 2 列，无注入轮次
    // 加列成功 = 3 列，两者皆合法；其余场景恒 2 列）。
    if (scenario == "alter") {
        check(t.visible_column_count() == 2 || t.visible_column_count() == 3,
              "alter_schema_consistent");
    } else {
        check(t.visible_column_count() == 2, "schema_unchanged");
    }

    // 恢复幂等：二次重开 committed_seq 与行集不变。
    const int64_t seq1 = eng->CommittedSeq();
    const size_t rows1 = all.Value().size();
    eng.reset();
    auto open2 = rtdb::Engine::Open(opts);
    if (!open2.IsOk()) {
        std::printf("CHAOS_VERIFY_FAIL reopen: %s\n", rtdb::ErrorMessage(open2.Error()));
        return 1;
    }
    auto eng2 = open2.TakeValue();
    check(eng2->CommittedSeq() == seq1, "recovery_idempotent_seq");
    auto t2_res = eng2->OpenTable("t");
    if (!t2_res.IsOk()) {
        std::printf("CHAOS_VERIFY_FAIL reopen_table: %s\n",
                    rtdb::ErrorMessage(t2_res.Error()));
        return 1;
    }
    const rtdb::TableHandle t2 = t2_res.Value();
    auto all2 = eng2->ScanAll(t2);
    check(all2.IsOk() && all2.Value().size() == rows1, "recovery_idempotent_rows");

    // 崩溃后可写性（自愈闭环）。
    const auto ins = eng2->Insert(t2, {rtdb::ColumnValue::Int(90001),
                                       rtdb::ColumnValue::Int(4242)});
    check(ins.IsOk(), "engine_writable_after_crash");
    if (ins.IsOk()) {
        auto got = eng2->GetByKey(t2, rtdb::ColumnValue::Int(90001));
        check(got.IsOk() && got.Value().size() >= 2 && got.Value()[1].as_int() == 4242,
              "post_crash_write_readable");
    }

    // 场景附加校验。
    if (scenario == "secidx") {
        auto q = eng2->GetBySecondary(t2, "idx_val", rtdb::ColumnValue::Int(10));
        // 索引要么不存在（崩溃于挂链前），要么存在且同值可查。
        check(!q.IsOk() ? q.Error() == rtdb::Err::kNotFound : !q.Value().empty(),
              "secidx_absent_or_consistent");
    }
    if (scenario == "snapshot") {
        const std::string snap = file_path + ".snap";
        std::error_code ec;
        if (std::filesystem::exists(snap, ec)) {
            // 副本须为可独立挂载的一致性点映像（换实例名，防命名映射混淆）。
            rtdb::Options sopts;
            sopts.instance_name = "chaos_verify_snap";
            sopts.file_path = snap;
            sopts.initial_size_bytes = 2 * 1024 * 1024;
            auto sopen = rtdb::Engine::Open(sopts);
            check(sopen.IsOk(), "snapshot_copy_loadable");
            if (sopen.IsOk()) {
                auto seng = sopen.TakeValue();
                auto st = seng->OpenTable("t");
                bool snap_rows = st.IsOk();
                if (st.IsOk()) {
                    for (int64_t i = 1; i <= kSeed; ++i) {
                        auto g = seng->GetByKey(st.Value(), rtdb::ColumnValue::Int(i));
                        if (!g.IsOk() || g.Value().size() < 2 || g.Value()[1].as_int() != i * 10) {
                            snap_rows = false;
                        }
                    }
                }
                check(snap_rows, "snapshot_copy_rows_intact");
            }
        } else {
            std::printf("CHAOS_CHECK PASS snapshot_absent_pre_copy\n");
        }
    }

    std::printf("CHAOS_VERIFY_%s failures=%d\n", failures == 0 ? "OK" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

/// W6 内部 bench（docs/05 §4 量级确认，docs/08 §3-W6-b）。
///
/// 指标与基线（docs/05 §4 单机 i7/32G 参考值，M1 只做量级确认）：
///  - 单行 upsert P99 ≤ 10μs（fsync=per_second，热路径不含 fsync）；
///  - 主键点查 P99 ≤ 3μs；
///  - 范围扫描 10 万行 ≤ 30ms（本 bench 按实测行数线性外报）；
///  - WAL 重放速率（基线 1GB 日志 ≤ 60s → ~200k 行/s 量级）。
/// M1 边界：WAL 环为单块 64KB（slab 最大级），重放样本以环内容为限，
/// 经多轮重开累计；行槽上限 8192 且行数据区 ≤64KB，单表 bench 行数
/// 受 rec_fixed_len × row_cap ≤ 64KB 约束。
int RunBenchChild(const std::string& file_path, uint32_t rows) {
    namespace ch = std::chrono;
    const auto pct = [](std::vector<double>& v, double p) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))];
    };

    std::error_code ec;
    std::filesystem::remove(file_path, ec);
    std::filesystem::remove(file_path + ".snap", ec);

    // 行槽约束：rec_fixed_len ≈ 24B（两个 i64 + null 位图 + 对齐），
    // 24 × row_cap ≤ 64KB → row_cap ≤ 2730；取 2048。
    const uint32_t kRowCap = 2048;
    const uint32_t n = rows < kRowCap ? rows : kRowCap;

    rtdb::Options opts;
    opts.instance_name = "bench";
    opts.file_path = file_path;
    opts.initial_size_bytes = 8ull * 1024 * 1024;
    opts.role = rtdb::Role::Standalone;
    opts.fsync = rtdb::FsyncPolicy::PerSecond;

    std::printf("BENCH rows=%u row_cap=%u image=%s\n", n, kRowCap, file_path.c_str());

    // ---- 1) 单行 upsert（per_second：热路径无 fsync）----
    auto open = rtdb::Engine::Open(opts);
    if (!open.IsOk()) {
        std::printf("BENCH_FAIL open: %s\n", rtdb::ErrorMessage(open.Error()));
        return 1;
    }
    auto eng = open.TakeValue();
    rtdb::TableDef def;
    def.name = "t";
    def.row_cap = kRowCap;
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
    auto t_res = eng->CreateTable(def);
    if (!t_res.IsOk()) {
        std::printf("BENCH_FAIL create: %s\n", rtdb::ErrorMessage(t_res.Error()));
        return 1;
    }
    const rtdb::TableHandle t = t_res.Value();

    std::vector<double> ins_us;
    ins_us.reserve(n);
    for (uint32_t i = 1; i <= n; ++i) {
        const auto t0 = ch::steady_clock::now();
        auto r = eng->Insert(t, {rtdb::ColumnValue::Int(static_cast<int64_t>(i)),
                                 rtdb::ColumnValue::Int(static_cast<int64_t>(i) * 10)});
        const auto t1 = ch::steady_clock::now();
        if (!r.IsOk()) {
            std::printf("BENCH_FAIL insert %u: %s\n", i, rtdb::ErrorMessage(r.Error()));
            return 1;
        }
        ins_us.push_back(static_cast<double>(
            ch::duration_cast<ch::nanoseconds>(t1 - t0).count()) / 1000.0);
    }
    std::printf("BENCH insert_per_second_us p50=%.2f p99=%.2f p999=%.2f max=%.2f (n=%zu)\n",
                pct(ins_us, 0.50), pct(ins_us, 0.99), pct(ins_us, 0.999),
                *std::max_element(ins_us.begin(), ins_us.end()), ins_us.size());

    // ---- 1b) 单行 upsert（always：每批一次 flush，量级参考）----
    {
        eng.reset();  // 先收尾 per_second 档实例
        rtdb::Options aopts = opts;
        aopts.fsync = rtdb::FsyncPolicy::Always;
        auto aopen = rtdb::Engine::Open(aopts);
        if (!aopen.IsOk()) {
            std::printf("BENCH_FAIL reopen_always: %s\n",
                        rtdb::ErrorMessage(aopen.Error()));
            return 1;
        }
        auto aeng = aopen.TakeValue();
        rtdb::TableDef adef;
        adef.name = "ta";
        adef.row_cap = kRowCap;
        adef.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
        adef.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
        auto at_res = aeng->CreateTable(adef);
        if (!at_res.IsOk()) {
            std::printf("BENCH_FAIL create_ta: %s\n", rtdb::ErrorMessage(at_res.Error()));
            return 1;
        }
        const rtdb::TableHandle at = at_res.Value();
        std::vector<double> aus;
        aus.reserve(n);
        for (uint32_t i = 1; i <= n; ++i) {
            const auto t0 = ch::steady_clock::now();
            auto r = aeng->Insert(at, {rtdb::ColumnValue::Int(static_cast<int64_t>(i)),
                                       rtdb::ColumnValue::Int(static_cast<int64_t>(i) * 10)});
            const auto t1 = ch::steady_clock::now();
            if (!r.IsOk()) {
                std::printf("BENCH_FAIL insert_always %u: %s\n", i,
                            rtdb::ErrorMessage(r.Error()));
                return 1;
            }
            aus.push_back(static_cast<double>(
                ch::duration_cast<ch::nanoseconds>(t1 - t0).count()) / 1000.0);
        }
        std::printf("BENCH insert_always_us p50=%.2f p99=%.2f p999=%.2f max=%.2f (n=%zu)\n",
                    pct(aus, 0.50), pct(aus, 0.99), pct(aus, 0.999),
                    *std::max_element(aus.begin(), aus.end()), aus.size());
    }

    // ---- 2) 主键点查 ----
    // 表句柄是纯值对象（table_id + desc_off 等），跨引擎实例有效，
    // 直接复用第一阶段句柄即可（重开后表结构不变）。
    {
        auto open2 = rtdb::Engine::Open(opts);
        if (!open2.IsOk()) {
            std::printf("BENCH_FAIL reopen: %s\n", rtdb::ErrorMessage(open2.Error()));
            return 1;
        }
        eng = open2.TakeValue();
    }

    std::vector<double> get_us;
    get_us.reserve(n);
    for (uint32_t i = 1; i <= n; ++i) {
        const auto t0 = ch::steady_clock::now();
        auto r = eng->GetByKey(t, rtdb::ColumnValue::Int(static_cast<int64_t>(i)));
        const auto t1 = ch::steady_clock::now();
        if (!r.IsOk()) {
            std::printf("BENCH_FAIL get %u: %s\n", i, rtdb::ErrorMessage(r.Error()));
            return 1;
        }
        get_us.push_back(static_cast<double>(
            ch::duration_cast<ch::nanoseconds>(t1 - t0).count()) / 1000.0);
    }
    std::printf("BENCH get_p99_us p50=%.2f p99=%.2f p999=%.2f max=%.2f (n=%zu)\n",
                pct(get_us, 0.50), pct(get_us, 0.99), pct(get_us, 0.999),
                *std::max_element(get_us.begin(), get_us.end()), get_us.size());

    // ---- 3) 原子批吞吐（独立表 tb，每批 500 行）----
    {
        rtdb::TableDef bdef;
        bdef.name = "tb";
        bdef.row_cap = kRowCap;
        bdef.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
        bdef.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
        auto tb_res = eng->CreateTable(bdef);
        if (!tb_res.IsOk()) {
            std::printf("BENCH_FAIL create_tb: %s\n", rtdb::ErrorMessage(tb_res.Error()));
            return 1;
        }
        const rtdb::TableHandle tb = tb_res.Value();
        const uint32_t kB = 500;
        const uint32_t batches = n / kB > 4 ? 4 : 1;
        uint64_t done = 0;
        const auto b0 = ch::steady_clock::now();
        for (uint32_t b = 0; b < batches; ++b) {
            std::vector<rtdb::Engine::BatchOp> ops;
            ops.reserve(kB);
            for (uint32_t i = 0; i < kB; ++i) {
                rtdb::Engine::BatchOp op;
                op.type = rtdb::Engine::BatchOp::Type::kInsert;
                op.table = tb;
                const int64_t id = 1000000 + static_cast<int64_t>(b) * kB + i;
                op.row = {rtdb::ColumnValue::Int(id), rtdb::ColumnValue::Int(id)};
                ops.push_back(std::move(op));
            }
            auto r = eng->ExecuteBatch(ops);
            if (!r.IsOk()) {
                std::printf("BENCH_FAIL batch %u: %s\n", b, rtdb::ErrorMessage(r.Error()));
                return 1;
            }
            done += kB;
        }
        const auto b1 = ch::steady_clock::now();
        const double secs =
            static_cast<double>(ch::duration_cast<ch::nanoseconds>(b1 - b0).count()) / 1e9;
        std::printf("BENCH batch_rows_per_sec=%.0f (rows=%llu secs=%.4f)\n",
                    static_cast<double>(done) / secs, static_cast<unsigned long long>(done),
                    secs);
    }

    // ---- 4) 范围扫描速率（全表一次，按行数线性外报 10 万行）----
    {
        const auto s0 = ch::steady_clock::now();
        auto rows_res = eng->ScanAll(t);
        const auto s1 = ch::steady_clock::now();
        if (!rows_res.IsOk()) {
            std::printf("BENCH_FAIL scan: %s\n", rtdb::ErrorMessage(rows_res.Error()));
            return 1;
        }
        const double secs =
            static_cast<double>(ch::duration_cast<ch::nanoseconds>(s1 - s0).count()) / 1e9;
        const double per_row_us = secs * 1e6 / static_cast<double>(rows_res.Value().size());
        std::printf("BENCH scan_rows=%zu secs=%.4f per_row_us=%.3f "
                    "extrapolated_100k_ms=%.2f\n",
                    rows_res.Value().size(), secs, per_row_us, per_row_us * 100000.0 / 1000.0);
    }

    // ---- 5) WAL 重放速率 ----
    // 环为单块 64KB（~780 条记录）：每轮「新表 + 满载」滚动环并推进
    // checkpoint，随后的重开即重放区间 (checkpoint, committed]——记录数
    // 由水位字精确取得（不靠估算），多轮累计给出量级。
    // 基线：1GB 日志 ≤ 60s → ~200k 行/s 量级。
    {
        const int kCycles = 5;
        uint64_t replayed_records = 0;
        double total_secs = 0.0;
        for (int c = 0; c < kCycles; ++c) {
            if (c == 0) {
                // 接续前四阶段的引擎（其环内已有本表批次记录）。
            } else {
                auto ro = rtdb::Engine::Open(opts);
                if (!ro.IsOk()) {
                    std::printf("BENCH_FAIL reopen %d: %s\n", c,
                                rtdb::ErrorMessage(ro.Error()));
                    return 1;
                }
                eng = ro.TakeValue();
            }
            // 写阶段：新表 + 满载（环滚动、checkpoint 推进）。
            const std::string tn = "r" + std::to_string(c);
            rtdb::TableDef d2;
            d2.name = tn;
            d2.row_cap = kRowCap;
            d2.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
            d2.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
            auto t2 = eng->CreateTable(d2);
            if (!t2.IsOk()) {
                std::printf("BENCH_FAIL create_%s: %s\n", tn.c_str(),
                            rtdb::ErrorMessage(t2.Error()));
                return 1;
            }
            for (uint32_t i = 1; i <= n; ++i) {
                if (!eng->Insert(t2.Value(), {rtdb::ColumnValue::Int(static_cast<int64_t>(i)),
                                              rtdb::ColumnValue::Int(static_cast<int64_t>(i))})
                         .IsOk()) {
                    std::printf("BENCH_FAIL insert_%s %u\n", tn.c_str(), i);
                    return 1;
                }
            }
            const int64_t ck = eng->CheckpointSeq();
            const int64_t cs = eng->CommittedSeq();
            eng.reset();  // 优雅关闭（flush + 水位落盘）
            // 重开计时：重放区间 = (ck, cs]。
            const auto r0 = ch::steady_clock::now();
            auto ro = rtdb::Engine::Open(opts);
            const auto r1 = ch::steady_clock::now();
            if (!ro.IsOk()) {
                std::printf("BENCH_FAIL reopen_replay %d: %s\n", c,
                            rtdb::ErrorMessage(ro.Error()));
                return 1;
            }
            total_secs +=
                static_cast<double>(ch::duration_cast<ch::nanoseconds>(r1 - r0).count()) / 1e9;
            replayed_records += static_cast<uint64_t>(cs - ck);
            eng = ro.TakeValue();
            // 抽验重放正确性：表行数与值。
            auto tr = eng->OpenTable(tn);
            if (!tr.IsOk()) {
                std::printf("BENCH_FAIL opentable_%s: %s\n", tn.c_str(),
                            rtdb::ErrorMessage(tr.Error()));
                return 1;
            }
            auto all = eng->ScanAll(tr.Value());
            if (!all.IsOk() || all.Value().size() != n ||
                all.Value().back()[0].as_int() != static_cast<int64_t>(n)) {
                std::printf("BENCH_FAIL replay_verify_%s (rows=%zu)\n", tn.c_str(),
                            all.IsOk() ? all.Value().size() : 0);
                return 1;
            }
        }
        if (total_secs > 0.0 && replayed_records > 0) {
            std::printf("BENCH wal_replay_records_per_sec=%.0f (records=%llu "
                        "reopen_secs=%.3f cycles=%d)\n",
                        static_cast<double>(replayed_records) / total_secs,
                        static_cast<unsigned long long>(replayed_records), total_secs,
                        kCycles);
        } else {
            std::printf("BENCH wal_replay skipped (no samples)\n");
        }
    }

    std::printf("BENCH_DONE\n");
    std::filesystem::remove(file_path, ec);
    std::filesystem::remove(file_path + ".snap", ec);
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
    if (argc >= 5 && std::string(argv[1]) == "--rtdb-child-chaos")
        return RunChaosWriteChild(argv[2], argv[3], argv[4]);
    if (argc >= 4 && std::string(argv[1]) == "--rtdb-verify-chaos")
        return RunChaosVerifyChild(argv[2], argv[3]);
    if (argc >= 4 && std::string(argv[1]) == "--rtdb-bench")
        return RunBenchChild(argv[2], static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10)));
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    // gcov 插桩构建（MinGW/UCRT）：libgcc 退出转储与 DLL 卸载/CRT 退出
    // 相互作用触发 fast-fail(0xC0000409)，且 .gcda 可能未落盘。主动转储
    // 后 _Exit 跳过退出清理——普通构建走原路径，行为不变。
#if defined(__GNUC__) && !defined(__clang__)
    if (__gcov_dump) {
        __gcov_dump();
        std::fflush(nullptr);
        std::_Exit(rc);
    }
#endif
    return rc;
}
