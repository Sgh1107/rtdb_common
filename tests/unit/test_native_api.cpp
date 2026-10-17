/// M2-W1 NativeApi 收敛单测（docs/10 §2-M2-D1）：
///  - TableDef 扩展：primary_key 显式声明/多列与非列 0 拒绝、secondary
///    建表直建与重名拒绝、max_rows_hint 双闸门（kMaxRowCap / 行数据区
///    64KB）与 row_cap 冲突；
///  - Row/Upsert 便利面：插入路径缺列填充（nullable→NULL / ext 默认值）、
///    合并路径未给列保持原值、主键缺失/重名列/表外列名/非空缺列拒绝；
///  - TableHandle 成员函数（Upsert/Get/Delete）与 Engine 静态面等价、
///    未绑定句柄拒绝；
///  - Result operator* 与 RTDB_CHECK / RTDB_ASSIGN_OR_MOVE 宏语义；
///  - 旧句柄可见性在新便利面下保持（加列后旧句柄 Upsert 不崩不错。

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rtdb/rtdb.hpp"  // 伞头（M2-D1）

namespace {

namespace fs = std::filesystem;
using rtdb::ColumnDef;
using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::Err;
using rtdb::Options;
using rtdb::Role;
using rtdb::Row;
using rtdb::TableDef;

class NativeApiTest : public ::testing::Test {
protected:
    void SetUp() override { Cleanup(); }
    void TearDown() override { Cleanup(); }

    static Options MakeOpts() {
        Options opts;
        opts.instance = "native_api_test";
        opts.path = kImage;
        opts.initial_size_bytes = 4ull << 20;  // 4MB
        opts.role = Role::Standalone;
        return opts;
    }

    static void Cleanup() {
        std::error_code ec;
        fs::remove(kImage, ec);
        fs::remove(std::string(kImage) + ".snap", ec);
    }

    /// id(i64 NOT NULL) + val(f64 nullable) + desc(变长 nullable)。
    static TableDef MakeDef(const std::string& name, size_t max_rows_hint = 0) {
        TableDef def;
        def.name = name;
        def.max_rows_hint = max_rows_hint;
        def.columns.push_back({"id", DataType::kInt64, 0, false, false});
        def.columns.push_back({"val", DataType::kFloat64, 0, true, false});
        def.columns.push_back({"desc", DataType::kUtf8Var, 0, true, false});
        return def;
    }

    static constexpr const char* kImage = "rtdb_gtest_native_api.rtdb";
};

// ---------------------------------------------------------------------------
// TableDef 扩展
// ---------------------------------------------------------------------------

TEST_F(NativeApiTest, ExplicitPrimaryKeyEquivalentToImplicit) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk()) << rtdb::ErrorMessage(open_res.Error());
    auto engine = open_res.TakeValue();

    TableDef def = MakeDef("t_explicit_pk");
    def.primary_key.push_back("id");  // 显式声明列 0
    auto created = engine->CreateTable(def);
    ASSERT_TRUE(created.IsOk()) << rtdb::ErrorMessage(created.Error());
    EXPECT_EQ(created.Value().name(), "t_explicit_pk");
}

TEST_F(NativeApiTest, MultiColumnPrimaryKeyUnsupported) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();

    TableDef def = MakeDef("t_multi_pk");
    def.primary_key.push_back("id");
    def.primary_key.push_back("val");
    auto created = engine->CreateTable(def);
    EXPECT_EQ(created.Error(), Err::kUnsupported);
}

TEST_F(NativeApiTest, PrimaryKeyNotColumnZeroUnsupported) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();

    TableDef def = MakeDef("t_pk_nonzero");
    def.primary_key.push_back("val");  // 列 1：任意位置主键未支持
    auto created = engine->CreateTable(def);
    EXPECT_EQ(created.Error(), Err::kUnsupported);
}

TEST_F(NativeApiTest, SecondaryIndexAtCreateReady) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();

    TableDef def = MakeDef("t_secidx_create");
    rtdb::IndexDef by_val;
    by_val.name = "by_val";
    by_val.columns.push_back("val");
    def.secondary.push_back(by_val);
    auto created = engine->CreateTable(def);
    ASSERT_TRUE(created.IsOk()) << rtdb::ErrorMessage(created.Error());

    auto tbl_res = engine->OpenTable("t_secidx_create");
    ASSERT_TRUE(tbl_res.IsOk());
    auto ups = tbl_res.Value().Upsert(Row{{"id", int64_t{7}}, {"val", 3.5}});
    ASSERT_TRUE(ups.IsOk()) << rtdb::ErrorMessage(ups.Error());

    auto hit = engine->GetBySecondary(tbl_res.Value(), "by_val", ColumnValue::Real(3.5));
    ASSERT_TRUE(hit.IsOk()) << rtdb::ErrorMessage(hit.Error());
    ASSERT_EQ(hit.Value().size(), 1u);
    EXPECT_EQ(hit.Value()[0][0].as_int(), 7);
}

TEST_F(NativeApiTest, SecondaryAtCreateDuplicateNameRejected) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();

    TableDef def = MakeDef("t_secidx_dup");
    rtdb::IndexDef a;
    a.name = "dup";
    a.columns.push_back("val");
    rtdb::IndexDef b;
    b.name = "dup";  // 同批重名
    b.columns.push_back("desc");
    def.secondary.push_back(a);
    def.secondary.push_back(b);
    auto created = engine->CreateTable(def);
    EXPECT_EQ(created.Error(), Err::kAlreadyExists);
}

TEST_F(NativeApiTest, SecondaryAtCreateUnknownColumnRejected) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();

    TableDef def = MakeDef("t_secidx_unknown");
    rtdb::IndexDef bad;
    bad.name = "bad";
    bad.columns.push_back("no_such_col");
    def.secondary.push_back(bad);
    auto created = engine->CreateTable(def);
    EXPECT_EQ(created.Error(), Err::kInvalidArgument);
}

TEST_F(NativeApiTest, MaxRowsHintGates) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();

    // 闸门①：超 kMaxRowCap（版本数组 ≤64KB 单 slab）→ kLimitExceeded。
    auto too_big = engine->CreateTable(MakeDef("t_hint_big", 100000));
    EXPECT_EQ(too_big.Error(), Err::kLimitExceeded);

    // 闸门②：行数据区 64KB。本表约 24B/行 → 2048 行 = 48KB 可行；
    // 4096 行 = 96KB 超限。
    auto ok = engine->CreateTable(MakeDef("t_hint_ok", 2048));
    ASSERT_TRUE(ok.IsOk()) << rtdb::ErrorMessage(ok.Error());
    EXPECT_EQ(ok.Value().row_cap(), 2048u);
    auto too_wide = engine->CreateTable(MakeDef("t_hint_wide", 4096));
    EXPECT_EQ(too_wide.Error(), Err::kLimitExceeded);
}

TEST_F(NativeApiTest, RowCapAndHintConflictRejected) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();

    TableDef def = MakeDef("t_conflict");
    def.row_cap = 256;
    def.max_rows_hint = 512;
    auto created = engine->CreateTable(def);
    EXPECT_EQ(created.Error(), Err::kInvalidArgument);
}

// ---------------------------------------------------------------------------
// Row / Upsert 便利面
// ---------------------------------------------------------------------------

TEST_F(NativeApiTest, UpsertInsertFillsNullableNull) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t_up_ins")).IsOk());

    auto tbl_res = engine->OpenTable("t_up_ins");
    ASSERT_TRUE(tbl_res.IsOk());
    const rtdb::TableHandle tbl = tbl_res.Value();

    auto up = tbl.Upsert(Row{{"id", int64_t{42}}, {"val", 220.5}});
    ASSERT_TRUE(up.IsOk()) << rtdb::ErrorMessage(up.Error());

    auto got = tbl.Get(ColumnValue::Int(42));
    ASSERT_TRUE(got.IsOk());
    ASSERT_EQ(got.Value().size(), 3u);
    EXPECT_EQ(got.Value()[0].as_int(), 42);
    EXPECT_DOUBLE_EQ(got.Value()[1].as_real(), 220.5);
    EXPECT_TRUE(got.Value()[2].is_null());  // desc 未提供 → NULL
}

TEST_F(NativeApiTest, UpsertMergeKeepsUntouchedColumns) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t_up_merge")).IsOk());

    auto tbl_res = engine->OpenTable("t_up_merge");
    ASSERT_TRUE(tbl_res.IsOk());
    const rtdb::TableHandle tbl = tbl_res.Value();

    ASSERT_TRUE(tbl.Upsert(Row{{"id", int64_t{42}}, {"val", 220.5}}).IsOk());
    // 合并：只给 desc，val 应保持 220.5。
    ASSERT_TRUE(tbl.Upsert(Row{{"id", int64_t{42}}, {"desc", "updated"}}).IsOk());

    auto got = tbl.Get(ColumnValue::Int(42));
    ASSERT_TRUE(got.IsOk());
    EXPECT_DOUBLE_EQ(got.Value()[1].as_real(), 220.5);
    EXPECT_EQ(got.Value()[2].as_bytes(), "updated");
}

TEST_F(NativeApiTest, UpsertInsertFillsExtColumnDefault) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t_up_ext")).IsOk());

    // 在线加列（带默认值 7），新句柄可见 4 列。
    auto tbl0_res = engine->OpenTable("t_up_ext");
    ASSERT_TRUE(tbl0_res.IsOk());
    ColumnDef extra;
    extra.name = "extra";
    extra.type = DataType::kInt64;
    extra.nullable = false;
    extra.has_default = true;
    extra.default_value.is_null = false;
    extra.default_value.int_value = 7;
    ASSERT_TRUE(engine->AlterTableAddColumn(tbl0_res.Value(), extra).IsOk());

    auto tbl_res = engine->OpenTable("t_up_ext");
    ASSERT_TRUE(tbl_res.IsOk());
    const rtdb::TableHandle tbl = tbl_res.Value();
    ASSERT_EQ(tbl.visible_column_count(), 4u);

    // 缺列 extra → ext 默认值 7 物化。
    auto up = tbl.Upsert(Row{{"id", int64_t{1}}, {"val", 1.5}});
    ASSERT_TRUE(up.IsOk()) << rtdb::ErrorMessage(up.Error());
    auto got = tbl.Get(ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk());
    ASSERT_EQ(got.Value().size(), 4u);
    EXPECT_EQ(got.Value()[3].as_int(), 7);
}

TEST_F(NativeApiTest, UpsertRequiresPrimaryKey) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t_up_nopk")).IsOk());

    auto tbl_res = engine->OpenTable("t_up_nopk");
    ASSERT_TRUE(tbl_res.IsOk());
    auto up = tbl_res.Value().Upsert(Row{{"val", 1.0}});  // 缺主键列
    EXPECT_EQ(up.Error(), Err::kInvalidArgument);
}

TEST_F(NativeApiTest, UpsertRejectsDuplicateAndUnknownColumns) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t_up_bad_cols")).IsOk());

    auto tbl_res = engine->OpenTable("t_up_bad_cols");
    ASSERT_TRUE(tbl_res.IsOk());
    const rtdb::TableHandle tbl = tbl_res.Value();

    auto dup = tbl.Upsert(Row{{"id", int64_t{1}}, {"id", int64_t{2}}});
    EXPECT_EQ(dup.Error(), Err::kInvalidArgument);
    auto unknown = tbl.Upsert(Row{{"id", int64_t{1}}, {"nope", int64_t{1}}});
    EXPECT_EQ(unknown.Error(), Err::kInvalidArgument);
}

TEST_F(NativeApiTest, UpsertNonNullableMissingRejected) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t_up_nn")).IsOk());

    auto tbl_res = engine->OpenTable("t_up_nn");
    ASSERT_TRUE(tbl_res.IsOk());
    // id 非空且无默认值：缺列不可填。
    auto up = tbl_res.Value().Upsert(Row{{"val", 1.0}});
    EXPECT_EQ(up.Error(), Err::kInvalidArgument);
}

TEST_F(NativeApiTest, HandleMembersEquivalentToEngineStatics) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t_members")).IsOk());

    auto tbl_res = engine->OpenTable("t_members");
    ASSERT_TRUE(tbl_res.IsOk());
    const rtdb::TableHandle tbl = tbl_res.Value();

    // 成员面与静态面写同一行，读回一致。
    ASSERT_TRUE(engine->Upsert(tbl, Row{{"id", int64_t{5}}, {"val", 1.0}}).IsOk());
    ASSERT_TRUE(tbl.Upsert(Row{{"id", int64_t{5}}, {"val", 2.0}}).IsOk());
    auto got = tbl.Get(ColumnValue::Int(5));
    ASSERT_TRUE(got.IsOk());
    EXPECT_DOUBLE_EQ(got.Value()[1].as_real(), 2.0);

    // Delete 成员面。
    ASSERT_TRUE(tbl.Delete(ColumnValue::Int(5)).IsOk());
    EXPECT_EQ(tbl.Get(ColumnValue::Int(5)).Error(), Err::kNotFound);

    // 未绑定句柄（默认构造）→ kInvalidArgument。
    const rtdb::TableHandle null_handle;
    EXPECT_EQ(null_handle.Upsert(Row{{"id", int64_t{1}}}).Error(), Err::kInvalidArgument);
    EXPECT_EQ(null_handle.Get(ColumnValue::Int(1)).Error(), Err::kInvalidArgument);
    EXPECT_EQ(null_handle.Delete(ColumnValue::Int(1)).Error(), Err::kInvalidArgument);
}

TEST_F(NativeApiTest, OldHandleVisibilityPreservedWithNewApis) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t_old_handle")).IsOk());

    auto old_res = engine->OpenTable("t_old_handle");
    ASSERT_TRUE(old_res.IsOk());
    const rtdb::TableHandle old_handle = old_res.Value();  // 绑定 3 列快照

    // 加列后旧句柄仍按 3 列语义 Upsert（新列不可见、不参与）。
    ColumnDef extra;
    extra.name = "extra";
    extra.type = DataType::kInt64;
    extra.nullable = true;
    ASSERT_TRUE(engine->AlterTableAddColumn(old_handle, extra).IsOk());

    auto up = old_handle.Upsert(Row{{"id", int64_t{9}}, {"val", 8.0}});
    ASSERT_TRUE(up.IsOk()) << rtdb::ErrorMessage(up.Error());
    auto got = old_handle.Get(ColumnValue::Int(9));
    ASSERT_TRUE(got.IsOk());
    EXPECT_EQ(got.Value().size(), 3u);  // 旧句柄仍见 3 列

    // 新句柄见 4 列，且能读到旧句柄写入行的新列默认值（NULL）。
    auto new_res = engine->OpenTable("t_old_handle");
    ASSERT_TRUE(new_res.IsOk());
    auto got_new = new_res.Value().Get(ColumnValue::Int(9));
    ASSERT_TRUE(got_new.IsOk());
    ASSERT_EQ(got_new.Value().size(), 4u);
    EXPECT_TRUE(got_new.Value()[3].is_null());
}

// ---------------------------------------------------------------------------
// Result 便利（operator* / RTDB_CHECK / RTDB_ASSIGN_OR_MOVE）
// ---------------------------------------------------------------------------

namespace {

/// RTDB_CHECK / RTDB_ASSIGN_OR_MOVE 的 Err 中转函数（宏契约：所在函数
/// 返回 Err，见 result.hpp 注释）。
Err CheckAndAssignFlow(const Options& opts, std::string* out_desc) {
    TableDef def;
    def.name = "t_macros";
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"desc", DataType::kUtf8Var, 0, true, false});

    RTDB_ASSIGN_OR_MOVE(auto eng, rtdb::Engine::Open(opts));
    RTDB_CHECK(eng->CreateTable(def));

    RTDB_ASSIGN_OR_MOVE(auto tbl, eng->OpenTable("t_macros"));  // operator* 亦可
    RTDB_CHECK(tbl.Upsert(Row{{"id", int64_t{3}}, {"desc", "macro"}}));

    auto got = tbl.Get(ColumnValue::Int(3));
    if (!got.IsOk()) return got.Error();
    *out_desc = got.Value()[1].as_bytes();
    return Err::kOk;
}

}  // namespace

TEST_F(NativeApiTest, ResultConvenienceMacros) {
    std::string desc;
    const Err err = CheckAndAssignFlow(MakeOpts(), &desc);
    EXPECT_EQ(err, Err::kOk);
    EXPECT_EQ(desc, "macro");
}

TEST_F(NativeApiTest, ResultOperatorStar) {
    const auto opts = MakeOpts();
    auto open_res = rtdb::Engine::Open(opts);
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    ASSERT_TRUE(engine->CreateTable(MakeDef("t_star")).IsOk());

    // docs/04 §3 写法：auto tbl = *eng.OpenTable(...);
    auto tbl = *engine->OpenTable("t_star");
    EXPECT_TRUE(tbl.IsValid());
    auto up = tbl.Upsert(Row{{"id", int64_t{1}}});
    ASSERT_TRUE(up.IsOk()) << rtdb::ErrorMessage(up.Error());
    EXPECT_EQ(up.Value(), 0u);  // 首个行槽
}

}  // namespace
