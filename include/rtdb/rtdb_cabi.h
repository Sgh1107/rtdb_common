#ifndef RTDB_CABI_H_
#define RTDB_CABI_H_

/*
 * rtdb C ABI（docs/04 §4，M2-W3；docs/10 §2-M2-D3）。
 *
 * 定位：稳定的 C 导出面，作为 Python(ctypes)/JNI 等其他语言绑定的基座。
 * 约定：
 *  - 符号前缀 `rtdb_abi_`，**冻结语义版本化**：RTDB_ABI_VER 递增表示
 *    既有符号语义/签名发生不兼容变化（只追加符号不改旧符号为兼容）；
 *  - 头文件自包含（纯 C89/C99，仅 stdint.h/stddef.h），可单独发行；
 *  - 边界不抛异常：全部函数以 int32_t 错误码返回（0 = 成功），
 *    与 C++ 侧 `rtdb::Err` 一一对应（见下方 RTDB_ERR_* 取值）；
 *  - 入参内存由调用方提供、函数内即刻消费（拷入映像或解码），
 *    不同步回写；出参结果由库内分配，须用对应 free 函数释放；
 *  - 句柄（rtdb_engine/rtdb_table）为不透明指针，只可由本库创建与释放；
 *    句柄不拥有引擎：rtdb_table 须先于 rtdb_engine 关闭。
 *
 * 与 docs/04 §4 草签的差异（已在 docs/10 §2-M2-D3 记录）：
 *  - rtdb_abi_open 增补 wal_fsync 档位参数（打开时确定组提交档位）；
 *  - 句柄类型由 void* 收敛为不透明 typedef（C 侧免强制转换）；
 *  - 订阅族（rtdb_abi_subscribe 等）随 M3 会话层追加。
 */

#include <stddef.h>
#include <stdint.h>

#define RTDB_ABI_VER 1

/* ---- 导出宏（自包含，不依赖 C++ 头 api_macro.hpp）---- */
#if defined(_WIN32)
#if defined(RTDB_CORE_BUILDING_DLL)
#define RTDB_CABI_API __declspec(dllexport)
#elif defined(RTDB_CORE_STATIC)
#define RTDB_CABI_API
#else
#define RTDB_CABI_API __declspec(dllimport)
#endif
#else
#define RTDB_CABI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 错误码（与 rtdb::Err 一一对应）---- */
#define RTDB_OK 0
/* 1xx 基础设施 */
#define RTDB_ERR_INVALID_ARGUMENT 101
#define RTDB_ERR_NOT_FOUND 102
#define RTDB_ERR_ALREADY_EXISTS 103
#define RTDB_ERR_IO 104
#define RTDB_ERR_UNSUPPORTED 105
#define RTDB_ERR_OUT_OF_MEMORY 106
#define RTDB_ERR_TIMEOUT 107
/* 2xx 存储/布局 */
#define RTDB_ERR_BAD_SUPER_BLOCK 201
#define RTDB_ERR_VERSION_MISMATCH 202
#define RTDB_ERR_INCOMPATIBLE_LAYOUT 203
#define RTDB_ERR_READ_ONLY 204
#define RTDB_ERR_CORRUPTION 205
/* 3xx 引擎 */
#define RTDB_ERR_POISONED 301
#define RTDB_ERR_TABLE_EXISTS 302
#define RTDB_ERR_TABLE_NOT_FOUND 303
#define RTDB_ERR_SCHEMA_MISMATCH 304
#define RTDB_ERR_BATCH_ABORTED 305
#define RTDB_ERR_LIMIT_EXCEEDED 306
/* 4xx 服务端（保留） */
#define RTDB_ERR_NOT_IMPLEMENTED 400
#define RTDB_ERR_NETWORK 401
/* 兜底 */
#define RTDB_ERR_INTERNAL 999

/* ---- 角色（与 rtdb::Role 同值）---- */
#define RTDB_ROLE_STANDALONE 0u
#define RTDB_ROLE_WRITER 1u
#define RTDB_ROLE_READER 2u

/* ---- 持久化档位（与 rtdb::FsyncPolicy 同值；docs/08 §2-D1）---- */
#define RTDB_FSYNC_NEVER 0u
#define RTDB_FSYNC_PER_SECOND 1u
#define RTDB_FSYNC_ALWAYS 2u

/* ---- 列类型（与 rtdb::DataType 同值；只增不改）---- */
#define RTDB_T_INT8 1
#define RTDB_T_INT16 2
#define RTDB_T_INT32 3
#define RTDB_T_INT64 4
#define RTDB_T_FLOAT32 5
#define RTDB_T_FLOAT64 6
#define RTDB_T_BOOL 7
#define RTDB_T_UTF8VAR 8
#define RTDB_T_BYTESVAR 9
#define RTDB_T_UNIXTSNS 10

/* ---- 不透明句柄 ---- */
typedef struct rtdb_engine rtdb_engine;
typedef struct rtdb_table rtdb_table;

/* ---- 值（行式入参/出参单位；含空值语义）---- */
typedef struct rtdb_value {
    int32_t type;        /* RTDB_T_*；is_null 时无意义 */
    int32_t is_null;     /* 非 0 = NULL */
    int64_t int_value;   /* 整数类/bool/时间戳值 */
    double real_value;   /* 浮点值 */
    const char* bytes;   /* 变长内容（出参时为库内缓冲，随结果释放） */
    uint32_t bytes_len;  /* 变长内容字节数 */
} rtdb_value;

/* ---- 列定义（建表入参）---- */
typedef struct rtdb_column_def {
    const char* name;
    int32_t type;     /* RTDB_T_* */
    uint32_t len;     /* 定长列字节覆盖（0 = 类型固有宽度；变长列须为 0） */
    int32_t nullable; /* 非 0 = 可空 */
} rtdb_column_def;

/* ---- 列式批入参：单列（载荷类型决定 data 的元素类型）----
 * type = RTDB_T_INT64   → data 指向 int64_t[rowcount]
 * type = RTDB_T_FLOAT64 → data 指向 double[rowcount]
 * type = RTDB_T_UTF8VAR / RTDB_T_BYTESVAR
 *                       → data 指向 const char*[rowcount]，
 *                         lengths 指向 uint32_t[rowcount]（不可为 NULL）
 */
typedef struct rtdb_column {
    const char* name;
    int32_t type;
    const void* data;
    const uint32_t* lengths;   /* 仅变长载荷使用 */
    const uint8_t* null_flags; /* 1 字节/行，非 0 = NULL；NULL = 全非空 */
} rtdb_column;

/* ---- 单行结果（rtdb_abi_get 输出，须 rtdb_abi_free_row 释放）---- */
typedef struct rtdb_row {
    rtdb_value* vals;
    size_t nvals;
} rtdb_row;

/* ---- 多行结果（rtdb_abi_scan 输出，须 rtdb_abi_free_rows 释放）---- */
typedef struct rtdb_rows {
    rtdb_row* rows;
    size_t nrows;
} rtdb_rows;

/* ===================== 符号集（v1）===================== */

/* ABI 版本（= RTDB_ABI_VER）。 */
RTDB_CABI_API uint32_t rtdb_abi_version(void);

/* 打开/创建实例。role 取 RTDB_ROLE_*，wal_fsync 取 RTDB_FSYNC_*。
 * 成功时 *out 由本库分配，须 rtdb_abi_close 释放。 */
RTDB_CABI_API int32_t rtdb_abi_open(const char* instance, const char* path, uint32_t role,
                                    uint32_t wal_fsync, rtdb_engine** out);

/* 关闭引擎（释放其全部资源；已发出的 rtdb_table 须先行 close）。 */
RTDB_CABI_API void rtdb_abi_close(rtdb_engine* engine);

/* 全局提交序列号（失败/空句柄返回 -1）。 */
RTDB_CABI_API int64_t rtdb_abi_committed_seq(rtdb_engine* engine);

/* 建表。cols/ncols 为列定义（列 0 为整数类非空主键）；max_rows_hint
 * 为 0 时取默认容量。成功时 *out 表句柄须 rtdb_abi_close_table 释放。 */
RTDB_CABI_API int32_t rtdb_abi_create_table(rtdb_engine* engine, const char* name,
                                            const rtdb_column_def* cols, size_t ncols,
                                            uint32_t max_rows_hint, rtdb_table** out);

/* 按名打开既有表（*out 同上须 close_table）。 */
RTDB_CABI_API int32_t rtdb_abi_open_table(rtdb_engine* engine, const char* name,
                                          rtdb_table** out);

/* 删除表。 */
RTDB_CABI_API int32_t rtdb_abi_drop_table(rtdb_engine* engine, const char* name);

/* 释放表句柄（不删表）。 */
RTDB_CABI_API void rtdb_abi_close_table(rtdb_table* table);

/* 插入一行（vals 长度须等于表列数，顺序与建表列序一致）；
 * out_row_no 可为 NULL。 */
RTDB_CABI_API int32_t rtdb_abi_insert(rtdb_engine* engine, rtdb_table* table,
                                      const rtdb_value* vals, size_t nvals,
                                      uint64_t* out_row_no);

/* 按主键读整行（out 由本库填充，须 rtdb_abi_free_row 释放）。 */
RTDB_CABI_API int32_t rtdb_abi_get(rtdb_engine* engine, rtdb_table* table,
                                   const rtdb_value* key, rtdb_row* out);

/* 主键范围扫描（lo/hi 为 NULL 表示该侧无界，闭区间；out 须
 * rtdb_abi_free_rows 释放）。 */
RTDB_CABI_API int32_t rtdb_abi_scan(rtdb_engine* engine, rtdb_table* table,
                                    const rtdb_value* lo, const rtdb_value* hi,
                                    rtdb_rows* out);

/* 列式块原子 upsert（docs/03 §2.3）：cols/ncols 为列数组，rowcount 为
 * 块行数（各列长度一致）；语义 = 整行覆盖（存在）/ 插入（不存在）。
 * out_committed_seq 可为 NULL。 */
RTDB_CABI_API int32_t rtdb_abi_upsert_block(rtdb_engine* engine, const char* table_name,
                                            const rtdb_column* cols, size_t ncols,
                                            size_t rowcount, uint64_t* out_committed_seq);

/* 释放单行结果（内部缓冲一并回收；可重复调用/传 NULL 安全）。 */
RTDB_CABI_API void rtdb_abi_free_row(rtdb_row* row);

/* 释放多行结果（同上）。 */
RTDB_CABI_API void rtdb_abi_free_rows(rtdb_rows* rows);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RTDB_CABI_H_ */
