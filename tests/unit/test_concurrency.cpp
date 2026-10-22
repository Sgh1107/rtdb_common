/// P1-2 回归：stores 句柄缓存的并发首访。
///
/// 读路径（GetByKey/ContainsKey/Scan 等）经 StoreFor 取表句柄；实例每次
/// 运行的首访（find 未命中 → attach + emplace）若发生于多线程并发，未加锁
/// 的容器（std::unordered_map）构成数据竞争。本用例在第二实例（句柄缓存
/// 为空）上并发发起读请求，作为该修复的功能回归——功能断言在正常构建下
/// 恒过，真正的竞争探测依赖 CI 的 TSan 作业（本机 MSVC 不支持 TSan）。
///
/// 注：M3 TSan 加固后写侧引擎读路径与写路径同取根锁（免锁遍历竞争收敛，
/// 见 docs/13 §8-W5），本用例继续覆盖"多线程使用同一引擎"的功能正确性；
/// stores 缓存自身的并发首访由 stores_mu 直接保证（构建期 TSan 复核）。
///
/// 覆盖边界：并发只读；写侧串行化语义不在本用例范围（单写者约定，
/// 见 docs/03）。

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "rtdb/engine.hpp"

namespace {

namespace fs = std::filesystem;
using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::TableDef;
using rtdb::TableHandle;

void RemoveFile(const char* path) {
    std::error_code ec;
    fs::remove(path, ec);
}

class EngineConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override { RemoveFile(kImage); }

    static rtdb::Options Opts() {
        rtdb::Options opts;
        opts.instance = "engine_concurrency_test";
        opts.path = kImage;
        opts.initial_size_bytes = 2ull << 20;
        opts.role = rtdb::Role::Standalone;
        return opts;
    }

    /// 建 4 张表并各写 16 行（独立实例，写完即关）。
    static void Seed() {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
        auto eng = open.TakeValue();
        for (int ti = 0; ti < 4; ++ti) {
            TableDef def;
            def.name = "t" + std::to_string(ti);
            def.columns.push_back({"id", DataType::kInt64, 0, false, false});
            def.columns.push_back({"val", DataType::kInt64, 0, true, false});
            def.row_cap = 64;
            auto t = eng->CreateTable(def);
            ASSERT_TRUE(t.IsOk()) << rtdb::ErrorMessage(t.Error());
            for (int64_t i = 0; i < 16; ++i) {
                ASSERT_TRUE(
                    eng->Insert(t.Value(), {ColumnValue::Int(i), ColumnValue::Int(i * 10)}).IsOk());
            }
        }
    }

    static constexpr const char* kImage = "rtdb_gtest_engine_concurrency.rtdb";
};

/// 并发首访同一批表：8 线程 × 200 轮混合读（GetByKey/ContainsKey/ScanAll）。
/// 修复前为未定义行为（容器竞争）；修复后应恒过且无错误。
TEST_F(EngineConcurrencyTest, ConcurrentFirstAccessOfStores) {
    Seed();

    // 重开：句柄缓存为空——各表的相关区域首访即发生在下述并发窗口内。
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    std::vector<TableHandle> tables;
    tables.reserve(4);
    for (int ti = 0; ti < 4; ++ti) {
        auto t = eng->OpenTable("t" + std::to_string(ti));
        ASSERT_TRUE(t.IsOk()) << rtdb::ErrorMessage(t.Error());
        tables.push_back(t.Value());
    }

    std::atomic<uint64_t> failures{0};
    std::vector<std::thread> pool;
    pool.reserve(8);
    for (int th = 0; th < 8; ++th) {
        pool.emplace_back([&, th] {
            for (int i = 0; i < 200; ++i) {
                const TableHandle& t = tables[static_cast<size_t>((th + i) % 4)];
                const int64_t key = (i % 16);
                auto got = eng->GetByKey(t, ColumnValue::Int(key));
                if (!got.IsOk()) {
                    ++failures;
                    continue;
                }
                auto has = eng->ContainsKey(t, ColumnValue::Int(key));
                if (!has.IsOk() || !has.Value()) ++failures;
                auto scan = eng->ScanAll(t);
                if (!scan.IsOk() || scan.Value().size() != 16u) ++failures;
            }
        });
    }
    for (std::thread& th : pool) th.join();
    EXPECT_EQ(failures.load(), 0u);

    // 并发读不改变可见行数。
    for (const TableHandle& t : tables) {
        auto scan = eng->ScanAll(t);
        ASSERT_TRUE(scan.IsOk());
        EXPECT_EQ(scan.Value().size(), 16u);
    }
}

/// P0-1 回归：并发 Upsert 的读-改-写必须原子。
///
/// 两个写者各自只提供一列（a / b），反复 upsert 同一主键。合并必须基于
/// 锁内的最新行值：任一写者都不能用陈旧快照覆盖另一列的更新。修复前
/// （存在性判定/读/写分别取锁）可观测到丢失更新。
TEST_F(EngineConcurrencyTest, ConcurrentUpsertMergesWithoutLostUpdate) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();

    TableDef def;
    def.name = "merge";
    def.row_cap = 16;
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"a", DataType::kInt64, 0, true, false});
    def.columns.push_back({"b", DataType::kInt64, 0, true, false});
    auto table_res = eng->CreateTable(def);
    ASSERT_TRUE(table_res.IsOk());
    const TableHandle table = table_res.Value();
    ASSERT_TRUE(eng->Insert(table, {ColumnValue::Int(1), ColumnValue::Int(0),
                                    ColumnValue::Int(0)})
                    .IsOk());

    std::atomic<int> errors{0};
    std::thread ta([&] {
        for (int i = 0; i < 200; ++i) {
            auto r = eng->Upsert(table, {{"id", ColumnValue::Int(1)},
                                         {"a", ColumnValue::Int(i + 1)}});
            if (!r.IsOk()) ++errors;
        }
    });
    std::thread tb([&] {
        for (int i = 0; i < 200; ++i) {
            auto r = eng->Upsert(table, {{"id", ColumnValue::Int(1)},
                                         {"b", ColumnValue::Int(i + 1)}});
            if (!r.IsOk()) ++errors;
        }
    });
    ta.join();
    tb.join();
    EXPECT_EQ(errors.load(), 0);

    auto got = eng->GetByKey(table, ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk());
    ASSERT_EQ(got.Value().size(), 3u);
    EXPECT_GT(got.Value()[1].as_int(), 0);
    EXPECT_GT(got.Value()[2].as_int(), 0);
    // 行只应有一条（合并而非重复插入）。
    auto scan = eng->ScanAll(table);
    ASSERT_TRUE(scan.IsOk());
    EXPECT_EQ(scan.Value().size(), 1u);
}

}  // namespace
