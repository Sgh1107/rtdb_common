#!/usr/bin/env python3
"""coverage_m1.py — M1 行覆盖聚合（docs/06 §4 门禁口径）。

口径与 CI coverage job 一致（gcovr --fail-under-line 的同类统计，仅统计
src/ 内核代码；tests/ 与三方 gtest 不计入）：
  - 全局（src/ 全部）            ≥ 80%
  - engine/ 与 storage/ 子目标   ≥ 85%（src/engine.cpp + src/storage/*）
  - infra/（src/infra + src/platform）单独列出供参考

前置：存在 gcc --coverage 构建（默认 build_cov，由 scripts/coverage_m1.sh
一键配置——必须带 -DRTDB_COVERAGE_STATIC_CORE=ON 与
-DRTDB_STATIC_TEST_DISCOVERY=ON，原因见该脚本头部注释；MinGW 上缺任一
参数将无法取得 src/ 内核覆盖率数据）。本脚本：
  1) 清理旧 .gcda → 2) 跑全量单测 → 3) gcov --json-format 逐 TU 导出 →
  4) 按文件跨 TU 合并（任一 TU 覆盖即计覆盖）→ 5) 输出分组报表与门禁判定。

用法：
  python3 scripts/coverage_m1.py [build_dir] [--fail-under-line 80]
                                   [--fail-under-sub 85]
退出码：0 = 达标；1 = 有门禁未过；2 = 环境错误。
证据：报表同时写入 <build_dir>/coverage_report.txt。
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def find_test_binary(build_dir: Path) -> Path | None:
    for cand in (
        build_dir / "rtdb_unit_tests.exe",
        build_dir / "rtdb_unit_tests",
        build_dir / "bin" / "rtdb_unit_tests",
    ):
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    return None


def run(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    build_dir = Path(args[0]).resolve() if args else (ROOT / "build_cov")
    fail_under_global = 80.0
    fail_under_sub = 85.0
    for a in sys.argv[1:]:
        if a.startswith("--fail-under-line="):
            fail_under_global = float(a.split("=", 1)[1])
        elif a.startswith("--fail-under-sub="):
            fail_under_sub = float(a.split("=", 1)[1])

    if not build_dir.is_dir():
        print(f"coverage_m1: 构建目录不存在：{build_dir}（先配置 --coverage 构建）")
        return 2
    test_bin = find_test_binary(build_dir)
    if test_bin is None:
        print(f"coverage_m1: 找不到 rtdb_unit_tests（{build_dir}）")
        return 2

    # 1) 清理旧计数数据。
    for gcda in build_dir.rglob("*.gcda"):
        gcda.unlink()

    # 2) 跑全量单测（gtest 主入口；子进程模式不影响——无参即 RUN_ALL_TESTS）。
    print(f"coverage_m1: 运行单测 {test_bin}")
    proc = subprocess.run([str(test_bin)], cwd=str(build_dir), capture_output=True, text=True)
    tail = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""
    print(f"coverage_m1: 单测结果 {tail!r} (rc={proc.returncode})")
    if proc.returncode != 0:
        print("coverage_m1: 单测未全绿，覆盖率数据不完整（仍继续统计）")

    # 3) gcov --json-format 逐 TU 导出（每个 .gcda 一份 JSON）。
    gcdas = sorted(build_dir.rglob("*.gcda"))
    if not gcdas:
        print("coverage_m1: 未生成任何 .gcda（确认构建带 --coverage）")
        return 2

    # file -> set(covered_lines) / set(instrumented_lines)，跨 TU 合并。
    covered: dict[str, set[int]] = {}
    instrumented: dict[str, set[int]] = {}
    json_blobs: list[dict] = []
    for gcda in gcdas:
        r = run(["gcov", "--json-format", "--stdout", str(gcda)], cwd=build_dir)
        if r.returncode != 0 or not r.stdout.strip():
            continue
        try:
            json_blobs.append(json.loads(r.stdout))
        except json.JSONDecodeError:
            continue

    for blob in json_blobs:
        for f in blob.get("files", []):
            path = os.path.normpath(f.get("file", ""))
            # 仅统计仓库内 src/ 内核代码（tests/、三方 gtest、系统头不计）。
            # 注意：gtest 源码路径形如 <build>/_deps/googletest-src/googletest/
            # src/gtest.cc，含 "/src/" 会误入全局统计，须先按 _deps 剔除。
            norm = path.replace("\\", "/")
            if "_deps/" in norm:
                continue
            if "/src/" not in norm and not norm.startswith("src/"):
                continue
            key = path.replace("\\", "/")
            for ln in f.get("lines", []):
                n = ln.get("line_number", 0)
                if n <= 0:
                    continue
                instrumented.setdefault(key, set()).add(n)
                if ln.get("count", 0) > 0:
                    covered.setdefault(key, set()).add(n)

    def group_of(rel: str) -> str:
        if rel.startswith("src/storage/"):
            return "storage"
        if rel.startswith("src/engine"):
            return "engine"
        return "infra"

    def pct(cov: set[int], inst: set[int]) -> float:
        return 100.0 * len(cov & inst) / len(inst) if inst else 0.0

    # 4) 分组聚合（按仓库相对路径归并 Windows/Posix 差异）。
    groups: dict[str, tuple[set[int], set[int]]] = {}
    per_file: list[tuple[str, int, int, float]] = []
    for path, inst in sorted(instrumented.items()):
        try:
            rel = str(Path(path).resolve().relative_to(ROOT)).replace("\\", "/")
        except ValueError:
            rel = path
        cov = covered.get(path, set())
        g = group_of(rel)
        gc, gi = groups.get(g, (set(), set()))
        groups[g] = (gc | cov, gi | inst)
        per_file.append((rel, len(cov & inst), len(inst), pct(cov, inst)))

    lines_out: list[str] = []
    lines_out.append("=== M1 行覆盖报告（gcov 口径，仅 src/） ===")
    lines_out.append(f"{'file':<46} {'cov/inst':>11} {'%':>7}")
    for rel, c, i, p in per_file:
        lines_out.append(f"{rel:<46} {c:>5}/{i:<5} {p:>6.1f}%")

    def group_pct(name: str) -> float:
        cov, inst = groups.get(name, (set(), set()))
        return pct(cov, inst)

    all_cov = set().union(*[v[0] for v in groups.values()]) if groups else set()
    all_inst = set().union(*[v[1] for v in groups.values()]) if groups else set()
    global_pct = pct(all_cov, all_inst)
    lines_out.append("---")
    lines_out.append(f"全局（src/ 全部）      : {global_pct:.1f}%  （门禁 ≥ {fail_under_global:.0f}%）")
    lines_out.append(
        f"engine + storage 子目标 : {group_pct('engine'):.1f}% / {group_pct('storage'):.1f}%"
        f"  （门禁 ≥ {fail_under_sub:.0f}%）"
    )
    lines_out.append(f"infra（参考项）         : {group_pct('infra'):.1f}%")

    report = "\n".join(lines_out)
    print(report)
    out = build_dir / "coverage_report.txt"
    out.write_text(report + "\n", encoding="utf-8")
    print(f"coverage_m1: 报表已写入 {out}")

    ok = global_pct >= fail_under_global and min(group_pct("engine"), group_pct("storage")) >= fail_under_sub
    print("coverage_m1: RESULT " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
