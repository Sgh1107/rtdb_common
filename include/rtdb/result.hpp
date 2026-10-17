#ifndef RTDB_RESULT_HPP_
#define RTDB_RESULT_HPP_

#include <cassert>
#include <optional>
#include <utility>

#include "rtdb/err.hpp"

namespace rtdb {

/// 轻量 Result<T>：跨界面禁止异常穿越（见 docs/04 §1），所有可能失败的
/// API 一律返回本类型。基于 std::optional 实现，仅用于宿主侧类型，
/// 共享内存内结构体不得包含它。
template <typename T>
class [[nodiscard]] Result {
public:
    static Result Ok(T value) {
        Result r;
        r.value_.emplace(std::move(value));
        return r;
    }
    static Result Fail(Err err) {
        assert(err != Err::kOk && "Fail() 必须携带非 kOk 错误码");
        Result r;
        r.err_ = err;
        return r;
    }

    bool IsOk() const noexcept { return value_.has_value(); }
    Err Error() const noexcept {
        assert(!IsOk());
        return err_;
    }

    /// \brief 解引用取值（docs/04 §3 `auto tbl = *eng.OpenTable(...)`）。
    /// 未 ok 时断言失败（与 Value() 同契约）。
    T& operator*() noexcept {
        assert(IsOk());
        return *value_;
    }
    const T& operator*() const noexcept {
        assert(IsOk());
        return *value_;
    }
    T* operator->() noexcept {
        assert(IsOk());
        return &*value_;
    }
    const T* operator->() const noexcept {
        assert(IsOk());
        return &*value_;
    }

    T& Value() noexcept {
        assert(IsOk());
        return *value_;
    }
    const T& Value() const noexcept {
        assert(IsOk());
        return *value_;
    }
    T TakeValue() noexcept {
        assert(IsOk());
        return std::move(*value_);
    }

private:
    Result() : err_(Err::kOk) {}
    std::optional<T> value_;
    Err err_;
};

/// Result<void> 特化：只表达成败。
template <>
class [[nodiscard]] Result<void> {
public:
    static Result Ok() { return Result(); }
    static Result Fail(Err err) {
        assert(err != Err::kOk);
        Result r;
        r.err_ = err;
        return r;
    }

    bool IsOk() const noexcept { return err_ == Err::kOk; }
    Err Error() const noexcept {
        assert(!IsOk());
        return err_;
    }

private:
    Result() : err_(Err::kOk) {}
    Err err_;
};

/// 便捷检查宏：表达式若失败则以该错误码从当前函数返回。
/// 用法示例：
///   RTDB_TRY_OR_RETURN(auto region, ShmRegion::Open(p));
#define RTDB_TRY_OR_RETURN(decl_target, expr)                         \
    auto decl_target##_rtdb_try_result = (expr);                      \
    if (!(decl_target##_rtdb_try_result).IsOk()) {                    \
        return RTDB_FWD_ERR((decl_target##_rtdb_try_result).Error()); \
    }                                                                 \
    decl_target = (decl_target##_rtdb_try_result).TakeValue();

#define RTDB_FWD_ERR(e) (e)

/// \brief 规范面检查宏（docs/04 §3，M2 新增）：expr 为 Result<T>，失败时
/// 以错误码从当前函数返回。**契约：所在函数须返回 Err**（或经 Err 中转，
/// 如 main 内的静态 Run 函数）；Result<T> 返回函数用显式
/// `return Result<T>::Fail(res.Error())` 或 RTDB_TRY_OR_RETURN。
#define RTDB_CHECK(expr)                             \
    do {                                             \
        const auto rtdb_check_res_ = (expr);         \
        if (!rtdb_check_res_.IsOk())                 \
            return rtdb_check_res_.Error();          \
    } while (0)

/// \brief 规范面取值宏（docs/04 §3 `RTDB_ASSIGN_OR_MOVE(auto eng, ...)`）：
/// expr 为 Result<T>，成功则移动赋值给 decl，失败以错误码返回。
/// 返回契约同 RTDB_CHECK（所在函数返回 Err）。
/// 临时变量名带行号消歧（同一作用域内多条不冲突）。
#define RTDB_CONCAT2_(a, b) a##b
#define RTDB_CONCAT_(a, b) RTDB_CONCAT2_(a, b)
#define RTDB_ASSIGN_OR_MOVE(decl, expr)                             \
    auto RTDB_CONCAT_(rtdb_assign_res_, __LINE__) = (expr);         \
    if (!RTDB_CONCAT_(rtdb_assign_res_, __LINE__).IsOk())           \
        return RTDB_CONCAT_(rtdb_assign_res_, __LINE__).Error();    \
    decl = RTDB_CONCAT_(rtdb_assign_res_, __LINE__).TakeValue();

}  // namespace rtdb

#endif  // RTDB_RESULT_HPP_
