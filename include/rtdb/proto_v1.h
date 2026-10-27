#ifndef RTDB_PROTO_V1_H_
#define RTDB_PROTO_V1_H_

/* ------------------------------------------------------------------------
 * rtdb 网络协议 v1 常量面（docs/04 §5 草案的落定版，docs/13 §3-D2/D5）。
 *
 * 本头文件为 C99 可包含的纯常量面，供 C/C++/其他语言绑定引用；
 * 数值 **只增不改**（功能号/子操作/标签/错误码一律追加到尾部语义段）。
 *
 * 字节序：所有多字节整数一律小端显式编码，与主机字节序无关。
 *
 * 帧格式（定长 20B 头 + 载荷）：
 *   [0..3]   magic = 'R','T','D','B'
 *   [4]      ver        协议版本（本文件 = 1）
 *   [5]      flags      bit0 = 应答帧（1=server→client 应答）
 *   [6..9]   session  u32 会话号（AUTH 前恒 0）
 *   [10..11] func     u16 功能号
 *   [12..15] payload_len u32 载荷字节数
 *   [16..19] crc      u32 IEEE CRC32（覆盖 crc 置 0 的头 20B + 载荷）
 *   [20..]   payload
 *
 * 载荷约定：
 *   - 请求：flags=0，payload = 请求体；
 *   - 应答：flags=RTDB_PROTO_FLAG_RESP，payload = [i16 err][体]；
 *     err != 0 时体 = [u16 msg_len][UTF-8 描述]（人读，客户端可忽略）。
 *
 * 错误码见 include/rtdb/err.hpp（4xx 服务端 / 5xx 权限段），
 * 本文件仅收录协议常用子集宏（数值与 err.hpp 一致）。
 * ---------------------------------------------------------------------- */

/* ---- 帧 ---- */
#define RTDB_PROTO_VERSION 1u
#define RTDB_PROTO_HEADER_BYTES 20u
#define RTDB_PROTO_MAGIC_0 0x52 /* 'R' */
#define RTDB_PROTO_MAGIC_1 0x54 /* 'T' */
#define RTDB_PROTO_MAGIC_2 0x44 /* 'D' */
#define RTDB_PROTO_MAGIC_3 0x42 /* 'B' */
#define RTDB_PROTO_FLAG_RESP 0x01u
/* flags bit1 保留：压缩（v1 恒 0，M3 不解压即拒绝） */
#define RTDB_PROTO_MAX_PAYLOAD_BYTES (4u * 1024u * 1024u)

/* ---- 功能号 ---- */
#define RTDB_PF_AUTH 1u
#define RTDB_PF_PING 2u
#define RTDB_PF_DDL 10u
#define RTDB_PF_EXEC_BATCH 11u
#define RTDB_PF_SCAN 12u
#define RTDB_PF_SCAN_PAGE 13u
#define RTDB_PF_SUBSCRIBE 20u
#define RTDB_PF_FETCH_DELTA 21u
#define RTDB_PF_ACK 22u
#define RTDB_PF_UNREGISTER 23u
/* M4 管理面（docs/16 D2/D3；只增不改）：
 * ADMIN 载荷首字节 = 子操作；STATS 应答 = [u16 ver][u32 n][(u16 key)(u64 value)]*；
 * AUTHZ_ME 应答 = [Str user][u8 flags][u32 n_r][Str]*[u32 n_w][Str]*。 */
#define RTDB_PF_ADMIN 40u
#define RTDB_PF_AUTHZ_ME 41u
#define RTDB_ADMIN_STATS 1u
#define RTDB_ADMIN_AUTHZ_SHOW 2u

/* ---- AUTHZ 标志位（AUTHZ_ME/AUTHZ_SHOW）---- */
#define RTDB_AUTHZ_FLAG_ADMIN 0x01u
#define RTDB_AUTHZ_FLAG_DDL 0x02u

/* ---- STATS 版本与键（u16 key；C4 文档逐项口径）---- */
#define RTDB_STATS_VERSION 1u
#define RTDB_STAT_UPTIME_MS 1u
#define RTDB_STAT_ACCEPTED 2u
#define RTDB_STAT_REJECTED_FULL 3u
#define RTDB_STAT_AUTH_OK 4u
#define RTDB_STAT_AUTH_FAIL 5u
#define RTDB_STAT_REQUESTS 6u
#define RTDB_STAT_FRAME_ERRORS 7u
#define RTDB_STAT_PAYLOAD_ERRORS 8u
#define RTDB_STAT_AUTHZ_DENIED 9u
#define RTDB_STAT_SESSIONS_CURRENT 10u
#define RTDB_STAT_SESSIONS_TOTAL 11u
#define RTDB_STAT_HANDSHAKE_TIMEOUTS 12u
#define RTDB_STAT_IDLE_TIMEOUTS 13u
#define RTDB_STAT_AUTH_LIMIT_CLOSED 14u
#define RTDB_STAT_ADMIN_REQUESTS 15u
#define RTDB_STAT_COMMITTED_SEQ 16u
#define RTDB_STAT_CHECKPOINT_SEQ 17u
#define RTDB_STAT_WRITER_HEARTBEAT_NS 18u
#define RTDB_STAT_SESSION_TTL_MS 19u

/* ---- DDL 子操作（RTDB_PF_DDL 载荷首字节）---- */
#define RTDB_DDL_CREATE_TABLE 1u
#define RTDB_DDL_DROP_TABLE 2u
#define RTDB_DDL_DESCRIBE_TABLE 3u

/* ---- EXEC_BATCH 模式（RTDB_PF_EXEC_BATCH 载荷首字节）---- */
#define RTDB_BATCH_MODE_OPS 0u   /* 操作表：逐 op 编码行值（= ExecuteBatch） */
#define RTDB_BATCH_MODE_BLOCK 1u /* 列式块（= UpsertBatch / RecordBlock） */

/* ---- SCAN_PAGE 子操作（RTDB_PF_SCAN_PAGE 载荷首字节）---- */
#define RTDB_SP_OPEN 0u
#define RTDB_SP_FETCH 1u
#define RTDB_SP_CLOSE 2u

/* ---- 值标签（行式编码，逐值）---- */
#define RTDB_VAL_NULL 0u /* 空值（无载荷） */
#define RTDB_VAL_I64 1u  /* [i64] 整数类/bool/时间戳统一以 i64 承载 */
#define RTDB_VAL_F64 2u  /* [f64] 浮点统一以 f64 承载 */
#define RTDB_VAL_VAR 3u  /* [u32 len][bytes] 变长列内容 */

/* ---- 块列载荷类型（列式块，BLOCK 模式）---- */
#define RTDB_BLKPT_I64 0u /* [i64 × n_rows] */
#define RTDB_BLKPT_F64 1u /* [f64 × n_rows] */
#define RTDB_BLKPT_VAR 2u /* [u32 len][bytes] × n_rows */

/* ---- 变更操作码（订阅增量，与 WAL op 独立映射）---- */
#define RTDB_CHG_INSERT 1u
#define RTDB_CHG_UPDATE 2u
#define RTDB_CHG_DELETE 3u

/* ---- 常用错误码（与 include/rtdb/err.hpp 对齐）---- */
#define RTDB_E_OK 0
#define RTDB_E_INVALID_ARGUMENT 101
#define RTDB_E_NOT_FOUND 102
#define RTDB_E_TIMEOUT 107
#define RTDB_E_TABLE_NOT_FOUND 303
#define RTDB_E_NOT_IMPLEMENTED 400
#define RTDB_E_NETWORK 401
#define RTDB_E_CURSOR_OVERFLOW 402
#define RTDB_E_PROTOCOL 403
#define RTDB_E_TOO_MANY_SESSIONS 404
#define RTDB_E_AUTH_FAILED 501
#define RTDB_E_TOKEN_EXPIRED 502
#define RTDB_E_PERMISSION_DENIED 503

/* ---- 连接默认值 ---- */
#define RTDB_PROTO_DEFAULT_PORT 7878

/* ---- 线格式字段取值范围（防御性上限，服务端强制）---- */
#define RTDB_PROTO_MAX_NAME_BYTES 1024u   /* 表名/列名 u16 前缀的语义上限 */
#define RTDB_PROTO_MAX_BLOCK_COLS 64u     /* 列式块列数上限（= kMaxInitCols 同量级） */
#define RTDB_PROTO_MAX_BLOCK_ROWS 1048576u /* 列式块行数上限（1M，载荷上限的保守界） */

#endif /* RTDB_PROTO_V1_H_ */
