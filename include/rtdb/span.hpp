#ifndef RTDB_SPAN_HPP_
#define RTDB_SPAN_HPP_

/// \file
/// \brief 轻量只读跨度视图（C++17 无 `std::span`，替代 docs/04 的 span 表述）。
///
/// 零拷贝、不持有所有权；仅用于公共 API 入参。调用方须保证被引用内存在
/// API 调用期间有效。可从裸指针+长度、C 数组、`std::vector` 隐式构造。

#include <cstddef>
#include <type_traits>
#include <vector>

namespace rtdb {

template <typename T>
class Span {
public:
    Span() noexcept = default;
    Span(const T* data, size_t size) noexcept : data_(data), size_(size) {}

    /// C 数组（含结尾静态数组常量）。
    template <size_t N>
    Span(const T (&arr)[N]) noexcept : data_(arr), size_(N) {}

    /// 容器（vector 等）：隐式取 data/size，不拷贝。
    Span(const std::vector<std::remove_const_t<T>>& v) noexcept
        : data_(v.data()), size_(v.size()) {}

    /// 禁止绑定临时容器（否则 Span 持有已析构缓冲的悬垂指针）。
    Span(std::vector<std::remove_const_t<T>>&&) = delete;

    const T* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    const T& operator[](size_t i) const noexcept { return data_[i]; }
    const T* begin() const noexcept { return data_; }
    const T* end() const noexcept { return data_ + size_; }

private:
    const T* data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace rtdb

#endif  // RTDB_SPAN_HPP_
