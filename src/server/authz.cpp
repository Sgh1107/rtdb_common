#include "server/authz.hpp"

namespace rtdb {
namespace server {

bool TablePatternMatches(std::string_view pattern, std::string_view table) noexcept {
    if (pattern.empty()) return false;
    if (pattern == "*") return true;
    if (pattern.back() == '*') {
        const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
        if (prefix.empty()) return true;
        return table.size() >= prefix.size() &&
               table.compare(0, prefix.size(), prefix) == 0;
    }
    return pattern == table;
}

bool TableListAllows(const std::vector<std::string>& patterns,
                     std::string_view table) noexcept {
    for (const std::string& p : patterns) {
        if (TablePatternMatches(p, table)) return true;
    }
    return false;
}

bool CanRead(const UserEntry& user, std::string_view table) noexcept {
    if (user.perms.is_legacy || user.perms.admin) return true;
    // 读/写严格分离：写权限不隐含读（docs/16 D1 表级 r/w 独立）。
    return TableListAllows(user.perms.read_tables, table);
}

bool CanWrite(const UserEntry& user, std::string_view table) noexcept {
    if (user.perms.is_legacy || user.perms.admin) return true;
    return TableListAllows(user.perms.write_tables, table);
}

bool CanDdl(const UserEntry& user) noexcept {
    return user.perms.is_legacy || user.perms.admin || user.perms.ddl;
}

bool CanAdmin(const UserEntry& user) noexcept {
    return user.perms.is_legacy || user.perms.admin;
}

}  // namespace server
}  // namespace rtdb
