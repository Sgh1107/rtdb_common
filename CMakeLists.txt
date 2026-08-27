cmake_minimum_required(VERSION 3.16)
project(rtdb_common VERSION 0.1.0 LANGUAGES CXX)

option(RTDB_BUILD_TESTS "Build unit tests" ON)
option(RTDB_BUILD_EXAMPLES "Build examples" ON)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------
# 核心动态库：rtdbcore
# ---------------------------------------------------------------
add_library(rtdbcore SHARED
  src/engine.cpp
  src/infra/err.cpp
  src/infra/shm_region.cpp
)
if(WIN32)
  target_sources(rtdbcore PRIVATE src/platform/win32/shared_memory_file_win32.cpp)
else()
  target_sources(rtdbcore PRIVATE src/platform/posix/shared_memory_file_posix.cpp)
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
  target_link_libraries(rtdbcore PRIVATE pthread rt)
endif()

# ---------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------
if(RTDB_BUILD_TESTS)
  enable_testing()
  add_executable(rtdb_unit_tests tests/unit/test_m0_smoke.cpp)
  target_link_libraries(rtdb_unit_tests PRIVATE rtdbcore)
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
  add_test(NAME m0_smoke COMMAND rtdb_unit_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
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
