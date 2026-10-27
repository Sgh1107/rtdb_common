#ifndef RTDB_SRC_SERVER_AUTHZ_HPP_
#define RTDB_SRC_SERVER_AUTHZ_HPP_

/// \file
/// \brief M4 Authz 判定（docs/16 D1/D2）：部署式用户表 + 表级读/写白名单。
///
/// 语义：
///  - `admin` 隐含全部权限（管理面 + ddl + r/w）；
///  - `ddl` 允许建表/删表/加列/二级索引（不含表数据读写）；
///  - 表级 `r`/`w` 白名单支持 `*` 与 `前缀*` 两种通配；
///  - 权限在 AUTH 成功时绑定到会话（用户表 Start 后冻结，不支持热更新）。
/// 本模块无锁、无分配（匹配路径只读 vector/string_view），供 Dispatch 前置检查。

#include <string>
#include <string_view>
#include <vector>

#include "server/config.hpp"

namespace rtdb {
namespace server {

/// 表名模式匹配：精确、`*`（全部）、`前缀*`（前缀匹配）。
bool TablePatternMatches(std::string_view pattern, std::string_view table) noexcept;

/// 表名白名单匹配（任一模式命中即允许）。
bool TableListAllows(const std::vector<std::string>& patterns,
                     std::string_view table) noexcept;

/// 表数据读权限（admin 或 read 白名单命中）。
bool CanRead(const UserEntry& user, std::string_view table) noexcept;

/// 表数据写权限（admin 或 write 白名单命中）。
bool CanWrite(const UserEntry& user, std::string_view table) noexcept;

/// DDL 权限（admin 或 ddl）。
bool CanDdl(const UserEntry& user) noexcept;

/// 管理面权限（ADMIN/STATS/AUTHZ_SHOW；仅 admin）。
bool CanAdmin(const UserEntry& user) noexcept;

}  // namespace server
}  // namespace rtdb

#endif  // RTDB_SRC_SERVER_AUTHZ_HPP_
