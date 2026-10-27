# 04 API 设计规范

## 1. 设计原则
1. **现代面**：原生 C++17 API 用 RAII/span/string_view/optional/Result<T>，不抛异常跨界（内部可用，边界捕获转错误码）；
2. **经典面**：C ABI 提供 `const char*`/定长风格包装，兼容老平台使用习惯并作为其他语言绑定基座；
3. **稳定承诺分层**：CAbi 符号冻结语义版本化（`RTDB_ABI_VER`），NativeApi 允许随主版本演进。

## 2. 核心 C++ 类型

```cpp
namespace rtdb {

enum class DataType : uint8 { Int8,Int16,Int32,Int64,Float32,Float64,
                              Bool,Timestamp,TimestampMs,String,Blob };

struct ColumnDef { std::string_view name; DataType type; uint16 len; bool nullable; };

struct TableDef {
  std::string_view        name;
  std::span<const ColumnDef> columns;
  std::span<const KeyDef>    primary_key;     // 复合主键=隐式唯一索引
  std::span<const IndexDef>  secondary;       // 附加索引
  size_t                  max_rows_hint;      // 容量提示，段按需扩展
};

template<class T> struct [[nodiscard]] Result { /* value or Err */ };
using RowId = int64;

} // namespace rtdb
```

## 3. Native API 用法示例（验收基准之一）

```cpp
#include <rtdb/rtdb.hpp>

// RTDB_CHECK / RTDB_ASSIGN_OR_MOVE 契约：所在函数返回 Err（main 内经
// 静态 Run 函数中转），失败即以错误码返回——见 result.hpp 注释。
static rtdb::Err Run() {
    rtdb::Options opt{ .instance="plant1", .role=rtdb::Role::Writer,
                       .path="./data/plant1.rtdb", .wal_fsync=rtdb::FsyncPolicy::PerSecond };
    RTDB_ASSIGN_OR_MOVE(auto eng, rtdb::Engine::Open(opt));

    const std::vector<rtdb::ColumnDef> cols = {
        {"id",   rtdb::DataType::kInt64,  0, false, false, {}},
        {"val",  rtdb::DataType::kFloat64,0, true,  false, {}},
        {"desc", rtdb::DataType::kUtf8Var,0, true,  false, {}},
    };
    RTDB_CHECK(eng->CreateTable({.name="measure", .columns=cols,
                                 .primary_key={{"id"}}, .max_rows_hint=2048}));

    auto tbl = *eng->OpenTable("measure");
    // 单条 upsert（存在则按所给列合并覆盖，不存在则插入）
    tbl.Upsert(rtdb::Row{{"id", int64_t{42}}, {"val", 220.5}});

    // 百万级批量入口：列式块（M2-W2 落地）
    rtdb::RecordBlock blk("measure", 512);
    blk.AppendColumn("id", ids).AppendColumn("val", vals);
    auto n = eng->UpsertBatch(blk);

    // 订阅：纳兹序列号游标增量推送（M3，本示例不含；engine 的 WalRing
    // 与 committed_seq 基座 M1 已就绪，只差会话层）
    // rtdb::ChangeCursor cur = eng.Subscribe({"measure"});
    // while (auto delta = cur.Next(std::chrono::milliseconds(50)))
    //     for (auto& chg : *delta) /* ... */;
    return rtdb::Err::kOk;
}
```

> **M2 收敛说明（2026-10，docs/10 §2-M2-D1，验收程序 `examples/api_demo`）**：
> ① `columns` 落定为 `std::vector<ColumnDef>`（原规范的 span 写法迁移成本高于
> 收益，记录在案）；② 变长列 `len` 必须为 0（M1 语义，单列长度上限约束列入
> 复评清单）；③ `max_rows_hint` 受两道 M1 闸门限制（`kMaxRowCap=8192` 与
> 行数据区 `rec_fixed_len × row_cap ≤ 64KB`），上例 3 列约 32B/行取 2048，
> 百万级提示随段目录化复评开放；④ `RecordBlock`/`UpsertBatch` 段落在 M2-W2
> 落地（C 侧对应 `rtdb_abi_upsert_block`）；⑤ 订阅族归 M3（docs/05 §3）；
> ⑥ 本库标准为 C++17，指示器初始化（ designated initializers ）在 MSVC 需
> C++20，验收程序按聚合赋值形态落定（不依赖编译器扩展）。

## 4. C ABI 面（导出符号前缀 `rtdb_abi_`）
```c
int32_t rtdb_abi_open(const char* instance, const char* path, uint32_t role, void** h);
int32_t rtdb_abi_exec_batch(void* h, const char* tbl,
                            const struct rtdb_column* cols, size_t ncols,
                            size_t rowcount, uint64_t* out_committed_seq);
int32_t rtdb_abi_subscribe(void* h, const char* table_filter, uint64_t from_seq,
                           void** subh);
/* … 错误码与枚举值与 C++ 侧一一对应，头文件单独发行 */
```

> **现态（M3 起）**：`RTDB_ABI_VER = RTDB_ABI_VERSION = 2`（M3 追加订阅/会话
> 符号族；v1 既有符号语义不变），`src/api/c_abi.cpp` 以 `static_assert` 校验
> 两侧一致。**M4 不新增 C ABI 符号**（保持 v2 冻结；SQL 仅内嵌 C++ 面），
> 详见 [docs/16 D6/D11](16_M4完善开发计划.md)。

## 5. 网络二进制协议 v1
```
帧: [magic'RTDB'][ver u8][flags u8][session u32][func u16][payload_len u32][crc u32][payload]
会话: AUTH(user,pass_sha256,salt) → token(有效期+滚动续期)
功能号: AUTH=1 PING=2 DDL=10 EXEC_BATCH=11 SCAN=12 SCAN_PAGE=13
        SUBSCRIBE=20 FETCH_DELTA=21 ACK=22 UNREGISTER=23
        ADMIN=40 AUTHZ_ME=41（M4 只增：STATS/AUTHZ_SHOW 与自身权限摘要）
FETCH_DELTA 请求携带 (from_seq, max_bytes)；服务端按 ChangeLog 区间回放，
包内附 last_seq 便于客户端推进游标。落后过深返回 ERR_CURSOR_OVERFLOW，
客户端降级整表 SCAN+重置游标。
字节序: 小端固定，帧头 flags 可带压缩(zstd预留位)
```
协议明确标注版本协商字段，为 v2 演进留空间（吸取老系统固定 4入参+3出参骨架不可演进的教训）。

> **as-built 注记（M3-W5 收口，实测口径见 docs/14）**：帧头 = 20 字节
> `[magic'RTDB'][ver u8][flags u8][session u32][func u16][payload_len u32]
> [crc32][payload]`，CRC 覆盖头（crc 字段置零）+ 载荷，载荷硬上限 4 MiB
> （`rtdb/proto_v1.h`）；功能号与上表一致，DDL 子码 CREATE/DROP/DESCRIBE =
> 1/2/3，EXEC_BATCH 双模式常载（mode 0 = 操作表 / mode 1 = 列式块），
> flags 现仅用 RESP 位（压缩为预留位、未实现）。"按 ChangeLog 区间回放"
> 落地为 D1 决策：**复用 WalRing 扫描，无独立 ChangeLogStore**；`last_seq`
> = 最后一条被编码变更的 seq（无变更时 = from；批标记 BEGIN/COMMIT 不产生
> 事件 ⇒ 订阅交付水位可与 committed 相差若干记录，不能直接比齐）。会话：
> AUTH 摘要 = SHA-256(配置盐 ‖ 口令)，token 带 TTL 与滚动续期；错误码
> 以 `RTDB_E_*` 常量为准（101/102/107/303/400/401/402/403/404/501/502；
> M4 追加 503 = 权限拒绝）。
>
> **M4 as-built（2026-10-27，docs/16 D2/D3）**：新增 `ADMIN=40`（载荷首字节
> 子操作：`1`=STATS、`2`=AUTHZ_SHOW）与 `AUTHZ_ME=41`（任意已认证用户）。
> STATS 应答 = `[u16 ver=1][u32 n][(u16 key)(u64 value)]*`（键 1~19：
> uptime/accepted/rejected_full/auth_ok/auth_fail/requests/frame_errors/
> payload_errors/authz_denied/sessions_current/sessions_total/
> handshake_timeouts/idle_timeouts/auth_limit_closed/admin_requests/
> committed_seq/checkpoint_seq/writer_heartbeat_ns/session_ttl_ms）；
> AUTHZ_ME/AUTHZ_SHOW 每用户 = `[Str user][u8 flags(bit0 admin,bit1 ddl)]
> [u32 n_r][Str]*[u32 n_w][Str]*`。AUTH 响应不变；既有 func/payload 不改
> （v1 只增不改），旧客户端连新服务端行为不变。

## 6. 命名与工程规范
- 代码：Google 风格基线；公共头 `include/rtdb/*.hpp`；内部头不出库；
- 版本：semver + 共享内存 layout 版独立编号；
- CI：Windows(msvc19.3x)/Linux(gcc9+clang12) 三矩阵必须绿；clang-format/tidy 强制；
- 所有公共 API 带 doxygen 注释与用法示例。
