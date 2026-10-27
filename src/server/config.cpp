#include "server/config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rtdb {
namespace server {

namespace {

std::string_view Trim(std::string_view s) noexcept {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

bool ParseU64(std::string_view s, uint64_t* out) noexcept {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

bool ParseBool(std::string_view s, bool* out) noexcept {
    if (s == "true" || s == "1" || s == "on" || s == "yes") {
        *out = true;
        return true;
    }
    if (s == "false" || s == "0" || s == "off" || s == "no") {
        *out = false;
        return true;
    }
    return false;
}

bool ParseFsync(std::string_view s, FsyncPolicy* out) noexcept {
    if (s == "never") {
        *out = FsyncPolicy::Never;
        return true;
    }
    if (s == "per_second" || s == "per-second" || s == "persecond") {
        *out = FsyncPolicy::PerSecond;
        return true;
    }
    if (s == "always") {
        *out = FsyncPolicy::Always;
        return true;
    }
    return false;
}

bool ValidTablePattern(std::string_view t) noexcept {
    if (t.empty() || t.size() > 64) return false;
    size_t n = t.size();
    if (t[n - 1] == '*') --n;  // '*' 或 '前缀*'
    for (size_t i = 0; i < n; ++i) {
        const char c = t[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool ParseTableList(std::string_view s, std::vector<std::string>* out) noexcept {
    if (s.empty() || out == nullptr) return false;
    size_t pos = 0;
    for (;;) {
        const size_t comma = s.find(',', pos);
        const std::string_view one =
            Trim(comma == std::string_view::npos ? s.substr(pos) : s.substr(pos, comma - pos));
        if (!ValidTablePattern(one)) return false;
        out->emplace_back(one);
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return true;
}

/// 解析 `user = <name> <64hex> [perm]` 一行（perm 为空 = M3 兼容 admin）。
Err ParseUserLine(std::string_view value, UserEntry* out) {
    const size_t sp = value.find_first_of(" \t");
    if (sp == std::string_view::npos) return Err::kInvalidArgument;
    const std::string_view name = Trim(value.substr(0, sp));
    const std::string_view rest = Trim(value.substr(sp + 1));
    const size_t sp2 = rest.find_first_of(" \t");
    const std::string_view hex = Trim(sp2 == std::string_view::npos ? rest : rest.substr(0, sp2));
    const std::string_view perm =
        sp2 == std::string_view::npos ? std::string_view{} : Trim(rest.substr(sp2 + 1));
    if (name.empty() || name.size() > 64) return Err::kInvalidArgument;
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '=' || c == '#') return Err::kInvalidArgument;
    }
    out->name.assign(name);
    if (!infra::FromHex(hex, out->digest.data(), out->digest.size()))
        return Err::kInvalidArgument;
    return ParsePermissions(perm, &out->perms);
}

}  // namespace

Err ParsePermissions(std::string_view spec, UserPerms* out) {
    if (out == nullptr) return Err::kInvalidArgument;
    *out = UserPerms{};
    const std::string_view t = Trim(spec);
    if (t.empty()) {  // M3 旧配置兼容：无权限规约 = admin
        out->admin = true;
        out->is_legacy = true;
        return Err::kOk;
    }
    size_t pos = 0;
    while (pos < t.size()) {
        while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t')) ++pos;
        if (pos >= t.size()) break;
        size_t end = pos;
        while (end < t.size() && t[end] != ' ' && t[end] != '\t') ++end;
        const std::string_view tok = t.substr(pos, end - pos);
        pos = end;
        if (tok == "admin") {
            out->admin = true;
        } else if (tok == "ddl") {
            out->ddl = true;
        } else if (tok.size() > 3 && tok.substr(0, 3) == "rw:") {
            if (!ParseTableList(tok.substr(3), &out->read_tables) ||
                !ParseTableList(tok.substr(3), &out->write_tables))
                return Err::kInvalidArgument;
        } else if (tok.size() > 2 && tok.substr(0, 2) == "r:") {
            if (!ParseTableList(tok.substr(2), &out->read_tables)) return Err::kInvalidArgument;
        } else if (tok.size() > 2 && tok.substr(0, 2) == "w:") {
            if (!ParseTableList(tok.substr(2), &out->write_tables)) return Err::kInvalidArgument;
        } else {
            return Err::kInvalidArgument;
        }
    }
    // 权限规约存在但无任何有效项（例如仅空白）按非法处理，避免静默降权/提权。
    if (!out->admin && !out->ddl && out->read_tables.empty() && out->write_tables.empty())
        return Err::kInvalidArgument;
    out->is_legacy = false;  // 显式声明：不再按 M3 admin 兼容
    return Err::kOk;
}

Result<Config> ParseConfigText(std::string_view text) {
    Config cfg;
    std::string_view rest = text;
    while (!rest.empty()) {
        const size_t nl = rest.find('\n');
        const std::string_view raw =
            nl == std::string_view::npos ? rest : rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);

        const std::string_view line = Trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) return Result<Config>::Fail(Err::kInvalidArgument);
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view val = Trim(line.substr(eq + 1));
        if (key.empty()) return Result<Config>::Fail(Err::kInvalidArgument);

        if (key == "instance") {
            if (val.empty()) return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.instance.assign(val);
        } else if (key == "path") {
            if (val.empty()) return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.path.assign(val);
        } else if (key == "listen") {
            if (!IsOk(ParseListen(val, &cfg.listen_host, &cfg.listen_port)))
                return Result<Config>::Fail(Err::kInvalidArgument);
        } else if (key == "size_mb") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v == 0 || v > 4096)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.initial_size_mb = v;
        } else if (key == "wal_ring_kb") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || (v != 0 && (v < 64 || v > 65536)))
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.wal_ring_kb = static_cast<uint32_t>(v);
        } else if (key == "fsync") {
            if (!ParseFsync(val, &cfg.fsync)) return Result<Config>::Fail(Err::kInvalidArgument);
        } else if (key == "auth_salt") {
            cfg.auth_salt.assign(val);
        } else if (key == "user") {
            UserEntry u;
            if (!IsOk(ParseUserLine(val, &u))) return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.users.push_back(std::move(u));
        } else if (key == "session_ttl_ms") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v == 0 || v > 86400000u)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.session_ttl_ms = static_cast<uint32_t>(v);
        } else if (key == "max_sessions") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v == 0 || v > 1024)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.max_sessions = static_cast<uint32_t>(v);
        } else if (key == "handshake_timeout_ms") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v > 3600000u)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.handshake_timeout_ms = static_cast<uint32_t>(v);
        } else if (key == "idle_timeout_ms") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v > 86400000u)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.idle_timeout_ms = static_cast<uint32_t>(v);
        } else if (key == "max_auth_failures") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v == 0 || v > 1000)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.max_auth_failures = static_cast<uint32_t>(v);
        } else if (key == "auth_backoff_ms") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v > 60000)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.auth_backoff_ms = static_cast<uint32_t>(v);
        } else if (key == "max_payload_kb") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v == 0 || v > 65536)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.max_payload_bytes = static_cast<uint32_t>(v * 1024);
        } else if (key == "heartbeat_ms") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v == 0 || v > 60000)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.heartbeat_ms = static_cast<uint32_t>(v);
        } else if (key == "snapshot_on_exit") {
            if (!ParseBool(val, &cfg.snapshot_on_exit))
                return Result<Config>::Fail(Err::kInvalidArgument);
        } else if (key == "max_delta_kb") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v == 0 || v > 65536)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.max_delta_bytes = static_cast<uint32_t>(v * 1024);
        } else if (key == "delta_max_timeout_ms") {
            uint64_t v = 0;
            if (!ParseU64(val, &v) || v == 0 || v > 3600000u)
                return Result<Config>::Fail(Err::kInvalidArgument);
            cfg.delta_max_timeout_ms = static_cast<uint32_t>(v);
        } else {
            return Result<Config>::Fail(Err::kInvalidArgument);  // 未知键拒绝（防拼写漂移）
        }
    }
    return Result<Config>::Ok(std::move(cfg));
}

Result<Config> LoadConfigFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Result<Config>::Fail(Err::kIoError);
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in.good() && !in.eof()) return Result<Config>::Fail(Err::kIoError);
    return ParseConfigText(ss.str());
}

Err ParseListen(std::string_view s, std::string* host, uint16_t* port) noexcept {
    if (host == nullptr || port == nullptr) return Err::kInvalidArgument;
    const std::string_view t = Trim(s);
    if (t.empty()) return Err::kInvalidArgument;

    std::string_view host_part;
    std::string_view port_part;
    if (t[0] == '[') {
        const size_t close = t.find(']');
        if (close == std::string_view::npos || close + 1 >= t.size() || t[close + 1] != ':')
            return Err::kInvalidArgument;
        host_part = t.substr(1, close - 1);
        port_part = t.substr(close + 2);
    } else {
        const size_t colon = t.rfind(':');
        if (colon == std::string_view::npos || colon + 1 >= t.size())
            return Err::kInvalidArgument;
        host_part = t.substr(0, colon);
        port_part = t.substr(colon + 1);
    }
    if (host_part.empty() || port_part.empty()) return Err::kInvalidArgument;
    uint64_t p = 0;
    if (!ParseU64(port_part, &p) || p > 65535) return Err::kInvalidArgument;
    host->assign(host_part);
    *port = static_cast<uint16_t>(p);
    return Err::kOk;
}

std::array<uint8_t, infra::kSha256DigestBytes> HashPassword(std::string_view salt,
                                                            std::string_view password) noexcept {
    return infra::Sha256Concat(salt, password);
}

std::string DigestHex(const std::array<uint8_t, infra::kSha256DigestBytes>& d) {
    return infra::DigestToHex(d);
}

}  // namespace server
}  // namespace rtdb
