cmake_minimum_required(VERSION 3.16)
project(rtdb_common VERSION 0.1.0 LANGUAGES CXX)

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
# 核心动态库：rtdbcore（零第三方依赖）
# ---------------------------------------------------------------
add_library(rtdbcore SHARED
  src/engine.cpp
  src/infra/err.cpp
  src/infra/shm_region.cpp
  src/infra/slab_allocator.cpp
  src/infra/shm_mutex.cpp
)
if(WIN32)
  target_sources(rtdbcore PRIVATE
    src/platform/win32/shared_memory_file_win32.cpp
    src/platform/win32/platform_process_win32.cpp
  )
else()
  target_sources(rtdbcore PRIVATE
    src/platform/posix/shared_memory_file_posix.cpp
    src/platform/posix/platform_process_posix.cpp
  )
endif()

target_compile_definitions(rtdbcore PRIVATE RTDB_CORE_BUILDING_DLL)
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

  add_executable(rtdb_unit_tests
    tests/unit/test_main.cpp
    tests/unit/spawn_self.cpp
    tests/unit/test_infra_base.cpp
    tests/unit/test_slab_allocator.cpp
    tests/unit/test_shared_btree.cpp
    tests/unit/test_shm_mutex.cpp
  )
  target_link_libraries(rtdb_unit_tests PRIVATE rtdbcore GTest::gtest_main)
  target_include_directories(rtdb_unit_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  if(MSVC)
    target_compile_options(rtdb_unit_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(rtdb_unit_tests PRIVATE -Wall -Wextra)
  endif()
  gtest_discover_tests(rtdb_unit_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

# ---------------------------------------------------------------
# 示例
# ---------------------------------------------------------------
if(RTDB_BUILD_EXAMPLES)
  add_executable(embed_demo examples/embed_demo/main.cpp)
  target_link_libraries(embed_demo PRIVATE rtdbcore)
  if(MSVC)
    target_compile_options(embed_demo PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(embed_demo PRIVATE -Wall -Wextra)
  endif()
  set_target_properties(embed_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
