#ifndef RTDB_SRC_SERVER_CONFIG_HPP_
#define RTDB_SRC_SERVER_CONFIG_HPP_

/// \file
/// \brief rtdbd 配置（文本 `key = value` 解析；CLI 参数以覆盖字段，docs/13 §5-W3）。
///
/// 字段语义（docs/13 §3-D6/D7）：
///  - `user = <name> <64 位十六进制摘要>`：内联用户表，摘要 =
///    `SHA-256(auth_salt || password)`（盐为部署共享值，非机密）；
///  - 会话 TTL 滚动续期；超限/超时给 404/502（协议错误面）；
///  - 监听默认仅回环（安全默认，docs/13 §7）；`0.0.0.0`/`::` 需显式配置。

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "infra/sha256.hpp"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"
#include "rtdb/result.hpp"

namespace rtdb {
namespace server {

/// 内联用户表项。
struct UserEntry {
    std::string name;
    std::array<uint8_t, infra::kSha256DigestBytes> digest{};
};

/// rtdbd 运行配置。
struct Config {
    std::string instance = "rtdbd";
    std::string path;                       ///< 映像路径（必填）
    std::string listen_host = "127.0.0.1";  ///< 默认仅回环（安全默认）
    uint16_t listen_port = 5599;
    uint64_t initial_size_mb = 256;
    FsyncPolicy fsync = FsyncPolicy::PerSecond;
    std::string auth_salt;             ///< 口令盐（部署共享）
    std::vector<UserEntry> users;      ///< 空 = 拒绝全部 AUTH（501）
    uint32_t session_ttl_ms = 30u * 60u * 1000u;  ///< 会话 TTL（滚动续期）
    uint32_t max_sessions = 64;                   ///< 并发会话上限（404）
    uint32_t max_payload_bytes = 4u << 20;        ///< 帧载荷上限（4 MiB）
    uint32_t heartbeat_ms = 1000;                 ///< 写者心跳周期
    bool snapshot_on_exit = false;                ///< 优雅退出时抓冷快照
    uint32_t max_delta_bytes = 1u << 20;          ///< FETCH_DELTA 响应体上限
    uint32_t delta_max_timeout_ms = 25000;        ///< FETCH_DELTA 长轮询上限
};

/// 解析配置文本（`#` 注释；未知键/非法值 → kInvalidArgument）。
Result<Config> ParseConfigText(std::string_view text);

/// 读文件并解析；打开/读失败 → kIoError。
Result<Config> LoadConfigFile(const std::string& path);

/// 解析 `host:port`（支持 `[v6]:port` 字面量；port 必填，允许 0）。
Err ParseListen(std::string_view s, std::string* host, uint16_t* port) noexcept;

/// 口令摘要：`SHA-256(salt || password)`（AUTH 两侧同算）。
std::array<uint8_t, infra::kSha256DigestBytes> HashPassword(std::string_view salt,
                                                            std::string_view password) noexcept;

/// 摘要十六进制（配置行/`--hash-password` 输出）。
std::string DigestHex(const std::array<uint8_t, infra::kSha256DigestBytes>& d);

}  // namespace server
}  // namespace rtdb

#endif  // RTDB_SRC_SERVER_CONFIG_HPP_
