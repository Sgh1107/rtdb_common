#ifndef RTDB_COLUMN_VALUE_HPP_
#define RTDB_COLUMN_VALUE_HPP_

/// \file
/// \brief 列值：宿主侧的带类型值容器（CRUD 入参与出参单位）。
///
/// 值为**自持有**的宿主侧对象（变长内容拷贝到 `std::string`），
/// 因此从引擎取回的值不依赖映像映射的生命周期。
/// 共享内存内不出现本类型（docs/01 约束：映像结构不得含 std 容器）。

#include <cstdint>
#include <string>
#include <utility>

#include "rtdb/api_macro.hpp"
#include "rtdb/table_def.hpp"

namespace rtdb {

#if defined(_MSC_VER)
#pragma warning(push)
// C4251: std::string 成员随值类型按值传递（不导出成员实体化），
// 与 TableHandle 的处理一致，属预期内安全用法。
#pragma warning(disable : 4251)
#endif

/// \brief 带类型的列值（含空值语义）。
class RTDB_API ColumnValue {
public:
    /// \brief 空值（NULL）。
    static ColumnValue Null() noexcept { return ColumnValue(); }

    /// \brief 整数类值（kInt8/kInt16/kInt32/kInt64/kUnixTsNs/kBool 的数值形式）。
    static ColumnValue Int(int64_t v) noexcept {
        ColumnValue cv;
        cv.type_ = DataType::kInt64;
        cv.is_null_ = false;
        cv.int_ = v;
        return cv;
    }

    /// \brief 指定整数类型与值（保持声明的列类型语义）。
    static ColumnValue IntOf(DataType type, int64_t v) noexcept {
        ColumnValue cv;
        cv.type_ = type;
        cv.is_null_ = false;
        cv.int_ = v;
        return cv;
    }

    /// \brief 浮点值。
    static ColumnValue Real(double v) noexcept {
        ColumnValue cv;
        cv.type_ = DataType::kFloat64;
        cv.is_null_ = false;
        cv.real_ = v;
        return cv;
    }

    /// \brief 布尔值。
    static ColumnValue Bool(bool v) noexcept {
        ColumnValue cv;
        cv.type_ = DataType::kBool;
        cv.is_null_ = false;
        cv.int_ = v ? 1 : 0;
        return cv;
    }

    /// \brief UTF-8 字符串值（变长列）。
    static ColumnValue Text(std::string s) {
        ColumnValue cv;
        cv.type_ = DataType::kUtf8Var;
        cv.is_null_ = false;
        cv.bytes_ = std::move(s);
        return cv;
    }

    /// \brief 二进制块值（变长列）。
    static ColumnValue Bytes(std::string s) {
        ColumnValue cv;
        cv.type_ = DataType::kBytesVar;
        cv.is_null_ = false;
        cv.bytes_ = std::move(s);
        return cv;
    }

    /// \brief 是否为空值。
    bool is_null() const noexcept { return is_null_; }

    /// \brief 值的类型标注（变长/定长分别对应列的 DataType）。
    DataType type() const noexcept { return type_; }

    /// \brief 以整数读取；浮点位模式/变长内容不参与。
    int64_t as_int() const noexcept { return int_; }

    /// \brief 以浮点读取。
    double as_real() const noexcept { return real_; }

    /// \brief 以布尔读取（非 0 即真）。
    bool as_bool() const noexcept { return int_ != 0; }

    /// \brief 变长内容（空值或非变长值为空串）。
    const std::string& as_bytes() const noexcept { return bytes_; }

private:
    ColumnValue() noexcept = default;

    DataType type_ = DataType::kInt64;
    bool is_null_ = true;
    int64_t int_ = 0;
    double real_ = 0.0;
    std::string bytes_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace rtdb

#endif  // RTDB_COLUMN_VALUE_HPP_
