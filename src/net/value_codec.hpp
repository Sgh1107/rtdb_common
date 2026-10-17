#ifndef RTDB_SRC_NET_VALUE_CODEC_HPP_
#define RTDB_SRC_NET_VALUE_CODEC_HPP_

/// \file
/// \brief 值/行/表定义/列式块的线编码（docs/13 §3-D5；标签常量见
/// include/rtdb/proto_v1.h）。
///
/// 原则：
///  - 行式值自描述（tag: NULL/i64/f64/var），读取端按 schema 列类型做
///    相容性校验（整数类统一 i64、浮点统一 f64、变长 u32 前缀）；
///  - 列式块对齐 `RecordBlockView`（i64/f64/var 三类载荷 + 逐列空值掩码）；
///  - 所有 Reader 解析以 `Finish()` 单一出口校验（载荷恰读尽且无失败）。

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/codec.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/record_block.hpp"
#include "rtdb/result.hpp"
#include "rtdb/span.hpp"
#include "rtdb/table_def.hpp"

namespace rtdb {
namespace net {

/// 列类型是否整数类（i8..i64/bool/UnixTsNs；线形统一 i64）。
bool IsIntClass(DataType t) noexcept;
/// 列类型是否浮点类（Float32/Float64；线形统一 f64）。
bool IsFloatClass(DataType t) noexcept;
/// 列类型是否变长类（Utf8Var/BytesVar；线形 u32 前缀字节）。
bool IsVarClass(DataType t) noexcept;

// ---- 值（行式）----

/// 写单值：空 → tag0；浮点 → tag2；变长 → tag3（u32 前缀）；其余 → tag1(i64)。
void WriteValue(Writer& w, const ColumnValue& v) noexcept;

/// 按 schema 列类型读单值；tag 与列类不相容时置 Reader 失败态。
ColumnValue ReadValueTyped(Reader& r, DataType col_type) noexcept;

/// 写整行（逐值；长度由 schema 决定，不写入行内长度）。
void WriteRow(Writer& w, const std::vector<ColumnValue>& row) noexcept;

/// 读整行（按列类型表逐值相容校验；失败置 Reader 失败态并返回已读部分）。
std::vector<ColumnValue> ReadRow(Reader& r, const std::vector<DataType>& col_types) noexcept;

// ---- 结果集/增量自描述头 ----

/// 写结果头：表名 + 列（名/类型）+ 行数（客户端无需预先 Describe）。
void WriteResultHeader(Writer& w, std::string_view table, Span<const ColumnDef> cols,
                        uint32_t n_rows) noexcept;

/// 结果头解析结果。
struct ResultSchema {
    std::string table;
    std::vector<std::string> col_names;
    std::vector<DataType> col_types;
    uint32_t n_rows = 0;
};

Result<ResultSchema> ReadResultHeader(Reader& r) noexcept;

// ---- 表定义（DDL）----

void WriteTableDef(Writer& w, const TableDef& def) noexcept;

/// 自持有表定义：`string_view` 字段指向 `storage`（先全部入存储、后回填
/// 视图，移动/拷贝后视图仍指向自身 storage）。
struct OwnedTableDef {
    TableDef def;
    std::vector<std::string> storage;
};

Result<OwnedTableDef> ReadTableDef(Reader& r) noexcept;

// ---- 列式块（EXEC_BATCH mode=1）----

/// 写块（客户端：由 RecordBlockView 编码；列名/行数一致性由调用方保证）。
void WriteBlock(Writer& w, const RecordBlockView& view) noexcept;

/// 解码后的自持有块：内部地址在解码完成后固定，`View()` 可直接作引擎入参。
struct OwnedBlock {
    std::string table;
    size_t row_count = 0;

    struct Col {
        std::string name;
        /// 线载荷类型（kInt64/kFloat64/kUtf8Var）。
        DataType ptype = DataType::kInt64;
        std::vector<int64_t> i64;
        std::vector<double> f64;
        std::vector<std::string> var;
        std::vector<std::string_view> var_views;  ///< 供 BlockColumn.data 使用
        std::vector<uint8_t> null_flags;          ///< 空 = 全非空
    };
    std::vector<Col> cols;
    std::vector<BlockColumn> views;  ///< 解码完成后构建

    RecordBlockView View() const noexcept;
};

Result<OwnedBlock> ReadBlock(Reader& r) noexcept;

}  // namespace net
}  // namespace rtdb

#endif  // RTDB_SRC_NET_VALUE_CODEC_HPP_
