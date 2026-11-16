#!/usr/bin/env python3
"""Compile actual tile conversion, packet packing and source lifetimes; no emulator."""
import argparse
import json
from pathlib import Path
from tile_host_common import HERE, ROOT, compile_run, prepare, replace_once


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, default=ROOT)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative", choices=("halo", "perspective", "colour", "capacity", "lifetime", "ownership"))
    a = ap.parse_args()
    source, work = a.source_root.resolve(), a.out.resolve()
    hashes = prepare(source, work)
    base = (work / "base_hosttest.c").read_text()
    base = replace_once(base, "void HWR_PS2_LockData(void *data) { (void)data; }",
        "static void *tile_spy_source; static int tile_spy_locks, tile_spy_unlocks, tile_spy_poison; static size_t tile_spy_bytes;\n"
        "void HWR_PS2_LockData(void *data) { if (data == tile_spy_source) tile_spy_locks++; }")
    base = replace_once(base, "void HWR_PS2_UnlockData(void *data) { (void)data; }",
        "void HWR_PS2_UnlockData(void *data) { if (data == tile_spy_source) { tile_spy_unlocks++; "
        "if (tile_spy_poison) memset(data, 0xEE, tile_spy_bytes); } }")
    base = replace_once(base, "static qw_t *pk_alloc(u32 n)\n{",
        "static void (*tile_spy_poll)(void);\nstatic qw_t *pk_alloc(u32 n)\n{\n\tif (tile_spy_poll) tile_spy_poll();")
    base = replace_once(base, "#define CAP_MAX (1 << 21)", "#define CAP_MAX (1 << 24)")
    if "tile_one_core(" in (source / "src/ps2/hw/ps2_hw_tile_draw.inc").read_text():
        base += "\n#define TILE_CORE_FAST_TEST 1\n"
    if "tile_cpu_source_bytes(" in (source / "src/ps2/hw/ps2_hw_tile.inc").read_text():
        base += "\n#define TILE_ALL_SOURCES_TEST 1\n"
    if "tile_fill_columns(" in (source / "src/ps2/hw/ps2_hw_tile.inc").read_text():
        base += "\n#define TILE_COLUMNS_TEST 1\n"
    (work / "base_hosttest.c").write_text(base)
    if a.negative:
        name = "ps2_hw_tile_draw.inc" if a.negative in ("perspective", "colour") else "ps2_hw_tex.inc" if a.negative == "capacity" else "ps2_hw_tile.inc"
        path = work / name
        text = path.read_text()
        if a.negative in ("perspective", "colour"):
            fault = "bad[i].a[0] *= bad[i].w;" if a.negative == "perspective" else "bad[i].a[2] = 0.0f;"
            text = replace_once(text, "emit_piece(tri, 3, &part);",
                "cv_t bad[3]; memcpy(bad, tri, sizeof bad); for (int i = 0; i < 3; i++) { " + fault + " } emit_piece(bad, 3, &part);")
        else:
            old, new = {
                "halo": ("int dx = x - 1;", "int dx = x;"),
                "lifetime": ("if (!--dc_source->busy)\n\t\t\tHWR_PS2_UnlockData(locked);", "HWR_PS2_UnlockData(locked);"),
                "ownership": ("if (!locked || retained)", "if (!locked)"),
                "capacity": ("return tile_source_record(m, psm, clut, baked, holes, shareable, has255, cw, ch);", "return NOREC;"),
            }[a.negative]
            if old not in text:
                raise RuntimeError("Negative-control anchor missing: " + old)
            text = text.replace(old, new)
        path.write_text(text)
    ran = compile_run(source, work, HERE / "tile_stream_hosttest.c", a.sanitize)
    output = ran.stdout + ran.stderr
    passed = ran.returncode == 0 and "TILE COMPLETE" in output
    expected = not a.negative
    matched = passed if expected else ran.returncode == 1 and "TILE FAIL" in output
    matched = matched and "runtime error:" not in output and "ERROR: AddressSanitizer" not in output
    (work / "result.json").write_text(json.dumps({"source_root": str(source), "source_sha256": hashes,
        "sanitize": a.sanitize, "negative": a.negative, "returncode": ran.returncode,
        "expected_pass": expected, "matched_expectation": matched, "emulator_used": False}, indent=2) + "\n")
    return int(not matched)


if __name__ == "__main__":
    raise SystemExit(main())
