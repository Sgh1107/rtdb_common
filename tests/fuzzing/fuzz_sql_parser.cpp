/// \file
/// \brief SQL 解析器 libFuzzer 目标（docs/16 §5/D4；默认不构建）。
///
/// 构建（Clang/GCC，Linux）：`cmake -DRTDB_BUILD_FUZZER=ON ...` 后运行
/// `fuzz_sql_parser -max_total_time=300`。目标只调用纯解析器（无 IO、无
/// 共享内存），崩溃/断言即解析器缺陷；子集外输入应返回 kUnsupported 而非
/// 崩溃。MSVC 无 libFuzzer，Windows 本地以单测覆盖。

#include <cstddef>
#include <cstdint>
#include <string>

#include "rtdb/sql.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view sql(reinterpret_cast<const char*>(data), size);
    rtdb::sql::Statement st;
    std::string err;
    (void)rtdb::sql::Parse(sql, &st, &err);
    return 0;
}
