/// P1-2 回归：stores 句柄缓存的并发首访。
///
/// 读路径（GetByKey/ContainsKey/Scan 等）在根锁外经 StoreFor 取表句柄；
/// 实例每次运行的首访（find 未命中 → attach + emplace）若发生于多线程
/// 并发，未加锁的容器（std::unordered_map）构成数据竞争。本用例在第二
/// 实例（句柄缓存为空）上并发发起读请求，作为该修复的功能回归——功能
/// 断言在正常构建下恒过，真正的竞争探测依赖 CI 的 TSan 作业（本机
/// MSVC 不支持 TSan）。
///
/// 覆盖边界：并发只读（免锁乐观读路径 + 根锁判定于 first-touch 之外），
/// 写侧串行化语义不在本用例范围（单写者约定，见 docs/03）。

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

}  // namespace
