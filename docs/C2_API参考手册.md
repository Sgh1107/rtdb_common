# C2 API 参考手册

> 适用版本：M4（docs/16 D10 文档矩阵）。本文逐面列出**当前代码实际导出**的符号，
> 所有命令、字段、常量均取自 `include/rtdb/*.hpp`、`include/rtdb/rtdb_cabi.h`、
> `src/infra/err.cpp` 与 `examples/`、`tests/unit/` 的真实写法；未实现的面不臆造。
> 配套阅读：C3（布局/协议）、C4（运维）、docs/04（API 设计规范）、docs/16 D4~D7/D10/D11。
>
> **标记约定**：`[待实测]` = 本机未跑通或需标准 CI/双机环境复核；`[待评审]` = 需在
> M4 收口评审确认的口径/不一致项。

## 1. 三张 API 面与总入口

| 面 | 头 | 定位 | 版本 |
|---|---|---|---|
| Native C++ | `rtdb/rtdb.hpp`（伞头）→ `engine.hpp` 等 | 嵌入使用者、服务端、工具的主面 | 随库版本 |
| C ABI v2 | `rtdb/rtdb_cabi.h`（纯 C99，单独发行） | ctypes/JNI 绑定基座 | `RTDB_ABI_VER=2` 冻结 |
| 内嵌 SQL | `rtdb/sql.hpp` + `Engine::ExecuteSql` | 单表子集，仅 C++、无网络面 | `RTDB_HAS_SQL` 条件导出 |

伞头 `#include <rtdb/rtdb.hpp>` 依次含 `column_value/engine/err/options/record_block/
result/span/table_def/table_handle/version`；`engine.hpp` 又含 `sql.hpp`/`change_cursor.hpp`，
故 SQL 类型经伞头亦可见。C ABI 头**不**随伞头包含，需单独包含；C++ 标准为 C++17。

```cpp
#include "rtdb/rtdb.hpp"        // examples/api_demo/main.cpp:23
#include "rtdb/version.hpp"     // examples/embed_demo/main.cpp:6（kVersionString）
```

## 2. 公共约定

- **`Result<T>` / `Result<void>`**（`result.hpp`）：全部可能失败的 API 返回本类型，
  异常不跨 ABI 边界。判成功用 `IsOk()`，取值用 `Value()` / `TakeValue()` / `operator*` /
  `operator->`，取错用 `Error()`；失败值上调用取值接口会 `assert` 失败。
- **`Err`**（`err.hpp`）：`enum class Err : int16_t`，见 §6；`ErrorMessage(Err)` 返回人读短文本。
- **检查宏**（`result.hpp`，契约：所在函数须返回 `Err`）：`RTDB_CHECK(expr)` 失败即
  `return expr.Error()`；`RTDB_ASSIGN_OR_MOVE(decl, expr)` 成功移动赋值、失败返回错误码
  （带行号消歧）；`RTDB_TRY_OR_RETURN(decl, expr)` 供返回 `Result<T>` 的函数使用。
- **`Options`**（`options.hpp`）：`instance/path/initial_size_bytes`（默认 64MiB）/
  `role`(`Standalone/Writer/Reader`)/`wal_fsync`(`Never/PerSecond/Always`)/
  `wal_ring_bytes`（默认 0 = 64KB，见 C3 §4）。
- **`ColumnValue`**（`column_value.hpp`）：`Null()/Int()/IntOf(type,v)/Real()/Bool()/Text()/
  Bytes()`；隐式构造 `int64_t/double/bool/const char*/string_view/string`；
  `is_null()/type()/as_int()/as_real()/as_bool()/as_bytes()`。`Row` =
  `vector<pair<string_view, ColumnValue>>`（按列名定位的部分行）。
- **`TableHandle`**（`table_handle.hpp`）：`CreateTable/OpenTable` 返回值，绑定发放时刻的
  `layout_version/列数/row_cap`；`IsValid()/table_id()/name()/column_count()/
  visible_column_count()` 等；成员便利面 `Upsert(Row)/Get(key)/Delete(key)`。

## 3. Native C++ 面（`engine.hpp`）

`Engine::Open(opts)` → `Result<unique_ptr<Engine>>`，不可拷贝、可移动；水位/计数：`CommittedSeq()/FlushedSeq()/CheckpointSeq()/SessionCount()/Metrics()`。

### 3.1 打开、建表、打开表、删表

`CreateTable(TableDef)` / `OpenTable(name)` / `DropTable(name)`。`TableDef`（`table_def.hpp`）：
`name/columns`(≥1)`/primary_key`（空=列 0 隐式主键）`/secondary`（建表直建二级索引）/
`max_rows_hint/row_cap`。`ColumnDef`：`name/type/len/nullable/has_default/default_value`；
`DataType` 十种 `kInt8=1 … kUnixTsNs=10`（只增不改，随 schema 持久化到 shm）。

```cpp
// 最小可编译片段：取自 examples/api_demo/main.cpp:27-56（C++17 聚合赋值形态）
rtdb::Options opt;
opt.instance = "api_demo";
opt.path = "api_demo.rtdb";
opt.role = rtdb::Role::Standalone;
opt.wal_fsync = rtdb::FsyncPolicy::PerSecond;
RTDB_ASSIGN_OR_MOVE(auto eng, rtdb::Engine::Open(opt));
std::vector<rtdb::ColumnDef> cols = {
    {"id", rtdb::DataType::kInt64, 0, false, false, {}},
    {"val", rtdb::DataType::kFloat64, 0, true, false, {}},
    {"desc", rtdb::DataType::kUtf8Var, 0, true, false, {}},
};
rtdb::TableDef def;
def.name = "measure";
def.columns = std::move(cols);
def.primary_key = {"id"};
def.max_rows_hint = 2048;                       // 受 64KB 行数据区约束
auto ct = eng->CreateTable(def);
if (!ct.IsOk() && ct.Error() != rtdb::Err::kTableExists) return ct.Error();
RTDB_ASSIGN_OR_MOVE(auto tbl, eng->OpenTable("measure"));
```

### 3.2 行 CRUD

`Insert(table, vector<ColumnValue>)` → `Result<uint64_t>`（行槽号）；`Upsert(table, Row)`
（按列名合并）；`GetByKey(table, key)` → 整行；`UpdateByKey`（`row[0]` 须为原主键）；
`DeleteByKey`；`ContainsKey`。表句柄可见列数 = 建表列 + 句柄绑定时刻的扩展列。

```cpp
// examples/api_demo/main.cpp:59-80
RTDB_CHECK(tbl.Upsert(rtdb::Row{{"id", int64_t{42}}, {"val", 220.5}}));
RTDB_ASSIGN_OR_MOVE(auto got, tbl.Get(rtdb::ColumnValue::Int(42)));
std::printf("desc_null=%d\n", got[2].is_null() ? 1 : 0);
RTDB_CHECK(tbl.Delete(rtdb::ColumnValue::Int(42)));
```

### 3.3 原子批 `ExecuteBatch` 与列式 `UpsertBatch`

`Engine::BatchOp { Type::kInsert=1/kUpdate=2/kDelete=3; TableHandle table; row }`；
`ExecuteBatch(vector<BatchOp>)` 三段式 WAL（`BATCH_BEGIN + N×op + BATCH_COMMIT`），
失败零副作用或毒化 `kPoisoned`。`UpsertBatch(RecordBlockView/RecordBlock)` 为**整行覆盖**
（区别于单行 `Upsert` 的合并），返回批提交 `committed_seq`。`RecordBlock`：
`AppendColumn(name, Span<int64_t|double|string_view>)`、`SetNullMask`、`View()`；
零拷贝，调用方列内存须在调用期有效。

```cpp
// 形态取自 include/rtdb/record_block.hpp:74-79 的文档示例
rtdb::RecordBlock blk("measure", 512);
blk.AppendColumn("id", ids).AppendColumn("val", vals);   // ids: Span<const int64_t>
auto seq = eng->UpsertBatch(blk);                          // Result<uint64_t>
```

### 3.4 扫描：`Scan / ScanAll / ScanLimited`

`Engine::KeyRange{ lo, hi, lo_inclusive=true, hi_inclusive=true }`，`lo/hi` 为
`ColumnValue::Null()` 表示该侧无界。`Scan` 物化返回按主键升序行集；`ScanAll` = 无界便捷；
`ScanLimited(table, range, max_rows, bool* out_more)` 用于分页（服务端 `SCAN_PAGE`
以"上一页末行主键开区间"续扫，单页 `O(log N + page_rows)`）。

### 3.5 订阅 `ChangeCursor` / 快照 / 在线加列 / 二级索引 / 指标 / SQL

| 方法 | 返回/语义 |
|---|---|
| `Subscribe(vector<string> tables)` | `Result<ChangeCursor>`；空列表=全部活跃表；初值=订阅时 `committed_seq`；Reader 主角色 `kUnsupported` |
| `ChangeCursor::Next(timeout_ms)` | `Result<ChangeBatch>`；`kCursorWaitForever=0xFFFFFFFFu`；溢出 `kCursorOverflow` 后须整表重取 + `ResetTo` |
| `CreateSnapshot()` | 停写→flush→推进 `checkpoint_seq`→拷贝 `<映像>.snap`；**不清空 WAL 环**；Reader `kReadOnlyHandle` |
| `AlterTableAddColumn(table, ColumnDef)` | 仅定长 ≤8B 标量；旧句柄读前缀列不崩、不见新列；`data_layout_ver` 递增 |
| `CreateSecondaryIndex(table, SecondaryIndexDef)` | 键 `[有序值][有序 row_no]`(16B)；变长列 `kUnsupported` |
| `DescribeTable(table)` | 句柄可见列的 `vector<ColumnDef>` |
| `Metrics()` | `EngineMetrics` 只读快照（16 项，见 §3.6） |
| `ExecuteSql(sql, [ExecuteOptions], [error])` | `Result<sql::SqlResult>`，**仅 `#if defined(RTDB_HAS_SQL)` 导出** |

```cpp
// examples/embed_demo/main.cpp:25-27（水位/会话计数打印）
std::printf("rtdb %s | sessions=%lld committed_seq=%lld\n", rtdb::kVersionString,
            (long long)eng->SessionCount(), (long long)eng->CommittedSeq());
```

### 3.6 `Engine::Metrics()` 字段（`EngineMetrics`，`engine.hpp:34-51`）

16 项只读快照：`writes_total / batch_ops / wal_appended_bytes / wal_appended_records /
wal_backlog_bytes / wal_reclaim_count / wal_reclaim_records / wal_retained_seq /
slot_reuse_count / flush_count / flush_us_total / scan_calls / scan_rows /
scan_limited_truncated / sub_deliveries / sub_overflows`。均为进程内 host 原子计数
（不写共享内存），水位字段取样时从 WAL 环实时读；`rtdbd` ADMIN/STATS 经此取样，口径见 C4。

## 4. C ABI v2（`include/rtdb/rtdb_cabi.h`）

纯 C99、自包含（仅 `stdint.h/stddef.h`），符号前缀 `rtdb_abi_`，不抛异常跨界
（每入口 `try/catch(...)` 兜底 `RTDB_ERR_INTERNAL`）；`RTDB_ERR_*` 与 `rtdb::Err` 数值一一对应。

### 4.1 版本单一真源

- `#define RTDB_ABI_VER 2`（`rtdb_cabi.h:32`）；`#define RTDB_ABI_VERSION 2`（`version.hpp:12`）。
- `src/api/c_abi.cpp:27` 的 `static_assert(RTDB_ABI_VERSION == RTDB_ABI_VER, ...)` 使任一
  漏改在编译期报错；`test_compat.cpp:57-59` 再次断言 `RTDB_ABI_VERSION == 2`；
  `rtdb_abi_version()` 运行期返回 `RTDB_ABI_VER`。
- 语义：**只增符号不改旧符号**。v1 = M2 基线 14 符号；v2 = M3-W2 订阅族 6 符号；
  M4 冻结 v2，不新增符号、不改签名（D6/D11）。

### 4.2 v1 符号集（14 个）

| 符号 | 作用 |
|---|---|
| `rtdb_abi_version` | 返回 `RTDB_ABI_VER` |
| `rtdb_abi_open` | `open(instance, path, role, wal_fsync, rtdb_engine** out)` |
| `rtdb_abi_close` | 关闭引擎（须先关全部表句柄） |
| `rtdb_abi_committed_seq` | 失败/空句柄返回 -1 |
| `rtdb_abi_create_table` | `cols/ncols`，列 0 为整数类非空主键 |
| `rtdb_abi_open_table` | 按名打开 |
| `rtdb_abi_drop_table` | 删表 |
| `rtdb_abi_close_table` | 释放表句柄（不删表） |
| `rtdb_abi_insert` | 行式插入，`out_row_no` 可为 NULL |
| `rtdb_abi_get` | 点查，结果须 `rtdb_abi_free_row` |
| `rtdb_abi_scan` | 范围扫描，`lo/hi` 传 NULL = 无界，结果须 `rtdb_abi_free_rows` |
| `rtdb_abi_upsert_block` | 列式块原子 upsert，`out_committed_seq` 可为 NULL |
| `rtdb_abi_free_row` / `rtdb_abi_free_rows` | 释放结果（可重复调用/传 NULL 安全） |

### 4.3 v2 订阅族（6 个，只增不改）

`rtdb_abi_subscribe(engine, tables, ntables, rtdb_cursor** out)`（ntables=0 且
tables=NULL = 全部表；Reader 主角色 `RTDB_ERR_UNSUPPORTED`）、
`rtdb_abi_cursor_next(cursor, timeout_ms, rtdb_batch* out)`（空批 = 无变更；
`RTDB_ERR_CURSOR_OVERFLOW` 须重取 + `rtdb_abi_cursor_reset`）、
`rtdb_abi_cursor_position`、`rtdb_abi_cursor_reset(cursor, seq)`、
`rtdb_abi_cursor_close`、`rtdb_abi_free_batch`。`rtdb_change.kind` 取
`RTDB_CHG_INSERT=1 / RTDB_CHG_UPDATE=2 / RTDB_CHG_DELETE=3`（宏有 `#ifndef` 保护，
可与 `proto_v1.h` 共存包含；顺序见 `test_compat.cpp:29-31`）。

### 4.4 句柄生命周期与 debug 断言

`rtdb_engine` / `rtdb_table` / `rtdb_cursor` 为不透明指针，仅本库创建/释放；
**表句柄与游标不拥有引擎，须先于 `rtdb_abi_close` 关闭**。debug 构建（`#ifndef NDEBUG`）
在 `rtdb_engine` 内计 `live_tables`：`create_table/open_table` 递增、`close_table` 递减，
`rtdb_abi_close` 以 `assert(engine->live_tables == 0 && "rtdb_abi_close: 仍有未关闭的 rtdb_table 句柄")`
断言（P2-2）。结果缓冲由库内 `malloc`，须配对 `free_*`。

```c
/* 最小可编译片段：取自 examples/cabi_demo/main.c:40-46, 170-172 */
#include "rtdb/rtdb_cabi.h"
rtdb_engine* eng = NULL;
printf("rtdb C ABI version=%u\n", (unsigned)rtdb_abi_version());
int32_t rc = rtdb_abi_open("cabi_demo", "cabi_demo.rtdb",
                           RTDB_ROLE_STANDALONE, RTDB_FSYNC_PER_SECOND, &eng);
/* ... rtdb_abi_create_table / insert / get / scan ... */
rtdb_abi_close_table(tbl);   /* 必须先于 close，否则 debug 断言 */
rtdb_abi_close(eng);
```

## 5. 内嵌 SQL 单表子集（`include/rtdb/sql.hpp`）

### 5.1 冻结语法（v1，docs/16 D4）

```
select_stmt := SELECT select_list FROM table [WHERE expr] [ORDER BY col [ASC|DESC][,...]] [LIMIT n [OFFSET m]]
insert_stmt := INSERT INTO table '(' col [,...] ')' VALUES '(' literal [,...] ')' [, ...]
update_stmt := UPDATE table SET col = literal [, ...] [WHERE expr]
delete_stmt := DELETE FROM table [WHERE expr]
expr        := expr AND expr | expr OR expr | NOT expr | '(' expr ')' | operand
operand     := col op literal | col IS [NOT] NULL
op          := '=' | '<>' | '!=' | '<' | '<=' | '>' | '>='
literal     := 整数 | 浮点 | 字符串（单引号，'' 转义） | TRUE/FALSE | NULL
```

关键字大小写不敏感，标识符大小写敏感；单条语句一次执行；允许结尾单个 `;` 与
`--` 行注释。**子集外**（JOIN/子查询/聚合/GROUP BY/HAVING/UNION/DISTINCT/函数/
多语句）明确 `kUnsupported`，**不静默降级**；词法/语法错误 `kSqlSyntax=307`。

### 5.2 公共符号

- `sql::Parse(sql, Statement* out, string* error)` → `Err`（`error` 含字符偏移）。
- AST：`Statement` / `SelectStatement` / `InsertStatement` / `UpdateStatement` /
  `DeleteStatement` / `Condition`（`kCompare/kIsNull/kIsNotNull/kAnd/kOr/kNot`）/
  `CompareOp`（`kEq=1…kGe=6`）/ `OrderBy`；诊断辅助 `StatementTypeName`/`CompareOpName`。
- `SqlColumnMeta{name,type}`；`SqlResult{statement, columns, rows, affected_rows,
  last_seq}`（SELECT 填列/行集，其余填 `affected_rows`；`last_seq` = 执行后 `committed_seq`）。
- `ExecuteOptions{ uint64_t max_rows = 100000; }`（`RTDB_SQL_MAX_ROWS` 的 C++ 常量面）；
  `DefaultMaxRows()` 返回默认预算，`max_rows=0` 即用默认 100000。该预算同时约束
  SELECT 物化行数与 UPDATE/DELETE 目标行数，超限 `kLimitExceeded` 且零副作用。
- `sql::Execute(Engine&, sql, options, error)`（自由函数，等价 `Engine::ExecuteSql`）；
  `Engine::ExecuteSql(sql, [options], [error])` 两个重载。
- **`SELECT col AS x` 别名只接受、不进入结果列名**（D4 冻结子集，
  `test_sql_exec.cpp::SelectProjectionAndAliasKeepsOriginalName` 断言列名仍为 `name`）。
  主键列不可 `UPDATE`；批内重复主键显式 `kAlreadyExists`；`col <op> NULL` 恒为 UNKNOWN；不做跨语句事务。

```cpp
/* 片段取自 tests/unit/test_sql_exec.cpp:80-119 的真实调用形态 */
#include "rtdb/sql.hpp"
#include "rtdb/engine.hpp"
auto r = eng->ExecuteSql("INSERT INTO meters (id, val, name, flag) VALUES "
                         "(1, 1.5, 'a', TRUE), (2, NULL, 'b', FALSE)");
if (!r.IsOk()) { /* r.Error()：kSqlSyntax / kUnsupported / kInvalidArgument ... */ }
auto q = eng->ExecuteSql("SELECT val, flag FROM meters WHERE id = 7");
/* 预算收口 */
rtdb::sql::ExecuteOptions small; small.max_rows = 2;
auto limited = eng->ExecuteSql("SELECT * FROM meters", small);  // 超预算 kLimitExceeded
```

## 6. 错误码表（`err.hpp` / `src/infra/err.cpp`）

`ErrorMessage(Err)` 人读文本逐项如下（1xx 基础设施 / 2xx 存储布局 / 3xx 引擎 /
4xx 服务端网络 / 5xx 权限 / 兜底）：

| 码 | 常量 | `ErrorMessage` 文本 |
|---|---|---|
| 0 | `kOk` | `ok` |
| 101 | `kInvalidArgument` | `invalid argument` |
| 102 | `kNotFound` | `not found` |
| 103 | `kAlreadyExists` | `already exists` |
| 104 | `kIoError` | `io error` |
| 105 | `kUnsupported` | `operation not supported` |
| 106 | `kOutOfMemory` | `out of memory` |
| 107 | `kTimeout` | `timeout` |
| 201 | `kBadSuperBlock` | `bad super block (magic mismatch or region too small)` |
| 202 | `kVersionMismatch` | `shm format version mismatch` |
| 203 | `kIncompatibleLayout` | `incompatible shm layout (pointer width / layout crc)` |
| 204 | `kReadOnlyHandle` | `handle is read-only` |
| 205 | `kCorruption` | `persistent structure corrupted` |
| 301 | `kPoisoned` | `engine poisoned (unrecoverable wal state, writes rejected)` |
| 302 | `kTableExists` | `table already exists` |
| 303 | `kTableNotFound` | `table not found` |
| 304 | `kSchemaMismatch` | `schema layout version mismatch` |
| 305 | `kBatchAborted` | `atomic batch aborted (no commit record)` |
| 306 | `kLimitExceeded` | `configured limit exceeded (tables / columns / indexes)` |
| 307 | `kSqlSyntax` | `SQL syntax error (unsupported subset returns kUnsupported)` |
| 400 | `kNotImplemented` | `not implemented yet` |
| 401 | `kNetworkError` | `network error` |
| 402 | `kCursorOverflow` | `subscription cursor overflowed (full resync required)` |
| 403 | `kProtocolError` | `protocol violation (bad frame or payload)` |
| 404 | `kTooManySessions` | `too many sessions` |
| 501 | `kAuthFailed` | `authentication failed` |
| 502 | `kTokenExpired` | `session expired or invalid` |
| 503 | `kPermissionDenied` | `permission denied (no privilege for this table/operation)` |
| 999 | `kInternal` | `unknown error`（`default` 分支按 `IsOk` 兜底） |

> 注意：307/503 在 `rtdb_cabi.h` 中**未**以 `RTDB_ERR_*` 宏收录（C ABI v2 冻结不新增宏），
> 但 `proto_v1.h` 收录 `RTDB_E_PERMISSION_DENIED=503`；307 仅出现在 C++ `Err` 与
> `ErrorMessage`。跨语言绑定如需这两个码应按数值自行定义。`[待评审]`

## 7. `RTDB_BUILD_SQL` 双面构建（D7）

`option(RTDB_BUILD_SQL "Build embedded SQL subset (parser/exec)" ON)`（CMakeLists.txt:14）。

| 面 | `=ON`（默认） | `=OFF` |
|---|---|---|
| `src/sql/parser.cpp` + `exec.cpp` | 编入 `rtdbcore` | 不编入 |
| `target_compile_definitions(rtdbcore PUBLIC RTDB_HAS_SQL=1)` | 传递 | 不定义 |
| `Engine::ExecuteSql` 声明 | `#if defined(RTDB_HAS_SQL)` 内可见 | **整体消失** |
| `test_sql.cpp` / `test_sql_exec.cpp`（25+17 例） | 编入 | 不编入（其余 257 例不受影响） |
| `sql::Parse` / `sql::Execute` 库内实现 | 有 | 无（头仍可包含，但链接期缺符号） |

双面构建验证记录见 docs/16 §13.3：OFF + `RTDB_BUILD_TESTS=ON` 全量配置/编译/链接通过。
应用代码若需同时支持两种构建，应以 `#if defined(RTDB_HAS_SQL)` 守卫 SQL 调用与
`Engine::ExecuteSql`；fuzz 目标 `RTDB_BUILD_FUZZER` 默认 OFF，不进 CI 门禁。

## 8. 待实测 / 待评审

- `[待实测]` SQL 语义 42 例、C ABI v2 符号链接、双面 OFF 构建需 M4 收口前在标准 CI 五作业复跑；本机 Qt MinGW 11.2 结果不作正式验收证据（docs/16 §13.3）。
- `[待实测]` `examples/` 下无 `sql_demo`（D7 标注"可选"未落地），§5.2 SQL 片段取自 `tests/unit/test_sql_exec.cpp` 真实调用形态；新增示例需评审补入。
- ~~版本宏与字符串不一致~~ ✅ 2026-11-09 已把 `RTDB_VERSION_MINOR` 修正为 `2`（D11 原文「0→1」为笔误，见 C3 §7）。
- `[待评审]` §6 注记的 C ABI 宏未收录 307/503。
