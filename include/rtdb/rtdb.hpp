#ifndef RTDB_RTDB_HPP_
#define RTDB_RTDB_HPP_

/// \file
/// \brief C++ 公共面伞头：一键包含全部原生 API（docs/04 §1"现代面"）。
///
/// 嵌入使用者只需 `#include <rtdb/rtdb.hpp>`；C ABI 面（docs/04 §4）为独立
/// 发行的 C 头 `rtdb/rtdb_cabi.h`，按需单独包含（M2-W3）。
///
/// 内部依赖顺序：version → err → result → span → options → table_def →
/// column_value → table_handle → record_block → engine。

#include "rtdb/column_value.hpp"
#include "rtdb/engine.hpp"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"
#include "rtdb/record_block.hpp"
#include "rtdb/result.hpp"
#include "rtdb/span.hpp"
#include "rtdb/table_def.hpp"
#include "rtdb/table_handle.hpp"
#include "rtdb/version.hpp"

#endif  // RTDB_RTDB_HPP_
