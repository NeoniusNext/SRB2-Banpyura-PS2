#!/usr/bin/env python3
"""Actual direct EE capture and tiled copy/wipe packets through a host GS boundary."""
import argparse
import json
from pathlib import Path
from tile_host_common import HERE, ROOT, compile_run, prepare


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, default=ROOT)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative", choices=("copy", "phase", "invalidate", "pin", "view"))
    a = ap.parse_args()
    source, work = a.source_root.resolve(), a.out.resolve()
    hashes = prepare(source, work)
    base = (work / "base_hosttest.c").read_text().split("int main(int argc, char **argv)")[0]
    base = base.replace("#define CAP_MAX (1 << 21)", "#define CAP_MAX (1 << 24)")
    (work / "base_hosttest.c").write_text(base)
    if a.negative:
        path = work / "ps2_hw_screen.inc"
        text = path.read_text()
        old, new = {
            "pin": ("ri = scr_resident(slot);", "ri = scr_resident(slot); if (saved_img != NOREC && H.rec[saved_img].used) H.rec[saved_img].pin = pinned;"),
            "view": ("img_of(saved_tex)", "saved_tex"),
            "copy": ("screen_copy_rect(&tile, 1, 1, x, y, w, h);", "screen_copy_rect(&tile, 0, 0, x, y, w, h);"),
            "phase": ("- (float)c0 + 1.0f;", "- (float)c0;"),
            "invalidate": ("tile_invalidate(); // a same-slot recapture", "/* omitted invalidation */ // a same-slot recapture"),
        }[a.negative]
        if old not in text:
            raise RuntimeError("Negative-control anchor missing: " + old)
        path.write_text(text.replace(old, new))
    ran = compile_run(source, work, HERE / "tile_capture_hosttest.c", a.sanitize)
    output = ran.stdout + ran.stderr
    passed = ran.returncode == 0 and "CAPTURE COMPLETE failed=0" in output
    matched = passed if not a.negative else ran.returncode == 1 and "CAPTURE FAIL" in output
    matched = matched and "runtime error:" not in output and "ERROR: AddressSanitizer" not in output
    (work / "result.json").write_text(json.dumps({"source_root": str(source), "source_sha256": hashes,
        "sanitize": a.sanitize, "negative": a.negative, "returncode": ran.returncode,
        "matched_expectation": matched, "emulator_used": False, "output": output}, indent=2) + "\n")
    return int(not matched)


if __name__ == "__main__":
    raise SystemExit(main())
