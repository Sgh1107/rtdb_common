#include "infra/sha256.hpp"

#include <cstring>

namespace rtdb {
namespace infra {

namespace {

/// FIPS 180-4 常量 K（前 64 个素数立方根小数前 32 位）。
constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t Rotr(uint32_t x, uint32_t n) noexcept { return (x >> n) | (x << (32u - n)); }
inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) noexcept { return (x & y) ^ (~x & z); }
inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}
inline uint32_t BigSigma0(uint32_t x) noexcept { return Rotr(x, 2) ^ Rotr(x, 13) ^ Rotr(x, 22); }
inline uint32_t BigSigma1(uint32_t x) noexcept { return Rotr(x, 6) ^ Rotr(x, 11) ^ Rotr(x, 25); }
inline uint32_t SmallSigma0(uint32_t x) noexcept { return Rotr(x, 7) ^ Rotr(x, 18) ^ (x >> 3); }
inline uint32_t SmallSigma1(uint32_t x) noexcept { return Rotr(x, 17) ^ Rotr(x, 19) ^ (x >> 10); }

inline uint32_t LoadBe32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void StoreBe32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

}  // namespace

void Sha256Ctx::Reset() noexcept {
    // FIPS 180-4 初值（前 8 个素数平方根小数前 32 位）。
    h_[0] = 0x6a09e667u;
    h_[1] = 0xbb67ae85u;
    h_[2] = 0x3c6ef372u;
    h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu;
    h_[5] = 0x9b05688cu;
    h_[6] = 0x1f83d9abu;
    h_[7] = 0x5be0cd19u;
    buf_len_ = 0;
    total_ = 0;
}

void Sha256Ctx::Compress(const uint8_t block[64]) noexcept {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = LoadBe32(block + i * 4);
    for (int i = 16; i < 64; ++i) {
        w[i] = SmallSigma1(w[i - 2]) + w[i - 7] + SmallSigma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + BigSigma1(e) + Ch(e, f, g) + kK[i] + w[i];
        const uint32_t t2 = BigSigma0(a) + Maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256Ctx::Update(const void* data, size_t len) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_ += len;
    while (len > 0) {
        const size_t take = (len < (64 - buf_len_)) ? len : (64 - buf_len_);
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ == 64) {
            Compress(buf_);
            buf_len_ = 0;
        }
    }
}

std::array<uint8_t, kSha256DigestBytes> Sha256Ctx::Final() noexcept {
    // 补位：0x80 + 若干 0x00 至 56 (mod 64)，再写 8B 大端消息位长。
    const uint64_t bit_len = total_ * 8u;
    const uint8_t pad_head = 0x80;
    Update(&pad_head, 1);  // 计入 total_ 无妨：位长已先取值
    const uint8_t zero = 0;
    while (buf_len_ != 56) Update(&zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) len_be[i] = static_cast<uint8_t>(bit_len >> (56 - 8 * i));
    Update(len_be, 8);

    std::array<uint8_t, kSha256DigestBytes> out{};
    for (int i = 0; i < 8; ++i) StoreBe32(out.data() + i * 4, h_[i]);
    return out;
}

std::array<uint8_t, kSha256DigestBytes> Sha256(const void* data, size_t len) noexcept {
    Sha256Ctx ctx;
    ctx.Update(data, len);
    return ctx.Final();
}

std::array<uint8_t, kSha256DigestBytes> Sha256Concat(std::string_view a,
                                                     std::string_view b) noexcept {
    Sha256Ctx ctx;
    ctx.Update(a.data(), a.size());
    ctx.Update(b.data(), b.size());
    return ctx.Final();
}

std::string ToHex(const uint8_t* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(kHex[data[i] >> 4]);
        s.push_back(kHex[data[i] & 0x0F]);
    }
    return s;
}

bool FromHex(std::string_view hex, uint8_t* out, size_t out_len) noexcept {
    if (hex.size() != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        uint8_t v = 0;
        for (int j = 0; j < 2; ++j) {
            const char c = hex[i * 2 + static_cast<size_t>(j)];
            uint8_t nib = 0;
            if (c >= '0' && c <= '9') {
                nib = static_cast<uint8_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nib = static_cast<uint8_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                nib = static_cast<uint8_t>(c - 'A' + 10);
            } else {
                return false;
            }
            v = static_cast<uint8_t>((v << 4) | nib);
        }
        out[i] = v;
    }
    return true;
}

std::string DigestToHex(const std::array<uint8_t, kSha256DigestBytes>& d) {
    return ToHex(d.data(), d.size());
}

bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) noexcept {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

}  // namespace infra
}  // namespace rtdb
