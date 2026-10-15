#ifndef RTDB_SRC_INFRA_CRC32_HPP_
#define RTDB_SRC_INFRA_CRC32_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rtdb {
namespace infra {

/// IEEE 802.3 CRC32（多项式 0xEDB88320），逐字节查表实现。
/// 用途仅限共享内存布局自校验与 WAL 记录尾校验，不用于安全场景。
/// 查表经函数局部静态（魔法静态，C++11 起线程安全）懒生成。
namespace crc_detail {

struct Table {
    uint32_t v[256];
    Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            v[i] = c;
        }
    }
};

inline const Table& Get() {
    static const Table t;
    return t;
}

}  // namespace crc_detail

/// \brief 续算式 CRC32：`crc` 传入上一次 `Crc32`/`Crc32Update` 的返回值
/// （首次传 0），可跨多段数据累积；结果与对拼接数据一次性计算等价。
inline uint32_t Crc32Update(uint32_t crc, const void* data, size_t len) noexcept {
    const uint32_t* table = crc_detail::Get().v;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) c = table[(c ^ bytes[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/// 单段便捷封装（语义与 W0 版本完全一致）。
inline uint32_t Crc32(const void* data, size_t len) noexcept {
    return Crc32Update(0, data, len);
}

}  // namespace infra
}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_CRC32_HPP_
