#ifndef RTDB_SRC_NET_WIRE_HPP_
#define RTDB_SRC_NET_WIRE_HPP_

/// \file
/// \brief 协议帧编解码与收发（docs/13 §3-D5；常量面见 include/rtdb/proto_v1.h）。
///
/// 帧 = 20B 定长头 + 载荷；CRC32 覆盖"crc 置 0 的头 + 载荷"。
/// 请求帧 flags=0（载荷 = 请求体）；应答帧 flags=bit0（载荷 =
/// `[i16 err]` + （成功时）体 / （失败时）`[u16 msg_len][UTF-8 描述]`）。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "net/codec.hpp"
#include "platform/net.hpp"
#include "rtdb/err.hpp"

namespace rtdb {
namespace net {

inline constexpr uint32_t kFrameHeaderBytes = 20;
inline constexpr uint8_t kProtoVersion = 1;
/// 应答帧标志（flags bit0）。
inline constexpr uint8_t kFlagResp = 0x01;
inline constexpr uint32_t kDefaultMaxPayloadBytes = 4u * 1024u * 1024u;

/// 帧头（内存态；编码为小端 20B）。
struct FrameHeader {
    uint8_t ver = kProtoVersion;
    uint8_t flags = 0;
    uint32_t session = 0;
    uint16_t func = 0;
    uint32_t payload_len = 0;
    uint32_t crc = 0;
};

/// 编码帧头（h.crc 原样写入；CRC 计算用 ComputeFrameCrc）。
void EncodeFrameHeader(const FrameHeader& h, uint8_t* out) noexcept;

/// 解码并校验帧头（magic/版本/压缩位/载荷上限）；违例 → kProtocolError。
Err DecodeFrameHeader(const uint8_t* in, uint32_t max_payload, FrameHeader* out) noexcept;

/// CRC32 = Crc32(头 crc 字段置 0 + 载荷)。
uint32_t ComputeFrameCrc(const FrameHeader& h, const uint8_t* payload) noexcept;

/// 发送一帧（自动计算 CRC；payload 可为 nullptr 当 payload_len==0）。
Err WriteFrame(platform::TcpSocket& sock, FrameHeader h, const void* payload) noexcept;

/// 读取一帧（校验全项；payload 输出至 vector）。
Err ReadFrame(platform::TcpSocket& sock, uint32_t max_payload, FrameHeader* h,
              std::vector<uint8_t>* payload) noexcept;

/// 应答前导：`[i16 err]`；err != kOk 时附 `[u16 msg_len][描述]`。
/// 成功路径由调用方随后追加应答体。
void WriteResponsePrologue(Writer& w, Err err, std::string_view msg = {}) noexcept;

/// 解析应答前导；返回错误码（kOk = 成功；msg 在失败时被填充）。
/// Reader 失败态返回 kProtocolError。
Err ReadResponsePrologue(Reader& r, std::string* msg) noexcept;

}  // namespace net
}  // namespace rtdb

#endif  // RTDB_SRC_NET_WIRE_HPP_
