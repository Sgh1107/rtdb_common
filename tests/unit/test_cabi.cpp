/// M2-W3 C ABI 单测（docs/10 §2-M2-D3）：进程内直调 extern "C" 符号，
/// 覆盖 版本/开闭/建表/行式 CRUD/列式块/范围扫描/错误码映射/结果释放/
/// 角色语义。跨语言绑定（Python ctypes）由 scripts/cabi_smoke.py 覆盖。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "rtdb/rtdb_cabi.h"

namespace {

namespace fs = std::filesystem;

class CAbiTest : public ::testing::Test {
protected:
    void SetUp() override { Cleanup(); }
    void TearDown() override {
        TearDownClose();
        Cleanup();
    }

    static void Cleanup() {
        std::error_code ec;
        fs::remove(kImage, ec);
        fs::remove(std::string(kImage) + ".snap", ec);
    }

    /// 打开引擎并建表（id i64 非空 / val f64 可空 / tag utf8 可空）。
    void OpenWithTable(uint32_t role = RTDB_ROLE_STANDALONE) {
        ASSERT_EQ(rtdb_abi_open("cabi_test", kImage, role, RTDB_FSYNC_PER_SECOND, &engine_),
                  RTDB_OK);
        ASSERT_NE(engine_, nullptr);
        rtdb_column_def cols[3];
        std::memset(cols, 0, sizeof(cols));
        cols[0].name = "id";
        cols[0].type = RTDB_T_INT64;
        cols[0].nullable = 0;
        cols[1].name = "val";
        cols[1].type = RTDB_T_FLOAT64;
        cols[1].nullable = 1;
        cols[2].name = "tag";
        cols[2].type = RTDB_T_UTF8VAR;
        cols[2].nullable = 1;
        ASSERT_EQ(rtdb_abi_create_table(engine_, "t", cols, 3, 0, &table_), RTDB_OK);
        ASSERT_NE(table_, nullptr);
    }

    static rtdb_value Int(int64_t v) {
        rtdb_value x;
        std::memset(&x, 0, sizeof(x));
        x.type = RTDB_T_INT64;
        x.int_value = v;
        return x;
    }

    static rtdb_value Real(double v) {
        rtdb_value x;
        std::memset(&x, 0, sizeof(x));
        x.type = RTDB_T_FLOAT64;
        x.real_value = v;
        return x;
    }

    static rtdb_value Text(const char* s) {
        rtdb_value x;
        std::memset(&x, 0, sizeof(x));
        x.type = RTDB_T_UTF8VAR;
        x.bytes = s;
        x.bytes_len = static_cast<uint32_t>(std::strlen(s));
        return x;
    }

    static constexpr const char* kImage = "rtdb_gtest_cabi.rtdb";

    rtdb_engine* engine_ = nullptr;
    rtdb_table* table_ = nullptr;
    rtdb_cursor* cursor_ = nullptr;

    void TearDownClose() {
        if (cursor_ != nullptr) rtdb_abi_cursor_close(cursor_);
        if (table_ != nullptr) rtdb_abi_close_table(table_);
        if (engine_ != nullptr) rtdb_abi_close(engine_);
        cursor_ = nullptr;
        table_ = nullptr;
        engine_ = nullptr;
    }
};

TEST_F(CAbiTest, VersionTwoAddsSubscriptionFamily) {
    EXPECT_EQ(rtdb_abi_version(), 2u);
    EXPECT_EQ(RTDB_ABI_VER, 2u);
}

TEST_F(CAbiTest, NullArgumentsRejected) {
    EXPECT_EQ(rtdb_abi_open(nullptr, kImage, RTDB_ROLE_STANDALONE, RTDB_FSYNC_PER_SECOND,
                            &engine_),
              RTDB_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(rtdb_abi_create_table(nullptr, "t", nullptr, 0, 0, &table_),
              RTDB_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(rtdb_abi_insert(nullptr, nullptr, nullptr, 0, nullptr), RTDB_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(rtdb_abi_committed_seq(nullptr), -1);
    EXPECT_EQ(rtdb_abi_subscribe(nullptr, nullptr, 0, nullptr), RTDB_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(rtdb_abi_cursor_next(nullptr, 0, nullptr), RTDB_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(rtdb_abi_cursor_position(nullptr), 0u);
    // 释放函数的空指针安全。
    rtdb_abi_free_row(nullptr);
    rtdb_abi_free_rows(nullptr);
    rtdb_abi_free_batch(nullptr);
    rtdb_abi_cursor_reset(nullptr, 1);
    rtdb_abi_cursor_close(nullptr);
    rtdb_abi_close(nullptr);
    rtdb_abi_close_table(nullptr);
}

TEST_F(CAbiTest, InsertGetScanRoundtrip) {
    OpenWithTable();
    const rtdb_value vals[3] = {Int(7), Real(3.5), Text("hello")};
    uint64_t row_no = 123;
    ASSERT_EQ(rtdb_abi_insert(engine_, table_, vals, 3, &row_no), RTDB_OK);
    EXPECT_EQ(row_no, 0u);

    rtdb_value key = Int(7);
    rtdb_row row;
    std::memset(&row, 0, sizeof(row));
    ASSERT_EQ(rtdb_abi_get(engine_, table_, &key, &row), RTDB_OK);
    ASSERT_EQ(row.nvals, 3u);
    EXPECT_EQ(row.vals[0].int_value, 7);
    EXPECT_DOUBLE_EQ(row.vals[1].real_value, 3.5);
    EXPECT_EQ(std::string(row.vals[2].bytes, row.vals[2].bytes_len), "hello");
    rtdb_abi_free_row(&row);
    EXPECT_EQ(row.vals, nullptr);  // 释放后清零

    rtdb_rows rows;
    std::memset(&rows, 0, sizeof(rows));
    ASSERT_EQ(rtdb_abi_scan(engine_, table_, nullptr, nullptr, &rows), RTDB_OK);
    ASSERT_EQ(rows.nrows, 1u);
    EXPECT_EQ(rows.rows[0].vals[0].int_value, 7);
    rtdb_abi_free_rows(&rows);
    EXPECT_EQ(rows.rows, nullptr);
    rtdb_abi_free_rows(&rows);  // 重复释放安全

    TearDownClose();
}

TEST_F(CAbiTest, UpsertBlockWithNullMaskAndVarLengths) {
    OpenWithTable();
    const int64_t ids[3] = {1, 2, 3};
    const double vls[3] = {1.0, 2.0, 3.0};
    const char* tags[3] = {"aa", "bbb", "c"};
    const uint32_t tag_lens[3] = {2, 3, 1};
    const uint8_t val_null[3] = {0, 1, 0};  // 第 2 行 val NULL
    rtdb_column cols[3];
    std::memset(cols, 0, sizeof(cols));
    cols[0].name = "id";
    cols[0].type = RTDB_T_INT64;
    cols[0].data = ids;
    cols[1].name = "val";
    cols[1].type = RTDB_T_FLOAT64;
    cols[1].data = vls;
    cols[1].null_flags = val_null;
    cols[2].name = "tag";
    cols[2].type = RTDB_T_UTF8VAR;
    cols[2].data = tags;
    cols[2].lengths = tag_lens;

    uint64_t seq = 0;
    ASSERT_EQ(rtdb_abi_upsert_block(engine_, "t", cols, 3, 3, &seq), RTDB_OK);
    EXPECT_GT(seq, 0u);

    // 第 2 行 val 为 NULL，tag 为 "bbb"（长度表生效）。
    rtdb_value key = Int(2);
    rtdb_row row;
    std::memset(&row, 0, sizeof(row));
    ASSERT_EQ(rtdb_abi_get(engine_, table_, &key, &row), RTDB_OK);
    EXPECT_EQ(row.vals[1].is_null, 1);
    EXPECT_EQ(std::string(row.vals[2].bytes, row.vals[2].bytes_len), "bbb");
    rtdb_abi_free_row(&row);

    // 有界扫描 [2, 3]。
    rtdb_value lo = Int(2);
    rtdb_value hi = Int(3);
    rtdb_rows rows;
    std::memset(&rows, 0, sizeof(rows));
    ASSERT_EQ(rtdb_abi_scan(engine_, table_, &lo, &hi, &rows), RTDB_OK);
    ASSERT_EQ(rows.nrows, 2u);
    EXPECT_EQ(rows.rows[0].vals[0].int_value, 2);
    EXPECT_EQ(rows.rows[1].vals[0].int_value, 3);
    rtdb_abi_free_rows(&rows);

    TearDownClose();
}

TEST_F(CAbiTest, ErrorCodeMapping) {
    OpenWithTable();

    // 重名建表。
    rtdb_column_def cols[1];
    std::memset(cols, 0, sizeof(cols));
    cols[0].name = "id";
    cols[0].type = RTDB_T_INT64;
    rtdb_table* dup = nullptr;
    EXPECT_EQ(rtdb_abi_create_table(engine_, "t", cols, 1, 0, &dup), RTDB_ERR_TABLE_EXISTS);

    // 主键重复插入。
    const rtdb_value vals[3] = {Int(9), Real(0.0), Text("x")};
    ASSERT_EQ(rtdb_abi_insert(engine_, table_, vals, 3, nullptr), RTDB_OK);
    EXPECT_EQ(rtdb_abi_insert(engine_, table_, vals, 3, nullptr), RTDB_ERR_ALREADY_EXISTS);

    // 缺键读取 / 缺表打开 / 缺表删除。
    rtdb_value key = Int(12345);
    rtdb_row row;
    std::memset(&row, 0, sizeof(row));
    EXPECT_EQ(rtdb_abi_get(engine_, table_, &key, &row), RTDB_ERR_NOT_FOUND);
    rtdb_table* missing = nullptr;
    EXPECT_EQ(rtdb_abi_open_table(engine_, "nope", &missing), RTDB_ERR_TABLE_NOT_FOUND);
    EXPECT_EQ(rtdb_abi_drop_table(engine_, "nope"), RTDB_ERR_TABLE_NOT_FOUND);

    // 非整数主键 / 未知列类型。
    rtdb_value fkey = Real(1.5);
    std::memset(&row, 0, sizeof(row));
    EXPECT_EQ(rtdb_abi_get(engine_, table_, &fkey, &row), RTDB_ERR_INVALID_ARGUMENT);
    rtdb_column_def bad[1];
    std::memset(bad, 0, sizeof(bad));
    bad[0].name = "id";
    bad[0].type = 999;
    rtdb_table* badt = nullptr;
    EXPECT_EQ(rtdb_abi_create_table(engine_, "bad", bad, 1, 0, &badt),
              RTDB_ERR_INVALID_ARGUMENT);

    TearDownClose();
}

TEST_F(CAbiTest, ReaderRoleRejectsWrites) {
    // 先以写角色建表并落盘，再以 Reader 重开：写操作应被拒。
    {
        OpenWithTable();
        TearDownClose();
    }
    ASSERT_EQ(rtdb_abi_open("cabi_test", kImage, RTDB_ROLE_READER, RTDB_FSYNC_PER_SECOND,
                            &engine_),
              RTDB_OK);
    ASSERT_EQ(rtdb_abi_open_table(engine_, "t", &table_), RTDB_OK);
    const rtdb_value vals[3] = {Int(1), Real(0.0), Text("x")};
    EXPECT_EQ(rtdb_abi_insert(engine_, table_, vals, 3, nullptr), RTDB_ERR_READ_ONLY);
    EXPECT_EQ(rtdb_abi_drop_table(engine_, "t"), RTDB_ERR_READ_ONLY);
    TearDownClose();
}

TEST_F(CAbiTest, ReopenSeesPersistedRows) {
    {
        OpenWithTable();
        const rtdb_value vals[3] = {Int(5), Real(0.5), Text("persist")};
        ASSERT_EQ(rtdb_abi_insert(engine_, table_, vals, 3, nullptr), RTDB_OK);
        TearDownClose();
    }
    ASSERT_EQ(rtdb_abi_open("cabi_test", kImage, RTDB_ROLE_STANDALONE, RTDB_FSYNC_PER_SECOND,
                            &engine_),
              RTDB_OK);
    ASSERT_EQ(rtdb_abi_open_table(engine_, "t", &table_), RTDB_OK);
    rtdb_value key = Int(5);
    rtdb_row row;
    std::memset(&row, 0, sizeof(row));
    ASSERT_EQ(rtdb_abi_get(engine_, table_, &key, &row), RTDB_OK);
    EXPECT_EQ(std::string(row.vals[2].bytes, row.vals[2].bytes_len), "persist");
    rtdb_abi_free_row(&row);
    TearDownClose();
}

/// v2 订阅冒烟：资源开闭顺序、空批、insert/update 事件、批释放、
/// 重置与位置查询（溢出/并发语义由 C++ 侧用例覆盖）。
TEST_F(CAbiTest, SubscribeCursorNextDeliversEvents) {
    OpenWithTable();
    ASSERT_EQ(rtdb_abi_subscribe(engine_, nullptr, 0, &cursor_), RTDB_OK);
    ASSERT_NE(cursor_, nullptr);
    EXPECT_EQ(rtdb_abi_cursor_position(cursor_),
              static_cast<uint64_t>(rtdb_abi_committed_seq(engine_)));

    // 无变更：立即返回空批（changes = NULL，nchanges = 0）。
    rtdb_batch batch;
    std::memset(&batch, 0, sizeof(batch));
    ASSERT_EQ(rtdb_abi_cursor_next(cursor_, 0, &batch), RTDB_OK);
    EXPECT_EQ(batch.nchanges, 0u);
    EXPECT_EQ(batch.changes, nullptr);

    // insert（行式）→ 事件交付。
    const rtdb_value vals[3] = {Int(1), Real(1.5), Text("a")};
    ASSERT_EQ(rtdb_abi_insert(engine_, table_, vals, 3, nullptr), RTDB_OK);

    // update（列式覆盖：id=1 已存在 → 整行覆盖）。
    const int64_t ids[1] = {1};
    const double reals[1] = {2.5};
    const char* tags[1] = {"b"};
    const uint32_t tag_lens[1] = {1};
    rtdb_column cols[3];
    std::memset(cols, 0, sizeof(cols));
    cols[0].name = "id";
    cols[0].type = RTDB_T_INT64;
    cols[0].data = ids;
    cols[1].name = "val";
    cols[1].type = RTDB_T_FLOAT64;
    cols[1].data = reals;
    cols[2].name = "tag";
    cols[2].type = RTDB_T_UTF8VAR;
    cols[2].data = tags;
    cols[2].lengths = tag_lens;
    ASSERT_EQ(rtdb_abi_upsert_block(engine_, "t", cols, 3, 1, nullptr), RTDB_OK);

    ASSERT_EQ(rtdb_abi_cursor_next(cursor_, 500, &batch), RTDB_OK);
    ASSERT_EQ(batch.nchanges, 2u);
    const rtdb_change& c0 = batch.changes[0];
    EXPECT_EQ(c0.kind, RTDB_CHG_INSERT);
    EXPECT_GT(c0.table_id, 0);
    EXPECT_EQ(std::string(c0.table), "t");
    ASSERT_EQ(c0.row.nvals, 3u);
    EXPECT_EQ(c0.row.vals[0].int_value, 1);
    EXPECT_DOUBLE_EQ(c0.row.vals[1].real_value, 1.5);
    // 变长列：插入事件已被列式覆盖写取代（池块回收）→ 交付 NULL。
    EXPECT_EQ(c0.row.vals[2].is_null, 1);
    const rtdb_change& c1 = batch.changes[1];
    EXPECT_EQ(c1.kind, RTDB_CHG_UPDATE);
    EXPECT_GT(c1.seq, c0.seq);
    ASSERT_EQ(c1.row.nvals, 3u);
    EXPECT_DOUBLE_EQ(c1.row.vals[1].real_value, 2.5);
    // 覆盖事件为行当前版本 → 变长列交付真实值。
    EXPECT_EQ(std::string(c1.row.vals[2].bytes, c1.row.vals[2].bytes_len), "b");
    EXPECT_EQ(batch.last_seq, static_cast<uint64_t>(rtdb_abi_committed_seq(engine_)));
    rtdb_abi_free_batch(&batch);
    EXPECT_EQ(batch.changes, nullptr);  // 释放后清零
    rtdb_abi_free_batch(&batch);        // 重复释放安全

    // 位置查询与重置（溢出重同步路径的接口契约）。
    rtdb_abi_cursor_reset(cursor_, static_cast<uint64_t>(rtdb_abi_committed_seq(engine_)));
    EXPECT_EQ(rtdb_abi_cursor_position(cursor_),
              static_cast<uint64_t>(rtdb_abi_committed_seq(engine_)));
    std::memset(&batch, 0, sizeof(batch));
    EXPECT_EQ(rtdb_abi_cursor_next(cursor_, 0, &batch), RTDB_OK);
    EXPECT_EQ(batch.nchanges, 0u);

    TearDownClose();
}

}  // namespace
