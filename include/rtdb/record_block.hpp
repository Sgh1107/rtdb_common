#ifndef RTDB_RECORD_BLOCK_HPP_
#define RTDB_RECORD_BLOCK_HPP_

/// \file
/// \brief 列式批量入口：`RecordBlock`（构建器）+ `RecordBlockView`（引擎入参）。
///
/// 对应 docs/03 §2.3 `UpsertBatch(RecordBlockView)` 与 docs/04 §3 的
/// `RecordBlock blk("measure", N); blk.AppendColumn(...)` 用法（M2-W2）。
///
/// 设计要点：
///  - **零拷贝**：`AppendColumn` 只记录列名/类型/数据指针/行数，不复制
///    列数据；调用方内存须在 `Engine::UpsertBatch` 调用期间有效。
///  - 支持载荷类型：`int64_t`（→ 整数类/bool/时间戳列）、`double`
///    （→ 浮点列）、`string_view`（→ 变长列）；列与表 schema 的映射按
///    列名在 `UpsertBatch` 内完成，未提供的列按 schema 填充（nullable →
///    NULL；ext 列默认值；非空缺列报错）。
///  - 空值：`SetNullMask` 附 1B/行掩码（非 0 = NULL），须在 AppendColumn 后调用。
///  - 构建期错误不抛异常：首个错误记录在 `Error()`，`UpsertBatch` 返回
///    `kInvalidArgument`（宏契约：边界不抛异常，docs/04 §1）。

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rtdb/api_macro.hpp"
#include "rtdb/err.hpp"
#include "rtdb/span.hpp"
#include "rtdb/table_def.hpp"

namespace rtdb {

/// \brief 列式块的单列视图（零拷贝；`data` 指向调用方数组首地址）。
struct BlockColumn {
    /// 列名（用于与表 schema 按名映射）。
    std::string_view name;

    /// 载荷类型（kInt64 / kFloat64 / kUtf8Var 三者之一；代表 `data` 的
    /// 元素类型，与目标列的 schema 类型须兼容，见 record_block.hpp 说明）。
    DataType type = DataType::kInt64;

    /// 数组首地址（`int64_t[]` / `double[]` / `std::string_view[]`）。
    const void* data = nullptr;

    /// 空值掩码（1B/行，非 0 = NULL）；nullptr = 全非空。
    const uint8_t* null_flags = nullptr;

    /// 本列行数（与块行数须一致）。
    size_t rows = 0;
};

/// \brief 引擎入参视图：列数组 + 行数 + 目标表名。
///
/// 可由 `RecordBlock::View()` 取得，也可由其他语言绑定/自定义构建器直接
/// 构造（无需经过 RecordBlock）。`columns` 的生命周期须覆盖
/// `UpsertBatch` 调用。
struct RecordBlockView {
    std::string_view table_name;
    Span<const BlockColumn> columns;
    size_t row_count = 0;
};

#if defined(_MSC_VER)
#pragma warning(push)
// C4251: std::string/vector 成员不参与 DLL 接口实体化（纯宿主侧构建器，
// 与 TableHandle/ColumnValue 的处理一致，属预期内安全用法）。
#pragma warning(disable : 4251)
#endif

/// \brief 列式块构建器（链式 AppendColumn，docs/04 §3）。
///
/// 用法：
/// ```cpp
/// rtdb::RecordBlock blk("measure", 512);
/// blk.AppendColumn("id", ids).AppendColumn("val", vals);
/// blk.AppendColumn("desc", descs).SetNullMask("desc", desc_null);
/// auto seq = eng->UpsertBatch(blk);
/// ```
/// 校验：列名非空且不重名、行数一致（首列定行数）、掩码长度等于行数、
/// 掩码在对应列之后设置。违例记录首个错误（`Error()`），`UpsertBatch`
/// 返回 `kInvalidArgument`。
class RTDB_API RecordBlock {
public:
    /// \param table_name 目标表名
    /// \param row_cap_hint 行数提示（仅供实现预留，语义上与批大小无强绑定）
    explicit RecordBlock(std::string_view table_name, uint32_t row_cap_hint = 0)
        : table_name_(table_name), row_cap_hint_(row_cap_hint) {}

    /// 追加整数列（int64 载荷：整数类/bool/时间戳列）。
    RecordBlock& AppendColumn(std::string_view name, Span<const int64_t> values) {
        return AppendImpl(name, DataType::kInt64, values.data(), nullptr, values.size());
    }

    /// 追加浮点列（double 载荷：float32/float64 列）。
    RecordBlock& AppendColumn(std::string_view name, Span<const double> values) {
        return AppendImpl(name, DataType::kFloat64, values.data(), nullptr, values.size());
    }

    /// 追加变长列（string_view 载荷：utf8/bytes 列，值在 UpsertBatch 内拷入池）。
    RecordBlock& AppendColumn(std::string_view name, Span<const std::string_view> values) {
        return AppendImpl(name, DataType::kUtf8Var, values.data(), nullptr, values.size());
    }

    /// \brief 为已追加列设置空值掩码（1B/行，非 0 = NULL）。
    RecordBlock& SetNullMask(std::string_view name, Span<const uint8_t> null_flags) {
        if (err_ != Err::kOk) return *this;
        for (BlockColumn& c : columns_) {
            if (c.name != name) continue;
            if (c.null_flags != nullptr) {
                err_ = Err::kInvalidArgument;  // 重复设置
                return *this;
            }
            if (null_flags.size() != c.rows) {
                err_ = Err::kInvalidArgument;
                return *this;
            }
            c.null_flags = null_flags.data();
            return *this;
        }
        err_ = Err::kInvalidArgument;  // 未知列名
        return *this;
    }

    const std::string& table_name() const noexcept { return table_name_; }
    uint32_t row_cap_hint() const noexcept { return row_cap_hint_; }
    size_t column_count() const noexcept { return columns_.size(); }

    /// 块行数（首列行数；未追加列时为 0）。
    size_t row_count() const noexcept { return columns_.empty() ? 0 : columns_[0].rows; }

    /// 构建是否有效（无首个错误、至少一列、行数 > 0）。
    bool IsValid() const noexcept { return err_ == Err::kOk && !columns_.empty() && row_count() > 0; }

    /// 首个构建错误（IsValid() 为 true 时恒 kOk）。
    Err Error() const noexcept { return IsValid() ? Err::kOk : (err_ != Err::kOk ? err_ : Err::kInvalidArgument); }

    Span<const BlockColumn> columns() const noexcept {
        return Span<const BlockColumn>(columns_.data(), columns_.size());
    }

    /// 转为引擎入参视图（零拷贝；生命周期随本对象及调用方列内存）。
    ///
    /// 构建期已检出错误的块转为**空视图**（空列 + 0 行），引擎据此返回
    /// kInvalidArgument——避免"部分有效"的块被静默接受。
    RecordBlockView View() const noexcept {
        if (!IsValid()) return RecordBlockView{};
        return RecordBlockView{table_name_, columns(), row_count()};
    }

private:
    RecordBlock& AppendImpl(std::string_view name, DataType payload_type, const void* data,
                            const uint8_t* null_flags, size_t rows) {
        if (err_ != Err::kOk) return *this;
        if (name.empty() || data == nullptr || rows == 0) {
            err_ = Err::kInvalidArgument;
            return *this;
        }
        for (const BlockColumn& c : columns_) {
            if (c.name == name) {
                err_ = Err::kInvalidArgument;  // 列名重复
                return *this;
            }
        }
        if (!columns_.empty() && rows != columns_[0].rows) {
            err_ = Err::kInvalidArgument;  // 行数不一致
            return *this;
        }
        columns_.push_back(BlockColumn{name, payload_type, data, null_flags, rows});
        return *this;
    }

    std::string table_name_;
    uint32_t row_cap_hint_ = 0;
    std::vector<BlockColumn> columns_;
    Err err_ = Err::kOk;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace rtdb

#endif  // RTDB_RECORD_BLOCK_HPP_
