#ifndef RTDB_OPTIONS_HPP_
#define RTDB_OPTIONS_HPP_

#include <cstdint>
#include <string>

namespace rtdb {

/// 实例角色（写入仲裁规则，见 docs/02 §2）。
enum class Role : uint8_t {
  /// 单机自治：本进程独占写入权（默认；多进程同时以 Standalone 打开同
  /// 一映像属于未定义行为，由上层保证）。
  Standalone = 0,
  /// 集群写者：参与 writer 主备仲裁。
  Writer = 1,
  /// 只读者：以只读方式映射数据区，不做任何写盘动作。
  Reader = 2,
};

/// WAL 刷盘策略（M1 生效；当前阶段仅随元数据持久化）。参考 docs/03 §3.2。
enum class FsyncPolicy : uint8_t {
  Never = 0,
  PerSecond = 1,
  Always = 2,
};

/// Engine 打开参数。
struct Options {
  /// 实例名：用作命名对象前缀与日志标识（不含路径分隔符）。
  std::string instance_name;

  /// 数据映像文件路径。内置文件映射共享内存的持久化载体，
  /// 同一文件的多次打开即多进程共享同一数据区。
  std::string file_path;

  /// 新建映像时的目标大小（含头部）。实际实现按需扩段时上限另议；
  /// 对已存在文件此参数被忽略。
  uint64_t initial_size_bytes = 64ull * 1024 * 1024;

  Role role = Role::Standalone;
  FsyncPolicy fsync = FsyncPolicy::PerSecond;
};

}  // namespace rtdb

#endif  // RTDB_OPTIONS_HPP_
