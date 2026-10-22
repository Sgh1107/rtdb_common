/// W3 行存储与 CRUD 单测：
///  - 存储层直测：主键索引、版本数组/墓碑不变量、变长池计账（更新/删除回收）；
///  - 引擎端到端：全类型往返、空值语义、主键唯一/复用、重启持久、
///    行槽耗尽、Reader 只读。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "infra/failpoint.hpp"
#include "infra/layout.hpp"
#include "infra/shm_region.hpp"
#include "infra/slab_allocator.hpp"
#include "rtdb/engine.hpp"
#include "storage/catalog.hpp"
#include "storage/table_store.hpp"
#include "storage/var_pool.hpp"

namespace {

namespace fs = std::filesystem;
using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::Err;
using rtdb::TableDef;
using rtdb::TableHandle;

/// 全类型表：id(i64,NOT NULL,主键) f64 i32 flag note(utf8) blob(bytes)
TableDef MakeRichDef(const std::string& name, uint32_t row_cap = 64) {
    TableDef def;
    def.name = name;
    def.row_cap = row_cap;
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"temp", DataType::kFloat64, 0, true, false});
    def.columns.push_back({"count", DataType::kInt32, 0, true, false});
    def.columns.push_back({"flag", DataType::kBool, 0, true, false});
    def.columns.push_back({"note", DataType::kUtf8Var, 0, true, false});
    def.columns.push_back({"blob", DataType::kBytesVar, 0, true, false});
    return def;
}

std::vector<ColumnValue> MakeRichRow(int64_t id, double temp, int32_t count, bool flag,
                                     const std::string& note, const std::string& blob) {
    return {ColumnValue::Int(id),
            ColumnValue::Real(temp),
            ColumnValue::IntOf(DataType::kInt32, count),
            ColumnValue::Bool(flag),
            ColumnValue::Text(note),
            ColumnValue::Bytes(blob)};
}

// ===========================================================================
// 引擎端到端 CRUD
// ===========================================================================
class EngineCrudTest : public ::testing::Test {
protected:
    void SetUp() override { Cleanup(); }
    void TearDown() override { Cleanup(); }

    rtdb::Options MakeOpts(rtdb::Role role = rtdb::Role::Standalone) const {
        rtdb::Options opts;
        opts.instance = "crud_test";
        opts.path = kImage;
        opts.initial_size_bytes = 2ull << 20;  // 2MB
        opts.role = role;
        return opts;
    }

    void Cleanup() {
        std::error_code ec;
        fs::remove(kImage, ec);
    }

    static constexpr const char* kImage = "rtdb_gtest_crud.rtdb";
};

TEST_F(EngineCrudTest, InsertGetAllTypesRoundtrip) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk()) << rtdb::ErrorMessage(open_res.Error());
    auto engine = open_res.TakeValue();
    auto table_res = engine->CreateTable(MakeRichDef("rich"));
    ASSERT_TRUE(table_res.IsOk()) << rtdb::ErrorMessage(table_res.Error());
    const TableHandle table = table_res.Value();

    auto ins =
        engine->Insert(table, MakeRichRow(7, 21.5, -3, true, "hello", std::string("\x00\x01", 2)));
    ASSERT_TRUE(ins.IsOk()) << rtdb::ErrorMessage(ins.Error());
    EXPECT_EQ(ins.Value(), 0u);  // 首个行槽

    auto got = engine->GetByKey(table, ColumnValue::Int(7));
    ASSERT_TRUE(got.IsOk()) << rtdb::ErrorMessage(got.Error());
    const auto& row = got.Value();
    ASSERT_EQ(row.size(), 6u);
    EXPECT_EQ(row[0].as_int(), 7);
    EXPECT_DOUBLE_EQ(row[1].as_real(), 21.5);
    EXPECT_EQ(row[2].as_int(), -3);  // i32 符号扩展
    EXPECT_TRUE(row[3].as_bool());
    EXPECT_EQ(row[4].as_bytes(), "hello");
    EXPECT_EQ(row[5].as_bytes(), std::string("\x00\x01", 2));  // 二进制含 NUL

    EXPECT_TRUE(engine->ContainsKey(table, ColumnValue::Int(7)).Value());
    EXPECT_FALSE(engine->ContainsKey(table, ColumnValue::Int(8)).Value());
    EXPECT_EQ(engine->GetByKey(table, ColumnValue::Int(8)).Error(), Err::kNotFound);
}

TEST_F(EngineCrudTest, NullSemantics) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    const TableHandle table = engine->CreateTable(MakeRichDef("nullable")).TakeValue();

    // 可空列写 NULL → 读回仍为 NULL。
    std::vector<ColumnValue> row = MakeRichRow(1, 0.0, 0, false, "x", "y");
    row[1] = ColumnValue::Null();  // temp
    row[2] = ColumnValue::Null();  // count
    row[4] = ColumnValue::Null();  // note
    row[5] = ColumnValue::Null();  // blob
    ASSERT_TRUE(engine->Insert(table, row).IsOk());

    auto got = engine->GetByKey(table, ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk());
    EXPECT_TRUE(got.Value()[1].is_null());
    EXPECT_TRUE(got.Value()[2].is_null());
    EXPECT_FALSE(got.Value()[3].is_null());
    EXPECT_TRUE(got.Value()[4].is_null());
    EXPECT_TRUE(got.Value()[5].is_null());

    // 空字符串与 NULL 可区分。
    std::vector<ColumnValue> row2 = MakeRichRow(2, 1.0, 1, true, "", "");
    ASSERT_TRUE(engine->Insert(table, row2).IsOk());
    auto got2 = engine->GetByKey(table, ColumnValue::Int(2));
    ASSERT_TRUE(got2.IsOk());
    EXPECT_FALSE(got2.Value()[4].is_null());
    EXPECT_EQ(got2.Value()[4].as_bytes(), "");

    // NOT NULL 主键写 NULL → 拒绝。
    std::vector<ColumnValue> bad = MakeRichRow(0, 1.0, 1, true, "n", "b");
    bad[0] = ColumnValue::Null();
    EXPECT_EQ(engine->Insert(table, bad).Error(), Err::kInvalidArgument);
}

TEST_F(EngineCrudTest, UniqueKeyUpdateAndDeleteReuse) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    const TableHandle table = engine->CreateTable(MakeRichDef("crud")).TakeValue();

    ASSERT_TRUE(engine->Insert(table, MakeRichRow(10, 1.0, 1, true, "a", "A")).IsOk());
    // 主键唯一
    EXPECT_EQ(engine->Insert(table, MakeRichRow(10, 2.0, 2, false, "b", "B")).Error(),
              Err::kAlreadyExists);
    // 列数不符
    EXPECT_EQ(engine->Insert(table, {ColumnValue::Int(11)}).Error(), Err::kInvalidArgument);

    // 更新：值变更、变长内容替换
    ASSERT_TRUE(engine
                    ->UpdateByKey(table, ColumnValue::Int(10),
                                  MakeRichRow(10, 2.5, 5, false, "updated", "B2"))
                    .IsOk());
    auto got = engine->GetByKey(table, ColumnValue::Int(10));
    ASSERT_TRUE(got.IsOk());
    EXPECT_DOUBLE_EQ(got.Value()[1].as_real(), 2.5);
    EXPECT_EQ(got.Value()[4].as_bytes(), "updated");

    // 更新不存在的主键 / 变更主键
    EXPECT_EQ(engine->UpdateByKey(table, ColumnValue::Int(99), MakeRichRow(99, 0, 0, false, "", ""))
                  .Error(),
              Err::kNotFound);
    EXPECT_EQ(engine->UpdateByKey(table, ColumnValue::Int(10), MakeRichRow(11, 0, 0, false, "", ""))
                  .Error(),
              Err::kInvalidArgument);

    // 删除 → 不可见；再次删除 → kNotFound；行槽复用（P0-3）
    ASSERT_TRUE(engine->DeleteByKey(table, ColumnValue::Int(10)).IsOk());
    EXPECT_EQ(engine->GetByKey(table, ColumnValue::Int(10)).Error(), Err::kNotFound);
    EXPECT_EQ(engine->DeleteByKey(table, ColumnValue::Int(10)).Error(), Err::kNotFound);
    auto reinsert = engine->Insert(table, MakeRichRow(10, 9.0, 9, true, "again", "Z"));
    ASSERT_TRUE(reinsert.IsOk());
    EXPECT_EQ(reinsert.Value(), 0u);  // 复用刚删除的墓碑槽位 0
    EXPECT_EQ(engine->GetByKey(table, ColumnValue::Int(10)).Value()[4].as_bytes(), "again");
}

TEST_F(EngineCrudTest, ScanLimitedPagesWithoutRescanningTail) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    const TableHandle table = engine->CreateTable(MakeRichDef("paged", 64)).TakeValue();
    for (int64_t i = 1; i <= 10; ++i) {
        ASSERT_TRUE(engine
                        ->Insert(table, MakeRichRow(i, 0, static_cast<int32_t>(i), false, "n", ""))
                        .IsOk());
    }

    // 每页 3 行：3/3/3/1，页间以末行主键（开区间）续扫。
    int64_t expect_id = 1;
    ColumnValue lo = ColumnValue::Null();
    bool has_lo = false;
    for (int page = 0; page < 4; ++page) {
        rtdb::Engine::KeyRange range;
        if (has_lo) {
            range.lo = lo;
            range.lo_inclusive = false;
        }
        bool more = false;
        auto res = engine->ScanLimited(table, range, 3, &more);
        ASSERT_TRUE(res.IsOk());
        const auto& rows = res.Value();
        const size_t expect_n = page < 3 ? 3u : 1u;
        ASSERT_EQ(rows.size(), expect_n) << "page " << page;
        for (const auto& row : rows) {
            ASSERT_FALSE(row.empty());
            EXPECT_EQ(row[0].as_int(), expect_id++);
        }
        EXPECT_EQ(more, page < 3) << "page " << page;
        if (!rows.empty()) {
            lo = rows.back()[0];
            has_lo = true;
        }
    }
    EXPECT_EQ(expect_id, 11);
}

TEST_F(EngineCrudTest, RowSlotReuseSurvivesChurnAndReplay) {
    {
        auto open_res = rtdb::Engine::Open(MakeOpts());
        ASSERT_TRUE(open_res.IsOk());
        auto engine = open_res.TakeValue();
        const TableHandle table = engine->CreateTable(MakeRichDef("reuse", 4)).TakeValue();

        for (int64_t i = 1; i <= 4; ++i) {
            ASSERT_TRUE(engine
                            ->Insert(table, MakeRichRow(i, 0.0, static_cast<int32_t>(i), false,
                                                        "v" + std::to_string(i), ""))
                            .IsOk());
        }
        // row_cap 已满且无墓碑：拒绝（仍是硬上限）。
        EXPECT_EQ(engine->Insert(table, MakeRichRow(9, 0, 0, false, "", "")).Error(),
                  Err::kOutOfMemory);

        // 删除两行释放槽位 1/2；随后的插入必须复用它们而不是再耗尽 row_cap。
        ASSERT_TRUE(engine->DeleteByKey(table, ColumnValue::Int(2)).IsOk());
        ASSERT_TRUE(engine->DeleteByKey(table, ColumnValue::Int(3)).IsOk());
        auto r5 = engine->Insert(table, MakeRichRow(5, 0, 0, false, "v5", ""));
        ASSERT_TRUE(r5.IsOk());
        auto r6 = engine->Insert(table, MakeRichRow(6, 0, 0, false, "v6", ""));
        ASSERT_TRUE(r6.IsOk());
        EXPECT_EQ(r5.Value(), 1u);  // 墓碑槽位复用
        EXPECT_EQ(r6.Value(), 2u);
        EXPECT_EQ(engine->GetByKey(table, ColumnValue::Int(5)).Value()[4].as_bytes(), "v5");
        EXPECT_EQ(engine->GetByKey(table, ColumnValue::Int(2)).Error(), Err::kNotFound);
    }

    // 重开：WAL 重放期间复用槽位的版本号单调递增，旧记录不得覆盖新行。
    auto reopen = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(reopen.IsOk()) << rtdb::ErrorMessage(reopen.Error());
    auto engine = reopen.TakeValue();
    auto table_res = engine->OpenTable("reuse");
    ASSERT_TRUE(table_res.IsOk());
    const TableHandle table = table_res.Value();

    for (int64_t id : {1, 4, 5, 6}) {
        auto got = engine->GetByKey(table, ColumnValue::Int(id));
        ASSERT_TRUE(got.IsOk()) << "id=" << id;
        EXPECT_EQ(got.Value()[4].as_bytes(), "v" + std::to_string(id)) << "id=" << id;
    }
    EXPECT_EQ(engine->GetByKey(table, ColumnValue::Int(2)).Error(), Err::kNotFound);
    EXPECT_EQ(engine->GetByKey(table, ColumnValue::Int(3)).Error(), Err::kNotFound);
}

TEST_F(EngineCrudTest, CrudSurvivesReopen) {
    {
        auto open_res = rtdb::Engine::Open(MakeOpts());
        ASSERT_TRUE(open_res.IsOk());
        auto engine = open_res.TakeValue();
        const TableHandle table = engine->CreateTable(MakeRichDef("persist", 32)).TakeValue();
        for (int64_t i = 0; i < 5; ++i)
            ASSERT_TRUE(
                engine
                    ->Insert(table, MakeRichRow(i, i * 1.5, static_cast<int32_t>(i), i % 2 == 0,
                                                "n" + std::to_string(i), "b" + std::to_string(i)))
                    .IsOk());
        ASSERT_TRUE(engine->DeleteByKey(table, ColumnValue::Int(3)).IsOk());
        ASSERT_TRUE(engine
                        ->UpdateByKey(table, ColumnValue::Int(4),
                                      MakeRichRow(4, 40.0, 40, true, "updated", "b4"))
                        .IsOk());
    }

    // 重启：表结构与行内容均从映像恢复。
    auto reopen = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(reopen.IsOk()) << rtdb::ErrorMessage(reopen.Error());
    auto engine = reopen.TakeValue();
    auto table_res = engine->OpenTable("persist");
    ASSERT_TRUE(table_res.IsOk());
    const TableHandle table = table_res.Value();

    for (int64_t i : {0, 1, 2}) {
        auto got = engine->GetByKey(table, ColumnValue::Int(i));
        ASSERT_TRUE(got.IsOk()) << "row " << i;
        EXPECT_EQ(got.Value()[4].as_bytes(), "n" + std::to_string(i));
        EXPECT_EQ(got.Value()[5].as_bytes(), "b" + std::to_string(i));
    }
    EXPECT_EQ(engine->GetByKey(table, ColumnValue::Int(3)).Error(), Err::kNotFound);
    auto updated = engine->GetByKey(table, ColumnValue::Int(4));
    ASSERT_TRUE(updated.IsOk());
    EXPECT_EQ(updated.Value()[4].as_bytes(), "updated");
    EXPECT_DOUBLE_EQ(updated.Value()[1].as_real(), 40.0);

    // 删除键可再次插入（索引键已摘除）。
    EXPECT_TRUE(engine->Insert(table, MakeRichRow(3, 3.0, 3, false, "back", "b3")).IsOk());
}

TEST_F(EngineCrudTest, RowCapExhaustion) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    const TableHandle table = engine->CreateTable(MakeRichDef("tiny", /*row_cap=*/2)).TakeValue();

    ASSERT_TRUE(engine->Insert(table, MakeRichRow(1, 0, 0, false, "a", "a")).IsOk());
    ASSERT_TRUE(engine->Insert(table, MakeRichRow(2, 0, 0, false, "b", "b")).IsOk());
    EXPECT_EQ(engine->Insert(table, MakeRichRow(3, 0, 0, false, "c", "c")).Error(),
              Err::kOutOfMemory);
}

TEST_F(EngineCrudTest, ReaderRoleCanReadButNotWrite) {
    {
        auto open_res = rtdb::Engine::Open(MakeOpts());
        ASSERT_TRUE(open_res.IsOk());
        auto engine = open_res.TakeValue();
        const TableHandle table = engine->CreateTable(MakeRichDef("ro")).TakeValue();
        ASSERT_TRUE(engine->Insert(table, MakeRichRow(1, 1.0, 1, true, "ro", "RO")).IsOk());
    }
    auto reader_res = rtdb::Engine::Open(MakeOpts(rtdb::Role::Reader));
    ASSERT_TRUE(reader_res.IsOk()) << rtdb::ErrorMessage(reader_res.Error());
    auto reader = reader_res.TakeValue();
    const TableHandle table = reader->OpenTable("ro").TakeValue();

    auto got = reader->GetByKey(table, ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk());
    EXPECT_EQ(got.Value()[4].as_bytes(), "ro");
    EXPECT_TRUE(reader->ContainsKey(table, ColumnValue::Int(1)).Value());
    EXPECT_EQ(reader->Insert(table, MakeRichRow(2, 0, 0, false, "x", "x")).Error(),
              Err::kReadOnlyHandle);
    EXPECT_EQ(reader->DeleteByKey(table, ColumnValue::Int(1)).Error(), Err::kReadOnlyHandle);
}

TEST_F(EngineCrudTest, InvalidTableValueRejected) {
    auto open_res = rtdb::Engine::Open(MakeOpts());
    ASSERT_TRUE(open_res.IsOk());
    auto engine = open_res.TakeValue();
    const TableHandle table = engine->CreateTable(MakeRichDef("t")).TakeValue();

    // 主键类型不受支持（变长/浮点）→ 明确错误
    EXPECT_EQ(engine->GetByKey(table, ColumnValue::Text("k")).Error(), Err::kInvalidArgument);
    EXPECT_EQ(engine->DeleteByKey(table, ColumnValue::Real(1.0)).Error(), Err::kInvalidArgument);
    // 无效句柄
    const TableHandle empty;
    EXPECT_EQ(engine->Insert(empty, MakeRichRow(1, 0, 0, false, "", "")).Error(),
              Err::kInvalidArgument);
}

// ===========================================================================
// 存储层直测：不变量与变长池回收
// ===========================================================================
class TableStoreDirectTest : public ::testing::Test {
protected:
    static constexpr uint64_t kRegionBytes = 1ull << 20;

    void SetUp() override {
        mem_ = static_cast<char*>(::operator new(kRegionBytes, std::align_val_t(4096)));
        std::memset(mem_, 0, kRegionBytes);
        sb_ = reinterpret_cast<rtdb::layout::shm_SuperBlock*>(mem_);
        rtdb::layout::InitSuperBlock(sb_, kRegionBytes, 0u, 0u, 0);
        alloc_ = rtdb::infra::SlabShmAllocator::Attach(mem_, kRegionBytes, true).TakeValue();
        catalog_ = rtdb::storage::Catalog::Attach(mem_, alloc_.get(), sb_, true).TakeValue();
    }

    void TearDown() override {
        catalog_.reset();
        alloc_.reset();
        ::operator delete(mem_, std::align_val_t(4096));
    }

    /// 变长池已用字节（经独立句柄观测，验证更新/删除的回收）。
    uint64_t PoolUsed(const rtdb::storage::shm_TableDescriptor* desc) {
        auto pool = rtdb::storage::MVarLenPool::Attach(mem_ + desc->var_pool_head,
                                                       desc->var_pool_bytes, false)
                        .TakeValue();
        return pool->UsedBytes();
    }

    char* mem_ = nullptr;
    rtdb::layout::shm_SuperBlock* sb_ = nullptr;
    std::unique_ptr<rtdb::infra::SlabShmAllocator> alloc_;
    std::unique_ptr<rtdb::storage::Catalog> catalog_;
};

TEST_F(TableStoreDirectTest, PhysicalRegionsAndInvariants) {
    auto desc_res = catalog_->CreateTable(MakeRichDef("phys", 8));
    ASSERT_TRUE(desc_res.IsOk()) << rtdb::ErrorMessage(desc_res.Error());
    const uint64_t desc_off = desc_res.Value();
    const auto* desc = catalog_->Descriptor(desc_off);
    ASSERT_NE(desc, nullptr);
    // 五个物理分区齐备且互不相同
    EXPECT_NE(desc->data_seg_head_off, 0u);
    EXPECT_NE(desc->ver_array_off, 0u);
    EXPECT_NE(desc->tombmap_off, 0u);
    EXPECT_NE(desc->var_pool_head, 0u);
    EXPECT_NE(desc->index_head_off[0], 0u);
    EXPECT_EQ(desc->var_pool_bytes, rtdb::storage::kVarPoolRegionBytes);
    for (int i = 1; i < 16; ++i) EXPECT_EQ(desc->index_head_off[i], 0u);

    auto store = rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_off).TakeValue();
    EXPECT_EQ(store->Validate(), Err::kOk);

    uint64_t row_no = 99;
    const std::string long_note(1000, 'n');
    ASSERT_EQ(store->Insert(MakeRichRow(5, 5.0, 5, true, long_note, "world"), &row_no), Err::kOk);
    EXPECT_EQ(row_no, 0u);
    EXPECT_EQ(store->live_rows(), 1u);
    EXPECT_EQ(store->used_rows(), 1u);
    EXPECT_EQ(store->Validate(), Err::kOk);
    const uint64_t used_after_insert = PoolUsed(desc);
    EXPECT_GE(used_after_insert, 1000u);

    // 更新：变长块整体替换，旧块（含 1000B 长串）被回收，占用显著下降。
    ASSERT_EQ(store->Update(ColumnValue::Int(5), MakeRichRow(5, 5.0, 5, true, "hi", "w")),
              Err::kOk);
    const uint64_t used_after_update = PoolUsed(desc);
    EXPECT_LT(used_after_update, used_after_insert);
    EXPECT_EQ(used_after_update, 80u);  // 两个最小块（40B）×2
    EXPECT_EQ(store->Validate(), Err::kOk);

    // 删除：墓碑置位、索引摘键、变长块全部回收。
    ASSERT_EQ(store->Remove(ColumnValue::Int(5)), Err::kOk);
    EXPECT_EQ(store->live_rows(), 0u);
    EXPECT_EQ(store->used_rows(), 1u);  // 高水位保持（槽位可被后续插入复用）
    EXPECT_EQ(PoolUsed(desc), 0u);
    EXPECT_EQ(store->Validate(), Err::kOk);
    EXPECT_EQ(store->Get(ColumnValue::Int(5)).Error(), Err::kNotFound);
}

TEST_F(TableStoreDirectTest, SecondaryIndexRebuildReclaimsOldNodes) {
    auto desc_res = catalog_->CreateTable(MakeRichDef("sec_rebuild", 64));
    ASSERT_TRUE(desc_res.IsOk()) << rtdb::ErrorMessage(desc_res.Error());
    auto store =
        rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_res.Value()).TakeValue();

    rtdb::SecondaryIndexDef idx;
    idx.name = "by_count";
    idx.column_index = 2;  // count（非唯一）
    idx.unique = false;
    ASSERT_EQ(store->CreateSecondaryIndex(idx), Err::kOk);
    for (int64_t i = 0; i < 32; ++i) {
        ASSERT_EQ(store->Insert(MakeRichRow(i, 0, static_cast<int32_t>(i), false, "x", ""),
                                nullptr),
                  Err::kOk);
    }

    const uint64_t used_before = alloc_->used_bytes();
    ASSERT_EQ(store->RebuildSecondaryIndexes(), Err::kOk);
    const uint64_t used_first = alloc_->used_bytes();
    // 重建后索引规模不变；旧树节点必须已归还（P0-4），不会累计增长。
    EXPECT_LE(used_first, used_before);
    for (int i = 0; i < 4; ++i) ASSERT_EQ(store->RebuildSecondaryIndexes(), Err::kOk);
    // 反复重建不得累计增长：旧树节点全部回收、复用块账本回补后应收敛稳定。
    EXPECT_LE(alloc_->used_bytes(), used_first);
    EXPECT_LE(alloc_->used_bytes(), used_before);
    EXPECT_EQ(store->Validate(), Err::kOk);
}

TEST_F(TableStoreDirectTest, FailedInsertOnReusedSlotRestoresTombstone) {
    auto desc_res = catalog_->CreateTable(MakeRichDef("reuse_fail", 8));
    ASSERT_TRUE(desc_res.IsOk()) << rtdb::ErrorMessage(desc_res.Error());
    auto store =
        rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_res.Value()).TakeValue();

    uint64_t row_no = 99;
    ASSERT_EQ(store->Insert(MakeRichRow(1, 0, 1, false, "a", ""), &row_no), Err::kOk);
    ASSERT_EQ(row_no, 0u);
    ASSERT_EQ(store->Remove(ColumnValue::Int(1)), Err::kOk);
    ASSERT_EQ(store->live_rows(), 0u);

    // 复用槽 0 的插入在索引发布点失败：ApplyStaged 内部已清墓碑并清零行。
    rtdb::failpoint::Arm("wal.index.insert", rtdb::failpoint::Mode::kError);
    rtdb::storage::StagedRowOp st;
    ASSERT_EQ(store->StageInsert(MakeRichRow(2, 0, 2, false, "b", ""), &st), Err::kOk);
    EXPECT_EQ(st.row_no, 0u);  // 墓碑槽复用
    EXPECT_EQ(store->ApplyStaged(&st), Err::kOutOfMemory);
    store->ReleaseStaged(&st);
    rtdb::failpoint::ClearAll();

    // 回滚必须恢复墓碑：空槽不可见、不计入 live，且不破坏不变量。
    EXPECT_EQ(store->live_rows(), 0u);
    EXPECT_EQ(store->Validate(), Err::kOk);
    auto has2 = store->Contains(ColumnValue::Int(2));
    ASSERT_TRUE(has2.IsOk());
    EXPECT_FALSE(has2.Value());

    // 墓碑恢复后同一槽位仍可复用。
    rtdb::storage::StagedRowOp st2;
    ASSERT_EQ(store->StageInsert(MakeRichRow(3, 0, 3, false, "c", ""), &st2), Err::kOk);
    EXPECT_EQ(st2.row_no, 0u);
    ASSERT_EQ(store->ApplyStaged(&st2), Err::kOk);
    EXPECT_EQ(store->live_rows(), 1u);
    auto got = store->Get(ColumnValue::Int(3));
    ASSERT_TRUE(got.IsOk());
    EXPECT_EQ(got.Value()[4].as_bytes(), "c");
    EXPECT_EQ(store->Validate(), Err::kOk);
}

TEST_F(TableStoreDirectTest, UnsupportedPrimaryKeyRejectedAtCreate) {
    // 变长主键
    TableDef var_pk;
    var_pk.name = "varpk";
    var_pk.columns.push_back({"k", DataType::kUtf8Var, 0, false, false});
    EXPECT_EQ(catalog_->CreateTable(var_pk).Error(), Err::kUnsupported);

    // 可空主键
    TableDef null_pk;
    null_pk.name = "nullpk";
    null_pk.columns.push_back({"k", DataType::kInt64, 0, /*nullable=*/true, false});
    EXPECT_EQ(catalog_->CreateTable(null_pk).Error(), Err::kInvalidArgument);

    // 行数据超单块上限（row_cap 过大）
    TableDef huge;
    huge.name = "huge";
    huge.row_cap = 8192;
    huge.columns.push_back({"k", DataType::kInt64, 0, false, false});
    for (int i = 0; i < 8; ++i) huge.columns.push_back({"f" + std::to_string(i), DataType::kInt64});
    EXPECT_EQ(catalog_->CreateTable(huge).Error(), Err::kLimitExceeded);
    // 失败后目录未被污染
    uint64_t active = 0;
    EXPECT_EQ(catalog_->Validate(&active), Err::kOk);
    EXPECT_EQ(active, 0u);
}

// ---------------------------------------------------------------------------
// f32 定长列与宽定长列（len > 8 的字节块）双向往返
// ---------------------------------------------------------------------------
TEST_F(TableStoreDirectTest, Float32AndWideFixedColumnRoundtrip) {
    TableDef def;
    def.name = "types";
    def.row_cap = 8;
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"f32", DataType::kFloat32, 0, true, false});
    def.columns.push_back({"tag16", DataType::kInt8, /*len=*/16, true, false});
    auto desc_res = catalog_->CreateTable(def);
    ASSERT_TRUE(desc_res.IsOk()) << rtdb::ErrorMessage(desc_res.Error());
    const auto* desc = catalog_->Descriptor(desc_res.Value());
    ASSERT_NE(desc, nullptr);
    // tag16 占据 16 字节定长区（覆盖 kInt8 固有宽度 1B）
    EXPECT_EQ(desc->cols[2].len, 16u);

    auto store =
        rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_res.Value()).TakeValue();
    const std::string tag = "0123456789abcdef";  // 16B
    ASSERT_EQ(store->Insert({ColumnValue::Int(1), ColumnValue::Real(1.5f), ColumnValue::Bytes(tag)},
                            nullptr),
              Err::kOk);

    auto got = store->Get(ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk()) << rtdb::ErrorMessage(got.Error());
    EXPECT_DOUBLE_EQ(got.Value()[1].as_real(), 1.5);  // f32 往返（值可精确表示）
    EXPECT_EQ(got.Value()[2].as_bytes(), tag);        // 宽定长列按字节块往返
    EXPECT_EQ(store->Validate(), Err::kOk);
}

// ---------------------------------------------------------------------------
// Attach 的损坏检出门槛（偏移为零 / 魔数错 / 分区缺失 / 变长池描述不一致）
// ---------------------------------------------------------------------------
TEST_F(TableStoreDirectTest, AttachRejectsCorruptedDescriptor) {
    auto desc_res = catalog_->CreateTable(MakeRichDef("guard", 8));
    ASSERT_TRUE(desc_res.IsOk());
    const uint64_t desc_off = desc_res.Value();
    auto* desc = const_cast<rtdb::storage::shm_TableDescriptor*>(catalog_->Descriptor(desc_off));

    EXPECT_EQ(rtdb::storage::TableStore::Attach(mem_, alloc_.get(), 0).Error(),
              Err::kInvalidArgument);

    const uint64_t saved_magic = desc->magic;
    desc->magic = 0;
    EXPECT_EQ(rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_off).Error(),
              Err::kCorruption);
    desc->magic = saved_magic;

    const uint64_t saved_data = desc->data_seg_head_off;
    desc->data_seg_head_off = 0;
    EXPECT_EQ(rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_off).Error(),
              Err::kCorruption);
    desc->data_seg_head_off = saved_data;

    const uint64_t saved_slot = desc->index_head_off[0];
    desc->index_head_off[0] = 0;
    EXPECT_EQ(rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_off).Error(),
              Err::kCorruption);
    desc->index_head_off[0] = saved_slot;

    const uint64_t saved_pool_bytes = desc->var_pool_bytes;
    desc->var_pool_bytes = 0;  // 池区存在但容量描述缺失
    EXPECT_EQ(rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_off).Error(),
              Err::kCorruption);
    desc->var_pool_bytes = saved_pool_bytes;

    // 恢复后应可正常附着
    EXPECT_TRUE(rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_off).IsOk());
}

// ---------------------------------------------------------------------------
// 乐观读：遭遇写入中（版本为奇数）时重试后返回 kTimeout，不采纳半更新数据
// ---------------------------------------------------------------------------
TEST_F(TableStoreDirectTest, OptimisticReadTimesOutOnInFlightWrite) {
    auto desc_res = catalog_->CreateTable(MakeRichDef("inflight", 8));
    ASSERT_TRUE(desc_res.IsOk());
    const auto* desc = catalog_->Descriptor(desc_res.Value());
    auto store =
        rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_res.Value()).TakeValue();
    ASSERT_EQ(store->Insert(MakeRichRow(1, 1.0, 1, true, "v", "b"), nullptr), Err::kOk);

    // 模拟写入方已置奇数版本但尚未完成提交。
    auto* ver = reinterpret_cast<uint64_t*>(mem_ + desc->ver_array_off);
    const uint64_t saved = *ver;
    *ver = 3;
    EXPECT_EQ(store->Get(ColumnValue::Int(1)).Error(), Err::kTimeout);

    *ver = saved;  // 恢复稳定态
    EXPECT_TRUE(store->Get(ColumnValue::Int(1)).IsOk());
}

// ---------------------------------------------------------------------------
// 区域释放（建表回滚路径）：释放分区并使后续附着失败
// ---------------------------------------------------------------------------
TEST_F(TableStoreDirectTest, ReleaseRegionsFreesPhysicalBlocks) {
    auto desc_res = catalog_->CreateTable(MakeRichDef("rollback", 8));
    ASSERT_TRUE(desc_res.IsOk());
    const uint64_t desc_off = desc_res.Value();
    const auto* desc = catalog_->Descriptor(desc_off);
    ASSERT_NE(desc, nullptr);

    const uint64_t used_before = alloc_->used_bytes();
    rtdb::storage::ReleaseRegions(desc, alloc_.get());
    EXPECT_LT(alloc_->used_bytes(), used_before);
    // 分区已归还：再次附着必须因区域缺失而失败。
    EXPECT_FALSE(rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_off).IsOk());
}

// ---------------------------------------------------------------------------
// A2（P0-2/P1-1）：发布点之后的应用失败——已发布行保持可读、变长块不得被
// 回收（重放幂等跳过无法修复，回收即引用空闲块）、行槽不得回退复用。
// ---------------------------------------------------------------------------
TEST_F(TableStoreDirectTest, PublishFailureKeepsVarBlocksOwnedAndSlotReserved) {
    auto desc_res = catalog_->CreateTable(MakeRichDef("pubfail", 8));
    ASSERT_TRUE(desc_res.IsOk()) << rtdb::ErrorMessage(desc_res.Error());
    const uint64_t desc_off = desc_res.Value();
    const auto* desc = catalog_->Descriptor(desc_off);
    ASSERT_NE(desc, nullptr);
    auto store = rtdb::storage::TableStore::Attach(mem_, alloc_.get(), desc_off).TakeValue();

    // 二级索引（列 2 = count，非唯一）：使"发布点后的二级索引维护"路径可达。
    rtdb::SecondaryIndexDef idx;
    idx.name = "by_count";
    idx.column_index = 2;
    idx.unique = false;
    ASSERT_EQ(store->CreateSecondaryIndex(idx), Err::kOk);

    ASSERT_EQ(store->Insert(MakeRichRow(1, 0.0, 1, false, "hello", "A"), nullptr), Err::kOk);
    const uint64_t used_before = PoolUsed(desc);

    // 注入：行数据 + 主键索引已发布后，二级索引维护失败。
    rtdb::failpoint::Arm("wal.secidx.after_publish", rtdb::failpoint::Mode::kError);
    EXPECT_EQ(store->Insert(MakeRichRow(2, 0.0, 2, false, "world", "B"), nullptr),
              Err::kOutOfMemory);

    // 已发布行可读且变长内容完好（旧实现：应用失败即回收其块 → 引用空闲块）。
    auto got = store->Get(ColumnValue::Int(2));
    ASSERT_TRUE(got.IsOk()) << rtdb::ErrorMessage(got.Error());
    EXPECT_EQ(got.Value()[4].as_bytes(), "world");
    EXPECT_EQ(got.Value()[5].as_bytes(), "B");
    // 块仍被持有：池占用含 "world"/"B" 两块（未回落到插入前水位）。
    EXPECT_GT(PoolUsed(desc), used_before);

    // 行槽未回退：后续插入必须用新槽（旧实现回退 row_count → 复用已发布槽）。
    uint64_t row_no = 999;
    ASSERT_EQ(store->Insert(MakeRichRow(3, 0.0, 3, false, "third", "C"), &row_no), Err::kOk);
    EXPECT_EQ(row_no, 2u);

    // 已发布行不受后续插入/池再分配影响（旧实现：块已回收 + 槽复用 → 内容被覆写）。
    got = store->Get(ColumnValue::Int(2));
    ASSERT_TRUE(got.IsOk());
    EXPECT_EQ(got.Value()[0].as_int(), 2);
    EXPECT_EQ(got.Value()[4].as_bytes(), "world");
    got = store->Get(ColumnValue::Int(3));
    ASSERT_TRUE(got.IsOk());
    EXPECT_EQ(got.Value()[4].as_bytes(), "third");
    // 后续插入的新块不来自行 2 仍持有的块（占用继续增长，无"净腾挪"）。
    EXPECT_GT(PoolUsed(desc), used_before);

    rtdb::failpoint::ClearAll();
}

}  // namespace
