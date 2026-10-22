/// W5 单测（docs/08 §3-W5，docs/06 §2-4 必测用例）：
///  - W5-a 冷快照：快照往返（checkpoint/flushed 推进）、重启闭环
///    （环回收后 committed 不回退、快照后新写可重放）、副本本身可
///    独立挂载恢复、Reader 拒绝、白盒验证环回收状态字、重复快照幂等；
///  - W5-a 崩溃注入：snapshot.before_copy 硬退出 → 活映像不受影响、
///    重开自洽可写（docs/06 §3 混沌矩阵 snapshot 中点）；
///  - W5-b 在线加列：默认值语义 + 跨版本句柄读写（旧句柄不崩、不见
///    新列、写入保留 ext）、无默认可空列读 NULL、参数校验（非空无默认/
///    变长/超宽/重名/总数上限）、ext 更新与显式 NULL、多级扩展链、
///    重启重放后缀恢复、加列后快照往返；
///  - W5-b 崩溃注入：alter.before_link 硬退出 → schema 不变、数据完好、
///    可写（docs/06 §3 混沌矩阵 alter 中点）；
///  - W5-c 范围扫描：开/闭/单侧无界/单点/逆域/越界边界、空表、增删后
///    扫描、多叶大表（>255 行/叶）、扩展列 + 旧句柄截断、Reader 扫描、
///    重启后顺序一致；
///  - W5-d 二级索引：非唯一多命中（同值按 row_no 升序）、增删改后索引
///    随动、唯一索引冲突拒绝（插入/更新/存量建索引）、NULL 不入索引、
///    定义校验（重名/越界/变长/上限）、扩展列索引（惰性默认值入索引）、
///    重启延迟重建自愈、快照副本独立挂载可查、Reader 查询、多叶大表；
///  - W5-d 崩溃注入：secidx.before_link 硬退出 → 表无此索引、数据完好、
///    可写、事后建索引成功（docs/06 §3 混沌矩阵 secidx 中点）。
///
/// 后续周次用例（W5-c 扫描 / W5-d 二级索引）在同一文件内按节追加，
/// 共用本文件的 MakeDef/Row 助手。
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "infra/failpoint.hpp"
#include "infra/shm_region.hpp"
#include "platform/platform_process.hpp"
#include "rtdb/engine.hpp"
#include "storage/wal_ring.hpp"
#include "tests/unit/spawn_self.hpp"

namespace {

namespace fs = std::filesystem;
using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::Err;
using rtdb::IsOk;
using rtdb::TableDef;
using rtdb::TableHandle;
namespace storage = rtdb::storage;
namespace failpoint = rtdb::failpoint;

/// 建表定义：id(i64,主键) val(i64)。
TableDef MakeDef(const std::string& name) {
    TableDef def;
    def.name = name;
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", DataType::kInt64, 0, true, false});
    return def;
}

std::vector<ColumnValue> Row(int64_t id, int64_t val) {
    return {ColumnValue::Int(id), ColumnValue::Int(val)};
}

void RemoveFile(const char* path) {
    std::error_code ec;
    fs::remove(path, ec);
}

// ===========================================================================
// W5-a 冷快照 + 重启闭环
// ===========================================================================

class W5SnapshotTest : public ::testing::Test {
protected:
    void SetUp() override {
        RemoveFile(kImage);
        RemoveFile(kSnap);
    }
    void TearDown() override {
        failpoint::ClearAll();
        RemoveFile(kImage);
        RemoveFile(kSnap);
    }

    rtdb::Options Opts() const {
        rtdb::Options opts;
        opts.instance = "w5_snap_a";
        opts.path = kImage;
        opts.initial_size_bytes = 2ull << 20;
        opts.role = rtdb::Role::Standalone;
        return opts;
    }

    /// 建表 + 写入 n 行，返回 (引擎, 表句柄, 写入后 committed_seq)。
    struct Seeded {
        std::unique_ptr<rtdb::Engine> eng;
        TableHandle table;
        int64_t committed = 0;
    };

    Seeded Seed(uint32_t rows) {
        Seeded s;
        auto open = rtdb::Engine::Open(Opts());
        if (!open.IsOk()) return s;
        s.eng = open.TakeValue();
        auto t_res = s.eng->CreateTable(MakeDef("t"));
        if (!t_res.IsOk()) return s;
        s.table = t_res.Value();
        for (uint32_t i = 1; i <= rows; ++i) {
            if (!s.eng->Insert(s.table, Row(static_cast<int64_t>(i), static_cast<int64_t>(i) * 10))
                     .IsOk())
                return s;
        }
        s.committed = s.eng->CommittedSeq();
        return s;
    }

    static constexpr const char* kImage = "rtdb_gtest_w5_snap.rtdb";
    static constexpr const char* kSnap = "rtdb_gtest_w5_snap.rtdb.snap";
};

/// 快照往返：水位推进 + 重启闭环（环回收后 committed 不回退；快照后
/// 新写经 (checkpoint, 尾] 重放恢复）。
TEST_F(W5SnapshotTest, SnapshotRoundTripAndRestartClosedLoop) {
    const int64_t committed_after_writes = 6;  // DDL + 5 行
    {
        auto s = Seed(5);
        ASSERT_NE(s.eng, nullptr);
        ASSERT_EQ(s.committed, committed_after_writes);

        auto snap = s.eng->CreateSnapshot();
        ASSERT_TRUE(snap.IsOk()) << rtdb::ErrorMessage(snap.Error());
        EXPECT_EQ(snap.Value(), kSnap);
        EXPECT_TRUE(fs::exists(kSnap));
        // 快照后水位：checkpoint/flushed 单调推进到 committed。
        EXPECT_EQ(s.eng->CheckpointSeq(), committed_after_writes);
        EXPECT_EQ(s.eng->FlushedSeq(), committed_after_writes);
        EXPECT_EQ(s.eng->CommittedSeq(), committed_after_writes);
        // 快照后新写：立即入环（环刚被回收，空间充足）。
        ASSERT_TRUE(s.eng->Insert(s.table, Row(6, 60)).IsOk());
        EXPECT_EQ(s.eng->CommittedSeq(), committed_after_writes + 1);
    }
    // 重开：环已回收（重放区间为空），committed 不得回退到 0。
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
        auto eng = open.TakeValue();
        EXPECT_EQ(eng->CommittedSeq(), committed_after_writes + 1);
        EXPECT_EQ(eng->CheckpointSeq(), committed_after_writes);
        auto t_res = eng->OpenTable("t");
        ASSERT_TRUE(t_res.IsOk());
        auto t = t_res.Value();
        for (int64_t i = 1; i <= 5; ++i) {
            auto got = eng->GetByKey(t, ColumnValue::Int(i));
            ASSERT_TRUE(got.IsOk());
            EXPECT_EQ(got.Value()[1].as_int(), i * 10);
        }
        // 快照后写入的行经重放恢复。
        auto got = eng->GetByKey(t, ColumnValue::Int(6));
        ASSERT_TRUE(got.IsOk());
        EXPECT_EQ(got.Value()[1].as_int(), 60);
    }
}

/// 快照副本本身是完整映像：以另一实例名挂载即可恢复一致性点数据。
/// （命名映射对象按名字全局可见——副本必须换实例名，否则与活映像的
/// 命名 section 混淆，这一点本身也是平台语义的回归护栏。）
TEST_F(W5SnapshotTest, SnapshotCopyIsLoadableImage) {
    {
        auto s = Seed(3);
        ASSERT_NE(s.eng, nullptr);
        ASSERT_EQ(s.committed, 4);
        auto snap = s.eng->CreateSnapshot();
        ASSERT_TRUE(snap.IsOk());
    }
    rtdb::Options sopts;
    sopts.instance = "w5_snap_copy";  // 区别于活映像实例名
    sopts.path = kSnap;
    sopts.initial_size_bytes = 2ull << 20;
    auto open = rtdb::Engine::Open(sopts);
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    for (int64_t i = 1; i <= 3; ++i) {
        auto got = eng->GetByKey(t, ColumnValue::Int(i));
        ASSERT_TRUE(got.IsOk());
        EXPECT_EQ(got.Value()[1].as_int(), i * 10);
    }
    EXPECT_EQ(eng->CommittedSeq(), 4);
    EXPECT_EQ(eng->CheckpointSeq(), 4);
    // 副本可继续写（独立实例，互不影响活映像语义）。
    ASSERT_TRUE(eng->Insert(t, Row(4, 40)).IsOk());
    EXPECT_EQ(eng->CommittedSeq(), 5);
}

/// Reader 角色拒绝快照；checkpoint 状态字可读。
TEST_F(W5SnapshotTest, ReaderCannotSnapshot) {
    {
        auto s = Seed(1);
        ASSERT_NE(s.eng, nullptr);
        auto snap = s.eng->CreateSnapshot();
        ASSERT_TRUE(snap.IsOk());
    }
    rtdb::Options ropts;
    ropts.instance = "w5_snap_reader";
    ropts.path = kImage;
    ropts.initial_size_bytes = 2ull << 20;
    ropts.role = rtdb::Role::Reader;
    auto open = rtdb::Engine::Open(ropts);
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    EXPECT_EQ(eng->CreateSnapshot().Error(), Err::kReadOnlyHandle);
    EXPECT_GE(eng->CheckpointSeq(), 0);
    EXPECT_EQ(eng->CommittedSeq(), 2);
}

/// 白盒验证活映像状态字与保留策略（引擎关闭后直连区域，docs/06 §1
/// "组件测试落真实 tmpdir 映像"）。
///
/// P0-5：冷快照只推进 checkpoint，不主动清空 WAL 环；保留水位仍停在
/// 最老记录（在线订阅者可继续增量读取），且 tail ≤ checkpoint + 1。
TEST_F(W5SnapshotTest, SnapshotAdvancesCheckpointKeepsRingHistory) {
    {
        auto s = Seed(3);
        ASSERT_NE(s.eng, nullptr);
        auto snap = s.eng->CreateSnapshot();
        ASSERT_TRUE(snap.IsOk());
    }
    rtdb::ShmRegionParams rp;
    rp.map_name = "w5_snap_whitebox";
    rp.file_path = kImage;
    rp.size_bytes = 2ull << 20;
    rp.mode = rtdb::OpenMode::OpenOrCreate;
    auto rres = rtdb::ShmRegion::Open(rp);
    ASSERT_TRUE(rres.IsOk()) << rtdb::ErrorMessage(rres.Error());
    auto region = rres.TakeValue();
    auto* sb = region->super_block();
    EXPECT_EQ(sb->checkpoint_seq, 4u);
    EXPECT_EQ(sb->committed_seq, 4u);
    EXPECT_EQ(sb->flushed_seq, 4u);

    bool created = true;
    auto ring_res = storage::WalRing::CreateOrAttach(region->base_address(), region->allocator(),
                                                     sb, &created);
    ASSERT_TRUE(ring_res.IsOk()) << rtdb::ErrorMessage(ring_res.Error());
    EXPECT_FALSE(created);
    auto ring = ring_res.TakeValue();
    // 覆盖协议不变量仍成立（tail ≤ checkpoint + 1）；记录仍在环内。
    EXPECT_LE(ring->oldest_seq(), sb->checkpoint_seq + 1);
    EXPECT_EQ(ring->oldest_seq(), 1u);
    EXPECT_EQ(ring->next_seq(), 5u);
}

/// 重复快照幂等（无新写入时 checkpoint 不前移、副本可重复覆盖）。
TEST_F(W5SnapshotTest, RepeatedSnapshotIdempotent) {
    auto s = Seed(2);
    ASSERT_NE(s.eng, nullptr);
    auto s1 = s.eng->CreateSnapshot();
    ASSERT_TRUE(s1.IsOk());
    const int64_t ck1 = s.eng->CheckpointSeq();
    auto s2 = s.eng->CreateSnapshot();
    ASSERT_TRUE(s2.IsOk());
    EXPECT_EQ(s2.Value(), s1.Value());
    EXPECT_EQ(s.eng->CheckpointSeq(), ck1);
    EXPECT_EQ(s.eng->CommittedSeq(), 3);
    EXPECT_TRUE(fs::exists(kSnap));
}

// ===========================================================================
// W5-a 崩溃注入：snapshot.before_copy（docs/06 §3 混沌矩阵 snapshot 中点）
// ===========================================================================

class W5SnapshotCrashTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override {
        failpoint::ClearAll();
        RemoveFile(kImage);
        std::error_code ec;
        fs::remove(std::string(kImage) + ".snap", ec);
    }

    /// 以注入规格拉起子进程，期望其在注入点 quick_exit(9)。
    void RunChildToCrash(const std::string& failpoint_spec) {
        const std::string exe = rtdb::platform::HostExecutablePath();
        if (exe.empty()) GTEST_SKIP() << "当前平台无法定位自身可执行文件";
        rtdb_test::SetEnvVar("RTDB_FAILPOINTS", failpoint_spec.c_str());
        const auto out = rtdb_test::SpawnSelf(exe, {rtdb_test::kW5SnapFlag, kImage});
        rtdb_test::UnsetEnvVar("RTDB_FAILPOINTS");
        ASSERT_TRUE(out.spawned) << "子进程派生失败";
        ASSERT_EQ(out.exit_code, failpoint::kCrashExitCode)
            << "子进程应死于注入点 " << failpoint_spec;
    }

    rtdb::Options Opts() const {
        rtdb::Options opts;
        opts.instance = "w5_snap_crash";
        opts.path = kImage;
        opts.initial_size_bytes = 2ull << 20;
        opts.role = rtdb::Role::Standalone;
        return opts;
    }

    static constexpr const char* kImage = "rtdb_gtest_w5_snap_crash.rtdb";
};

/// 拷贝前硬退出：checkpoint 已推进、环已回收、副本缺失——活映像必须
/// 不受影响，重开自洽（committed 不回退）且可继续写。
TEST_F(W5SnapshotCrashTest, CrashBeforeCopyKeepsImageUsable) {
    RunChildToCrash("snapshot.before_copy=crash");
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    for (int64_t i = 1; i <= 3; ++i) {
        auto got = eng->GetByKey(t, ColumnValue::Int(i));
        ASSERT_TRUE(got.IsOk());
        EXPECT_EQ(got.Value()[1].as_int(), i * 10);
    }
    // 关键回归点：环回收后重开，committed 曾被误发布为 0（W5-a 修复）。
    EXPECT_EQ(eng->CommittedSeq(), 4);
    EXPECT_EQ(eng->CheckpointSeq(), 4);
    // 崩溃后再次快照可成功（自愈闭环）。
    auto snap = eng->CreateSnapshot();
    ASSERT_TRUE(snap.IsOk()) << rtdb::ErrorMessage(snap.Error());
    ASSERT_TRUE(eng->Insert(t, Row(4, 40)).IsOk());
}

// ===========================================================================
// W5-b 在线加列（docs/02 §3 要点 3，docs/06 §2-4 跨版本句柄读）
// ===========================================================================

/// 扩展列定义助手（M1：定长 ≤8B 标量默认值）。
rtdb::ColumnDef ExtCol(const char* name, rtdb::DataType type, bool nullable, bool has_default,
                       int64_t def_int, double def_real = 0.0, bool def_is_null = false) {
    rtdb::ColumnDef c;
    c.name = name;
    c.type = type;
    c.nullable = nullable;
    c.has_default = has_default;
    c.default_value.is_null = def_is_null;
    c.default_value.int_value = def_int;
    c.default_value.real_value = def_real;
    return c;
}

class W5AlterTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override {
        failpoint::ClearAll();
        RemoveFile(kImage);
    }

    rtdb::Options Opts() const {
        rtdb::Options opts;
        opts.instance = "w5_alter_a";
        opts.path = kImage;
        opts.initial_size_bytes = 2ull << 20;
        opts.role = rtdb::Role::Standalone;
        return opts;
    }

    static constexpr const char* kImage = "rtdb_gtest_w5_alter.rtdb";
};

/// 默认值语义 + 跨版本句柄：旧句柄按旧 schema 读写不崩；新句柄见新列，
/// 存量行惰性读默认值；旧句柄写入的行 ext 未设置仍读默认值。
TEST_F(W5AlterTest, AddColumnDefaultAndOldHandleSemantics) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    const TableHandle old_handle = t_res.Value();  // 加列前句柄（旧 schema 视图）
    ASSERT_TRUE(eng->Insert(old_handle, Row(1, 10)).IsOk());
    ASSERT_TRUE(eng->Insert(old_handle, Row(2, 20)).IsOk());

    // 加列：tag(i64, nullable, 默认 42)。
    auto add = eng->AlterTableAddColumn(old_handle, ExtCol("tag", DataType::kInt64,
                                                           /*nullable=*/true, /*has_default=*/true,
                                                           /*def_int=*/42));
    ASSERT_TRUE(add.IsOk()) << rtdb::ErrorMessage(add.Error());

    // 旧句柄：仍按 2 列读，不崩、不见新列（docs/02 §3 要点 3）。
    auto got_old = eng->GetByKey(old_handle, ColumnValue::Int(1));
    ASSERT_TRUE(got_old.IsOk());
    EXPECT_EQ(got_old.Value().size(), 2u);

    // 新句柄：3 列；存量行读默认值 42（惰性，未物化）。
    auto t2_res = eng->OpenTable("t");
    ASSERT_TRUE(t2_res.IsOk());
    const TableHandle new_handle = t2_res.Value();
    ASSERT_EQ(new_handle.visible_column_count(), 3u);
    ASSERT_EQ(new_handle.layout_version(), old_handle.layout_version() + 1);
    auto got_new = eng->GetByKey(new_handle, ColumnValue::Int(1));
    ASSERT_TRUE(got_new.IsOk());
    ASSERT_EQ(got_new.Value().size(), 3u);
    EXPECT_EQ(got_new.Value()[2].as_int(), 42);

    // 新句柄写入带 ext 值 → 读回。
    ASSERT_TRUE(eng->Insert(new_handle,
                            {ColumnValue::Int(3), ColumnValue::Int(30), ColumnValue::Int(7)})
                    .IsOk());
    auto g3 = eng->GetByKey(new_handle, ColumnValue::Int(3));
    ASSERT_TRUE(g3.IsOk());
    EXPECT_EQ(g3.Value()[2].as_int(), 7);

    // 旧句柄插入（2 列）→ ext 未设置 → 新句柄读默认值。
    ASSERT_TRUE(eng->Insert(old_handle, Row(4, 40)).IsOk());
    auto g4 = eng->GetByKey(new_handle, ColumnValue::Int(4));
    ASSERT_TRUE(g4.IsOk());
    EXPECT_EQ(g4.Value()[2].as_int(), 42);
}

/// 无默认值的可空扩展列：存量行与显式 NULL 均读 NULL。
TEST_F(W5AlterTest, AddColumnNoDefaultReadsNull) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    ASSERT_TRUE(eng->Insert(t, Row(1, 10)).IsOk());

    auto add = eng->AlterTableAddColumn(t, ExtCol("note", DataType::kInt64, /*nullable=*/true,
                                                  /*has_default=*/false, 0));
    ASSERT_TRUE(add.IsOk());

    auto t2 = eng->OpenTable("t").Value();
    auto g1 = eng->GetByKey(t2, ColumnValue::Int(1));
    ASSERT_TRUE(g1.IsOk());
    EXPECT_TRUE(g1.Value()[2].is_null());  // 存量行：无默认 → NULL

    // 显式 NULL 与显式值均可写入。
    ASSERT_TRUE(eng->Insert(t2, {ColumnValue::Int(2), ColumnValue::Int(20), ColumnValue::Null()})
                    .IsOk());
    ASSERT_TRUE(eng->Insert(t2, {ColumnValue::Int(3), ColumnValue::Int(30), ColumnValue::Int(5)})
                    .IsOk());
    EXPECT_TRUE(eng->GetByKey(t2, ColumnValue::Int(2)).Value()[2].is_null());
    EXPECT_EQ(eng->GetByKey(t2, ColumnValue::Int(3)).Value()[2].as_int(), 5);
}

/// 加列参数校验：非空无默认 / 变长类型 / 超宽 / 重名 / 总数上限。
TEST_F(W5AlterTest, AddColumnValidation) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t")).Value();
    ASSERT_TRUE(eng->Insert(t, Row(1, 10)).IsOk());

    // 非空且无默认值：存量行无处可取 → 拒绝。
    EXPECT_EQ(eng->AlterTableAddColumn(t, ExtCol("bad", DataType::kInt64, /*nullable=*/false,
                                                 /*has_default=*/false, 0))
                 .Error(),
             Err::kInvalidArgument);
    // 非空且默认为 NULL：同样拒绝。
    EXPECT_EQ(eng->AlterTableAddColumn(t, ExtCol("bad2", DataType::kInt64, /*nullable=*/false,
                                                 /*has_default=*/true, 0, 0.0,
                                                 /*def_is_null=*/true))
                 .Error(),
             Err::kInvalidArgument);
    // 变长类型：M1 不支持（归 M2）。
    EXPECT_EQ(eng->AlterTableAddColumn(t, ExtCol("txt", DataType::kUtf8Var, true, false, 0)).Error(),
             Err::kUnsupported);
    // 超宽定长（len > 8）：M1 不支持。
    rtdb::ColumnDef wide = ExtCol("wide", DataType::kInt64, true, false, 0);
    wide.len = 16;
    EXPECT_EQ(eng->AlterTableAddColumn(t, wide).Error(), Err::kUnsupported);
    // 列名与内联列重复。
    EXPECT_EQ(eng->AlterTableAddColumn(t, ExtCol("id", DataType::kInt64, true, false, 0)).Error(),
             Err::kInvalidArgument);
    // 空列名 / 超长列名。
    EXPECT_EQ(eng->AlterTableAddColumn(t, ExtCol("", DataType::kInt64, true, false, 0)).Error(),
             Err::kInvalidArgument);

    // 合法加列成功后，重名（含与既有扩展列重复）拒绝。
    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("tag", DataType::kInt64, true, false, 0)).IsOk());
    EXPECT_EQ(eng->AlterTableAddColumn(t, ExtCol("tag", DataType::kInt64, true, false, 0)).Error(),
             Err::kInvalidArgument);

    // 类型覆盖：int32/float64/bool/unixTs 定长标量均可作扩展列。
    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("i32", DataType::kInt32, true, true, 3)).IsOk());
    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("f64", DataType::kFloat64, true, true, 0, 2.5))
                    .IsOk());
    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("flag", DataType::kBool, true, true, 1)).IsOk());
    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("ts", DataType::kUnixTsNs, true, true, 7)).IsOk());

    // 总数上限：2 内联 + 30 扩展 = 32（kMaxInitCols + kMaxExtCols）；
    // 第 31 个扩展列 → kLimitExceeded。（已加 5 个，再补 25 个到顶。）
    for (int i = 0; i < 25; ++i) {
        const std::string n = "x" + std::to_string(i);
        ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol(n.c_str(), DataType::kInt64, true, false, 0))
                        .IsOk())
            << n;
    }
    EXPECT_EQ(eng->AlterTableAddColumn(t, ExtCol("overflow", DataType::kInt64, true, false, 0))
                 .Error(),
             Err::kLimitExceeded);
}

/// ext 值更新与显式 NULL；旧句柄更新保留 ext 值。
TEST_F(W5AlterTest, AddColumnUpdateAndExplicitNull) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    const TableHandle old_handle = t_res.Value();
    ASSERT_TRUE(eng->Insert(old_handle, Row(1, 10)).IsOk());
    ASSERT_TRUE(eng->AlterTableAddColumn(old_handle,
                                         ExtCol("score", DataType::kInt32, /*nullable=*/true,
                                                /*has_default=*/true, /*def_int=*/0))
                .IsOk());
    const TableHandle nh = eng->OpenTable("t").Value();

    // 新句柄整行更新（含 ext）。
    ASSERT_TRUE(eng->UpdateByKey(nh, ColumnValue::Int(1),
                                 {ColumnValue::Int(1), ColumnValue::Int(11), ColumnValue::Int(9)})
                    .IsOk());
    EXPECT_EQ(eng->GetByKey(nh, ColumnValue::Int(1)).Value()[2].as_int(), 9);

    // 显式 NULL：压过默认值 0。
    ASSERT_TRUE(eng->UpdateByKey(nh, ColumnValue::Int(1),
                                 {ColumnValue::Int(1), ColumnValue::Int(12), ColumnValue::Null()})
                    .IsOk());
    EXPECT_TRUE(eng->GetByKey(nh, ColumnValue::Int(1)).Value()[2].is_null());

    // 旧句柄更新（2 列）：ext 槽不动（仍为显式 NULL）。
    ASSERT_TRUE(eng->UpdateByKey(old_handle, ColumnValue::Int(1), Row(1, 13)).IsOk());
    auto got = eng->GetByKey(nh, ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk());
    EXPECT_EQ(got.Value()[1].as_int(), 13);
    EXPECT_TRUE(got.Value()[2].is_null());

    // 旧句柄读不到 ext 列。
    EXPECT_EQ(eng->GetByKey(old_handle, ColumnValue::Int(1)).Value().size(), 2u);
}

/// 行宽校验：insert/update 的列值数须等于句柄可见列数。
TEST_F(W5AlterTest, AddColumnRowWidthValidation) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t")).Value();
    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("tag", DataType::kInt64, true, true, 42)).IsOk());
    const TableHandle nh = eng->OpenTable("t").Value();

    EXPECT_EQ(eng->Insert(nh, Row(1, 10)).Error(), Err::kInvalidArgument);  // 少一列
    EXPECT_EQ(eng->Insert(nh, {ColumnValue::Int(1), ColumnValue::Int(10), ColumnValue::Int(1),
                               ColumnValue::Int(2)})
                 .Error(),
             Err::kInvalidArgument);  // 多一列
    ASSERT_TRUE(eng->Insert(nh, {ColumnValue::Int(1), ColumnValue::Int(10), ColumnValue::Int(1)})
                    .IsOk());
    EXPECT_EQ(eng->UpdateByKey(nh, ColumnValue::Int(1), Row(1, 20)).Error(), Err::kInvalidArgument);
    ASSERT_TRUE(eng->UpdateByKey(nh, ColumnValue::Int(1),
                                 {ColumnValue::Int(1), ColumnValue::Int(20), ColumnValue::Int(2)})
                    .IsOk());

    // 批内同样校验。
    std::vector<rtdb::Engine::BatchOp> batch;
    rtdb::Engine::BatchOp op;
    op.type = rtdb::Engine::BatchOp::Type::kInsert;
    op.table = nh;
    op.row = Row(2, 20);  // 宽度不足
    batch.push_back(std::move(op));
    EXPECT_EQ(eng->ExecuteBatch(batch).Error(), Err::kInvalidArgument);
}

/// 多级扩展链：多个 ext 列（不同类型/默认值）并存，旧行读各自默认值。
TEST_F(W5AlterTest, AddColumnMultipleExtChain) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    ASSERT_TRUE(eng->Insert(t, Row(1, 10)).IsOk());  // 加列前的存量行

    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("a", DataType::kInt32, true, true, 1)).IsOk());
    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("b", DataType::kFloat64, true, true, 0, 2.5))
                    .IsOk());
    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("c", DataType::kBool, true, false, 0)).IsOk());

    auto nh = eng->OpenTable("t").Value();
    ASSERT_EQ(nh.visible_column_count(), 5u);

    // 存量行：a=1, b=2.5, c=NULL（无默认）。
    auto g1 = eng->GetByKey(nh, ColumnValue::Int(1));
    ASSERT_TRUE(g1.IsOk());
    EXPECT_EQ(g1.Value()[2].as_int(), 1);
    EXPECT_DOUBLE_EQ(g1.Value()[3].as_real(), 2.5);
    EXPECT_TRUE(g1.Value()[4].is_null());

    // 新行显式写全部 ext 列。
    ASSERT_TRUE(eng->Insert(nh, {ColumnValue::Int(2), ColumnValue::Int(20), ColumnValue::Int(5),
                                 ColumnValue::Real(1.25), ColumnValue::Bool(true)})
                    .IsOk());
    auto g2 = eng->GetByKey(nh, ColumnValue::Int(2));
    ASSERT_TRUE(g2.IsOk());
    EXPECT_EQ(g2.Value()[2].as_int(), 5);
    EXPECT_DOUBLE_EQ(g2.Value()[3].as_real(), 1.25);
    EXPECT_EQ(g2.Value()[4].as_bool(), true);
}

/// 重启闭环：加列 + ext 写入后重开，WAL 重放后缀恢复 ext 值；
/// 加列前写入的行无后缀 → 读默认值。
TEST_F(W5AlterTest, AddColumnSurvivesRestart) {
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t_res = eng->CreateTable(MakeDef("t"));
        ASSERT_TRUE(t_res.IsOk());
        auto t = t_res.Value();
        ASSERT_TRUE(eng->Insert(t, Row(1, 10)).IsOk());
        ASSERT_TRUE(eng->Insert(t, Row(2, 20)).IsOk());  // 加列前的存量行
        ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("tag", DataType::kInt64, true, true, 42))
                        .IsOk());
        auto nh = eng->OpenTable("t").Value();
        ASSERT_TRUE(eng->Insert(nh, {ColumnValue::Int(3), ColumnValue::Int(30),
                                     ColumnValue::Int(7)})
                        .IsOk());
        ASSERT_TRUE(eng->UpdateByKey(nh, ColumnValue::Int(1),
                                     {ColumnValue::Int(1), ColumnValue::Int(11), ColumnValue::Int(99)})
                        .IsOk());
        EXPECT_EQ(eng->CommittedSeq(), 5);  // DDL + 2 插 + 1 插 + 1 改
    }
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    ASSERT_EQ(t.visible_column_count(), 3u);
    EXPECT_EQ(eng->CommittedSeq(), 5);

    auto g1 = eng->GetByKey(t, ColumnValue::Int(1));  // 更新带后缀 → 重放恢复
    ASSERT_TRUE(g1.IsOk());
    EXPECT_EQ(g1.Value()[1].as_int(), 11);
    EXPECT_EQ(g1.Value()[2].as_int(), 99);
    auto g2 = eng->GetByKey(t, ColumnValue::Int(2));  // 加列前插入 → 无后缀 → 默认值
    ASSERT_TRUE(g2.IsOk());
    EXPECT_EQ(g2.Value()[2].as_int(), 42);
    auto g3 = eng->GetByKey(t, ColumnValue::Int(3));  // 加列后插入 → 后缀重放
    ASSERT_TRUE(g3.IsOk());
    EXPECT_EQ(g3.Value()[2].as_int(), 7);
}

/// 加列后快照：ext 区随映像拷贝，副本与重开均保持默认值/显式值。
TEST_F(W5AlterTest, AddColumnThenSnapshotRoundTrip) {
    const char* kSnap = "rtdb_gtest_w5_alter.rtdb.snap";
    RemoveFile(kSnap);
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t = eng->CreateTable(MakeDef("t")).Value();
        ASSERT_TRUE(eng->Insert(t, Row(1, 10)).IsOk());
        ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("tag", DataType::kInt64, true, true, 42))
                        .IsOk());
        auto nh = eng->OpenTable("t").Value();
        ASSERT_TRUE(eng->Insert(nh, {ColumnValue::Int(2), ColumnValue::Int(20),
                                     ColumnValue::Int(7)})
                        .IsOk());
        auto snap = eng->CreateSnapshot();
        ASSERT_TRUE(snap.IsOk()) << rtdb::ErrorMessage(snap.Error());
    }
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->OpenTable("t").Value();
    EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(1)).Value()[2].as_int(), 42);
    EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(2)).Value()[2].as_int(), 7);

    // 副本独立挂载：ext 语义一致。
    rtdb::Options sopts;
    sopts.instance = "w5_alter_snap_copy";
    sopts.path = kSnap;
    sopts.initial_size_bytes = 2ull << 20;
    auto copen = rtdb::Engine::Open(sopts);
    ASSERT_TRUE(copen.IsOk()) << rtdb::ErrorMessage(copen.Error());
    auto ceng = copen.TakeValue();
    auto ct = ceng->OpenTable("t").Value();
    ASSERT_EQ(ct.visible_column_count(), 3u);
    EXPECT_EQ(ceng->GetByKey(ct, ColumnValue::Int(1)).Value()[2].as_int(), 42);
    EXPECT_EQ(ceng->GetByKey(ct, ColumnValue::Int(2)).Value()[2].as_int(), 7);
    RemoveFile(kSnap);
}

// ===========================================================================
// W5-b 崩溃注入：alter.before_link（docs/06 §3 混沌矩阵 alter 中点）
// ===========================================================================

class W5AlterCrashTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override {
        failpoint::ClearAll();
        RemoveFile(kImage);
    }

    rtdb::Options Opts() const {
        rtdb::Options opts;
        opts.instance = "w5_alter_crash";
        opts.path = kImage;
        opts.initial_size_bytes = 2ull << 20;
        opts.role = rtdb::Role::Standalone;
        return opts;
    }

    static constexpr const char* kImage = "rtdb_gtest_w5_alter_crash.rtdb";
};

/// 挂链前硬退出：区域已分配未挂链 → 表 schema 不变、数据完好、可写。
TEST_F(W5AlterCrashTest, CrashBeforeLinkKeepsSchema) {
    const std::string exe = rtdb::platform::HostExecutablePath();
    if (exe.empty()) GTEST_SKIP() << "当前平台无法定位自身可执行文件";
    rtdb_test::SetEnvVar("RTDB_FAILPOINTS", "alter.before_link=crash");
    const auto out = rtdb_test::SpawnSelf(exe, {rtdb_test::kW5AlterFlag, kImage});
    rtdb_test::UnsetEnvVar("RTDB_FAILPOINTS");
    ASSERT_TRUE(out.spawned) << "子进程派生失败";
    ASSERT_EQ(out.exit_code, failpoint::kCrashExitCode) << "子进程应死于 alter.before_link";

    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    //  schema 未变（2 列），数据完好。
    EXPECT_EQ(t.visible_column_count(), 2u);
    for (int64_t i = 1; i <= 3; ++i) {
        auto got = eng->GetByKey(t, ColumnValue::Int(i));
        ASSERT_TRUE(got.IsOk());
        EXPECT_EQ(got.Value()[1].as_int(), i * 10);
    }
    EXPECT_EQ(eng->CommittedSeq(), 4);
    // 引擎可写；事后加列成功（自愈闭环）。
    ASSERT_TRUE(eng->Insert(t, Row(4, 40)).IsOk());
    ASSERT_TRUE(eng->AlterTableAddColumn(t, ExtCol("tag", DataType::kInt64, true, true, 42)).IsOk());
    auto nh = eng->OpenTable("t").Value();
    EXPECT_EQ(eng->GetByKey(nh, ColumnValue::Int(1)).Value()[2].as_int(), 42);
}

// ===========================================================================
// W5-c 范围扫描（docs/03 §4.2，docs/08 §3-W5 Scan 桥接）
// ===========================================================================

class W5ScanTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override {
        eng_.reset();  // 先释放映射再删文件（Windows 删除被映射文件会共享冲突）
        RemoveFile(kImage);
    }

    rtdb::Options Opts() const {
        rtdb::Options opts;
        opts.instance = "w5_scan_a";
        opts.path = kImage;
        opts.initial_size_bytes = 4ull << 20;
        opts.role = rtdb::Role::Standalone;
        return opts;
    }

    /// 建表 + 逆序插入 1..n（验证扫描结果按主键升序，与插入序无关）。
    TableHandle SeedDescending(uint32_t n) {
        auto open = rtdb::Engine::Open(Opts());
        EXPECT_TRUE(open.IsOk());
        eng_ = open.TakeValue();
        auto t_res = eng_->CreateTable(MakeDef("t"));
        EXPECT_TRUE(t_res.IsOk());
        TableHandle t = t_res.Value();
        for (uint32_t i = n; i >= 1; --i)
            EXPECT_TRUE(eng_->Insert(t, Row(static_cast<int64_t>(i), static_cast<int64_t>(i) * 10))
                            .IsOk());
        return t;
    }

    /// 扫描结果的主键序列。
    static std::vector<int64_t> Keys(rtdb::Engine& eng, const TableHandle& t,
                                     const rtdb::Engine::KeyRange& r) {
        auto res = eng.Scan(t, r);
        EXPECT_TRUE(res.IsOk()) << rtdb::ErrorMessage(res.Error());
        std::vector<int64_t> keys;
        if (res.IsOk()) {
            for (const auto& row : res.Value()) keys.push_back(row[0].as_int());
        }
        return keys;
    }

    std::unique_ptr<rtdb::Engine> eng_;
    static constexpr const char* kImage = "rtdb_gtest_w5_scan.rtdb";
};

/// 边界用例：闭/开区间、单侧无界、单点、空域、越界、逆域。
TEST_F(W5ScanTest, ScanRangeBoundaries) {
    TableHandle t = SeedDescending(10);
    ASSERT_NE(t.table_id(), 0);

    rtdb::Engine::KeyRange r;
    r.lo = ColumnValue::Int(3);
    r.hi = ColumnValue::Int(7);
    EXPECT_EQ(Keys(*eng_, t, r), std::vector<int64_t>({3, 4, 5, 6, 7}));

    r.lo_inclusive = false;
    r.hi_inclusive = false;
    EXPECT_EQ(Keys(*eng_, t, r), std::vector<int64_t>({4, 5, 6}));

    r.lo_inclusive = false;
    r.hi_inclusive = true;
    EXPECT_EQ(Keys(*eng_, t, r), std::vector<int64_t>({4, 5, 6, 7}));

    r.lo_inclusive = true;
    r.hi_inclusive = false;
    EXPECT_EQ(Keys(*eng_, t, r), std::vector<int64_t>({3, 4, 5, 6}));

    // 单点闭区间（重置两端为闭）。
    r.lo_inclusive = true;
    r.hi_inclusive = true;
    r.lo = ColumnValue::Int(5);
    r.hi = ColumnValue::Int(5);
    EXPECT_EQ(Keys(*eng_, t, r), std::vector<int64_t>({5}));
    // 单点全开 → 空。
    r.lo_inclusive = false;
    r.hi_inclusive = false;
    EXPECT_TRUE(Keys(*eng_, t, r).empty());

    // 单侧无界。
    rtdb::Engine::KeyRange lo_only;
    lo_only.lo = ColumnValue::Int(8);
    EXPECT_EQ(Keys(*eng_, t, lo_only), std::vector<int64_t>({8, 9, 10}));
    rtdb::Engine::KeyRange hi_only;
    hi_only.hi = ColumnValue::Int(3);
    EXPECT_EQ(Keys(*eng_, t, hi_only), std::vector<int64_t>({1, 2, 3}));

    // 逆域（lo > hi）与数据域外 → 空。
    rtdb::Engine::KeyRange reversed;
    reversed.lo = ColumnValue::Int(8);
    reversed.hi = ColumnValue::Int(3);
    EXPECT_TRUE(Keys(*eng_, t, reversed).empty());
    rtdb::Engine::KeyRange outside;
    outside.lo = ColumnValue::Int(100);
    outside.hi = ColumnValue::Int(200);
    EXPECT_TRUE(Keys(*eng_, t, outside).empty());

    // 无界全扫 = ScanAll，且升序（val = i × 10）。
    auto all = eng_->ScanAll(t);
    ASSERT_TRUE(all.IsOk());
    ASSERT_EQ(all.Value().size(), 10u);
    for (size_t i = 0; i < all.Value().size(); ++i) {
        EXPECT_EQ(all.Value()[i][0].as_int(), static_cast<int64_t>(i) + 1);
        EXPECT_EQ(all.Value()[i][1].as_int(), (static_cast<int64_t>(i) + 1) * 10);
    }
}

/// 空表扫描；删除/更新后扫描反映最新状态。
TEST_F(W5ScanTest, ScanEmptyAndAfterMutations) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t")).Value();
    EXPECT_TRUE(eng->ScanAll(t).Value().empty());  // 空表

    for (int64_t i = 1; i <= 5; ++i) ASSERT_TRUE(eng->Insert(t, Row(i, i * 10)).IsOk());
    rtdb::Engine::KeyRange r;
    r.lo = ColumnValue::Int(2);
    r.hi = ColumnValue::Int(4);
    EXPECT_EQ(Keys(*eng, t, r), std::vector<int64_t>({2, 3, 4}));

    // 删除区间内的行 → 缺席；更新 → 新值。
    ASSERT_TRUE(eng->DeleteByKey(t, ColumnValue::Int(3)).IsOk());
    ASSERT_TRUE(eng->UpdateByKey(t, ColumnValue::Int(2), Row(2, 999)).IsOk());
    EXPECT_EQ(Keys(*eng, t, r), std::vector<int64_t>({2, 4}));
    auto rows = eng->Scan(t, r).Value();
    EXPECT_EQ(rows[0][1].as_int(), 999);
}

/// 多叶扫描：行数超过单叶容量（255）仍按序完整交付。
TEST_F(W5ScanTest, ScanMultiLeafLargeTable) {
    const uint32_t n = 600;  // > kCapLeaf(255)，必落多叶 + 叶链推进
    TableHandle t = SeedDescending(n);
    ASSERT_NE(t.table_id(), 0);
    auto all = eng_->ScanAll(t);
    ASSERT_TRUE(all.IsOk());
    ASSERT_EQ(all.Value().size(), n);
    for (uint32_t i = 0; i < n; ++i)
        EXPECT_EQ(all.Value()[i][0].as_int(), static_cast<int64_t>(i) + 1);

    rtdb::Engine::KeyRange r;
    r.lo = ColumnValue::Int(100);
    r.hi = ColumnValue::Int(500);
    auto sub = eng_->Scan(t, r);
    ASSERT_TRUE(sub.IsOk());
    ASSERT_EQ(sub.Value().size(), 401u);
    EXPECT_EQ(sub.Value().front()[0].as_int(), 100);
    EXPECT_EQ(sub.Value().back()[0].as_int(), 500);
}

/// 扩展列 + 旧句柄截断：扫描结果按句柄可见列数返回。
TEST_F(W5ScanTest, ScanWithExtColumnsAndOldHandle) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    const TableHandle old_handle = t_res.Value();
    for (int64_t i = 1; i <= 3; ++i) ASSERT_TRUE(eng->Insert(old_handle, Row(i, i * 10)).IsOk());

    ASSERT_TRUE(eng->AlterTableAddColumn(old_handle, ExtCol("tag", DataType::kInt64, true, true, 42))
                    .IsOk());
    auto nh = eng->OpenTable("t").Value();
    ASSERT_TRUE(eng->Insert(nh, {ColumnValue::Int(4), ColumnValue::Int(40), ColumnValue::Int(7)})
                    .IsOk());

    // 新句柄扫描：4 行 3 列；存量行默认值、新行显式值。
    auto rows = eng->ScanAll(nh);
    ASSERT_TRUE(rows.IsOk());
    ASSERT_EQ(rows.Value().size(), 4u);
    for (const auto& row : rows.Value()) {
        ASSERT_EQ(row.size(), 3u);
        if (row[0].as_int() == 4) EXPECT_EQ(row[2].as_int(), 7);
        else EXPECT_EQ(row[2].as_int(), 42);
    }

    // 旧句柄扫描：截断为 2 列，不崩。
    auto old_rows = eng->ScanAll(old_handle);
    ASSERT_TRUE(old_rows.IsOk());
    ASSERT_EQ(old_rows.Value().size(), 4u);
    for (const auto& row : old_rows.Value()) EXPECT_EQ(row.size(), 2u);
}

/// Reader 角色可扫描（只读映射）。
TEST_F(W5ScanTest, ReaderCanScan) {
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t = eng->CreateTable(MakeDef("t")).Value();
        for (int64_t i = 1; i <= 4; ++i) ASSERT_TRUE(eng->Insert(t, Row(i, i * 10)).IsOk());
    }
    rtdb::Options ropts;
    ropts.instance = "w5_scan_reader";
    ropts.path = kImage;
    ropts.initial_size_bytes = 4ull << 20;
    ropts.role = rtdb::Role::Reader;
    auto open = rtdb::Engine::Open(ropts);
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto rows = eng->ScanAll(t_res.Value());
    ASSERT_TRUE(rows.IsOk());
    ASSERT_EQ(rows.Value().size(), 4u);
    EXPECT_EQ(rows.Value()[3][0].as_int(), 4);
}

/// 扫描后重启：数据与顺序保持一致。
TEST_F(W5ScanTest, ScanSurvivesRestart) {
    {
        TableHandle t = SeedDescending(6);
        ASSERT_NE(t.table_id(), 0);
        ASSERT_TRUE(eng_->DeleteByKey(t, ColumnValue::Int(2)).IsOk());
    }
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->OpenTable("t").Value();
    auto rows = eng->ScanAll(t);
    ASSERT_TRUE(rows.IsOk());
    ASSERT_EQ(rows.Value().size(), 5u);
    EXPECT_EQ(rows.Value()[0][0].as_int(), 1);
    EXPECT_EQ(rows.Value()[1][0].as_int(), 3);
    EXPECT_EQ(rows.Value()[4][0].as_int(), 6);
}

// ===========================================================================
// W5-d 二级索引（docs/03 §2.2，docs/08 §3-W5）
// ===========================================================================

/// 二级索引定义助手（M1：列序号 + 唯一性）。
rtdb::SecondaryIndexDef SecIdx(const char* name, uint32_t col, bool unique) {
    rtdb::SecondaryIndexDef d;
    d.name = name;
    d.column_index = col;
    d.unique = unique;
    return d;
}

class W5SecIdxTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override {
        failpoint::ClearAll();
        eng_.reset();  // 先释放映射再删文件（Windows 删除被映射文件会共享冲突）
        RemoveFile(kImage);
    }

    rtdb::Options Opts() const {
        rtdb::Options opts;
        opts.instance = "w5_secidx_a";
        opts.path = kImage;
        opts.initial_size_bytes = 4ull << 20;
        opts.role = rtdb::Role::Standalone;
        return opts;
    }

    std::unique_ptr<rtdb::Engine> eng_;
    static constexpr const char* kImage = "rtdb_gtest_w5_secidx.rtdb";
};

/// 非唯一索引多命中：同值按 row_no 升序；增删改后索引随动。
TEST_F(W5SecIdxTest, SecIdxNonUniqueMultiHit) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    eng_ = open.TakeValue();
    auto t = eng_->CreateTable(MakeDef("t")).Value();

    // val 重复值：10, 20, 10, 30, 20, 10。
    ASSERT_TRUE(eng_->Insert(t, Row(1, 10)).IsOk());
    ASSERT_TRUE(eng_->Insert(t, Row(2, 20)).IsOk());
    ASSERT_TRUE(eng_->Insert(t, Row(3, 10)).IsOk());
    ASSERT_TRUE(eng_->Insert(t, Row(4, 30)).IsOk());
    ASSERT_TRUE(eng_->Insert(t, Row(5, 20)).IsOk());
    ASSERT_TRUE(eng_->Insert(t, Row(6, 10)).IsOk());

    auto cr = eng_->CreateSecondaryIndex(t, SecIdx("idx_val", 1, false));
    ASSERT_TRUE(cr.IsOk()) << rtdb::ErrorMessage(cr.Error());

    // 同值多行按 row_no 升序（= 插入序）。
    auto q10 = eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(10));
    ASSERT_TRUE(q10.IsOk());
    ASSERT_EQ(q10.Value().size(), 3u);
    EXPECT_EQ(q10.Value()[0][0].as_int(), 1);
    EXPECT_EQ(q10.Value()[1][0].as_int(), 3);
    EXPECT_EQ(q10.Value()[2][0].as_int(), 6);
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(20)).Value().size(), 2u);

    // 不存在的值 → 空；空值键 → kInvalidArgument；索引名不存在 → kNotFound。
    EXPECT_TRUE(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(999)).Value().empty());
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Null()).Error(),
              Err::kInvalidArgument);
    EXPECT_EQ(eng_->GetBySecondary(t, "nope", ColumnValue::Int(10)).Error(), Err::kNotFound);

    // 删除后缺席；更新换值后随新值命中、旧值缺席。
    ASSERT_TRUE(eng_->DeleteByKey(t, ColumnValue::Int(3)).IsOk());
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(10)).Value().size(), 2u);
    ASSERT_TRUE(eng_->UpdateByKey(t, ColumnValue::Int(1), Row(1, 77)).IsOk());
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(10)).Value().size(), 1u);
    auto q77 = eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(77));
    ASSERT_TRUE(q77.IsOk());
    ASSERT_EQ(q77.Value().size(), 1u);
    EXPECT_EQ(q77.Value()[0][0].as_int(), 1);

    // 新插入立即可查（写路径维护）。
    ASSERT_TRUE(eng_->Insert(t, Row(7, 20)).IsOk());
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(20)).Value().size(), 3u);
}

/// 唯一索引：插入/更新冲突拒绝、自身值放行、NULL 豁免、存量建索引校验。
TEST_F(W5SecIdxTest, SecIdxUniqueEnforcement) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    eng_ = open.TakeValue();
    auto t = eng_->CreateTable(MakeDef("t")).Value();
    ASSERT_TRUE(eng_->Insert(t, Row(1, 10)).IsOk());
    ASSERT_TRUE(eng_->Insert(t, Row(2, 20)).IsOk());

    auto cr = eng_->CreateSecondaryIndex(t, SecIdx("uq_val", 1, true));
    ASSERT_TRUE(cr.IsOk()) << rtdb::ErrorMessage(cr.Error());

    // 插入同值 → kAlreadyExists（零副作用：行未落）。
    EXPECT_EQ(eng_->Insert(t, Row(3, 20)).Error(), Err::kAlreadyExists);
    EXPECT_EQ(eng_->GetByKey(t, ColumnValue::Int(3)).Error(), Err::kNotFound);
    // 更新他人行到已占用值 → 拒绝。
    EXPECT_EQ(eng_->UpdateByKey(t, ColumnValue::Int(1), Row(1, 20)).Error(),
              Err::kAlreadyExists);
    // 更新到自身当前值 → 放行（唯一索引排除自身条目）。
    ASSERT_TRUE(eng_->UpdateByKey(t, ColumnValue::Int(1), Row(1, 10)).IsOk());
    // 换到未占用值 → 放行，旧值释放后可被他人使用。
    ASSERT_TRUE(eng_->UpdateByKey(t, ColumnValue::Int(1), Row(1, 30)).IsOk());
    EXPECT_EQ(eng_->GetBySecondary(t, "uq_val", ColumnValue::Int(30)).Value().size(), 1u);
    EXPECT_TRUE(eng_->GetBySecondary(t, "uq_val", ColumnValue::Int(10)).Value().empty());
    ASSERT_TRUE(eng_->Insert(t, Row(3, 10)).IsOk());

    // NULL 不入索引：多行 NULL 不违唯一（SQL 语义）。
    ASSERT_TRUE(eng_->Insert(t, {ColumnValue::Int(4), ColumnValue::Null()}).IsOk());
    ASSERT_TRUE(eng_->Insert(t, {ColumnValue::Int(5), ColumnValue::Null()}).IsOk());

    // 建索引时存量冲突 → kAlreadyExists（分配回滚，表可继续用）。
    auto t2 = eng_->CreateTable(MakeDef("t2")).Value();
    ASSERT_TRUE(eng_->Insert(t2, Row(1, 5)).IsOk());
    ASSERT_TRUE(eng_->Insert(t2, Row(2, 5)).IsOk());
    EXPECT_EQ(eng_->CreateSecondaryIndex(t2, SecIdx("uq2", 1, true)).Error(),
              Err::kAlreadyExists);
    // 非唯一索引可建于同数据。
    ASSERT_TRUE(eng_->CreateSecondaryIndex(t2, SecIdx("nq2", 1, false)).IsOk());
    EXPECT_EQ(eng_->GetBySecondary(t2, "nq2", ColumnValue::Int(5)).Value().size(), 2u);
}

/// 定义校验：空名/重名/列序号越界/变长列/每表索引数上限；主键列可建。
TEST_F(W5SecIdxTest, SecIdxValidation) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    eng_ = open.TakeValue();
    auto t = eng_->CreateTable(MakeDef("t")).Value();
    ASSERT_TRUE(eng_->Insert(t, Row(1, 10)).IsOk());

    // 空索引名 / 重名。
    EXPECT_EQ(eng_->CreateSecondaryIndex(t, SecIdx("", 1, false)).Error(),
              Err::kInvalidArgument);
    ASSERT_TRUE(eng_->CreateSecondaryIndex(t, SecIdx("a", 1, false)).IsOk());
    EXPECT_EQ(eng_->CreateSecondaryIndex(t, SecIdx("a", 1, false)).Error(),
              Err::kAlreadyExists);
    // 列序号越界（当前 2 列；扩展列尚未添加）。
    EXPECT_EQ(eng_->CreateSecondaryIndex(t, SecIdx("b", 2, false)).Error(),
              Err::kInvalidArgument);
    EXPECT_EQ(eng_->CreateSecondaryIndex(t, SecIdx("b", 99, false)).Error(),
              Err::kInvalidArgument);
    // 变长列：M1 不支持。
    rtdb::TableDef vdef;
    vdef.name = "tv";
    vdef.columns.push_back({"id", DataType::kInt64, 0, false, false});
    vdef.columns.push_back({"txt", DataType::kUtf8Var, 0, true, false});
    auto tv = eng_->CreateTable(vdef).Value();
    EXPECT_EQ(eng_->CreateSecondaryIndex(tv, SecIdx("vt", 1, false)).Error(),
              Err::kUnsupported);
    // 主键列可建索引（冗余但合法）。
    ASSERT_TRUE(eng_->CreateSecondaryIndex(t, SecIdx("pk", 0, false)).IsOk());
    EXPECT_EQ(eng_->GetBySecondary(t, "pk", ColumnValue::Int(1)).Value().size(), 1u);

    // 每表索引数上限：已建 2 个，再补 14 个到顶（kMaxSecIdxPerTable=16），
    // 第 17 个 → kLimitExceeded。
    for (uint32_t i = 0; i < 14; ++i) {
        const std::string n = "i" + std::to_string(i);
        ASSERT_TRUE(eng_->CreateSecondaryIndex(t, SecIdx(n.c_str(), 1, false)).IsOk()) << n;
    }
    EXPECT_EQ(eng_->CreateSecondaryIndex(t, SecIdx("overflow", 1, false)).Error(),
              Err::kLimitExceeded);
}

/// 扩展列索引：存量行按惰性默认值入索引；旧句柄查询截断；更新随动。
TEST_F(W5SecIdxTest, SecIdxOnExtColumn) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    eng_ = open.TakeValue();
    auto t_res = eng_->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    const TableHandle old_handle = t_res.Value();  // 加列前句柄
    ASSERT_TRUE(eng_->Insert(old_handle, Row(1, 10)).IsOk());

    // 加列 tag(i64, nullable, 默认 42)。
    ASSERT_TRUE(eng_->AlterTableAddColumn(old_handle,
                                          ExtCol("tag", DataType::kInt64, /*nullable=*/true,
                                                 /*has_default=*/true, /*def_int=*/42))
                .IsOk());
    const TableHandle nh = eng_->OpenTable("t").Value();
    ASSERT_EQ(nh.visible_column_count(), 3u);
    // 新句柄显式写 tag。
    ASSERT_TRUE(eng_->Insert(nh, {ColumnValue::Int(2), ColumnValue::Int(20),
                                  ColumnValue::Int(7)})
                .IsOk());

    // 索引建于扩展列（全局列序号 2）。
    auto cr = eng_->CreateSecondaryIndex(nh, SecIdx("idx_tag", 2, false));
    ASSERT_TRUE(cr.IsOk()) << rtdb::ErrorMessage(cr.Error());
    // 存量行未设置 → 惰性默认值 42 入索引。
    auto q42 = eng_->GetBySecondary(nh, "idx_tag", ColumnValue::Int(42));
    ASSERT_TRUE(q42.IsOk());
    ASSERT_EQ(q42.Value().size(), 1u);
    EXPECT_EQ(q42.Value()[0][0].as_int(), 1);
    auto q7 = eng_->GetBySecondary(nh, "idx_tag", ColumnValue::Int(7));
    ASSERT_TRUE(q7.IsOk());
    ASSERT_EQ(q7.Value().size(), 1u);
    EXPECT_EQ(q7.Value()[0][0].as_int(), 2);

    // 旧句柄查询：行截断为 2 列（不崩、不见新列）。
    auto q42_old = eng_->GetBySecondary(old_handle, "idx_tag", ColumnValue::Int(42));
    ASSERT_TRUE(q42_old.IsOk());
    ASSERT_EQ(q42_old.Value().size(), 1u);
    EXPECT_EQ(q42_old.Value()[0].size(), 2u);

    // 更新 ext 值 → 索引随动；显式 NULL → 出索引。
    ASSERT_TRUE(eng_->UpdateByKey(nh, ColumnValue::Int(1),
                                  {ColumnValue::Int(1), ColumnValue::Int(11),
                                   ColumnValue::Int(8)})
                .IsOk());
    EXPECT_TRUE(eng_->GetBySecondary(nh, "idx_tag", ColumnValue::Int(42)).Value().empty());
    EXPECT_EQ(eng_->GetBySecondary(nh, "idx_tag", ColumnValue::Int(8)).Value().size(), 1u);
    ASSERT_TRUE(eng_->UpdateByKey(nh, ColumnValue::Int(2),
                                  {ColumnValue::Int(2), ColumnValue::Int(21),
                                   ColumnValue::Null()})
                .IsOk());
    EXPECT_TRUE(eng_->GetBySecondary(nh, "idx_tag", ColumnValue::Int(7)).Value().empty());
}

/// 重启闭环：Open 全量重建（延迟重建自愈）→ 查询一致、写路径继续维护。
TEST_F(W5SecIdxTest, SecIdxSurvivesRestart) {
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t = eng->CreateTable(MakeDef("t")).Value();
        ASSERT_TRUE(eng->Insert(t, Row(1, 10)).IsOk());
        ASSERT_TRUE(eng->Insert(t, Row(2, 20)).IsOk());
        ASSERT_TRUE(eng->Insert(t, Row(3, 10)).IsOk());
        ASSERT_TRUE(eng->CreateSecondaryIndex(t, SecIdx("idx_val", 1, false)).IsOk());
        // 建索引后的写入与删除（写路径维护 + 墓碑行缺席）。
        ASSERT_TRUE(eng->Insert(t, Row(4, 30)).IsOk());
        ASSERT_TRUE(eng->DeleteByKey(t, ColumnValue::Int(2)).IsOk());
        EXPECT_EQ(eng->GetBySecondary(t, "idx_val", ColumnValue::Int(10)).Value().size(), 2u);
    }
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    eng_ = open.TakeValue();
    auto t = eng_->OpenTable("t").Value();
    // 重建后与崩溃前一致（已删除行不出现在旧值查询）。
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(10)).Value().size(), 2u);
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(30)).Value().size(), 1u);
    EXPECT_TRUE(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(20)).Value().empty());
    // 重建后写路径继续维护。
    ASSERT_TRUE(eng_->Insert(t, Row(5, 10)).IsOk());
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(10)).Value().size(), 3u);
    ASSERT_TRUE(eng_->UpdateByKey(t, ColumnValue::Int(1), Row(1, 99)).IsOk());
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(10)).Value().size(), 2u);
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(99)).Value().size(), 1u);
}

/// 快照后闭环：活映像与副本（独立挂载）均在 Open 重建索引并可查可写。
TEST_F(W5SecIdxTest, SecIdxSnapshotRoundTrip) {
    const char* kSnap = "rtdb_gtest_w5_secidx.rtdb.snap";
    RemoveFile(kSnap);
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t = eng->CreateTable(MakeDef("t")).Value();
        for (int64_t i = 1; i <= 3; ++i) ASSERT_TRUE(eng->Insert(t, Row(i, i * 10)).IsOk());
        ASSERT_TRUE(eng->CreateSecondaryIndex(t, SecIdx("idx_val", 1, false)).IsOk());
        auto snap = eng->CreateSnapshot();
        ASSERT_TRUE(snap.IsOk()) << rtdb::ErrorMessage(snap.Error());
    }
    // 活映像重开：重建后一致。
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        eng_ = open.TakeValue();
        auto t = eng_->OpenTable("t").Value();
        EXPECT_EQ(eng_->GetBySecondary(t, "idx_val", ColumnValue::Int(20)).Value().size(), 1u);
    }
    // 副本独立挂载：Open 时同样重建 → 可查、可继续写。
    rtdb::Options sopts;
    sopts.instance = "w5_secidx_copy";
    sopts.path = kSnap;
    sopts.initial_size_bytes = 4ull << 20;
    auto copen = rtdb::Engine::Open(sopts);
    ASSERT_TRUE(copen.IsOk()) << rtdb::ErrorMessage(copen.Error());
    auto ceng = copen.TakeValue();
    auto ct = ceng->OpenTable("t").Value();
    auto q = ceng->GetBySecondary(ct, "idx_val", ColumnValue::Int(30));
    ASSERT_TRUE(q.IsOk());
    ASSERT_EQ(q.Value().size(), 1u);
    EXPECT_EQ(q.Value()[0][0].as_int(), 3);
    ASSERT_TRUE(ceng->Insert(ct, Row(4, 30)).IsOk());
    EXPECT_EQ(ceng->GetBySecondary(ct, "idx_val", ColumnValue::Int(30)).Value().size(), 2u);
    RemoveFile(kSnap);
}

/// Reader 角色可查询二级索引；拒绝建索引。
TEST_F(W5SecIdxTest, ReaderCanQuerySecondary) {
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t = eng->CreateTable(MakeDef("t")).Value();
        ASSERT_TRUE(eng->Insert(t, Row(1, 10)).IsOk());
        ASSERT_TRUE(eng->Insert(t, Row(2, 10)).IsOk());
        ASSERT_TRUE(eng->CreateSecondaryIndex(t, SecIdx("idx_val", 1, false)).IsOk());
    }
    rtdb::Options ropts;
    ropts.instance = "w5_secidx_reader";
    ropts.path = kImage;
    ropts.initial_size_bytes = 4ull << 20;
    ropts.role = rtdb::Role::Reader;
    auto open = rtdb::Engine::Open(ropts);
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto q = eng->GetBySecondary(t_res.Value(), "idx_val", ColumnValue::Int(10));
    ASSERT_TRUE(q.IsOk());
    EXPECT_EQ(q.Value().size(), 2u);
    EXPECT_EQ(eng->CreateSecondaryIndex(t_res.Value(), SecIdx("x", 1, false)).Error(),
              Err::kReadOnlyHandle);
}

/// 多叶大表：600 行（>单叶容量 ~170），同值 120 行连续、按 row_no 升序。
TEST_F(W5SecIdxTest, SecIdxMultiLeafLargeTable) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    eng_ = open.TakeValue();
    auto t = eng_->CreateTable(MakeDef("t")).Value();
    for (int64_t i = 1; i <= 600; ++i) {
        ASSERT_TRUE(eng_->Insert(t, Row(i, i % 5)).IsOk()) << i;
    }
    ASSERT_TRUE(eng_->CreateSecondaryIndex(t, SecIdx("idx_mod", 1, false)).IsOk());
    for (int64_t v = 0; v < 5; ++v) {
        auto q = eng_->GetBySecondary(t, "idx_mod", ColumnValue::Int(v));
        ASSERT_TRUE(q.IsOk()) << v;
        ASSERT_EQ(q.Value().size(), 120u) << v;
        for (size_t k = 1; k < q.Value().size(); ++k) {
            EXPECT_LT(q.Value()[k - 1][0].as_int(), q.Value()[k][0].as_int());
        }
    }
    // 删除后规模正确（墓碑行出索引）。
    ASSERT_TRUE(eng_->DeleteByKey(t, ColumnValue::Int(5)).IsOk());  // val = 0
    EXPECT_EQ(eng_->GetBySecondary(t, "idx_mod", ColumnValue::Int(0)).Value().size(), 119u);
}

// ===========================================================================
// W5-d 崩溃注入：secidx.before_link（docs/06 §3 混沌矩阵 secidx 中点）
// ===========================================================================

class W5SecIdxCrashTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override {
        failpoint::ClearAll();
        RemoveFile(kImage);
    }

    rtdb::Options Opts() const {
        rtdb::Options opts;
        opts.instance = "w5_secidx_crash";
        opts.path = kImage;
        opts.initial_size_bytes = 2ull << 20;
        opts.role = rtdb::Role::Standalone;
        return opts;
    }

    static constexpr const char* kImage = "rtdb_gtest_w5_secidx_crash.rtdb";
};

/// 挂链前硬退出：元数据已分配未挂链 → 表无此索引、数据完好、可写；
/// 事后建索引成功（自愈闭环）。
TEST_F(W5SecIdxCrashTest, CrashBeforeLinkKeepsTableUsable) {
    const std::string exe = rtdb::platform::HostExecutablePath();
    if (exe.empty()) GTEST_SKIP() << "当前平台无法定位自身可执行文件";
    rtdb_test::SetEnvVar("RTDB_FAILPOINTS", "secidx.before_link=crash");
    const auto out = rtdb_test::SpawnSelf(exe, {rtdb_test::kW5SecIdxFlag, kImage});
    rtdb_test::UnsetEnvVar("RTDB_FAILPOINTS");
    ASSERT_TRUE(out.spawned) << "子进程派生失败";
    ASSERT_EQ(out.exit_code, failpoint::kCrashExitCode) << "子进程应死于 secidx.before_link";

    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    // schema 未变（2 列），索引不存在，数据完好。
    EXPECT_EQ(t.visible_column_count(), 2u);
    EXPECT_EQ(eng->GetBySecondary(t, "idx_val", ColumnValue::Int(10)).Error(), Err::kNotFound);
    for (int64_t i = 1; i <= 3; ++i) {
        auto got = eng->GetByKey(t, ColumnValue::Int(i));
        ASSERT_TRUE(got.IsOk());
        EXPECT_EQ(got.Value()[1].as_int(), i * 10);
    }
    EXPECT_EQ(eng->CommittedSeq(), 4);
    // 可写；事后建索引成功且存量全量灌入（自愈闭环）。
    ASSERT_TRUE(eng->Insert(t, Row(4, 40)).IsOk());
    ASSERT_TRUE(eng->CreateSecondaryIndex(t, SecIdx("idx_val", 1, false)).IsOk());
    auto q = eng->GetBySecondary(t, "idx_val", ColumnValue::Int(10));
    ASSERT_TRUE(q.IsOk());
    ASSERT_EQ(q.Value().size(), 1u);
    EXPECT_EQ(q.Value()[0][0].as_int(), 1);
}

}  // namespace
