#include "net/wire.hpp"

#include <cstring>

#include "infra/crc32.hpp"

namespace rtdb {
namespace net {

namespace {

inline void PutU16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void PutU32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
inline uint16_t GetU16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t GetU32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

void EncodeFrameHeader(const FrameHeader& h, uint8_t* out) noexcept {
    out[0] = 'R';
    out[1] = 'T';
    out[2] = 'D';
    out[3] = 'B';
    out[4] = h.ver;
    out[5] = h.flags;
    PutU32(out + 6, h.session);
    PutU16(out + 10, h.func);
    PutU32(out + 12, h.payload_len);
    PutU32(out + 16, h.crc);
}

Err DecodeFrameHeader(const uint8_t* in, uint32_t max_payload, FrameHeader* out) noexcept {
    if (in == nullptr || out == nullptr) return Err::kInvalidArgument;
    if (in[0] != 'R' || in[1] != 'T' || in[2] != 'D' || in[3] != 'B') return Err::kProtocolError;

    FrameHeader h;
    h.ver = in[4];
    h.flags = in[5];
    h.session = GetU32(in + 6);
    h.func = GetU16(in + 10);
    h.payload_len = GetU32(in + 12);
    h.crc = GetU32(in + 16);

    if (h.ver != kProtoVersion) return Err::kProtocolError;
    if ((h.flags & 0x02u) != 0) return Err::kProtocolError;  // 压缩位 v1 恒 0
    if (h.payload_len > max_payload) return Err::kProtocolError;
    *out = h;
    return Err::kOk;
}

uint32_t ComputeFrameCrc(const FrameHeader& h, const uint8_t* payload) noexcept {
    FrameHeader zeroed = h;
    zeroed.crc = 0;
    zeroed.ver = kProtoVersion;  // 编码路径恒以当前版本写头
    uint8_t hdr[kFrameHeaderBytes];
    EncodeFrameHeader(zeroed, hdr);
    uint32_t crc = infra::Crc32Update(0, hdr, sizeof(hdr));
    if (payload != nullptr && zeroed.payload_len > 0) {
        crc = infra::Crc32Update(crc, payload, zeroed.payload_len);
    }
    return crc;
}

Err WriteFrame(platform::TcpSocket& sock, FrameHeader h, const void* payload) noexcept {
    if (h.payload_len > kDefaultMaxPayloadBytes) return Err::kInvalidArgument;
    if (h.payload_len > 0 && payload == nullptr) return Err::kInvalidArgument;
    h.ver = kProtoVersion;
    h.crc = ComputeFrameCrc(h, static_cast<const uint8_t*>(payload));
    uint8_t hdr[kFrameHeaderBytes];
    EncodeFrameHeader(h, hdr);
    Err e = sock.SendAll(hdr, sizeof(hdr));
    if (e != Err::kOk) return e;
    if (h.payload_len > 0) {
        e = sock.SendAll(payload, h.payload_len);
        if (e != Err::kOk) return e;
    }
    return Err::kOk;
}

Err ReadFrame(platform::TcpSocket& sock, uint32_t max_payload, FrameHeader* h,
              std::vector<uint8_t>* payload) noexcept {
    if (h == nullptr || payload == nullptr) return Err::kInvalidArgument;
    if (max_payload == 0 || max_payload > kDefaultMaxPayloadBytes) {
        max_payload = kDefaultMaxPayloadBytes;
    }
    uint8_t hdr[kFrameHeaderBytes];
    Err e = sock.RecvExact(hdr, sizeof(hdr));
    if (e != Err::kOk) return e;
    e = DecodeFrameHeader(hdr, max_payload, h);
    if (e != Err::kOk) return e;

    payload->clear();
    if (h->payload_len > 0) {
        payload->resize(h->payload_len);
        e = sock.RecvExact(payload->data(), h->payload_len);
        if (e != Err::kOk) {
            payload->clear();
            return e;
        }
    }
    const uint8_t* body = payload->empty() ? nullptr : payload->data();
    if (ComputeFrameCrc(*h, body) != h->crc) {
        payload->clear();
        return Err::kProtocolError;
    }
    return Err::kOk;
}

void WriteResponsePrologue(Writer& w, Err err, std::string_view msg) noexcept {
    w.I16(static_cast<int16_t>(err));
    if (err != Err::kOk) {
        std::string_view m = msg;
        if (m.empty()) m = std::string_view(ErrorMessage(err));
        if (m.size() > 0xFFFFu) m = m.substr(0, 0xFFFFu);
        w.Str(m);
    }
}

Err ReadResponsePrologue(Reader& r, std::string* msg) noexcept {
    const int16_t code = r.I16();
    if (!r.ok()) return Err::kProtocolError;
    if (code != 0 && msg != nullptr) r.Str(msg);
    if (code != 0 && !r.ok()) return Err::kProtocolError;
    return static_cast<Err>(code);
}

}  // namespace net
}  // namespace rtdb
