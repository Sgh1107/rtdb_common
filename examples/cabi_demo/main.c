/*
 * cabi_demo：C ABI 验收程序（M2-W3，docs/10 §2-M2-D3）。
 *
 * 纯 C99 编写，只依赖 include/rtdb/rtdb_cabi.h（单独发行的稳定面）。
 * 覆盖：version → open → create_table → insert → get → upsert_block →
 * scan → 释放语义 → close。与 api_demo（C++ 面）流程对应，证明"同一
 * 引擎能力的 C 绑定基座"可用。
 *
 * 用法：cabi_demo [image_path]
 */

#include <stdio.h>
#include <string.h>

#include "rtdb/rtdb_cabi.h"

static void print_row(const rtdb_row* row) {
    size_t i;
    for (i = 0; i < row->nvals; ++i) {
        const rtdb_value* v = &row->vals[i];
        if (v->is_null) {
            printf("NULL");
        } else if (v->type == RTDB_T_FLOAT32 || v->type == RTDB_T_FLOAT64) {
            printf("%g", v->real_value);
        } else if (v->type == RTDB_T_UTF8VAR || v->type == RTDB_T_BYTESVAR) {
            printf("%.*s", (int)v->bytes_len, v->bytes != NULL ? v->bytes : "");
        } else {
            printf("%lld", (long long)v->int_value);
        }
        printf(i + 1 < row->nvals ? " | " : "\n");
    }
}

int main(int argc, char** argv) {
    const char* image = argc > 1 ? argv[1] : "cabi_demo.rtdb";
    rtdb_engine* eng = NULL;
    rtdb_table* tbl = NULL;
    int32_t rc;

    printf("rtdb C ABI version=%u\n", (unsigned)rtdb_abi_version());

    rc = rtdb_abi_open("cabi_demo", image, RTDB_ROLE_STANDALONE, RTDB_FSYNC_PER_SECOND, &eng);
    if (rc != RTDB_OK) {
        fprintf(stderr, "open failed: %d\n", (int)rc);
        return 1;
    }

    /* 建表：id(i64 非空) + val(f64 可空) + tag(变长 可空)。 */
    {
        rtdb_column_def cols[3];
        int32_t crc;
        memset(cols, 0, sizeof(cols));
        cols[0].name = "id";
        cols[0].type = RTDB_T_INT64;
        cols[0].nullable = 0;
        cols[1].name = "val";
        cols[1].type = RTDB_T_FLOAT64;
        cols[1].nullable = 1;
        cols[2].name = "tag";
        cols[2].type = RTDB_T_UTF8VAR;
        cols[2].nullable = 1;

        crc = rtdb_abi_create_table(eng, "measure", cols, 3, 0, &tbl);
        if (crc == RTDB_ERR_TABLE_EXISTS) {
            /* 重跑：改为打开既有表（保持示例可重复执行）。 */
            crc = rtdb_abi_open_table(eng, "measure", &tbl);
        }
        if (crc != RTDB_OK) {
            fprintf(stderr, "create/open table failed: %d\n", (int)crc);
            rtdb_abi_close(eng);
            return 1;
        }
    }

    /* 行式插入一行。 */
    {
        rtdb_value vals[3];
        uint64_t row_no = 0;
        memset(vals, 0, sizeof(vals));
        vals[0].type = RTDB_T_INT64;
        vals[0].int_value = 42;
        vals[1].type = RTDB_T_FLOAT64;
        vals[1].real_value = 220.5;
        vals[2].type = RTDB_T_UTF8VAR;
        vals[2].bytes = "inserted";
        vals[2].bytes_len = 8;
        rc = rtdb_abi_insert(eng, tbl, vals, 3, &row_no);
        if (rc != RTDB_OK && rc != RTDB_ERR_ALREADY_EXISTS) {
            fprintf(stderr, "insert failed: %d\n", (int)rc);
            rtdb_abi_close_table(tbl);
            rtdb_abi_close(eng);
            return 1;
        }
        printf("insert id=42 -> rc=%d row_no=%llu\n", (int)rc,
               (unsigned long long)row_no);
    }

    /* 点查。 */
    {
        rtdb_value key;
        rtdb_row row;
        memset(&key, 0, sizeof(key));
        memset(&row, 0, sizeof(row));
        key.type = RTDB_T_INT64;
        key.int_value = 42;
        rc = rtdb_abi_get(eng, tbl, &key, &row);
        if (rc != RTDB_OK) {
            fprintf(stderr, "get failed: %d\n", (int)rc);
            rtdb_abi_close_table(tbl);
            rtdb_abi_close(eng);
            return 1;
        }
        printf("get   id=42 -> ");
        print_row(&row);
        rtdb_abi_free_row(&row);
    }

    /* 列式块 upsert（id 100/101，覆盖与插入混合）。 */
    {
        const int64_t ids[2] = {100, 101};
        const double vls[2] = {1.5, 2.5};
        const char* tags[2] = {"block_a", "block_b"};
        const uint32_t tag_lens[2] = {7, 7};
        rtdb_column bcols[3];
        uint64_t seq = 0;
        memset(bcols, 0, sizeof(bcols));
        bcols[0].name = "id";
        bcols[0].type = RTDB_T_INT64;
        bcols[0].data = ids;
        bcols[1].name = "val";
        bcols[1].type = RTDB_T_FLOAT64;
        bcols[1].data = vls;
        bcols[2].name = "tag";
        bcols[2].type = RTDB_T_UTF8VAR;
        bcols[2].data = tags;
        bcols[2].lengths = tag_lens;
        rc = rtdb_abi_upsert_block(eng, "measure", bcols, 3, 2, &seq);
        if (rc != RTDB_OK) {
            fprintf(stderr, "upsert_block failed: %d\n", (int)rc);
            rtdb_abi_close_table(tbl);
            rtdb_abi_close(eng);
            return 1;
        }
        printf("block upsert 2 rows -> rc=%d committed_seq=%llu\n", (int)rc,
               (unsigned long long)seq);
    }

    /* 范围扫描（无界 = 全表）。 */
    {
        rtdb_rows rows;
        size_t i;
        memset(&rows, 0, sizeof(rows));
        rc = rtdb_abi_scan(eng, tbl, NULL, NULL, &rows);
        if (rc != RTDB_OK) {
            fprintf(stderr, "scan failed: %d\n", (int)rc);
            rtdb_abi_close_table(tbl);
            rtdb_abi_close(eng);
            return 1;
        }
        printf("scan  %llu row(s):\n", (unsigned long long)rows.nrows);
        for (i = 0; i < rows.nrows; ++i) {
            printf("  #%llu ", (unsigned long long)i);
            print_row(&rows.rows[i]);
        }
        rtdb_abi_free_rows(&rows);
    }

    printf("committed_seq=%lld\n", (long long)rtdb_abi_committed_seq(eng));

    rtdb_abi_close_table(tbl);
    rtdb_abi_close(eng);
    printf("cabi_demo OK\n");
    return 0;
}
