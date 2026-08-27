#ifndef RTDB_TESTS_SIMPLE_TEST_HARNESS_H_
#define RTDB_TESTS_SIMPLE_TEST_HARNESS_H_

/// M0 阶段的极简测试骨架（无第三方依赖）。M2 按测试规范引入
/// GoogleTest 后本头文件废弃 —— 见 docs/06 §1。

#include <cstdio>
#include <string>

namespace test_harness {

inline int g_failures = 0;
inline int g_checks = 0;

#define RTDB_CHECK(cond)                                                       \
  do {                                                                         \
    ++::test_harness::g_checks;                                                \
    if (!(cond)) {                                                             \
      ++::test_harness::g_failures;                                            \
      std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                          \
  } while (0)

#define RTDB_CHECK_EQ(a, b)                                                    \
  do {                                                                         \
    ++::test_harness::g_checks;                                                \
    if (!((a) == (b))) {                                                       \
      ++::test_harness::g_failures;                                            \
      std::printf("  [FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);   \
    }                                                                          \
  } while (0)

#define RTDB_RUN_CASE(name, fn)                                    \
  do {                                                             \
    const int before = ::test_harness::g_failures;                 \
    std::printf("[RUN ] %s\n", name);                              \
    fn();                                                          \
    if (::test_harness::g_failures == before)                      \
      std::printf("[PASS] %s\n", name);                            \
    else                                                           \
      std::printf("[FAIL] %s\n", name);                            \
  } while (0)

}  // namespace test_harness

#endif  // RTDB_TESTS_SIMPLE_TEST_HARNESS_H_
