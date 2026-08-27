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
int main() {
    rtdb::Options opt{ .instance="plant1", .role=rtdb::Role::Writer,
                       .path="./data/plant1.rtdb", .wal_fsync=FsyncPolicy::PerSecond };
    RTDB_ASSIGN_OR_MOVE(auto eng, rtdb::Engine::Open(opt));

    static const rtdb::ColumnDef kCols[] = {
        {"id",   rtdb::DataType::Int64,  0, false},
        {"val",  rtdb::DataType::Float64,0, true },
        {"desc", rtdb::DataType::String, 64,true },
    };
    RTDB_CHECK(eng.CreateTable({.name="measure", .columns=kCols,
                                .primary_key={{"id"}}, .max_rows_hint=1000000}));

    auto tbl = *eng.OpenTable("measure");
    // 单条 upsert（免锁乐观读，失败自动重试）
    tbl.Upsert(rtdb::Row{{"id", int64_t{42}}, {"val", 220.5}});

    // 百万级批量入口：列式块
    rtdb::RecordBlock blk("measure", 100000);
    blk.AppendColumn("id", ids).AppendColumn("val", vals);
    auto n = eng.UpsertBatch(blk);

    // 订阅：纳秒序列号游标增量推送（本机事件唤醒，非轮询）
    rtdb::ChangeCursor cur = eng.Subscribe({"measure"});
    while (auto delta = cur.Next(std::chrono::milliseconds(50)))
        for (auto& chg : *delta) /* ... */;
}
```

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

## 5. 网络二进制协议 v1
```
帧: [magic'RTDB'][ver u8][flags u8][session u32][func u16][payload_len u32][crc u32][payload]
会话: AUTH(user,pass_sha256,salt) → token(有效期+滚动续期)
功能号: AUTH=1 PING=2 DDL=10 EXEC_BATCH=11 SCAN=12 SCAN_PAGE=13
        SUBSCRIBE=20 FETCH_DELTA=21 ACK=22 UNREGISTER=23
FETCH_DELTA 请求携带 (from_seq, max_bytes)；服务端按 ChangeLog 区间回放，
包内附 last_seq 便于客户端推进游标。落后过深返回 ERR_CURSOR_OVERFLOW，
客户端降级整表 SCAN+重置游标。
字节序: 小端固定，帧头 flags 可带压缩(zstd预留位)
```
协议明确标注版本协商字段，为 v2 演进留空间（吸取老系统固定 4入参+3出参骨架不可演进的教训）。

## 6. 命名与工程规范
- 代码：Google 风格基线；公共头 `include/rtdb/*.hpp`；内部头不出库；
- 版本：semver + 共享内存 layout 版独立编号；
- CI：Windows(msvc19.3x)/Linux(gcc9+clang12) 三矩阵必须绿；clang-format/tidy 强制；
- 所有公共 API 带 doxygen 注释与用法示例。
