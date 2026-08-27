#ifndef RTDB_SRC_PLATFORM_PLATFORM_HPP_
#define RTDB_SRC_PLATFORM_PLATFORM_HPP_

#include <cstdint>
#include <memory>
#include <string>

#include "rtdb/err.hpp"
#include "rtdb/result.hpp"
#include "rtdb/version.hpp"

namespace rtdb {
enum class OpenMode : uint8_t {
  OpenOrCreate = 0,  ///< 不存在则创建，存在则打开
  CreateNew = 1,     ///< 必须不存在，否则 kAlreadyExists
  OpenExisting = 2,  ///< 必须存在，否则 kNotFound
};

namespace platform {

struct ShmParams {
  /// 命名对象名（POSIX shm_open 名或 Windows 命名对象；当前实现以
  /// file_path 为准，本字段为未来页表 backed 匿名共享预留）。
  std::string map_name;
  std::string file_path;  ///< 映像文件路径
  uint64_t size_bytes;    ///< 新建时的目标大小
  OpenMode mode = OpenMode::OpenOrCreate;
  bool readonly = false;  ///< 只读映射（Reader 角色）
};

/// 平台无关的文件映像共享内存抽象（docs/03 §1 SharedMemoryFile）。
/// 同一 file_path 的多个实例间满足数据一致性保证（Windows 数据一致性 /
/// POSIX 同一文件的 mmap 共享页），这是跨进程直读模型的根基。
class SharedMemoryFile {
 public:
  virtual ~SharedMemoryFile() = default;

  virtual void* base_address() const noexcept = 0;
  virtual uint64_t size() const noexcept = 0;

  /// 将映射视图的指定区间回写底层文件。addr+len 必须落在视图内。
  virtual Err flush(const void* addr, size_t len) noexcept = 0;

  SharedMemoryFile() = default;
  SharedMemoryFile(const SharedMemoryFile&) = delete;
  SharedMemoryFile& operator=(const SharedMemoryFile&) = delete;
};

/// 工厂：按当前编译平台选择实现（win32 -> MapViewOfFile,
/// posix -> open+ftruncate+mmap）。
Result<std::unique_ptr<SharedMemoryFile>> OpenSharedMemory(
    const ShmParams& params);

}  // namespace platform
}  // namespace rtdb

#endif  // RTDB_SRC_PLATFORM_PLATFORM_HPP_
