/// 基础设施回归：CRC32、错误文本、Engine 生命周期全路径。

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "infra/crc32.hpp"
#include "rtdb/engine.hpp"

namespace fs = std::filesystem;

namespace {

const char* kTmpFile = "rtdb_gtest_infra.rtdb";

void CleanupTempFile() {
    std::error_code ec;
    fs::remove(kTmpFile, ec);
}

rtdb::Options MakeOpts(const char* name, uint64_t size = 1024 * 1024) {
    rtdb::Options o;
    o.instance_name = name;
    o.file_path = kTmpFile;
    o.initial_size_bytes = size;
    return o;
}

}  // namespace

TEST(InfraBase, Crc32IeeeKnownVector) {
    EXPECT_EQ(rtdb::infra::Crc32("123456789", 9), 0xCBF43926u);
}

TEST(InfraBase, ErrorMessageKnownCode) {
    EXPECT_NE(rtdb::ErrorMessage(rtdb::Err::kBadSuperBlock), nullptr);
}

class EngineLifecycleTest : public ::testing::Test {
protected:
    void TearDown() override { CleanupTempFile(); }
};

TEST_F(EngineLifecycleTest, CreatePersistReopen) {
    const auto opts = MakeOpts("m0");
    {
        auto res = rtdb::Engine::Open(opts);
        ASSERT_TRUE(res.IsOk());
        auto eng = res.TakeValue();
        EXPECT_GE(eng->SessionCount(), 1);
        EXPECT_EQ(eng->CommittedSeq(), 0);  // 初始化值
    }
    auto res2 = rtdb::Engine::Open(opts);
    ASSERT_TRUE(res2.IsOk());
    EXPECT_EQ(res2.Value()->SessionCount(), 2);  // 会话计数跨进程累计
}

TEST_F(EngineLifecycleTest, CorruptMagicRejected) {
    const auto opts = MakeOpts("bad");
    {
        ASSERT_TRUE(rtdb::Engine::Open(opts).IsOk());
    }
    {
        std::fstream f(kTmpFile, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(0);
        f.write("JUNKJUNK", 8);
        ASSERT_TRUE(f.good());
    }
    auto res = rtdb::Engine::Open(opts);
    ASSERT_FALSE(res.IsOk());
    EXPECT_EQ(res.Error(), rtdb::Err::kBadSuperBlock);
}

TEST_F(EngineLifecycleTest, ReaderOnMissingFailsNotFound) {
    rtdb::Options ro = MakeOpts("reader");
    ro.role = rtdb::Role::Reader;
    auto res = rtdb::Engine::Open(ro);
    ASSERT_FALSE(res.IsOk());
    EXPECT_EQ(res.Error(), rtdb::Err::kNotFound);
}

TEST_F(EngineLifecycleTest, UndersizedImageRejected) {
    // 小于 元数据区+一页数据 ⇒ 参数级拒绝。
    auto res = rtdb::Engine::Open(MakeOpts("tiny", 4096));
    ASSERT_FALSE(res.IsOk());
    EXPECT_EQ(res.Error(), rtdb::Err::kIncompatibleLayout);
}

TEST_F(EngineLifecycleTest, ReaderRoleHasNoWriteSideHandles) {
    const auto opts = MakeOpts("rw_first");
    {
        ASSERT_TRUE(rtdb::Engine::Open(opts).IsOk());
    }
    rtdb::Options ro = MakeOpts("rw_first");
    ro.role = rtdb::Role::Reader;
    auto res = rtdb::Engine::Open(ro);
    ASSERT_TRUE(res.IsOk());
    EXPECT_EQ(res.Value()->RawAllocator(), nullptr);
    EXPECT_EQ(res.Value()->RawRootMutex(), nullptr);
    EXPECT_EQ(res.Value()->LockRoot(0), rtdb::Err::kUnsupported);
}
