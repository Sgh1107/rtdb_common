/// \file W1 协议基座单测（docs/13 §5-W1）：读写游标 / 帧编解码 /
/// 值·行·表定义·列式块线编码。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "net/codec.hpp"
#include "net/value_codec.hpp"
#include "net/wire.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/record_block.hpp"
#include "rtdb/span.hpp"
#include "rtdb/table_def.hpp"

namespace {

using rtdb::BlockColumn;
using rtdb::ColumnDef;
using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::Err;
using rtdb::IndexDef;
using rtdb::RecordBlockView;
using rtdb::Span;
using rtdb::TableDef;
using rtdb::net::FrameHeader;
using rtdb::net::Reader;
using rtdb::net::Writer;

// ------------------------------------------------------------ 读写游标

TEST(ProtoCodecTest, ScalarsRoundTrip) {
    std::vector<uint8_t> buf;
    Writer w(&buf);
    w.U8(0xAB);
    w.U16(0x1234);
    w.U32(0xDEADBEEFu);
    w.U64(0x0123456789ABCDEFull);
    w.I64(-42);
    w.F64(3.25);
    w.Str("hello");
    w.Var(std::string_view("world!", 6));

    Reader r(buf.data(), buf.size());
    EXPECT_EQ(r.U8(), 0xAB);
    EXPECT_EQ(r.U16(), 0x1234);
    EXPECT_EQ(r.U32(), 0xDEADBEEFu);
    EXPECT_EQ(r.U64(), 0x0123456789ABCDEFull);
    EXPECT_EQ(r.I64(), -42);
    EXPECT_DOUBLE_EQ(r.F64(), 3.25);
    std::string s;
    EXPECT_TRUE(r.Str(&s));
    EXPECT_EQ(s, "hello");
    std::string v;
    EXPECT_TRUE(r.Var(&v));
    EXPECT_EQ(v, "world!");
    EXPECT_TRUE(r.Finish());
}

TEST(ProtoCodecTest, ReaderStickyFail) {
    const uint8_t data[2] = {0x01, 0x02};
    Reader r(data, sizeof(data));
    EXPECT_EQ(r.U32(), 0u);  // 越界：置失败并返回 0
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.remaining(), 0u);
    EXPECT_EQ(r.U8(), 0);  // 失败态下读取不再改变状态
    EXPECT_FALSE(r.Finish());
}

TEST(ProtoCodecTest, FinishRequiresExactConsumption) {
    std::vector<uint8_t> buf;
    Writer w(&buf);
    w.U32(7);
    w.U32(8);
    Reader r(buf.data(), buf.size());
    EXPECT_EQ(r.U32(), 7u);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.Finish());  // 仍有剩余
    EXPECT_EQ(r.U32(), 8u);
    EXPECT_TRUE(r.Finish());
}

// ------------------------------------------------------------ 帧

TEST(ProtoFrameTest, HeaderRoundTrip) {
    FrameHeader h;
    h.flags = rtdb::net::kFlagResp;
    h.session = 0x11223344u;
    h.func = 11;
    h.payload_len = 123;
    h.crc = 0xAABBCCDDu;
    uint8_t out[rtdb::net::kFrameHeaderBytes];
    rtdb::net::EncodeFrameHeader(h, out);

    FrameHeader back;
    EXPECT_EQ(rtdb::net::DecodeFrameHeader(out, 1024, &back), Err::kOk);
    EXPECT_EQ(back.ver, h.ver);
    EXPECT_EQ(back.flags, h.flags);
    EXPECT_EQ(back.session, h.session);
    EXPECT_EQ(back.func, h.func);
    EXPECT_EQ(back.payload_len, h.payload_len);
    EXPECT_EQ(back.crc, h.crc);
}

TEST(ProtoFrameTest, HeaderValidation) {
    uint8_t out[rtdb::net::kFrameHeaderBytes];
    FrameHeader h;
    h.payload_len = 8;
    rtdb::net::EncodeFrameHeader(h, out);
    FrameHeader back;
    uint8_t bad[rtdb::net::kFrameHeaderBytes];

    std::memcpy(bad, out, sizeof(bad));
    bad[0] = 'X';  // magic 破坏
    EXPECT_EQ(rtdb::net::DecodeFrameHeader(bad, 1024, &back), Err::kProtocolError);

    std::memcpy(bad, out, sizeof(bad));
    bad[4] = 2;  // 版本不符
    EXPECT_EQ(rtdb::net::DecodeFrameHeader(bad, 1024, &back), Err::kProtocolError);

    std::memcpy(bad, out, sizeof(bad));
    bad[5] = 0x02;  // 压缩保留位（v1 恒 0）
    EXPECT_EQ(rtdb::net::DecodeFrameHeader(bad, 1024, &back), Err::kProtocolError);

    std::memcpy(bad, out, sizeof(bad));
    rtdb::net::FrameHeader big;
    big.payload_len = 0xFFFFFFFFu;
    rtdb::net::EncodeFrameHeader(big, bad);
    EXPECT_EQ(rtdb::net::DecodeFrameHeader(bad, 16, &back), Err::kProtocolError);

    EXPECT_EQ(rtdb::net::DecodeFrameHeader(nullptr, 16, &back), Err::kInvalidArgument);
}

TEST(ProtoFrameTest, CrcCoversPayloadAndHeader) {
    FrameHeader h;
    h.func = 11;
    h.payload_len = 4;
    const uint8_t payload[4] = {1, 2, 3, 4};
    const uint32_t crc = rtdb::net::ComputeFrameCrc(h, payload);

    uint8_t payload2[4] = {1, 2, 3, 5};
    EXPECT_NE(rtdb::net::ComputeFrameCrc(h, payload2), crc);

    FrameHeader h2 = h;
    h2.func = 12;
    EXPECT_NE(rtdb::net::ComputeFrameCrc(h2, payload), crc);

    FrameHeader h3 = h;
    h3.crc = 0xFFFFFFFFu;  // crc 字段不参与计算（置 0 后统一）
    EXPECT_EQ(rtdb::net::ComputeFrameCrc(h3, payload), crc);
}

// ------------------------------------------------------------ 值

TEST(ProtoValueTest, IntegerClassesRoundTrip) {
    const DataType int_types[] = {DataType::kInt8,  DataType::kInt16,  DataType::kInt32,
                                  DataType::kInt64, DataType::kBool, DataType::kUnixTsNs};
    for (DataType t : int_types) {
        std::vector<uint8_t> buf;
        Writer w(&buf);
        rtdb::net::WriteValue(w, ColumnValue::IntOf(t, -7));

        Reader r(buf.data(), buf.size());
        const ColumnValue v = rtdb::net::ReadValueTyped(r, t);
        EXPECT_TRUE(r.Finish());
        EXPECT_FALSE(v.is_null());
        EXPECT_EQ(v.type(), t);
        EXPECT_EQ(v.as_int(), -7);
    }
}

TEST(ProtoValueTest, FloatClassesRoundTrip) {
    const DataType float_types[] = {DataType::kFloat32, DataType::kFloat64};
    for (DataType t : float_types) {
        std::vector<uint8_t> buf;
        Writer w(&buf);
        rtdb::net::WriteValue(w, ColumnValue::Real(2.5));

        Reader r(buf.data(), buf.size());
        const ColumnValue v = rtdb::net::ReadValueTyped(r, t);
        EXPECT_TRUE(r.Finish());
        EXPECT_FALSE(v.is_null());
        EXPECT_DOUBLE_EQ(v.as_real(), 2.5);
    }
}

TEST(ProtoValueTest, VarClassesRoundTrip) {
    {
        std::vector<uint8_t> buf;
        Writer w(&buf);
        rtdb::net::WriteValue(w, ColumnValue::Text("abc"));
        Reader r(buf.data(), buf.size());
        const ColumnValue v = rtdb::net::ReadValueTyped(r, DataType::kUtf8Var);
        EXPECT_TRUE(r.Finish());
        EXPECT_EQ(v.type(), DataType::kUtf8Var);
        EXPECT_EQ(v.as_bytes(), "abc");
    }
    {
        std::string bin;
        bin.push_back('\x01');
        bin.push_back('\x00');
        bin.push_back('\xFF');
        std::vector<uint8_t> buf;
        Writer w(&buf);
        rtdb::net::WriteValue(w, ColumnValue::Bytes(bin));
        Reader r(buf.data(), buf.size());
        const ColumnValue v = rtdb::net::ReadValueTyped(r, DataType::kBytesVar);
        EXPECT_TRUE(r.Finish());
        EXPECT_EQ(v.type(), DataType::kBytesVar);
        EXPECT_EQ(v.as_bytes(), bin);
    }
}

TEST(ProtoValueTest, NullRoundTrip) {
    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteValue(w, ColumnValue::Null());
    Reader r(buf.data(), buf.size());
    const ColumnValue v = rtdb::net::ReadValueTyped(r, DataType::kInt64);
    EXPECT_TRUE(r.Finish());
    EXPECT_TRUE(v.is_null());
}

TEST(ProtoValueTest, TagTypeMismatchPoisons) {
    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteValue(w, ColumnValue::Int(5));  // tag = i64
    Reader r(buf.data(), buf.size());
    rtdb::net::ReadValueTyped(r, DataType::kFloat64);  // 按浮点列读 → 违例
    EXPECT_FALSE(r.ok());
}

TEST(ProtoValueTest, TruncatedValuePoisons) {
    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteValue(w, ColumnValue::Text("abcdef"));  // u32 前缀 + 6B
    buf.pop_back();
    Reader r(buf.data(), buf.size());
    rtdb::net::ReadValueTyped(r, DataType::kUtf8Var);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.Finish());
}

// ------------------------------------------------------------ 表定义

TEST(ProtoTableDefTest, RoundTrip) {
    TableDef def;
    def.name = "measure";

    ColumnDef id;
    id.name = "id";
    id.type = DataType::kInt64;
    id.nullable = false;
    ColumnDef val;
    val.name = "val";
    val.type = DataType::kFloat64;
    ColumnDef desc;
    desc.name = "desc";
    desc.type = DataType::kUtf8Var;
    ColumnDef flag;
    flag.name = "flag";
    flag.type = DataType::kInt32;
    flag.has_default = true;
    flag.default_value.is_null = false;
    flag.default_value.int_value = 3;
    def.columns = {id, val, desc, flag};

    def.primary_key = {"id"};
    IndexDef ix;
    ix.name = "ix_val";
    ix.columns = {"val"};
    ix.unique = true;
    def.secondary = {ix};
    def.max_rows_hint = 512;

    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteTableDef(w, def);

    Reader r(buf.data(), buf.size());
    auto rd = rtdb::net::ReadTableDef(r);
    ASSERT_TRUE(rd.IsOk());
    EXPECT_TRUE(r.Finish());

    const TableDef& d = rd->def;
    EXPECT_EQ(d.name, "measure");
    ASSERT_EQ(d.columns.size(), 4u);
    EXPECT_EQ(d.columns[0].name, "id");
    EXPECT_EQ(d.columns[0].type, DataType::kInt64);
    EXPECT_FALSE(d.columns[0].nullable);
    EXPECT_EQ(d.columns[2].type, DataType::kUtf8Var);
    EXPECT_TRUE(d.columns[3].has_default);
    EXPECT_FALSE(d.columns[3].default_value.is_null);
    EXPECT_EQ(d.columns[3].default_value.int_value, 3);
    ASSERT_EQ(d.primary_key.size(), 1u);
    EXPECT_EQ(d.primary_key[0], "id");
    ASSERT_EQ(d.secondary.size(), 1u);
    EXPECT_EQ(d.secondary[0].name, "ix_val");
    EXPECT_TRUE(d.secondary[0].unique);
    ASSERT_EQ(d.secondary[0].columns.size(), 1u);
    EXPECT_EQ(d.secondary[0].columns[0], "val");
    EXPECT_EQ(d.max_rows_hint, 512u);
}

TEST(ProtoTableDefTest, TruncatedFails) {
    TableDef def;
    def.name = "t";
    ColumnDef c;
    c.name = "id";
    c.type = DataType::kInt64;
    c.nullable = false;
    def.columns = {c};

    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteTableDef(w, def);
    buf.resize(buf.size() / 2);  // 截断到一半

    Reader r(buf.data(), buf.size());
    auto rd = rtdb::net::ReadTableDef(r);
    EXPECT_FALSE(rd.IsOk());
    EXPECT_EQ(rd.Error(), Err::kProtocolError);
}

// ------------------------------------------------------------ 列式块

TEST(ProtoBlockTest, RoundTripWithNullMask) {
    const int64_t ids[3] = {10, 20, 30};
    const double vals[3] = {1.5, 2.5, 3.5};
    const std::string_view names[3] = {"a", "bb", "ccc"};
    const uint8_t names_null[3] = {0, 1, 0};

    BlockColumn cols[3];
    cols[0].name = "id";
    cols[0].type = DataType::kInt64;
    cols[0].data = ids;
    cols[0].rows = 3;
    cols[1].name = "val";
    cols[1].type = DataType::kFloat64;
    cols[1].data = vals;
    cols[1].rows = 3;
    cols[2].name = "name";
    cols[2].type = DataType::kUtf8Var;
    cols[2].data = names;
    cols[2].null_flags = names_null;
    cols[2].rows = 3;
    const RecordBlockView view{"measure", Span<const BlockColumn>(cols, 3), 3};

    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteBlock(w, view);

    Reader r(buf.data(), buf.size());
    auto ob = rtdb::net::ReadBlock(r);
    ASSERT_TRUE(ob.IsOk());
    EXPECT_TRUE(r.Finish());

    EXPECT_EQ(ob->table, "measure");
    EXPECT_EQ(ob->row_count, 3u);
    ASSERT_EQ(ob->cols.size(), 3u);
    EXPECT_EQ(ob->cols[0].i64.size(), 3u);
    EXPECT_EQ(ob->cols[0].i64[2], 30);
    EXPECT_DOUBLE_EQ(ob->cols[1].f64[0], 1.5);
    EXPECT_EQ(ob->cols[2].var[2], "ccc");
    ASSERT_EQ(ob->cols[2].null_flags.size(), 3u);
    EXPECT_EQ(ob->cols[2].null_flags[1], 1);
    EXPECT_EQ(ob->cols[0].null_flags.size(), 0u);  // 未携带掩码

    // 引擎入参视图：指针/名称可用且值正确
    const RecordBlockView v2 = ob->View();
    ASSERT_EQ(v2.columns.size(), 3u);
    EXPECT_EQ(v2.row_count, 3u);
    EXPECT_EQ(v2.table_name, "measure");
    EXPECT_EQ(v2.columns[2].name, "name");
    ASSERT_NE(v2.columns[2].null_flags, nullptr);
    const auto* ids2 = static_cast<const int64_t*>(v2.columns[0].data);
    EXPECT_EQ(ids2[1], 20);
    const auto* sv2 = static_cast<const std::string_view*>(v2.columns[2].data);
    EXPECT_EQ(sv2[2], "ccc");
}

TEST(ProtoBlockTest, TruncatedFails) {
    const int64_t ids[2] = {1, 2};
    BlockColumn col;
    col.name = "id";
    col.type = DataType::kInt64;
    col.data = ids;
    col.rows = 2;
    const RecordBlockView view{"t", Span<const BlockColumn>(&col, 1), 2};

    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteBlock(w, view);
    buf.pop_back();

    Reader r(buf.data(), buf.size());
    auto ob = rtdb::net::ReadBlock(r);
    EXPECT_FALSE(ob.IsOk());
    EXPECT_EQ(ob.Error(), Err::kProtocolError);
}

TEST(ProtoBlockTest, MutatedCountsRejected) {
    const int64_t ids[2] = {1, 2};
    BlockColumn col;
    col.name = "id";
    col.type = DataType::kInt64;
    col.data = ids;
    col.rows = 2;
    const RecordBlockView view{"t", Span<const BlockColumn>(&col, 1), 2};

    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteBlock(w, view);
    // 将行数清零（布局：[u16 tlen]['t'][u32 n_rows][u32 n_cols]…），
    // 块行数 0 属非法（协议拒绝）。
    buf[3] = 0;
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;
    Reader r(buf.data(), buf.size());
    auto ob = rtdb::net::ReadBlock(r);
    EXPECT_FALSE(ob.IsOk());
    EXPECT_EQ(ob.Error(), Err::kProtocolError);
}

// ------------------------------------------------------------ 结果头

TEST(ProtoResultHeaderTest, RoundTripWithRow) {
    std::vector<ColumnDef> cols(2);
    cols[0].name = "id";
    cols[0].type = DataType::kInt64;
    cols[1].name = "v";
    cols[1].type = DataType::kFloat64;

    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteResultHeader(w, "t1", Span<const ColumnDef>(cols), 5);
    const std::vector<ColumnValue> row = {ColumnValue::Int(1), ColumnValue::Real(2.0)};
    rtdb::net::WriteRow(w, row);

    Reader r(buf.data(), buf.size());
    auto s = rtdb::net::ReadResultHeader(r);
    ASSERT_TRUE(s.IsOk());
    EXPECT_EQ(s->table, "t1");
    EXPECT_EQ(s->n_rows, 5u);
    ASSERT_EQ(s->col_names.size(), 2u);
    EXPECT_EQ(s->col_names[1], "v");
    ASSERT_EQ(s->col_types.size(), 2u);
    EXPECT_EQ(s->col_types[1], DataType::kFloat64);

    const std::vector<ColumnValue> got = rtdb::net::ReadRow(r, s->col_types);
    EXPECT_TRUE(r.Finish());
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].as_int(), 1);
    EXPECT_DOUBLE_EQ(got[1].as_real(), 2.0);
}

TEST(ProtoResultHeaderTest, EmptyTableNameRejected) {
    std::vector<ColumnDef> cols(1);
    cols[0].name = "id";
    cols[0].type = DataType::kInt64;
    std::vector<uint8_t> buf;
    Writer w(&buf);
    rtdb::net::WriteResultHeader(w, "", Span<const ColumnDef>(cols), 0);

    Reader r(buf.data(), buf.size());
    auto s = rtdb::net::ReadResultHeader(r);
    EXPECT_FALSE(s.IsOk());
    EXPECT_EQ(s.Error(), Err::kProtocolError);
}

}  // namespace
