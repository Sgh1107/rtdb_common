/// \file
/// \brief M4-W3 `migrate_from_psrtdb` 用例（docs/16 D8）：
/// fixture 生成/解析、dry-run 预估、完整迁移与校验报告、目标映像可重开、
/// 未知列/未知类型拒绝、--limit/批预算、目标已存在拒绝。
///
/// 真实 PSRtdb 二进制映像不可得，本工具以自描述 fixture 作为默认源适配器；
/// 该限制在迁移报告的 note 字段中明确写出（见 migrate_core.hpp）。

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "rtdb/engine.hpp"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"
#include "tools/migrate_from_psrtdb/migrate_core.hpp"

namespace {

namespace fs = std::filesystem;
namespace mg = rtdb::migrate;

constexpr const char* kFixture = "rtdb_gtest_migrate.fixture";
constexpr const char* kMappingPath = "rtdb_gtest_migrate.mapping.json";
constexpr const char* kTarget = "rtdb_gtest_migrate.rtdb";
constexpr const char* kSnap = "rtdb_gtest_migrate.rtdb.snap";
constexpr const char* kReport = "rtdb_gtest_migrate.report.json";

const char* kGoodMapping = R"({
  "tables": [
    {
      "source": "meters", "target": "meters", "primary_key": "id",
      "columns": [
        {"source": "id",   "target": "id",   "type": "int64",   "nullable": false},
        {"source": "val",  "target": "val",  "type": "float64", "nullable": true},
        {"source": "name", "target": "name", "type": "utf8",    "nullable": true}
      ],
      "defaults": {"name": ""}
    }
  ]
})";

class MigrateTest : public ::testing::Test {
protected:
    void SetUp() override { Cleanup(); }
    void TearDown() override { Cleanup(); }

    static void Cleanup() {
        std::error_code ec;
        for (const char* p : {kFixture, kMappingPath, kTarget, kSnap, kReport})
            fs::remove(p, ec);
    }

    void WriteSources(const std::string& mapping) {
        std::string err;
        ASSERT_TRUE(mg::MakeFixture(kFixture, &err)) << err;
        ASSERT_TRUE(mg::WriteFile(kMappingPath, mapping, &err)) << err;
    }

    static mg::MigrateParams Params() {
        mg::MigrateParams p;
        p.source = kFixture;
        p.mapping = kMappingPath;
        p.target = kTarget;
        p.target_size_mb = 16;
        p.batch_rows = 2;
        return p;
    }
};

TEST_F(MigrateTest, MakeFixtureParsesIntoTables) {
    std::string err;
    ASSERT_TRUE(mg::MakeFixture(kFixture, &err)) << err;
    mg::FixtureAdapter adapter(kFixture);
    std::vector<mg::SrcTable> tables;
    ASSERT_TRUE(adapter.Tables(&tables, &err)) << err;
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0].name, "meters");
    ASSERT_EQ(tables[0].columns.size(), 3u);
    EXPECT_EQ(tables[0].columns[0].type, rtdb::DataType::kInt64);
    ASSERT_EQ(tables[0].rows.size(), 3u);
    EXPECT_EQ(tables[0].rows[0][0].as_int(), 1);
    EXPECT_DOUBLE_EQ(tables[0].rows[0][1].as_real(), 1.5);
    EXPECT_EQ(tables[0].rows[0][2].as_bytes(), "alpha");
    EXPECT_TRUE(tables[0].rows[1][1].is_null());
    EXPECT_TRUE(tables[0].rows[2][2].is_null());
}

TEST_F(MigrateTest, DryRunEstimatesRowsWithoutCreatingTarget) {
    WriteSources(kGoodMapping);
    mg::MigrateParams p = Params();
    p.dry_run = true;
    std::string err;
    mg::MigrationReport rep;
    ASSERT_EQ(mg::Run(p, &rep, &err), rtdb::Err::kOk) << err;
    EXPECT_TRUE(rep.dry_run);
    EXPECT_TRUE(rep.passed);
    ASSERT_EQ(rep.tables.size(), 1u);
    EXPECT_EQ(rep.tables[0].source_rows, 3u);
    EXPECT_EQ(rep.tables[0].target_rows, 0u);
    EXPECT_FALSE(fs::exists(kTarget));
}

TEST_F(MigrateTest, FullMigrationPassesVerificationAndReport) {
    WriteSources(kGoodMapping);
    mg::MigrateParams p = Params();
    p.report = kReport;
    std::string err;
    mg::MigrationReport rep;
    ASSERT_EQ(mg::Run(p, &rep, &err), rtdb::Err::kOk) << err;
    EXPECT_TRUE(rep.passed);
    ASSERT_EQ(rep.tables.size(), 1u);
    EXPECT_EQ(rep.tables[0].source_rows, 3u);
    EXPECT_EQ(rep.tables[0].target_rows, 3u);
    EXPECT_NE(rep.tables[0].key_checksum, 0u);
    EXPECT_GT(rep.tables[0].samples, 0u);
    EXPECT_TRUE(rep.tables[0].sample_ok);
    EXPECT_GE(rep.committed_seq, 1);
    EXPECT_NE(rep.note.find("fixture"), std::string::npos);

    const std::string json = mg::ReportJson(rep);
    EXPECT_NE(json.find("\"passed\":true"), std::string::npos);
    std::string written;
    ASSERT_TRUE(mg::ReadFile(kReport, &written, &err)) << err;
    EXPECT_NE(written.find("\"key_checksum\":"), std::string::npos);

    // 目标映像是完整独立映像：重开后行数与抽样值一致。
    rtdb::Options opts;
    opts.instance = "migrate_verify";
    opts.path = kTarget;
    opts.initial_size_bytes = 16ull << 20;
    opts.role = rtdb::Role::Standalone;
    auto opened = rtdb::Engine::Open(opts);
    ASSERT_TRUE(opened.IsOk()) << rtdb::ErrorMessage(opened.Error());
    auto eng = opened.TakeValue();
    auto table = eng->OpenTable("meters");
    ASSERT_TRUE(table.IsOk()) << rtdb::ErrorMessage(table.Error());
    auto rows = eng->ScanAll(table.Value());
    ASSERT_TRUE(rows.IsOk()) << rtdb::ErrorMessage(rows.Error());
    ASSERT_EQ(rows.Value().size(), 3u);
    EXPECT_EQ(rows.Value()[0][0].as_int(), 1);
    EXPECT_EQ(rows.Value()[2][2].as_bytes(), "");  // NULL 列按 mapping 默认值转为空串
}

TEST_F(MigrateTest, UnknownSourceColumnIsRejected) {
    const std::string mapping = R"({"tables":[{"source":"meters","target":"meters",
      "columns":[{"source":"nope","target":"id","type":"int64"}]}]})";
    WriteSources(mapping);
    std::string err;
    mg::MigrationReport rep;
    EXPECT_EQ(mg::Run(Params(), &rep, &err), rtdb::Err::kInvalidArgument);
    EXPECT_NE(err.find("unknown source column"), std::string::npos) << err;
}

TEST_F(MigrateTest, UnknownColumnTypeIsRejected) {
    const std::string mapping = R"({"tables":[{"source":"meters","target":"meters",
      "columns":[{"source":"id","target":"id","type":"uuid"}]}]})";
    WriteSources(mapping);
    std::string err;
    mg::MigrationReport rep;
    EXPECT_EQ(mg::Run(Params(), &rep, &err), rtdb::Err::kInvalidArgument);
    EXPECT_NE(err.find("unknown column type"), std::string::npos) << err;
}

TEST_F(MigrateTest, LimitAndBatchAreRespected) {
    WriteSources(kGoodMapping);
    mg::MigrateParams p = Params();
    p.limit = 2;
    p.batch_rows = 1;
    std::string err;
    mg::MigrationReport rep;
    ASSERT_EQ(mg::Run(p, &rep, &err), rtdb::Err::kOk) << err;
    ASSERT_EQ(rep.tables.size(), 1u);
    EXPECT_EQ(rep.tables[0].source_rows, 2u);
    EXPECT_EQ(rep.tables[0].target_rows, 2u);
    EXPECT_TRUE(rep.tables[0].sample_ok);
}

TEST_F(MigrateTest, ExistingTargetIsNeverOverwritten) {
    WriteSources(kGoodMapping);
    std::string err;
    ASSERT_TRUE(mg::WriteFile(kTarget, "existing image", &err)) << err;
    mg::MigrationReport rep;
    EXPECT_EQ(mg::Run(Params(), &rep, &err), rtdb::Err::kAlreadyExists);
    EXPECT_NE(err.find("already exists"), std::string::npos) << err;
    std::string content;
    ASSERT_TRUE(mg::ReadFile(kTarget, &content, &err)) << err;
    EXPECT_EQ(content, "existing image");
}

}  // namespace
