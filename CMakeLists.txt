cmake_minimum_required(VERSION 3.16)
project(rtdb_common VERSION 0.1.0 LANGUAGES CXX C)

option(RTDB_BUILD_TESTS "Build unit tests" ON)
# GoogleTest 仅在构建测试时经 FetchContent 拉取（docs/01 技术选型约定），
# 不污染核心库的零第三方依赖特性。离线环境可关掉 RTDB_BUILD_TESTS。
if(RTDB_BUILD_TESTS AND NOT DEFINED RTDB_GTEST_READY)
  option(RTDB_FETCH_GTEST "Fetch GoogleTest at configure time" ON)
endif()
option(RTDB_BUILD_EXAMPLES "Build examples" ON)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------
# 核心库：rtdbcore（零第三方依赖）
# ---------------------------------------------------------------
# 覆盖率构建（MinGW + gcov）特例：DLL 的 gcov 计数仅在 DLL 卸载
# （detach）时转储，而插桩后的 DLL 卸载会触发 UCRT fast-fail
# (0xC0000409)，导致 src/ 下覆盖率数据恒为空。故静态链接进测试
# 可执行（单模块单 gcov 运行时，__gcov_dump 一次拿全量数据）。
option(RTDB_COVERAGE_STATIC_CORE "Static-link core into tests (gcov builds)" OFF)

set(rtdb_core_sources
  src/engine.cpp
  src/api/c_abi.cpp
  src/infra/err.cpp
  src/infra/failpoint.cpp
  src/infra/shm_region.cpp
  src/infra/slab_allocator.cpp
  src/infra/shm_mutex.cpp
  src/storage/catalog.cpp
  src/storage/table_store.cpp
  src/storage/var_pool.cpp
  src/storage/wal_ring.cpp
  src/storage/group_commit.cpp
  src/storage/snapshot.cpp
  src/storage/alter.cpp
  src/storage/sec_index.cpp
)
if(WIN32)
  list(APPEND rtdb_core_sources
    src/platform/win32/shared_memory_file_win32.cpp
    src/platform/win32/platform_process_win32.cpp
  )
else()
  list(APPEND rtdb_core_sources
    src/platform/posix/shared_memory_file_posix.cpp
    src/platform/posix/platform_process_posix.cpp
  )
endif()

if(RTDB_COVERAGE_STATIC_CORE)
  add_library(rtdbcore STATIC ${rtdb_core_sources})
  target_compile_definitions(rtdbcore PUBLIC RTDB_CORE_STATIC)
else()
  add_library(rtdbcore SHARED ${rtdb_core_sources})
  target_compile_definitions(rtdbcore PRIVATE RTDB_CORE_BUILDING_DLL)
endif()
target_compile_features(rtdbcore PUBLIC cxx_std_17)
target_include_directories(rtdbcore
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
  target_compile_options(rtdbcore PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(rtdbcore PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(UNIX)
  # rt 仅 Linux 需要(clock_gettime)；其他 POSIX 静默忽略亦可。
  target_link_libraries(rtdbcore PRIVATE pthread)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(rtdbcore PRIVATE rt)
  endif()
endif()

# ---------------------------------------------------------------
# 单元测试：GoogleTest 驱动
# ---------------------------------------------------------------
if(RTDB_BUILD_TESTS)
  enable_testing()
  if(RTDB_FETCH_GTEST)
    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
      URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
    )
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)  # 提供 gtest_discover_tests
    # 抑制三方源码里的高等级告警噪音。
    if(MSVC)
      foreach(gt gtest gtest_main)
        if(TARGET ${gt})
          target_compile_options(${gt} PRIVATE /W3)
        endif()
      endforeach()
    endif()
  endif()

  set(rtdb_test_sources
    tests/unit/test_main.cpp
    tests/unit/spawn_self.cpp
    tests/unit/test_infra_base.cpp
    tests/unit/test_slab_allocator.cpp
    tests/unit/test_shared_btree.cpp
    tests/unit/test_shm_mutex.cpp
    tests/unit/test_storage_schema.cpp
    tests/unit/test_var_pool.cpp
    tests/unit/test_catalog.cpp
    tests/unit/test_table_store.cpp
    tests/unit/test_wal.cpp
    tests/unit/test_w5.cpp
    tests/unit/test_native_api.cpp
    tests/unit/test_record_block.cpp
    tests/unit/test_concurrency.cpp
    tests/unit/test_cabi.cpp
  )
  add_executable(rtdb_unit_tests ${rtdb_test_sources})
  target_link_libraries(rtdb_unit_tests PRIVATE rtdbcore GTest::gtest_main)
  target_include_directories(rtdb_unit_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  if(MSVC)
    target_compile_options(rtdb_unit_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(rtdb_unit_tests PRIVATE -Wall -Wextra
      # 测试代码大量使用「刻意部分聚合初始化」：ColumnDef 省略的
      # default_value 成员，其默认值（is_null=true）恰是测试所需语义。
      # gcc -Wextra 的 -Wmissing-field-initializers 对此类写法报警
      # （甚至不在 -Wall），属知名噪音；产品库侧不屏蔽、保持全严格。
      -Wno-missing-field-initializers)
  endif()
  # 串行执行：各用例经全局命名资源耦合（映像文件路径 + 命名映射对象名，
  # 后者在 Windows 上按内核对象名全局可见）。并行 ctest 下进程 A 的
  # TearDown 删文件/释映射会与进程 B 的 SetUp/Open 偶发竞争（曾观测到
  # 3/113 间歇失败），故强制 RUN_SERIAL；整套仅数秒，无并行收益损失。
  option(RTDB_STATIC_TEST_DISCOVERY "Static test discovery (gcov builds)" OFF)
  if(RTDB_STATIC_TEST_DISCOVERY)
    # 覆盖率构建（MinGW + gcov）：gtest_discover_tests 的 POST_BUILD 探测
    # 以 --gtest_list_tests 运行二进制，gtest 列完即在 InitGoogleTest 内部
    # exit()，libgcc 退出转储与 DLL 卸载相互作用触发 UCRT
    # fast-fail(0xC0000409)，令构建必败（gcda 实际已落盘）。改用静态扫
    # 源码发现（本仓均为 TEST/TEST_F，无 TEST_P，无遗漏），TEST_LIST 回填
    # RUN_SERIAL（同一全局命名资源竞争原因，见上）。
    gtest_add_tests(TARGET rtdb_unit_tests
      SOURCES ${rtdb_test_sources}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      TEST_LIST rtdb_discovered_tests)
    set_tests_properties(${rtdb_discovered_tests} PROPERTIES RUN_SERIAL TRUE)
  else()
    gtest_discover_tests(rtdb_unit_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

# ---------------------------------------------------------------
# 示例
# ---------------------------------------------------------------
if(RTDB_BUILD_EXAMPLES)
  add_executable(embed_demo examples/embed_demo/main.cpp)
  target_link_libraries(embed_demo PRIVATE rtdbcore)
  # M2-W1：docs/04 §3 Native API 验收程序（伞头 + 规范面便利 API）。
  add_executable(api_demo examples/api_demo/main.cpp)
  target_link_libraries(api_demo PRIVATE rtdbcore)
  # M2-W3：C ABI 验收程序（纯 C，验证跨语言绑定基座）。
  add_executable(cabi_demo examples/cabi_demo/main.c)
  target_link_libraries(cabi_demo PRIVATE rtdbcore)
  set_target_properties(cabi_demo PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON)
  if(MSVC)
    target_compile_options(embed_demo PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(api_demo PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(cabi_demo PRIVATE /W4 /utf-8)
  else()
    target_compile_options(embed_demo PRIVATE -Wall -Wextra)
    target_compile_options(api_demo PRIVATE -Wall -Wextra)
    target_compile_options(cabi_demo PRIVATE -Wall -Wextra)
  endif()
  set_target_properties(embed_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  set_target_properties(api_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  set_target_properties(cabi_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ---------------------------------------------------------------
# 本地诊断可执行（默认不构建；-DRTDB_BUILD_DIAG=ON 启用）
# ---------------------------------------------------------------
option(RTDB_BUILD_DIAG "Build local diagnostic repro" OFF)
if(RTDB_BUILD_DIAG)
  add_executable(dbg_repro examples/dbg_repro/main.cpp)
  target_link_libraries(dbg_repro PRIVATE rtdbcore)
  target_include_directories(dbg_repro PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(MSVC)
    target_compile_options(dbg_repro PRIVATE /W4 /permissive- /utf-8)
  endif()
endif()
