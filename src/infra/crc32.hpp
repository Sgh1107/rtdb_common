#ifndef RTDB_SRC_INFRA_CRC32_HPP_
#define RTDB_SRC_INFRA_CRC32_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rtdb {
namespace infra {

/// IEEE 802.3 CRC32（多项式 0xEDB88320），逐字节查表实现。
/// 用途仅限共享内存布局自校验，不用于安全场景。查表懒生成，
/// 线程内首次调用构建（首次使用在引擎单线程初始化路径上）。
inline uint32_t Crc32(const void* data, size_t len) noexcept {
  static uint32_t table[256];
  static bool table_ready = false;
  if (!table_ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    table_ready = true;
  }

  uint32_t crc = 0xFFFFFFFFu;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i)
    crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace infra
}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_CRC32_HPP_
