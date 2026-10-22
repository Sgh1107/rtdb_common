cmake_minimum_required(VERSION 3.16)
project(rtdb_common VERSION 0.1.0 LANGUAGES CXX C)

option(RTDB_BUILD_TESTS "Build unit tests" ON)
# GoogleTest 仅在构建测试时经 FetchContent 拉取（docs/01 技术选型约定），
# 不污染核心库的零第三方依赖特性。离线环境可关掉 RTDB_BUILD_TESTS。
if(RTDB_BUILD_TESTS AND NOT DEFINED RTDB_GTEST_READY)
  option(RTDB_FETCH_GTEST "Fetch GoogleTest at configure time" ON)
endif()
option(RTDB_BUILD_EXAMPLES "Build examples" ON)
option(RTDB_BUILD_TOOLS "Build tools (bench_net 等)" ON)

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
  src/infra/sha256.cpp
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
    src/platform/win32/ipc_wait_win32.cpp
  )
else()
  list(APPEND rtdb_core_sources
    src/platform/posix/shared_memory_file_posix.cpp
    src/platform/posix/platform_process_posix.cpp
    src/platform/posix/ipc_wait_posix.cpp
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
# 网络层：rtdbnet（协议帧/值编码 + TCP 传输；M3，docs/13 §4）
# rtdbd 与 rtdb_client 共用；引擎及核心库不引用本层。
# ---------------------------------------------------------------
set(rtdb_net_sources
  src/net/wire.cpp
  src/net/value_codec.cpp
)
if(WIN32)
  list(APPEND rtdb_net_sources src/platform/win32/tcp_win32.cpp)
else()
  list(APPEND rtdb_net_sources src/platform/posix/tcp_posix.cpp)
endif()
add_library(rtdbnet STATIC ${rtdb_net_sources})
# 该静态库会被链入共享库 librtdbclient.so（Linux 默认 SHARED）——静态库须
# 带 PIC，否则链接期报 relocation R_X86_64_PC32 ... recompile with -fPIC
# （M3 CI 首跑暴露；Windows/MSVC 无此概念，该属性为无害默认）。
set_target_properties(rtdbnet PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(rtdbnet PUBLIC cxx_std_17)
target_include_directories(rtdbnet
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rtdbnet PUBLIC rtdbcore)
if(WIN32)
  target_link_libraries(rtdbnet PUBLIC ws2_32)
endif()
if(UNIX)
  target_link_libraries(rtdbnet PUBLIC pthread)
endif()
if(MSVC)
  target_compile_options(rtdbnet PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(rtdbnet PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------
# 服务端：rtdbserver（rtdbd 核心，M3-W3，docs/13 §5）
# 静态库：同进程集成测试直接持 Server 对象；rtdbd 可执行仅入口。
# ---------------------------------------------------------------
add_library(rtdbserver STATIC
  src/server/config.cpp
  src/server/server.cpp
)
target_compile_features(rtdbserver PUBLIC cxx_std_17)
target_include_directories(rtdbserver
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rtdbserver PUBLIC rtdbnet rtdbcore)
if(MSVC)
  target_compile_options(rtdbserver PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(rtdbserver PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(rtdbd src/server/main.cpp)
target_include_directories(rtdbd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rtdbd PRIVATE rtdbserver)
if(MSVC)
  target_compile_options(rtdbd PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(rtdbd PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------
# 客户端：rtdb_client（远程客户端库，M3-W4，docs/13 §5）
# 复用 rtdbnet（帧/值编码 + TCP）；测试与服务端进程内配对。
# ---------------------------------------------------------------
set(rtdb_client_sources
  src/client/rtdb_client.cpp
  src/client/client_c_abi.cpp
)
if(RTDB_COVERAGE_STATIC_CORE)
  add_library(rtdbclient STATIC ${rtdb_client_sources})
  target_compile_definitions(rtdbclient PUBLIC RTDB_CLIENT_STATIC)
else()
  add_library(rtdbclient SHARED ${rtdb_client_sources})
  target_compile_definitions(rtdbclient PRIVATE RTDB_CLIENT_BUILDING_DLL)
endif()
target_compile_features(rtdbclient PUBLIC cxx_std_17)
target_include_directories(rtdbclient
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rtdbclient PUBLIC rtdbnet rtdbcore)
if(MSVC)
  target_compile_options(rtdbclient PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(rtdbclient PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------
# 单元测试：GoogleTest 驱动
# ---------------------------------------------------------------
if(RTDB_BUILD_TESTS)
  enable_testing()
  if(DEFINED RTDB_GTEST_ROOT)
    # 离线构建：使用本地预编译 GoogleTest（CACHE 变量指向包含
    # gtest/gtest.h 与 gtest.lib 的包根目录），避免配置期访问 GitHub。
    # 目录约定：<root>/include/gtest/gtest.h、<root>/lib/gtest.lib。
    include(GoogleTest)
    get_filename_component(RTDB_GTEST_ROOT "${RTDB_GTEST_ROOT}" ABSOLUTE
      BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    add_library(GTest::gtest STATIC IMPORTED)
    add_library(GTest::gtest_main STATIC IMPORTED)
    set_target_properties(GTest::gtest PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${RTDB_GTEST_ROOT}/include"
      IMPORTED_LOCATION "${RTDB_GTEST_ROOT}/lib/gtest.lib")
    set_target_properties(GTest::gtest_main PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${RTDB_GTEST_ROOT}/include"
      INTERFACE_LINK_LIBRARIES GTest::gtest
      IMPORTED_LOCATION "${RTDB_GTEST_ROOT}/lib/gtest_main.lib")
  elseif(RTDB_FETCH_GTEST)
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
    tests/unit/test_subscribe.cpp
    tests/unit/test_native_api.cpp
    tests/unit/test_record_block.cpp
    tests/unit/test_concurrency.cpp
    tests/unit/test_cabi.cpp
    tests/unit/test_proto.cpp
    tests/unit/test_net.cpp
    tests/unit/test_ipc_wait.cpp
    tests/unit/test_server.cpp
    tests/unit/test_client.cpp
    tests/unit/test_chaos_net.cpp
  )
  add_executable(rtdb_unit_tests ${rtdb_test_sources})
  target_link_libraries(rtdb_unit_tests PRIVATE rtdbcore rtdbnet rtdbserver rtdbclient GTest::gtest_main)
  # rtdbd 拉起烟测（test_server）：把可执行路径编进测试（生成器表达式，
  # 单/多配置构建都指向真实产物）。
  target_compile_definitions(rtdb_unit_tests PRIVATE
    RTDBD_EXE_PATH="$<TARGET_FILE:rtdbd>"
  )
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
  # M3-W4：双端远程演示（--serve 在进程内起服务端；默认客户端订阅打印）。
  add_executable(remote_demo examples/remote_demo/main.cpp)
  target_link_libraries(remote_demo PRIVATE rtdbclient rtdbserver)
  target_include_directories(remote_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(MSVC)
    target_compile_options(embed_demo PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(api_demo PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(cabi_demo PRIVATE /W4 /utf-8)
    target_compile_options(remote_demo PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(embed_demo PRIVATE -Wall -Wextra)
    target_compile_options(api_demo PRIVATE -Wall -Wextra)
    target_compile_options(cabi_demo PRIVATE -Wall -Wextra)
    target_compile_options(remote_demo PRIVATE -Wall -Wextra)
  endif()
  set_target_properties(embed_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  set_target_properties(api_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  set_target_properties(cabi_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  set_target_properties(remote_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  if(WIN32 AND NOT RTDB_COVERAGE_STATIC_CORE)
    # Windows 无 RPATH：示例统一落在 bin/<cfg>，而 rtdbcore/rtdbclient 是
    # DLL（默认落 <cfg> 顶层）——不拷贝则示例启动即 0xC0000135（W5 双机
    # 演示实测）。与 bench_net "输出留在默认层"口径互补。
    foreach(demo_tgt embed_demo api_demo cabi_demo remote_demo)
      add_custom_command(TARGET ${demo_tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rtdbcore> $<TARGET_FILE:rtdbclient>
                $<TARGET_FILE_DIR:${demo_tgt}>)
    endforeach()
  endif()
endif()

# ---------------------------------------------------------------
# 工具：网络基准（M3-W5；docs/13 §5-W5①）
# ---------------------------------------------------------------
if(RTDB_BUILD_TOOLS)
  add_executable(bench_net tools/bench_net/main.cpp)
  target_link_libraries(bench_net PRIVATE rtdbclient rtdbserver)
  target_include_directories(bench_net PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(MSVC)
    target_compile_options(bench_net PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(bench_net PRIVATE -Wall -Wextra)
  endif()
  # 输出留在默认目录（与 rtdbclient/rtdbd 同层）：Windows 下 DLL 与
  # 可执行同目录才可被 ctest 直接拉起（无需 PATH 注入）。
  if(RTDB_BUILD_TESTS)
    # CI 下限断言（回环 ≥50k 行/s、单条写/PING P99 ≤5ms 量级；消毒构建
    # 经环境变量、Debug 构建（覆盖率作业/本地 gcov）经 --relaxed 自动
    # 放宽，见 tools/bench_net/main.cpp）。规模由 --ci 收敛。
    add_test(NAME BenchNetCi COMMAND $<TARGET_FILE:bench_net> --ci
      $<$<CONFIG:Debug>:--relaxed>)
    set_tests_properties(BenchNetCi PROPERTIES RUN_SERIAL TRUE
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  endif()
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
