/// W2 目录 Catalog 单测：
///  - 存储层直测（宿主堆缓冲扮演映像）：建/删/开表、重名与非法定义拒绝、
///    墓碑语义、目录结构校验、损坏检出、复附着持久一致；
///  - 引擎端到端（真实映像文件）：建表跨重启持久、删表语义、Reader 只读。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "infra/layout.hpp"
#include "infra/shm_region.hpp"
#include "infra/slab_allocator.hpp"
#include "rtdb/engine.hpp"
#include "storage/catalog.hpp"

namespace {

namespace fs = std::filesystem;
using rtdb::ColumnDef;
using rtdb::DataType;
using rtdb::Err;
using rtdb::TableDef;
using rtdb::storage::Catalog;
using rtdb::storage::shm_CatalogRoot;
using rtdb::storage::shm_TableDescriptor;

/// 便捷建表定义：id(i64, NOT NULL) + value(变长字符串, nullable)。
TableDef MakeDef(const std::string& name, uint32_t row_cap = 0) {
    TableDef def;
    def.name = name;
    def.row_cap = row_cap;
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"value", DataType::kUtf8Var, 0, true, false});
    return def;
}

// ===========================================================================
// 存储层直测：宿主堆缓冲作为映像替身
// ===========================================================================
class CatalogDirectTest : public ::testing::Test {
protected:
    static constexpr uint64_t kRegionBytes = 1ull << 20;  // 1MB

    void SetUp() override {
        mem_ = static_cast<char*>(::operator new(kRegionBytes, std::align_val_t(4096)));
        std::memset(mem_, 0, kRegionBytes);
        sb_ = reinterpret_cast<rtdb::layout::shm_SuperBlock*>(mem_);
        rtdb::layout::InitSuperBlock(sb_, kRegionBytes, 0u, 0u, 0);
        auto alloc_res = rtdb::infra::SlabShmAllocator::Attach(mem_, kRegionBytes, true);
        ASSERT_TRUE(alloc_res.IsOk());
        alloc_ = alloc_res.TakeValue();
        auto cat_res = Catalog::Attach(mem_, alloc_.get(), sb_, /*create_new=*/true);
        ASSERT_TRUE(cat_res.IsOk());
        catalog_ = cat_res.TakeValue();
    }

    void TearDown() override {
        catalog_.reset();
        alloc_.reset();
        ::operator delete(mem_, std::align_val_t(4096));
    }

    shm_CatalogRoot* Root() {
        return reinterpret_cast<shm_CatalogRoot*>(mem_ + sb_->catalog_desc_off);
    }
    shm_TableDescriptor* Desc(uint64_t off) {
        return reinterpret_cast<shm_TableDescriptor*>(mem_ + off);
    }

    char* mem_ = nullptr;
    rtdb::layout::shm_SuperBlock* sb_ = nullptr;
    std::unique_ptr<rtdb::infra::SlabShmAllocator> alloc_;
    std::unique_ptr<Catalog> catalog_;
};

TEST_F(CatalogDirectTest, CreateOpenDropRoundtrip) {
    ASSERT_TRUE(catalog_->available());
    EXPECT_EQ(catalog_->next_table_id(), 1u);
    EXPECT_EQ(catalog_->table_count(), 0u);

    auto create_res = catalog_->CreateTable(MakeDef("meters", 128));
    ASSERT_TRUE(create_res.IsOk()) << rtdb::ErrorMessage(create_res.Error());
    const uint64_t desc_off = create_res.Value();

    const shm_TableDescriptor* desc = catalog_->Descriptor(desc_off);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->magic, rtdb::storage::kTableDescMagic);
    EXPECT_EQ(desc->table_id, 1u);
    EXPECT_STREQ(desc->name, "meters");
    EXPECT_EQ(desc->col_count, 2u);
    EXPECT_EQ(desc->row_cap, 128u);
    EXPECT_EQ(desc->state, static_cast<uint32_t>(rtdb::storage::TableState::kActive));
    EXPECT_EQ(desc->data_layout_ver, 1u);
    // id@0(8B) + value 变长槽@8(8B) + null 位图@16(1B) → 对齐 24
    EXPECT_EQ(desc->rec_fixed_len, 24u);
    EXPECT_EQ(desc->cols[0].offset, 0u);
    EXPECT_EQ(desc->cols[0].len, 8u);
    EXPECT_EQ(desc->cols[1].offset, 8u);
    EXPECT_EQ(desc->cols[1].len, 0u);
    EXPECT_STREQ(desc->cols[1].name, "value");

    EXPECT_EQ(catalog_->table_count(), 1u);
    EXPECT_EQ(catalog_->next_table_id(), 2u);

    // 按名打开 / 按 id 定位必须一致。
    auto open_res = catalog_->OpenTable("meters");
    ASSERT_TRUE(open_res.IsOk());
    EXPECT_EQ(open_res.Value(), desc_off);
    auto id_res = catalog_->FindById(1);
    ASSERT_TRUE(id_res.IsOk());
    EXPECT_EQ(id_res.Value(), desc_off);

    uint64_t active = 0;
    EXPECT_EQ(catalog_->Validate(&active), Err::kOk);
    EXPECT_EQ(active, 1u);

    // 删除后：按名不可见，id 索引保留（墓碑）。
    EXPECT_EQ(catalog_->DropTable("meters"), Err::kOk);
    EXPECT_EQ(catalog_->OpenTable("meters").Error(), Err::kTableNotFound);
    EXPECT_EQ(catalog_->DropTable("meters"), Err::kTableNotFound);
    EXPECT_EQ(catalog_->table_count(), 0u);
    // 墓碑描述符仍可按 id 定位（物理目录完整），但状态须为墓碑态。
    // 注意：不得对成功的 Result 调用 Error()/Value() 以外的越权访问——
    // 二者带 assert(!IsOk())/assert(IsOk())，Debug 构建会直接中止。
    const auto tomb_desc = catalog_->FindById(1);
    ASSERT_TRUE(tomb_desc.IsOk()) << rtdb::ErrorMessage(tomb_desc.Error());
    EXPECT_EQ(catalog_->Descriptor(tomb_desc.Value())->state,
              static_cast<uint32_t>(rtdb::storage::TableState::kTombstone));
    EXPECT_EQ(catalog_->Validate(&active), Err::kOk);
    EXPECT_EQ(active, 0u);
}

TEST_F(CatalogDirectTest, RejectDuplicateAndInvalidDefinitions) {
    ASSERT_TRUE(catalog_->CreateTable(MakeDef("t1")).IsOk());
    // 重名
    EXPECT_EQ(catalog_->CreateTable(MakeDef("t1")).Error(), Err::kTableExists);

    // 空表名 / 超长表名 / 含 NUL 表名
    EXPECT_EQ(catalog_->CreateTable(MakeDef("")).Error(), Err::kInvalidArgument);
    EXPECT_EQ(catalog_->CreateTable(MakeDef(std::string(64, 'x'))).Error(), Err::kInvalidArgument);
    std::string with_nul = "a";
    with_nul.push_back('\0');
    with_nul.push_back('b');
    EXPECT_EQ(catalog_->CreateTable(MakeDef(with_nul)).Error(), Err::kInvalidArgument);

    // 零列
    TableDef no_cols;
    no_cols.name = "empty";
    EXPECT_EQ(catalog_->CreateTable(no_cols).Error(), Err::kInvalidArgument);

    // 超过内联列上限（kMaxInitCols = 16）
    TableDef many;
    many.name = "many";
    for (int i = 0; i < 17; ++i)
        many.columns.push_back({"c" + std::to_string(i), DataType::kInt64});
    EXPECT_EQ(catalog_->CreateTable(many).Error(), Err::kLimitExceeded);

    // 重复列名
    TableDef dup_cols;
    dup_cols.name = "dup";
    dup_cols.columns.push_back({"a", DataType::kInt64});
    dup_cols.columns.push_back({"a", DataType::kInt64});
    EXPECT_EQ(catalog_->CreateTable(dup_cols).Error(), Err::kInvalidArgument);

    // 非法列名（空 / 超长）
    TableDef bad_name;
    bad_name.name = "badname";
    bad_name.columns.push_back({"", DataType::kInt64});
    EXPECT_EQ(catalog_->CreateTable(bad_name).Error(), Err::kInvalidArgument);
    TableDef long_name;
    long_name.name = "longname";
    long_name.columns.push_back({std::string(32, 'c'), DataType::kInt64});
    EXPECT_EQ(catalog_->CreateTable(long_name).Error(), Err::kInvalidArgument);

    // 未知类型 / 变长列带 len / 定长 len 小于固有宽度
    TableDef unknown;
    unknown.name = "unknown";
    unknown.columns.push_back({"x", static_cast<DataType>(99)});
    EXPECT_EQ(catalog_->CreateTable(unknown).Error(), Err::kInvalidArgument);

    TableDef var_len;
    var_len.name = "varlen";
    var_len.columns.push_back({"s", DataType::kUtf8Var, /*len=*/8});
    EXPECT_EQ(catalog_->CreateTable(var_len).Error(), Err::kInvalidArgument);

    TableDef narrow;
    narrow.name = "narrow";
    narrow.columns.push_back({"i", DataType::kInt64, /*len=*/4});
    EXPECT_EQ(catalog_->CreateTable(narrow).Error(), Err::kInvalidArgument);

    // 失败路径不得污染目录状态或泄漏 id。
    EXPECT_EQ(catalog_->table_count(), 1u);
    EXPECT_EQ(catalog_->next_table_id(), 2u);
    uint64_t active = 0;
    EXPECT_EQ(catalog_->Validate(&active), Err::kOk);
    EXPECT_EQ(active, 1u);
}

TEST_F(CatalogDirectTest, DroppedNameIsReusableWithNewId) {
    ASSERT_TRUE(catalog_->CreateTable(MakeDef("reuse")).IsOk());
    ASSERT_EQ(catalog_->DropTable("reuse"), Err::kOk);
    auto again = catalog_->CreateTable(MakeDef("reuse"));
    ASSERT_TRUE(again.IsOk());
    // 新表分配新 id（id 单调不回收），旧描述符保留在 id 索引中。
    EXPECT_EQ(catalog_->Descriptor(again.Value())->table_id, 2u);
    EXPECT_EQ(catalog_->table_count(), 1u);
    uint64_t active = 0;
    EXPECT_EQ(catalog_->Validate(&active), Err::kOk);
    EXPECT_EQ(active, 1u);
}

TEST_F(CatalogDirectTest, ReattachSeesSameCatalog) {
    ASSERT_TRUE(catalog_->CreateTable(MakeDef("persist", 64)).IsOk());
    catalog_.reset();  // 模拟进程退出：仅释放宿主侧句柄

    auto reattach = Catalog::Attach(mem_, alloc_.get(), sb_, /*create_new=*/false);
    ASSERT_TRUE(reattach.IsOk()) << rtdb::ErrorMessage(reattach.Error());
    catalog_ = reattach.TakeValue();
    ASSERT_TRUE(catalog_->available());
    EXPECT_EQ(catalog_->table_count(), 1u);
    EXPECT_EQ(catalog_->next_table_id(), 2u);
    auto open_res = catalog_->OpenTable("persist");
    ASSERT_TRUE(open_res.IsOk());
    EXPECT_EQ(catalog_->Descriptor(open_res.Value())->row_cap, 64u);
    uint64_t active = 0;
    EXPECT_EQ(catalog_->Validate(&active), Err::kOk);
    EXPECT_EQ(active, 1u);
}

TEST_F(CatalogDirectTest, CorruptionIsDetected) {
    ASSERT_TRUE(catalog_->CreateTable(MakeDef("good")).IsOk());
    const uint64_t root_off = sb_->catalog_desc_off;

    // 目录根块魔数损坏 → 复附着拒绝。
    const uint64_t saved_magic = Root()->magic;
    Root()->magic = 0;
    EXPECT_EQ(Catalog::Attach(mem_, alloc_.get(), sb_, false).Error(), Err::kCorruption);
    Root()->magic = saved_magic;

    // 描述符魔数损坏 → Validate 检出 kCorruption。
    auto open_res = catalog_->OpenTable("good");
    ASSERT_TRUE(open_res.IsOk());
    const uint64_t desc_off = open_res.Value();
    const uint64_t saved_desc_magic = Desc(desc_off)->magic;
    Desc(desc_off)->magic = 0;
    EXPECT_EQ(catalog_->Validate(), Err::kCorruption);
    Desc(desc_off)->magic = saved_desc_magic;
    EXPECT_EQ(catalog_->Validate(), Err::kOk);

    // SuperBlock 未记录目录偏移时的兼容行为：可附着但不可用。
    const uint64_t saved_cat_off = sb_->catalog_desc_off;
    sb_->catalog_desc_off = 0;
    auto empty = Catalog::Attach(mem_, alloc_.get(), sb_, /*create_new=*/false);
    ASSERT_TRUE(empty.IsOk());
    EXPECT_FALSE(empty.Value()->available());
    EXPECT_EQ(empty.Value()->OpenTable("good").Error(), Err::kUnsupported);
    sb_->catalog_desc_off = saved_cat_off;
    EXPECT_EQ(root_off, sb_->catalog_desc_off);
}

TEST_F(CatalogDirectTest, UninitializedCatalogReportsUnsupported) {
    // 宿主侧不写 catalog_desc_off 的映像（等价 M0 期映像）：
    // 直连 Catalog 时必须给出明确错误而非崩溃。
    char* cold = static_cast<char*>(::operator new(kRegionBytes, std::align_val_t(4096)));
    std::memset(cold, 0, kRegionBytes);
    auto* sb = reinterpret_cast<rtdb::layout::shm_SuperBlock*>(cold);
    rtdb::layout::InitSuperBlock(sb, kRegionBytes, 0u, 0u, 0);
    auto alloc_res = rtdb::infra::SlabShmAllocator::Attach(cold, kRegionBytes, true);
    ASSERT_TRUE(alloc_res.IsOk());
    auto cat_res = Catalog::Attach(cold, alloc_res.Value().get(), sb, /*create_new=*/false);
    ASSERT_TRUE(cat_res.IsOk());
    EXPECT_FALSE(cat_res.Value()->available());
    EXPECT_EQ(cat_res.Value()->CreateTable(MakeDef("x")).Error(), Err::kUnsupported);
    EXPECT_EQ(cat_res.Value()->DropTable("x"), Err::kUnsupported);
    EXPECT_EQ(cat_res.Value()->FindById(1).Error(), Err::kUnsupported);
    alloc_res.Value().reset();
    ::operator delete(cold, std::align_val_t(4096));
}

// ===========================================================================
// 引擎端到端：真实映像文件 + 跨重启持久性 + 角色语义
// ===========================================================================
class EngineCatalogTest : public ::testing::Test {
protected:
    void SetUp() override { Cleanup(); }

    void TearDown() override { Cleanup(); }

    rtdb::Options MakeOpts(rtdb::Role role = rtdb::Role::Standalone) {
        rtdb::Options opts;
        opts.instance_name = "catalog_test";
        opts.file_path = kImage;
        opts.initial_size_bytes = 1ull << 20;
        opts.role = role;
        return opts;
    }

    void Cleanup() {
        std::error_code ec;
        fs::remove(kImage, ec);
    }

    static constexpr const char* kImage = "rtdb_gtest_catalog.rtdb";
};

TEST_F(EngineCatalogTest, CreateTablePersistsAcrossReopen) {
    const auto opts = MakeOpts();
    {
        auto open_res = rtdb::Engine::Open(opts);
        ASSERT_TRUE(open_res.IsOk()) << rtdb::ErrorMessage(open_res.Error());
        auto engine = open_res.TakeValue();

        auto created = engine->CreateTable(MakeDef("telemetry", 256));
        ASSERT_TRUE(created.IsOk()) << rtdb::ErrorMessage(created.Error());
        const rtdb::TableHandle handle = created.Value();
        EXPECT_TRUE(handle.IsValid());
        EXPECT_EQ(handle.table_id(), 1);
        EXPECT_EQ(handle.name(), "telemetry");
        EXPECT_EQ(handle.column_count(), 2u);
        EXPECT_EQ(handle.row_cap(), 256u);
        EXPECT_EQ(handle.layout_version(), 1u);

        auto op = engine->OpenTable("telemetry");
        ASSERT_TRUE(op.IsOk());
        EXPECT_EQ(op.Value().table_id(), 1);
    }

    // 重启：目录从映像恢复。
    auto reopen = rtdb::Engine::Open(opts);
    ASSERT_TRUE(reopen.IsOk()) << rtdb::ErrorMessage(reopen.Error());
    auto engine = reopen.TakeValue();
    auto op = engine->OpenTable("telemetry");
    ASSERT_TRUE(op.IsOk()) << rtdb::ErrorMessage(op.Error());
    EXPECT_EQ(op.Value().row_cap(), 256u);
    EXPECT_EQ(engine->CreateTable(MakeDef("telemetry")).Error(), Err::kTableExists);
    EXPECT_EQ(engine->OpenTable("absent").Error(), Err::kTableNotFound);

    // 新表 id 单调续接。
    auto second = engine->CreateTable(MakeDef("status"));
    ASSERT_TRUE(second.IsOk());
    EXPECT_EQ(second.Value().table_id(), 2);
}

TEST_F(EngineCatalogTest, DropTablePersistsAcrossReopen) {
    const auto opts = MakeOpts();
    {
        auto open_res = rtdb::Engine::Open(opts);
        ASSERT_TRUE(open_res.IsOk());
        auto engine = open_res.TakeValue();
        ASSERT_TRUE(engine->CreateTable(MakeDef("gone")).IsOk());
        ASSERT_TRUE(engine->DropTable("gone").IsOk());
        EXPECT_EQ(engine->OpenTable("gone").Error(), Err::kTableNotFound);
        EXPECT_EQ(engine->DropTable("gone").Error(), Err::kTableNotFound);
    }
    auto reopen = rtdb::Engine::Open(opts);
    ASSERT_TRUE(reopen.IsOk());
    EXPECT_EQ(reopen.Value()->OpenTable("gone").Error(), Err::kTableNotFound);
    // 删除后表名可复用（跨重启一致）。
    EXPECT_TRUE(reopen.Value()->CreateTable(MakeDef("gone")).IsOk());
}

TEST_F(EngineCatalogTest, ReaderRoleIsReadOnly) {
    const auto write_opts = MakeOpts();
    {
        auto open_res = rtdb::Engine::Open(write_opts);
        ASSERT_TRUE(open_res.IsOk());
        auto engine = open_res.TakeValue();
        ASSERT_TRUE(engine->CreateTable(MakeDef("ro_table", 32)).IsOk());
    }
    auto reader_res = rtdb::Engine::Open(MakeOpts(rtdb::Role::Reader));
    ASSERT_TRUE(reader_res.IsOk()) << rtdb::ErrorMessage(reader_res.Error());
    auto reader = reader_res.TakeValue();

    // 只读可查表
    auto op = reader->OpenTable("ro_table");
    ASSERT_TRUE(op.IsOk());
    EXPECT_EQ(op.Value().row_cap(), 32u);
    // 写操作显式拒绝
    EXPECT_EQ(reader->CreateTable(MakeDef("nope")).Error(), Err::kReadOnlyHandle);
    EXPECT_EQ(reader->DropTable("ro_table").Error(), Err::kReadOnlyHandle);
}

TEST_F(EngineCatalogTest, WriterRoleRequiresExistingImage) {
    // Writer 角色不隐式建映像（既定语义，docs/07 §7 边界表）。
    auto missing = rtdb::Engine::Open(MakeOpts(rtdb::Role::Writer));
    ASSERT_FALSE(missing.IsOk());
    EXPECT_EQ(missing.Error(), Err::kNotFound);

    {
        auto open_res = rtdb::Engine::Open(MakeOpts());
        ASSERT_TRUE(open_res.IsOk());
    }
    auto writer = rtdb::Engine::Open(MakeOpts(rtdb::Role::Writer));
    ASSERT_TRUE(writer.IsOk());
    EXPECT_TRUE(writer.Value()->CreateTable(MakeDef("w_table")).IsOk());
}

}  // namespace
