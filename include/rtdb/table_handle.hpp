#ifndef RTDB_TABLE_HANDLE_HPP_
#define RTDB_TABLE_HANDLE_HPP_

/// \file
/// \brief 表句柄：`Engine::CreateTable` / `Engine::OpenTable` 的返回值。
///
/// 句柄在打开时刻绑定表定义快照（`layout_version`、列数、行容量），
/// 后续在线加列不会改变已发放句柄的读取语义（docs/02 §3 要点 3）。
/// 句柄为轻量值类型，可自由拷贝；其有效性由引擎侧状态决定。

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "rtdb/api_macro.hpp"

namespace rtdb {

class Engine;

#if defined(_MSC_VER)
#pragma warning(push)
// C4251: std::string 成员不参与 DLL 接口实体化（句柄仅按值传递），
// 与 Engine 的 PImpl 处理一致，属预期内安全用法。
#pragma warning(disable : 4251)
#endif

/// \brief 表句柄（只读快照语义）。
class RTDB_API TableHandle {
public:
    /// \brief 构造一个无效句柄。
    TableHandle() noexcept = default;

    /// \brief 句柄是否有效（成功建表/打开表后为 true）。
    bool IsValid() const noexcept { return table_id_ > 0; }

    /// \brief 目录分配的表 id（单调递增，从 1 起）。
    int64_t table_id() const noexcept { return table_id_; }

    /// \brief 表名。
    const std::string& name() const noexcept { return name_; }

    /// \brief 句柄绑定的行布局版本（在线加列递增）。
    uint32_t layout_version() const noexcept { return layout_version_; }

    /// \brief 建表时的列数（不含后续扩展列）。
    uint32_t column_count() const noexcept { return column_count_; }

    /// \brief 句柄绑定时刻的扩展列数（在线加列追加；旧句柄保持旧值，
    /// 因而看不到其后新增的列，docs/02 §3 要点 3）。
    uint32_t ext_column_count() const noexcept { return ext_column_count_; }

    /// \brief 句柄可见的总列数（内联列 + 绑定时刻的扩展列）。
    uint32_t visible_column_count() const noexcept { return column_count_ + ext_column_count_; }

    /// \brief 建表时定格的行槽容量。
    uint32_t row_cap() const noexcept { return row_cap_; }

private:
    friend class Engine;

    /// \brief 内部构造（仅引擎使用）。
    /// \param desc_off 描述符在映像内的偏移（不透明内部令牌）。
    TableHandle(int64_t table_id, std::string name, uint32_t layout_version, uint32_t col_count,
                uint32_t ext_col_count, uint32_t row_cap, uint64_t desc_off) noexcept
        : table_id_(table_id),
          name_(std::move(name)),
          layout_version_(layout_version),
          column_count_(col_count),
          ext_column_count_(ext_col_count),
          row_cap_(row_cap),
          desc_off_(desc_off) {}

    /// \brief 描述符偏移（内部令牌；Public API 使用者不得解释其含义）。
    uint64_t desc_off() const noexcept { return desc_off_; }

    int64_t table_id_ = 0;
    std::string name_;
    uint32_t layout_version_ = 0;
    uint32_t column_count_ = 0;
    uint32_t ext_column_count_ = 0;
    uint32_t row_cap_ = 0;
    uint64_t desc_off_ = 0;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace rtdb

#endif  // RTDB_TABLE_HANDLE_HPP_
