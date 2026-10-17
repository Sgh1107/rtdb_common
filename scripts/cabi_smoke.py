#!/usr/bin/env python3
"""cabi_smoke.py — C ABI 跨语言绑定冒烟（M2-W3，docs/10 §2-M2-D3）。

用 Python 标准库 ctypes 直接加载 librtdbcore，按 include/rtdb/rtdb_cabi.h
声明的符号集走一遍：version → open → create_table → insert → get →
upsert_block → scan → free → close，并断言关键取值。用途：证明 C ABI 作为
"其他语言绑定基座"可用（不依赖任何第三方包，可直接进 CI）。

用法：
    python scripts/cabi_smoke.py [--lib <path>] [--image <path>]

库定位顺序：--lib → 环境变量 RTDB_CORE_LIB → build/Release/rtdbcore.dll
→ build/Debug/rtdbcore.dll → build_gcc/librtdbcore.dll（Linux 为 .so）。
退出码：0 全部通过；1 断言/调用失败；2 环境错误（找不到库等）。
"""

import argparse
import ctypes
import os
import sys
import tempfile

# ---- 与 rtdb_cabi.h 对齐的常量（防止头文件改动后脚本静默漂移）----
RTDB_ABI_VER_EXPECT = 2
RTDB_OK = 0
RTDB_ERR_INVALID_ARGUMENT = 101
RTDB_ERR_ALREADY_EXISTS = 103
RTDB_ERR_NOT_FOUND = 102
RTDB_ERR_TABLE_EXISTS = 302
RTDB_ROLE_STANDALONE = 0
RTDB_FSYNC_PER_SECOND = 1
RTDB_T_INT64 = 4
RTDB_T_FLOAT64 = 6
RTDB_T_UTF8VAR = 8


class RtdbValue(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int32),
        ("is_null", ctypes.c_int32),
        ("int_value", ctypes.c_int64),
        ("real_value", ctypes.c_double),
        ("bytes", ctypes.c_void_p),
        ("bytes_len", ctypes.c_uint32),
    ]


class RtdbColumnDef(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("type", ctypes.c_int32),
        ("len", ctypes.c_uint32),
        ("nullable", ctypes.c_int32),
    ]


class RtdbColumn(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("type", ctypes.c_int32),
        ("data", ctypes.c_void_p),
        ("lengths", ctypes.c_void_p),
        ("null_flags", ctypes.c_void_p),
    ]


class RtdbRow(ctypes.Structure):
    _fields_ = [("vals", ctypes.POINTER(RtdbValue)), ("nvals", ctypes.c_size_t)]


class RtdbRows(ctypes.Structure):
    _fields_ = [("rows", ctypes.POINTER(RtdbRow)), ("nrows", ctypes.c_size_t)]


def find_library(explicit=None):
    if explicit:
        return explicit
    env = os.environ.get("RTDB_CORE_LIB")
    if env:
        return env
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidates = [
        os.path.join(root, "build", "Release", "rtdbcore.dll"),
        os.path.join(root, "build", "Debug", "rtdbcore.dll"),
        os.path.join(root, "build_gcc", "librtdbcore.dll"),
        os.path.join(root, "build", "librtdbcore.so"),
        os.path.join(root, "build_gcc", "librtdbcore.so"),
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return None


def bind(lib):
    fns = {
        "version": (ctypes.c_uint32, []),
        "open": (
            ctypes.c_int32,
            [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32,
             ctypes.POINTER(ctypes.c_void_p)],
        ),
        "close": (None, [ctypes.c_void_p]),
        "committed_seq": (ctypes.c_int64, [ctypes.c_void_p]),
        "create_table": (
            ctypes.c_int32,
            [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(RtdbColumnDef),
             ctypes.c_size_t, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)],
        ),
        "open_table": (ctypes.c_int32, [ctypes.c_void_p, ctypes.c_char_p,
                                        ctypes.POINTER(ctypes.c_void_p)]),
        "drop_table": (ctypes.c_int32, [ctypes.c_void_p, ctypes.c_char_p]),
        "close_table": (None, [ctypes.c_void_p]),
        "insert": (ctypes.c_int32, [ctypes.c_void_p, ctypes.c_void_p,
                                    ctypes.POINTER(RtdbValue), ctypes.c_size_t,
                                    ctypes.POINTER(ctypes.c_uint64)]),
        "get": (ctypes.c_int32, [ctypes.c_void_p, ctypes.c_void_p,
                                 ctypes.POINTER(RtdbValue), ctypes.POINTER(RtdbRow)]),
        "scan": (ctypes.c_int32, [ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.POINTER(RtdbValue), ctypes.POINTER(RtdbValue),
                                  ctypes.POINTER(RtdbRows)]),
        "upsert_block": (ctypes.c_int32, [ctypes.c_void_p, ctypes.c_char_p,
                                          ctypes.POINTER(RtdbColumn), ctypes.c_size_t,
                                          ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64)]),
        "free_row": (None, [ctypes.POINTER(RtdbRow)]),
        "free_rows": (None, [ctypes.POINTER(RtdbRows)]),
    }
    ns = {}
    for name, (restype, argtypes) in fns.items():
        fn = getattr(lib, "rtdb_abi_" + name)
        fn.restype = restype
        fn.argtypes = argtypes
        ns[name] = fn
    return ns


def text_of(value):
    if value.is_null:
        return None
    return ctypes.string_at(value.bytes, value.bytes_len).decode("utf-8")


def make_value(kind, int_v=0, real_v=0.0, text_v=None, keep=None):
    v = RtdbValue()
    v.type = kind
    v.is_null = 0
    if kind == RTDB_T_INT64:
        v.int_value = int_v
    elif kind == RTDB_T_FLOAT64:
        v.real_value = real_v
    elif kind == RTDB_T_UTF8VAR:
        buf = ctypes.create_string_buffer(text_v.encode("utf-8"))
        keep.append(buf)  # 防止被 GC（结构里只存裸指针）
        v.bytes = ctypes.cast(buf, ctypes.c_void_p)
        v.bytes_len = len(text_v.encode("utf-8"))
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", default=None)
    ap.add_argument("--image", default=None)
    args = ap.parse_args()

    lib_path = find_library(args.lib)
    if not lib_path:
        print("FAIL: 找不到 rtdbcore 库（用 --lib 或 RTDB_CORE_LIB 指定）")
        return 2
    image = args.image or os.path.join(tempfile.gettempdir(), "rtdb_cabi_smoke.rtdb")
    for suffix in ("", ".snap"):
        try:
            os.remove(image + suffix)
        except OSError:
            pass

    lib = ctypes.CDLL(lib_path)
    api = bind(lib)
    checks = []

    def check(cond, what):
        checks.append((bool(cond), what))
        print(("PASS " if cond else "FAIL ") + what)

    # C 面 + C++ 面版本一致性（引擎侧 rtdb::kVersionString 不含 ABI，这里只查 C 面）。
    check(api["version"]() == RTDB_ABI_VER_EXPECT, "version == %d" % RTDB_ABI_VER_EXPECT)

    eng = ctypes.c_void_p()
    rc = api["open"](b"cabi_smoke", image.encode(), RTDB_ROLE_STANDALONE,
                     RTDB_FSYNC_PER_SECOND, ctypes.byref(eng))
    check(rc == RTDB_OK and eng.value, "open (rc=%d)" % rc)
    if rc != RTDB_OK:
        return 1

    try:
        cols = (RtdbColumnDef * 3)()
        cols[0] = RtdbColumnDef(b"id", RTDB_T_INT64, 0, 0)
        cols[1] = RtdbColumnDef(b"val", RTDB_T_FLOAT64, 0, 1)
        cols[2] = RtdbColumnDef(b"tag", RTDB_T_UTF8VAR, 0, 1)
        tbl = ctypes.c_void_p()
        rc = api["create_table"](eng, b"t", cols, 3, 0, ctypes.byref(tbl))
        check(rc == RTDB_OK, "create_table (rc=%d)" % rc)

        # 重名建表 → TABLE_EXISTS。
        dup = ctypes.c_void_p()
        rc = api["create_table"](eng, b"t", cols, 3, 0, ctypes.byref(dup))
        check(rc == RTDB_ERR_TABLE_EXISTS, "duplicate create -> TABLE_EXISTS (rc=%d)" % rc)

        # 行式插入（字符串经 create_string_buffer 保活）。
        keep = []
        vals = (RtdbValue * 3)()
        vals[0] = make_value(RTDB_T_INT64, int_v=42)
        vals[1] = make_value(RTDB_T_FLOAT64, real_v=220.5)
        vals[2] = make_value(RTDB_T_UTF8VAR, text_v="py-insert", keep=keep)
        row_no = ctypes.c_uint64()
        rc = api["insert"](eng, tbl, vals, 3, ctypes.byref(row_no))
        check(rc == RTDB_OK, "insert (rc=%d row_no=%d)" % (rc, row_no.value))
        rc = api["insert"](eng, tbl, vals, 3, None)
        check(rc == RTDB_ERR_ALREADY_EXISTS, "duplicate insert -> ALREADY_EXISTS (rc=%d)" % rc)

        # 点查。
        row = RtdbRow()
        key = make_value(RTDB_T_INT64, int_v=42)
        rc = api["get"](eng, tbl, ctypes.byref(key), ctypes.byref(row))
        ok = rc == RTDB_OK and row.nvals == 3
        check(ok, "get id=42 (rc=%d nvals=%d)" % (rc, row.nvals))
        if ok:
            check(row.vals[0].int_value == 42, "  value int == 42")
            check(abs(row.vals[1].real_value - 220.5) < 1e-9, "  value real == 220.5")
            check(text_of(row.vals[2]) == "py-insert", "  value text == 'py-insert'")
        api["free_row"](ctypes.byref(row))
        check(row.vals is None or not row.vals, "free_row resets handle")

        # 缺键点查 → NOT_FOUND。
        missing = make_value(RTDB_T_INT64, int_v=999)
        rc = api["get"](eng, tbl, ctypes.byref(missing), ctypes.byref(row))
        check(rc == RTDB_ERR_NOT_FOUND, "missing key -> NOT_FOUND (rc=%d)" % rc)

        # 列式块 upsert：2 行（含变长长度表与空值掩码）。
        ids = (ctypes.c_int64 * 2)(100, 101)
        real_vals = (ctypes.c_double * 2)(1.5, 2.5)
        tag_bufs = [ctypes.create_string_buffer(b"blk_a"), ctypes.create_string_buffer(b"blk_b")]
        tag_ptrs = (ctypes.c_char_p * 2)(
            ctypes.cast(tag_bufs[0], ctypes.c_char_p),
            ctypes.cast(tag_bufs[1], ctypes.c_char_p),
        )
        tag_lens = (ctypes.c_uint32 * 2)(5, 5)
        nullv = (ctypes.c_uint8 * 2)(0, 1)  # 第 2 行 val 为 NULL
        bcols = (RtdbColumn * 3)()
        bcols[0] = RtdbColumn(b"id", RTDB_T_INT64, ctypes.cast(ids, ctypes.c_void_p), None, None)
        bcols[1] = RtdbColumn(b"val", RTDB_T_FLOAT64, ctypes.cast(real_vals, ctypes.c_void_p),
                              None, ctypes.cast(nullv, ctypes.c_void_p))
        bcols[2] = RtdbColumn(b"tag", RTDB_T_UTF8VAR, ctypes.cast(tag_ptrs, ctypes.c_void_p),
                              ctypes.cast(tag_lens, ctypes.c_void_p), None)
        seq = ctypes.c_uint64()
        rc = api["upsert_block"](eng, b"t", bcols, 3, 2, ctypes.byref(seq))
        check(rc == RTDB_OK and seq.value > 0, "upsert_block 2 rows (rc=%d seq=%d)" % (rc, seq.value))

        rows = RtdbRows()
        rc = api["scan"](eng, tbl, None, None, ctypes.byref(rows))
        check(rc == RTDB_OK and rows.nrows == 3, "scan all -> 3 rows (rc=%d nrows=%d)" % (rc, rows.nrows))
        if rc == RTDB_OK and rows.nrows == 3:
            ids_seen = [rows.rows[i].vals[0].int_value for i in range(rows.nrows)]
            check(ids_seen == [42, 100, 101], "  scan ids == [42, 100, 101]")
            check(rows.rows[2].vals[1].is_null == 1, "  block row 2 val is NULL")
            check(text_of(rows.rows[1].vals[2]) == "blk_a", "  block row 1 tag == 'blk_a'")
        api["free_rows"](ctypes.byref(rows))
        check(not rows.rows, "free_rows resets handle")

        # 有界扫描 [100, 101]。
        lo = make_value(RTDB_T_INT64, int_v=100)
        hi = make_value(RTDB_T_INT64, int_v=101)
        rc = api["scan"](eng, tbl, ctypes.byref(lo), ctypes.byref(hi), ctypes.byref(rows))
        check(rc == RTDB_OK and rows.nrows == 2, "scan [100,101] -> 2 rows (rc=%d nrows=%d)"
              % (rc, rows.nrows))
        api["free_rows"](ctypes.byref(rows))

        check(api["committed_seq"](eng) > 0, "committed_seq > 0")
        api["close_table"](tbl)
    finally:
        api["close"](eng)

    failed = [w for ok, w in checks if not ok]
    print("----")
    print("cabi_smoke: %d checks, %d failed" % (len(checks), len(failed)))
    if failed:
        for w in failed:
            print("  FAILED:", w)
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
