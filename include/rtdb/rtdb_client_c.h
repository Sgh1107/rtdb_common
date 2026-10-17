#ifndef RTDB_CLIENT_C_H_
#define RTDB_CLIENT_C_H_

/*
 * rtdb_client C ABI（docs/13 §4，M3-W4）：远程客户端最小稳定面
 * （连接/PING/建表/删表/块写/扫描/订阅回调）。
 *
 * 约定（与 rtdb_cabi.h 同风格）：
 *  - 前缀 `rtdb_client_*`；边界不抛异常，错误码 `RTDB_OK`/`RTDB_ERR_*`
 *    （与 rtdb::Err 一一对应，见 rtdb_cabi.h）；
 *  - 入库数据由调用方提供、函数内即刻消费；出参由本库分配，须用对应
 *    free 函数释放；
 *  - **订阅回调在客户端泵线程触发**：仅在回调内可读传入结构（其指针
 *    随回调返回失效），需要保留请自行拷贝；回调内不要阻塞，也不要销毁
 *    订阅句柄；
 *  - 值/列/行/变更结构复用 rtdb_cabi.h（同仓发行）。
 */

#include <stddef.h>
#include <stdint.h>

#include "rtdb/rtdb_cabi.h"

/* ---- 导出宏（自包含，与 api_macro.hpp 的 RTDB_CLIENT_API 同口径）---- */
#if defined(_WIN32)
#if defined(RTDB_CLIENT_BUILDING_DLL)
#define RTDB_CLIENT_C_API __declspec(dllexport)
#elif defined(RTDB_CLIENT_STATIC)
#define RTDB_CLIENT_C_API
#else
#define RTDB_CLIENT_C_API __declspec(dllimport)
#endif
#else
#define RTDB_CLIENT_C_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄：客户端连接 / 订阅。 */
typedef struct rtdb_client rtdb_client;
typedef struct rtdb_client_sub rtdb_client_sub;

/* 连接参数（host/auth_salt 可为 NULL；timeout 为 0 时取默认值）。 */
typedef struct rtdb_client_options {
    const char* host;      /* NULL → "127.0.0.1" */
    uint16_t port;         /* 必填（0 = 非法） */
    const char* user;
    const char* password;
    const char* auth_salt; /* NULL → 空盐 */
    uint32_t connect_timeout_ms; /* 0 → 3000 */
    uint32_t request_timeout_ms; /* 0 → 5000 */
} rtdb_client_options;

/* 连接 + AUTH。成功时 *out 由本库分配，须 rtdb_client_close 释放。 */
RTDB_CLIENT_C_API int32_t rtdb_client_connect(const rtdb_client_options* opts,
                                              rtdb_client** out);

/* 关闭连接（幂等；订阅各自独立，须先自行关闭）。 */
RTDB_CLIENT_C_API void rtdb_client_close(rtdb_client* c);

/* PING（读类；自动重连重发）。出参均可为 NULL。 */
RTDB_CLIENT_C_API int32_t rtdb_client_ping(rtdb_client* c, int64_t* committed_seq,
                                           uint64_t* heartbeat_ns, uint32_t* ttl_ms);

/* 建表（写类；列 0 = 整数类非空主键；max_rows_hint = 0 取默认容量）。 */
RTDB_CLIENT_C_API int32_t rtdb_client_create_table(rtdb_client* c, const char* name,
                                                   const rtdb_column_def* cols, size_t ncols,
                                                   uint32_t max_rows_hint);

/* 删表（写类）。 */
RTDB_CLIENT_C_API int32_t rtdb_client_drop_table(rtdb_client* c, const char* name);

/* 列式块 upsert（写类）：type 取 RTDB_T_* 载荷（INT64/FLOAT64/VAR
 * 三族，其余整数类归 INT64 载荷）；out_committed_seq 可为 NULL。 */
RTDB_CLIENT_C_API int32_t rtdb_client_upsert_block(rtdb_client* c, const char* table,
                                                   const rtdb_column* cols, size_t ncols,
                                                   size_t rowcount,
                                                   uint64_t* out_committed_seq);

/* 主键范围扫描（读类；lo/hi 为 NULL = 该侧无界，闭区间）。
 * 结果须 rtdb_client_free_rows 释放。 */
RTDB_CLIENT_C_API int32_t rtdb_client_scan(rtdb_client* c, const char* table,
                                           const rtdb_value* lo, const rtdb_value* hi,
                                           rtdb_rows* out);

/* 释放扫描结果（内部缓冲一并回收；可重复调用/传 NULL 安全）。 */
RTDB_CLIENT_C_API void rtdb_client_free_rows(rtdb_rows* rows);

/* 订阅回调族（泵线程触发；chg 指针仅回调期间有效）。 */
typedef void (*rtdb_client_change_fn)(const rtdb_change* chg, void* user);
typedef void (*rtdb_client_overflow_fn)(void* user);
typedef void (*rtdb_client_state_fn)(int32_t connected, uint32_t attempt, void* user);

/* 订阅（tables = NULL 且 ntables = 0 → 全部活跃表）。成功时 *out 须
 * rtdb_client_sub_close 释放；溢出回调内/之后应整表重取并调用
 * rtdb_client_sub_reset（否则泵挂起等待）。 */
RTDB_CLIENT_C_API int32_t rtdb_client_subscribe(rtdb_client* c, const char* const* tables,
                                                size_t ntables,
                                                rtdb_client_change_fn on_change,
                                                rtdb_client_overflow_fn on_overflow,
                                                rtdb_client_state_fn on_state, void* user,
                                                rtdb_client_sub** out);

/* 关闭订阅（停泵 + 断开；幂等）。 */
RTDB_CLIENT_C_API void rtdb_client_sub_close(rtdb_client_sub* sub);

/* 最后交付 seq（续传点；未就绪返回 0）。 */
RTDB_CLIENT_C_API uint64_t rtdb_client_sub_position(const rtdb_client_sub* sub);

/* 重置续传点并唤醒泵（溢出重同步）。 */
RTDB_CLIENT_C_API void rtdb_client_sub_reset(rtdb_client_sub* sub, uint64_t seq);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RTDB_CLIENT_C_H_ */
