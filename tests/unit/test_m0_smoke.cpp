/// M0 冒烟测试：平台层共享内存 + SuperBlock 校验路径全覆盖。

#include <filesystem>
#include <fstream>

#include "infra/crc32.hpp"
#include "rtdb/engine.hpp"
#include "tests/unit/simple_test_harness.hpp"

namespace {

namespace fs = std::filesystem;

const char* kTmpFile = "rtdb_m0_smoke_test.rtdb";

void CleanupTempFile() {
  std::error_code ec;
  fs::remove(kTmpFile, ec);
}

void TestCrc32KnownValue() {
  // IEEE CRC32("123456789") == 0xCBF43926 是标准校验向量。
  const uint32_t got = rtdb::infra::Crc32("123456789", 9);
  RTDB_CHECK_EQ(got, 0xCBF43926u);
}

void TestEngineCreatePersistReopen() {
  CleanupTempFile();

  rtdb::Options opts;
  opts.instance_name = "m0";
  opts.file_path = kTmpFile;
  opts.initial_size_bytes = 1024 * 1024;

  {
    auto res = rtdb::Engine::Open(opts);
    RTDB_CHECK(res.IsOk());
    if (!res.IsOk()) return;
    auto engine = res.TakeValue();
    RTDB_CHECK(engine->SessionCount() >= 1);
    RTDB_CHECK(engine->CommittedSeq() >= 0);  // 初始化后为 0
  }  // 关闭：映射解除，数据已 Flush 到映像文件

  auto res2 = rtdb::Engine::Open(opts);
  RTDB_CHECK(res2.IsOk());
  if (!res2.IsOk()) return;
  auto engine2 = res2.TakeValue();
  RTDB_CHECK(engine2->SessionCount() == 2);  // 会话计数跨进程持久累计

  CleanupTempFile();
}

void TestBadMagicRejected() {
  CleanupTempFile();
  rtdb::Options opts;
  opts.instance_name = "bad";
  opts.file_path = kTmpFile;
  opts.initial_size_bytes = 1024 * 1024;

  { auto e = rtdb::Engine::Open(opts); RTDB_CHECK(e.IsOk()); }

  // 直接破坏文件的 magic（SuperBlock 前 8 字节）。
  {
    std::fstream f(kTmpFile, std::ios::binary | std::ios::in | std::ios::out);
    char junk[8] = {'J', 'U', 'N', 'K', 'J', 'U', 'N', 'K'};
    f.seekp(0);
    f.write(junk, sizeof(junk));
    RTDB_CHECK(f.good());
  }

  auto res = rtdb::Engine::Open(opts);
  RTDB_CHECK(!res.IsOk());
  if (!res.IsOk()) {
    RTDB_CHECK(rtdb::ErrorMessage(res.Error()) != nullptr);
    RTDB_CHECK(res.Error() == rtdb::Err::kBadSuperBlock);
  }
  CleanupTempFile();
}

void TestCreateNewConflict() {
  CleanupTempFile();
  // Reader 角色要求挂载既有映像：不存在时应报 kNotFound。
  rtdb::Options ro_opts;
  ro_opts.instance_name = "reader";
  ro_opts.file_path = kTmpFile;
  ro_opts.role = rtdb::Role::Reader;
  auto missing = rtdb::Engine::Open(ro_opts);
  RTDB_CHECK(!missing.IsOk());
  RTDB_CHECK(missing.Error() == rtdb::Err::kNotFound);
  CleanupTempFile();
}

}  // namespace

int main() {
  using namespace test_harness;
  RTDB_RUN_CASE("crc32 known vector", TestCrc32KnownValue);
  RTDB_RUN_CASE("engine create/persist/reopen", TestEngineCreatePersistReopen);
  RTDB_RUN_CASE("bad magic rejected", TestBadMagicRejected);
  RTDB_RUN_CASE("open-missing reader fails", TestCreateNewConflict);

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
