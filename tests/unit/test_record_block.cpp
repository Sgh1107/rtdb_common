/// M2-W2 列式批量（RecordBlock / UpsertBatch）单测（docs/10 §2-M2-D2）：
///  - 构建期校验：列名重复/行数不一致/掩码长度不符/未知掩码列/空块；
///  - 插入+更新混合块、变长列往返、NULL 掩码、缺列填充（nullable →
///    NULL；ext 列默认值）；
///  - 载荷类型不兼容/未知列名/块内主键重复/原子失败零副作用；
///  - 跨重启持久；
///  - 崩溃恢复：block.commit.before（无 COMMIT → 整批作废）与
///    block.apply.mid（有 COMMIT → 重放整批收敛）。

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "infra/failpoint.hpp"
#include "platform/platform_process.hpp"
#include "rtdb/rtdb.hpp"
#include "spawn_self.hpp"

namespace {

namespace fs = std::filesystem;
using rtdb::ColumnDef;
using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::Err;
using rtdb::Options;
using rtdb::RecordBlock;
using rtdb::Role;
using rtdb::TableDef;

/// id(i64 NOT NULL) + val(i64 nullable) + desc(utf8 nullable)。
TableDef MakeDef(const std::string& name) {
    TableDef def;
    def.name = name;
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", DataType::kInt64, 0, true, false});
    def.columns.push_back({"desc", DataType::kUtf8Var, 0, true, false});
    return def;
}

class RecordBlockTest : public ::testing::Test {
protected:
    void SetUp() override { Cleanup(); }
    void TearDown() override { Cleanup(); }

    static Options MakeOpts() {
        Options opts;
        opts.instance = "record_block_test";
        opts.path = kImage;
        opts.initial_size_bytes = 4ull << 20;
        opts.role = Role::Standalone;
        return opts;
    }

    static void Cleanup() {
        std::error_code ec;
        fs::remove(kImage, ec);
        fs::remove(std::string(kImage) + ".snap", ec);
    }

    static constexpr const char* kImage = "rtdb_gtest_record_block.rtdb";
};

// ---------------------------------------------------------------------------
// 构建期校验
// ---------------------------------------------------------------------------

TEST_F(RecordBlockTest, BuildValidationRejectsBadBlocks) {
    std::vector<int64_t> ids{1, 2, 3};
    std::vector<int64_t> two{1, 2};
    std::vector<uint8_t> mask2{0, 0};
    std::vector<uint8_t> mask3{0, 0, 0};

    RecordBlock dup("t");
    dup.AppendColumn("id", ids).AppendColumn("id", ids);  // 列名重复
    EXPECT_FALSE(dup.IsValid());
    EXPECT_EQ(dup.Error(), Err::kInvalidArgument);

    RecordBlock mismatch("t");
    mismatch.AppendColumn("id", ids).AppendColumn("val", two);  // 行数不一致
    EXPECT_FALSE(mismatch.IsValid());
    EXPECT_EQ(mismatch.Error(), Err::kInvalidArgument);

    RecordBlock mask_len("t");
    mask_len.AppendColumn("id", ids);
    mask_len.SetNullMask("id", mask2);  // 掩码长度不符
    EXPECT_FALSE(mask_len.IsValid());

    RecordBlock mask_col("t");
    mask_col.AppendColumn("id", ids);
    mask_col.SetNullMask("nope", mask3);  // 未知列
    EXPECT_FALSE(mask_col.IsValid());

    RecordBlock empty("t");
    EXPECT_FALSE(empty.IsValid());
}

TEST_F(RecordBlockTest, InvalidBlockRejectedByEngine) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t")).IsOk());

    std::vector<int64_t> ids{1, 2};
    std::vector<int64_t> one{1};
    RecordBlock bad("t");
    bad.AppendColumn("id", ids).AppendColumn("val", one);
    auto res = engine->UpsertBatch(bad);
    EXPECT_EQ(res.Error(), Err::kInvalidArgument);
}

// ---------------------------------------------------------------------------
// 插入/更新、NULL、缺列、类型
// ---------------------------------------------------------------------------

TEST_F(RecordBlockTest, InsertsThenUpdates) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t")).IsOk());
    auto tbl_res = engine->OpenTable("t");
    ASSERT_TRUE(tbl_res.IsOk());
    const rtdb::TableHandle tbl = tbl_res.Value();

    // 首轮：3 行插入（desc 缺列 → NULL）。
    std::vector<int64_t> ids{1, 2, 3};
    std::vector<int64_t> vals{10, 20, 30};
    RecordBlock blk("t", 3);
    blk.AppendColumn("id", ids).AppendColumn("val", vals);
    auto seq1 = engine->UpsertBatch(blk);
    ASSERT_TRUE(seq1.IsOk()) << rtdb::ErrorMessage(seq1.Error());
    EXPECT_GT(seq1.Value(), 0);

    auto got = tbl.Get(ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk());
    EXPECT_EQ(got.Value()[1].as_int(), 10);
    EXPECT_TRUE(got.Value()[2].is_null());

    // 次轮：同键覆盖（整行覆盖语义：val 换值、desc 显式给值）。
    std::vector<int64_t> ids2{1, 2, 3};
    std::vector<int64_t> vals2{110, 220, 330};
    std::vector<std::string> desc_storage{"a", "b", "c"};
    std::vector<std::string_view> descs{desc_storage[0], desc_storage[1], desc_storage[2]};
    RecordBlock blk2("t", 3);
    blk2.AppendColumn("id", ids2).AppendColumn("val", vals2).AppendColumn("desc", descs);
    auto seq2 = engine->UpsertBatch(blk2);
    ASSERT_TRUE(seq2.IsOk()) << rtdb::ErrorMessage(seq2.Error());
    EXPECT_GT(seq2.Value(), seq1.Value());

    auto got2 = tbl.Get(ColumnValue::Int(2));
    ASSERT_TRUE(got2.IsOk());
    EXPECT_EQ(got2.Value()[1].as_int(), 220);
    EXPECT_EQ(got2.Value()[2].as_bytes(), "b");

    auto all = engine->ScanAll(tbl);
    ASSERT_TRUE(all.IsOk());
    EXPECT_EQ(all.Value().size(), 3u);  // 覆盖不产生新行
}

TEST_F(RecordBlockTest, NullMaskAndNullableFill) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t")).IsOk());
    auto tbl_res = engine->OpenTable("t");
    ASSERT_TRUE(tbl_res.IsOk());
    const rtdb::TableHandle tbl = tbl_res.Value();

    std::vector<int64_t> ids{1, 2};
    std::vector<int64_t> vals{10, 20};
    std::vector<uint8_t> val_null{0, 1};  // 第 2 行 val 为 NULL
    RecordBlock blk("t", 2);
    blk.AppendColumn("id", ids).AppendColumn("val", vals).SetNullMask("val", val_null);
    ASSERT_TRUE(engine->UpsertBatch(blk).IsOk());

    auto r1 = tbl.Get(ColumnValue::Int(1));
    ASSERT_TRUE(r1.IsOk());
    EXPECT_EQ(r1.Value()[1].as_int(), 10);
    auto r2 = tbl.Get(ColumnValue::Int(2));
    ASSERT_TRUE(r2.IsOk());
    EXPECT_TRUE(r2.Value()[1].is_null());

    // NOT NULL 主键带 NULL 掩码 → 整批拒绝、零副作用。
    std::vector<int64_t> ids3{3};
    std::vector<uint8_t> id_null{1};
    RecordBlock badnull("t", 1);
    badnull.AppendColumn("id", ids3).SetNullMask("id", id_null);
    auto bad = engine->UpsertBatch(badnull);
    EXPECT_EQ(bad.Error(), Err::kInvalidArgument);
    EXPECT_EQ(tbl.Get(ColumnValue::Int(3)).Error(), Err::kNotFound);
}

TEST_F(RecordBlockTest, ExtColumnDefaultFill) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t")).IsOk());
    auto tbl0_res = engine->OpenTable("t");
    ASSERT_TRUE(tbl0_res.IsOk());

    ColumnDef extra;
    extra.name = "extra";
    extra.type = DataType::kInt64;
    extra.nullable = false;
    extra.has_default = true;
    extra.default_value.is_null = false;
    extra.default_value.int_value = 7;
    ASSERT_TRUE(engine->AlterTableAddColumn(tbl0_res.Value(), extra).IsOk());

    // 块不含 extra 列 → 缺列走 ext 默认值 7。
    std::vector<int64_t> ids{1};
    std::vector<int64_t> vals{10};
    RecordBlock blk("t", 1);
    blk.AppendColumn("id", ids).AppendColumn("val", vals);
    ASSERT_TRUE(engine->UpsertBatch(blk).IsOk());

    auto tbl_res = engine->OpenTable("t");
    ASSERT_TRUE(tbl_res.IsOk());
    auto got = engine->GetByKey(tbl_res.Value(), ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk());
    ASSERT_EQ(got.Value().size(), 4u);
    EXPECT_EQ(got.Value()[3].as_int(), 7);
}

TEST_F(RecordBlockTest, RejectsTypeMismatchAndUnknownColumn) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t")).IsOk());

    std::vector<int64_t> ids{1};
    // val 是整数列，double 载荷不兼容。
    std::vector<double> dvals{1.5};
    RecordBlock bad_type("t", 1);
    bad_type.AppendColumn("id", ids).AppendColumn("val", dvals);
    EXPECT_EQ(engine->UpsertBatch(bad_type).Error(), Err::kInvalidArgument);

    // 表外列名。
    std::vector<int64_t> extra{9};
    RecordBlock bad_name("t", 1);
    bad_name.AppendColumn("id", ids).AppendColumn("nope", extra);
    EXPECT_EQ(engine->UpsertBatch(bad_name).Error(), Err::kInvalidArgument);

    // 表不存在。
    RecordBlock no_table("missing", 1);
    no_table.AppendColumn("id", ids);
    EXPECT_EQ(engine->UpsertBatch(no_table).Error(), Err::kTableNotFound);
}

TEST_F(RecordBlockTest, DuplicatePrimaryKeyRejectedAtomically) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t")).IsOk());
    auto tbl_res = engine->OpenTable("t");
    ASSERT_TRUE(tbl_res.IsOk());
    const rtdb::TableHandle tbl = tbl_res.Value();

    std::vector<int64_t> ids{7, 7};  // 块内主键重复
    std::vector<int64_t> vals{1, 2};
    RecordBlock blk("t", 2);
    blk.AppendColumn("id", ids).AppendColumn("val", vals);
    EXPECT_EQ(engine->UpsertBatch(blk).Error(), Err::kInvalidArgument);
    EXPECT_EQ(tbl.Get(ColumnValue::Int(7)).Error(), Err::kNotFound);  // 零副作用
}

TEST_F(RecordBlockTest, AtomicFailureLeavesNoRows) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t")).IsOk());
    auto tbl_res = engine->OpenTable("t");
    ASSERT_TRUE(tbl_res.IsOk());
    const rtdb::TableHandle tbl = tbl_res.Value();

    // 第 2 行带 NOT NULL 主键的 NULL 掩码 → 整批拒绝，第 1 行也不落。
    std::vector<int64_t> ids{1, 2};
    std::vector<uint8_t> id_null{0, 1};
    RecordBlock blk("t", 2);
    blk.AppendColumn("id", ids).SetNullMask("id", id_null);
    EXPECT_EQ(engine->UpsertBatch(blk).Error(), Err::kInvalidArgument);
    EXPECT_EQ(tbl.Get(ColumnValue::Int(1)).Error(), Err::kNotFound);
    EXPECT_EQ(tbl.Get(ColumnValue::Int(2)).Error(), Err::kNotFound);
    EXPECT_EQ(engine->ScanAll(tbl).Value().size(), 0u);
}

TEST_F(RecordBlockTest, PersistsAcrossReopen) {
    std::vector<int64_t> ids{1, 2};
    std::vector<int64_t> vals{11, 22};
    {
        auto open_res = rtdb::Engine::Open(MakeOpts());
        ASSERT_TRUE(open_res.IsOk());
        auto engine = open_res.TakeValue();
        ASSERT_TRUE(engine->CreateTable(MakeDef("t")).IsOk());
        RecordBlock blk("t", 2);
        blk.AppendColumn("id", ids).AppendColumn("val", vals);
        ASSERT_TRUE(engine->UpsertBatch(blk).IsOk());
    }
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    auto tbl_res = engine->OpenTable("t");
    ASSERT_TRUE(tbl_res.IsOk());
    auto got = engine->GetByKey(tbl_res.Value(), ColumnValue::Int(2));
    ASSERT_TRUE(got.IsOk());
    EXPECT_EQ(got.Value()[1].as_int(), 22);
}

// ---------------------------------------------------------------------------
// 崩溃恢复：复用混沌子进程的 block 场景（建表 + 5 行播种 + 混合块 upsert）
// ---------------------------------------------------------------------------

class RecordBlockCrashTest : public ::testing::Test {
protected:
    void SetUp() override { Cleanup(); }
    void TearDown() override {
        rtdb::failpoint::ClearAll();
        Cleanup();
    }

    static void Cleanup() {
        std::error_code ec;
        fs::remove(kImage, ec);
    }

    /// 拉起混沌写子进程（block 场景），期望其死于注入点（退出码 9）。
    void RunChildToCrash(const std::string& failpoint_spec) {
        const std::string exe = rtdb::platform::HostExecutablePath();
        if (exe.empty()) GTEST_SKIP() << "当前平台无法定位自身可执行文件";
        rtdb_test::SetEnvVar("RTDB_FAILPOINTS", failpoint_spec.c_str());
        const auto out = rtdb_test::SpawnSelf(
            exe, {"--rtdb-child-chaos", kImage, "block", "per_second"});
        rtdb_test::UnsetEnvVar("RTDB_FAILPOINTS");
        ASSERT_TRUE(out.spawned) << "子进程派生失败";
        ASSERT_EQ(out.exit_code, rtdb::failpoint::kCrashExitCode)
            << "子进程应死于注入点 " << failpoint_spec;
    }

    struct Snapshot {
        int64_t committed_seq = 0;
        size_t rows = 0;
        bool values_ok = true;
    };

    /// 重开映像：核对行集与 val=id*10 不变量。
    Snapshot Reopen() {
        Snapshot s;
        Options opts;
        opts.instance = "record_block_crash";
        opts.path = kImage;
        opts.initial_size_bytes = 2ull << 20;
        opts.role = Role::Standalone;
        auto open_res = rtdb::Engine::Open(opts);
        EXPECT_TRUE(open_res.IsOk()) << rtdb::ErrorMessage(open_res.Error());
        if (!open_res.IsOk()) return s;
        auto engine = open_res.TakeValue();
        auto tbl_res = engine->OpenTable("t");
        EXPECT_TRUE(tbl_res.IsOk());
        if (!tbl_res.IsOk()) return s;
        auto all = engine->ScanAll(tbl_res.Value());
        EXPECT_TRUE(all.IsOk());
        if (!all.IsOk()) return s;
        s.committed_seq = engine->CommittedSeq();
        s.rows = all.Value().size();
        for (const auto& r : all.Value()) {
            if (r.size() < 2 || r[1].as_int() != r[0].as_int() * 10) s.values_ok = false;
        }
        return s;
    }

    static constexpr const char* kImage = "rtdb_gtest_record_block_crash.rtdb";
};

TEST_F(RecordBlockCrashTest, NoCommitVoidsWholeBatch) {
    // 崩溃于 COMMIT 落环前：批无 COMMIT → 恢复整批作废，仅播种 5 行存活。
    RunChildToCrash("block.commit.before=crash");
    const Snapshot s = Reopen();
    EXPECT_TRUE(s.values_ok);
    EXPECT_EQ(s.rows, 5u);
    EXPECT_EQ(s.committed_seq, 6);  // 建表 DDL + 5 行播种
}

TEST_F(RecordBlockCrashTest, MidApplyReplaysWholeBatch) {
    // 崩溃于首行应用后（COMMIT 已在环）：恢复重放整批 → 11 行齐全。
    RunChildToCrash("block.apply.mid=crash");
    const Snapshot s = Reopen();
    EXPECT_TRUE(s.values_ok);
    EXPECT_EQ(s.rows, 11u);  // 5 播种 + 6 新键（2 既有键为覆盖更新）
}

TEST_F(RecordBlockCrashTest, CrashMidWalVoidsBatch) {
    // 崩溃于块 WAL 追加中途（无 COMMIT）→ 整批作废。
    RunChildToCrash("wal.batch.wal_mid=crash");
    const Snapshot s = Reopen();
    EXPECT_TRUE(s.values_ok);
    EXPECT_EQ(s.rows, 5u);
}

}  // namespace
