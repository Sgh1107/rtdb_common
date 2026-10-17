/// 基础设施回归：CRC32、SHA-256、错误文本、Engine 生命周期全路径。

#include <array>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "infra/crc32.hpp"
#include "infra/sha256.hpp"
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
    o.instance = name;
    o.path = kTmpFile;
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

TEST(InfraBase, Sha256KnownVectors) {
    // FIPS 180-4 / NIST 示例向量（空串、"abc"、两句拼接）。
    EXPECT_EQ(rtdb::infra::DigestToHex(rtdb::infra::Sha256("", 0)),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(rtdb::infra::DigestToHex(rtdb::infra::Sha256("abc", 3)),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two_block =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_EQ(rtdb::infra::DigestToHex(
                  rtdb::infra::Sha256(two_block.data(), two_block.size())),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // 一百万 'a'（FIPS 长向量）——流式分块喂入，顺带覆盖多块路径。
    {
        const std::string chunk(1000, 'a');
        rtdb::infra::Sha256Ctx ctx;
        for (int i = 0; i < 1000; ++i) ctx.Update(chunk.data(), chunk.size());
        EXPECT_EQ(rtdb::infra::DigestToHex(ctx.Final()),
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
}

TEST(InfraBase, Sha256ChunkedAndHexRoundtrip) {
    const std::string msg(1000, 'x');
    // 分块（含 0 长度片段、跨 64B 边界）与一次性等价。
    rtdb::infra::Sha256Ctx ctx;
    size_t off = 0;
    const size_t steps[] = {0, 1, 63, 64, 65, 200};
    for (size_t s : steps) {
        ctx.Update(msg.data() + off, s);
        off += s;
    }
    ctx.Update(msg.data() + off, msg.size() - off);
    EXPECT_EQ(rtdb::infra::DigestToHex(ctx.Final()),
              rtdb::infra::DigestToHex(rtdb::infra::Sha256(msg.data(), msg.size())));

    // 拼接摘要 == 合并字节摘要（AUTH 口令路径）。
    EXPECT_EQ(rtdb::infra::DigestToHex(rtdb::infra::Sha256Concat("salt-", "pw")),
              rtdb::infra::DigestToHex(rtdb::infra::Sha256("salt-pw", 7)));

    // hex 往返 + 定长常量时间比较。
    const auto d = rtdb::infra::Sha256("abc", 3);
    std::array<uint8_t, 32> back{};
    EXPECT_TRUE(rtdb::infra::FromHex(rtdb::infra::DigestToHex(d), back.data(), back.size()));
    EXPECT_TRUE(rtdb::infra::ConstantTimeEqual(d.data(), back.data(), back.size()));
    back[31] ^= 1;
    EXPECT_FALSE(rtdb::infra::ConstantTimeEqual(d.data(), back.data(), back.size()));
    EXPECT_FALSE(rtdb::infra::FromHex("zz", back.data(), 1));   // 非法字符
    EXPECT_FALSE(rtdb::infra::FromHex("abc", back.data(), 1));  // 长度不符
}

class EngineLifecycleTest : public ::testing::Test {
protected:
    void TearDown() override { CleanupTempFile(); }
};

TEST_F(EngineLifecycleTest, CreatePersistReopen) {
    const auto opts = MakeOpts("m0");
    {
        auto res = rtdb::Engine::Open(opts);
        ASSERT_TRUE(res.IsOk()) << "create: " << rtdb::ErrorMessage(res.Error());
        auto eng = res.TakeValue();
        EXPECT_GE(eng->SessionCount(), 1);
        EXPECT_EQ(eng->CommittedSeq(), 0);  // 初始化值
    }
    auto res2 = rtdb::Engine::Open(opts);
    ASSERT_TRUE(res2.IsOk()) << "reopen: " << rtdb::ErrorMessage(res2.Error());
    EXPECT_EQ(res2.Value()->SessionCount(), 2);  // 会话计数跨进程累计
}

TEST_F(EngineLifecycleTest, CorruptMagicRejected) {
    const auto opts = MakeOpts("bad");
    {
        auto res = rtdb::Engine::Open(opts);
        ASSERT_TRUE(res.IsOk()) << "create: " << rtdb::ErrorMessage(res.Error());
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
        auto res = rtdb::Engine::Open(opts);
        ASSERT_TRUE(res.IsOk()) << "create: " << rtdb::ErrorMessage(res.Error());
    }
    rtdb::Options ro = MakeOpts("rw_first");
    ro.role = rtdb::Role::Reader;
    auto res = rtdb::Engine::Open(ro);
    ASSERT_TRUE(res.IsOk()) << "reader: " << rtdb::ErrorMessage(res.Error());
    EXPECT_EQ(res.Value()->RawAllocator(), nullptr);
    EXPECT_EQ(res.Value()->RawRootMutex(), nullptr);
    EXPECT_EQ(res.Value()->LockRoot(0), rtdb::Err::kUnsupported);
}
