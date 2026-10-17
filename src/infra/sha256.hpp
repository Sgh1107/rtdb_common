#ifndef RTDB_SRC_INFRA_SHA256_HPP_
#define RTDB_SRC_INFRA_SHA256_HPP_

/// \file
/// \brief SHA-256（FIPS 180-4 纯自实现，零第三方依赖）。
///
/// 用途：rtdbd AUTH 口令摘要（`SHA-256(salt || password)`，docs/13 §3-D6）
/// 与 rtdb_client（W4）对侧计算；亦供测试向量校验。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "rtdb/api_macro.hpp"

namespace rtdb {
namespace infra {

/// 摘要字节数（32）。
inline constexpr size_t kSha256DigestBytes = 32;

/// \brief 流式 SHA-256 上下文（Update 可多次；Final 后可 Reset 复用）。
class RTDB_API Sha256Ctx {
public:
    Sha256Ctx() noexcept { Reset(); }

    void Reset() noexcept;

    /// 追加数据（任意长度）。
    void Update(const void* data, size_t len) noexcept;

    /// 完成并输出 32B 摘要（大端字节序）。
    std::array<uint8_t, kSha256DigestBytes> Final() noexcept;

private:
    void Compress(const uint8_t block[64]) noexcept;

    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

/// 一次性摘要。
RTDB_API std::array<uint8_t, kSha256DigestBytes> Sha256(const void* data, size_t len) noexcept;

/// 拼接摘要 `SHA-256(a || b)`（免中间分配；AUTH 口令路径）。
RTDB_API std::array<uint8_t, kSha256DigestBytes> Sha256Concat(std::string_view a,
                                                              std::string_view b) noexcept;

/// 摘要转小写十六进制（64 字符）。
RTDB_API std::string ToHex(const uint8_t* data, size_t len);

/// 十六进制转字节（要求恰 `out_len * 2` 个字符、大小写不敏感）；非法返回 false。
RTDB_API bool FromHex(std::string_view hex, uint8_t* out, size_t out_len) noexcept;

/// 摘要的定长小写十六进制字符串。
RTDB_API std::string DigestToHex(const std::array<uint8_t, kSha256DigestBytes>& d);

/// 定长常量时间比较（防时序侧信道；AUTH 校验用）。
RTDB_API bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) noexcept;

}  // namespace infra
}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_SHA256_HPP_
