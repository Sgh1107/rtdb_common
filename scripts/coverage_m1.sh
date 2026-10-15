#!/usr/bin/env bash
# coverage_m1.sh — M1 覆盖率构建驱动（docs/06 §4 门禁口径）。
#
# 背景（本机 Windows + MSYS2 UCRT64 gcc 实测结论）：
#   gcov 插桩 + MinGW DLL 组合下，DLL 的覆盖率计数仅在 DLL 卸载
#   （detach）时转储，而插桩后的卸载/CRT 退出会触发 UCRT fast-fail
#   (0xC0000409)：进程退出码退化、且 src/ 内核（DLL 内）的 .gcda
#   恒不产生。两个规避参数缺一不可：
#     -DRTDB_COVERAGE_STATIC_CORE=ON   核心静态链入测试 exe（单模块
#       单 gcov 运行时，__gcov_dump 一次拿全量数据；test_main.cpp 的
#       main 在 gcov 构建下主动 dump 后 _Exit 跳过退出清理）；
#     -DRTDB_STATIC_TEST_DISCOVERY=ON  gtest_discover_tests 的
#       POST_BUILD 探测以 --gtest_list_tests 运行二进制，gtest 列完即在
#       InitGoogleTest 内部 exit()，同样触发 fast-fail 令构建必败；改用
#       静态扫源码发现（本仓均为 TEST/TEST_F，无 TEST_P，无遗漏）。
#
# 用法：scripts/coverage_m1.sh [build_dir]   （默认 build_cov）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build_cov}"

# MSBuild/Git Bash 环境坑：https_proxy 等大小写键冲突使 CL.exe 崩溃；
# gcc 链路同样受代理变量干扰，统一清掉。
unset http_proxy https_proxy all_proxy || true

GXX="${GXX:-D:/msys64/ucrt64/bin/g++.exe}"
GTEST_SRC="$ROOT/build_wal/_deps/googletest-src"
if [[ -d "$GTEST_SRC" ]]; then
  GTEST_ARG="-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$GTEST_SRC"
else
  GTEST_ARG=""  # 无本地缓存则走网络拉取（FetchContent）
fi

echo "coverage_m1: 配置 $BUILD_DIR（gcc --coverage + 静态核心 + 静态发现）"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="$GXX" \
  -DCMAKE_CXX_FLAGS="--coverage -O0 -g" \
  -DRTDB_COVERAGE_STATIC_CORE=ON \
  -DRTDB_STATIC_TEST_DISCOVERY=ON \
  $GTEST_ARG

echo "coverage_m1: 构建"
cmake --build "$BUILD_DIR" -j 8

echo "coverage_m1: 聚合（清理 .gcda → 全量单测 → gcov 导出 → 门禁判定）"
python3 "$ROOT/scripts/coverage_m1.py" "$BUILD_DIR"
