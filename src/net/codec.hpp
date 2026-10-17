#ifndef RTDB_SRC_NET_CODEC_HPP_
#define RTDB_SRC_NET_CODEC_HPP_

/// \file
/// \brief 线格式读写游标（docs/13 §3-D5）：全部多字节整数显式小端编码。
///
/// `Writer` 追加写入调用方提供的 `std::vector<uint8_t>`；
/// `Reader` 具**粘滞失败态**——任何越界/违规读取使游标失效并返回零值，
/// 后续读取不再改变状态；调用方在解析尾部统一检查 `Finish()`：
///   ok 且恰好读完 = 合法载荷；否则一律 kProtocolError。
/// 该设计让"逐字段解析 + 单一出口校验"成为可能，避免每步分支。

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace rtdb {
namespace net {

/// \brief 线格式写缓冲（小端）。
class Writer {
public:
    explicit Writer(std::vector<uint8_t>* buf) noexcept : buf_(buf) {}

    void U8(uint8_t v) { buf_->push_back(v); }
    void U16(uint16_t v) {
        U8(static_cast<uint8_t>(v));
        U8(static_cast<uint8_t>(v >> 8));
    }
    void U32(uint32_t v) {
        U16(static_cast<uint16_t>(v));
        U16(static_cast<uint16_t>(v >> 16));
    }
    void U64(uint64_t v) {
        U32(static_cast<uint32_t>(v));
        U32(static_cast<uint32_t>(v >> 32));
    }
    void I16(int16_t v) { U16(static_cast<uint16_t>(v)); }
    void I64(int64_t v) { U64(static_cast<uint64_t>(v)); }
    /// 以位模式承载（非数值转换），主机字节序无关。
    void F64(double v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        U64(bits);
    }
    void Raw(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf_->insert(buf_->end(), b, b + n);
    }
    /// u16 长度前缀 + 原样字节（UTF-8 字符串/标识名）。
    void Str(std::string_view s) {
        U16(static_cast<uint16_t>(s.size()));
        if (!s.empty()) Raw(s.data(), s.size());
    }
    /// u32 长度前缀 + 原样字节（变长列内容等较大载荷）。
    void Var(std::string_view s) {
        U32(static_cast<uint32_t>(s.size()));
        if (!s.empty()) Raw(s.data(), s.size());
    }

    size_t size() const noexcept { return buf_->size(); }
    const uint8_t* data() const noexcept { return buf_->data(); }

private:
    std::vector<uint8_t>* buf_;
};

/// \brief 线格式读游标（小端；越界即粘滞失败）。
class Reader {
public:
    Reader(const uint8_t* p, size_t n) noexcept : p_(p), n_(n) {}

    /// 是否未处于失败态。
    bool ok() const noexcept { return ok_; }
    /// 剩余未读字节；失败态返回 0。
    size_t remaining() const noexcept { return ok_ ? n_ - off_ : 0; }
    /// 是否恰好读完且无失败（载荷解析的单一出口校验）。
    bool Finish() const noexcept { return ok_ && off_ == n_; }
    /// 主动置失败（如语义校验不过、字段越界）。
    void Poison() noexcept { ok_ = false; }

    uint8_t U8() {
        if (!Need(1)) return 0;
        return p_[off_++];
    }
    uint16_t U16() {
        if (!Need(2)) return 0;
        const uint16_t v = static_cast<uint16_t>(static_cast<uint16_t>(p_[off_]) |
                                                 static_cast<uint16_t>(
                                                     static_cast<uint16_t>(p_[off_ + 1]) << 8));
        off_ += 2;
        return v;
    }
    uint32_t U32() {
        if (!Need(4)) return 0;
        const uint32_t v = static_cast<uint32_t>(p_[off_]) |
                           (static_cast<uint32_t>(p_[off_ + 1]) << 8) |
                           (static_cast<uint32_t>(p_[off_ + 2]) << 16) |
                           (static_cast<uint32_t>(p_[off_ + 3]) << 24);
        off_ += 4;
        return v;
    }
    uint64_t U64() {
        if (!Need(8)) return 0;
        const uint64_t lo = static_cast<uint64_t>(p_[off_]) |
                            (static_cast<uint64_t>(p_[off_ + 1]) << 8) |
                            (static_cast<uint64_t>(p_[off_ + 2]) << 16) |
                            (static_cast<uint64_t>(p_[off_ + 3]) << 24);
        const uint64_t hi = static_cast<uint64_t>(p_[off_ + 4]) |
                            (static_cast<uint64_t>(p_[off_ + 5]) << 8) |
                            (static_cast<uint64_t>(p_[off_ + 6]) << 16) |
                            (static_cast<uint64_t>(p_[off_ + 7]) << 24);
        off_ += 8;
        return lo | (hi << 32);
    }
    int16_t I16() { return static_cast<int16_t>(U16()); }
    int64_t I64() { return static_cast<int64_t>(U64()); }
    double F64() {
        uint64_t bits = U64();
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    /// 原样读取 n 字节；越界置失败并返回 false。
    bool Raw(void* dst, size_t n) {
        if (!Need(n)) return false;
        std::memcpy(dst, p_ + off_, n);
        off_ += n;
        return true;
    }
    bool Skip(size_t n) {
        if (!Need(n)) return false;
        off_ += n;
        return true;
    }
    /// u16 长度前缀字符串（拷贝）。
    bool Str(std::string* out) {
        if (out == nullptr) return false;
        uint16_t n = U16();
        if (!Need(n)) return false;
        out->assign(reinterpret_cast<const char*>(p_ + off_), n);
        off_ += n;
        return true;
    }
    /// u16 长度前缀字符串（视图，指向底层载荷缓冲；须立即使用）。
    std::string_view StrView() {
        uint16_t n = U16();
        if (!Need(n)) return {};
        std::string_view v(reinterpret_cast<const char*>(p_ + off_), n);
        off_ += n;
        return v;
    }
    /// u32 长度前缀内容（拷贝）。
    bool Var(std::string* out) {
        if (out == nullptr) return false;
        uint32_t n = U32();
        if (!Need(n)) return false;
        out->assign(reinterpret_cast<const char*>(p_ + off_), n);
        off_ += n;
        return true;
    }

private:
    bool Need(size_t n) noexcept {
        if (!ok_ || n > n_ - off_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

}  // namespace net
}  // namespace rtdb

#endif  // RTDB_SRC_NET_CODEC_HPP_
