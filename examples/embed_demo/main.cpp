/// 内嵌模式最小示例：演示"打开即直读共享内存"的使用方式。

#include <cstdio>

#include "rtdb/engine.hpp"
#include "rtdb/version.hpp"

int main(int argc, char** argv) {
  const char* image_path = argc > 1 ? argv[1] : "demo_instance.rtdb";

  rtdb::Options opts;
  opts.instance_name = "demo";
  opts.file_path = image_path;
  opts.initial_size_bytes = 8 * 1024 * 1024;
  opts.role = rtdb::Role::Standalone;

  auto open_res = rtdb::Engine::Open(opts);
  if (!open_res.IsOk()) {
    std::fprintf(stderr, "open failed: %s (%d)\n",
                 rtdb::ErrorMessage(open_res.Error()),
                 static_cast<int>(open_res.Error()));
    return 1;
  }
  auto engine = open_res.TakeValue();

  std::printf(
      "rtdb %s | instance=%s | sessions=%lld committed_seq=%lld\n",
      rtdb::kVersionString, image_path,
      static_cast<long long>(engine->SessionCount()),
      static_cast<long long>(engine->CommittedSeq()));
  std::printf("(rerun to see session_count increase; tables arrive in M1)\n");
  return 0;
}
