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

/// 用户权限（M4 Authz，docs/16 D1）：
///  - admin：管理面（ADMIN/STATS/AUTHZ_SHOW）+ 隐含全部 ddl/r/w；
///  - ddl：建/删表、加列、二级索引；
///  - read_tables / write_tables：表级读/写白名单，支持 `*` 与 `prefix*`。
struct UserPerms {
    bool admin = false;
    bool ddl = false;
    std::vector<std::string> read_tables;
    std::vector<std::string> write_tables;
    /// M3 旧配置/程序化构造未显式声明权限时保持 admin 兼容语义
    /// （显式权限规约解析后置 false；见 ParsePermissions）。
    bool is_legacy = true;
};

/// 内联用户表项。
struct UserEntry {
    std::string name;
    std::array<uint8_t, infra::kSha256DigestBytes> digest{};
    /// 空权限规约（M3 旧配置 `user = name digest`）解析为 admin，保持
    /// M3 脚本/测试/双机演示兼容（docs/16 D1）。
    UserPerms perms;
};

/// 解析权限规约（配置文件 `user = <name> <64hex> [perm]` 的 perm 部分，
/// 以及 CLI `--user name=64hex#perm`）。语法：
///   perm := token [(' '|'\t') token]*；token ∈
///           `admin` | `ddl` | `r:<tables>` | `w:<tables>` | `rw:<tables>`
///   <tables> := table[','table]*；table = 名称、`*` 或 `前缀*`
/// 空输入 = 旧配置兼容（admin）。非法 token/空表名 → kInvalidArgument。
Err ParsePermissions(std::string_view spec, UserPerms* out);

/// rtdbd 运行配置。
struct Config {
    std::string instance = "rtdbd";
    std::string path;                       ///< 映像路径（必填）
    std::string listen_host = "127.0.0.1";  ///< 默认仅回环（安全默认）
    uint16_t listen_port = 5599;
    uint64_t initial_size_mb = 256;
    /// WAL 环容量（KB；0 = 引擎默认 64KB）。仅新建映像时生效；允许
    /// 64~65536（64KB~64MB），见 Options::wal_ring_bytes。
    uint32_t wal_ring_kb = 0;
    FsyncPolicy fsync = FsyncPolicy::PerSecond;
    std::string auth_salt;             ///< 口令盐（部署共享）
    std::vector<UserEntry> users;      ///< 空 = 拒绝全部 AUTH（501）
    uint32_t session_ttl_ms = 30u * 60u * 1000u;  ///< 会话 TTL（滚动续期）
    uint32_t max_sessions = 64;                   ///< 并发会话上限（404）
    /// 未认证连接的握手窗口：超时关闭，防止静默连接长期占用会话名额
    /// （P1-7：每连接一线程，连接洪泛即线程/名额耗尽）。
    uint32_t handshake_timeout_ms = 10000;
    /// 已认证连接的空闲上限（0 = 不限制；滚动刷新）。订阅长轮询 ≤ 25s，
    /// 默认 5 分钟留足余量。
    uint32_t idle_timeout_ms = 5u * 60u * 1000u;
    /// 单连接 AUTH 失败上限（达到即断连）与线性退避基数（毫秒）。
    uint32_t max_auth_failures = 5;
    uint32_t auth_backoff_ms = 200;
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
