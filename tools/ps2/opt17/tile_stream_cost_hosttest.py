#!/usr/bin/env python3
"""Count actual full-size PostImg packet allocations/uploads with a bounded host recorder.

This executes production classification, source fill and packet generation. It
does not rasterize, measure PS2 frame time, or retain all generated packet bytes.
"""
import argparse
import json
from pathlib import Path
from tile_host_common import HERE, ROOT, compile_run, prepare, replace_once


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, default=ROOT)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--sanitize", action="store_true")
    a = ap.parse_args()
    source, work = a.source_root.resolve(), a.out.resolve()
    hashes = prepare(source, work)
    base = (work / "base_hosttest.c").read_text().split("int main(int argc, char **argv)")[0]
    base = replace_once(base, "#define CAP_MAX (1 << 21)", "#define CAP_MAX (1 << 18)")
    base = replace_once(base, "static qw_t *pk_alloc(u32 n)\n{", "static uint64_t cost_qwords;\nstatic qw_t *pk_alloc(u32 n)\n{\n"
        "cost_qwords += n;\nif (cap_n + n > CAP_MAX - 64) { cap_n = 0; cap_nmarks = 0; H.wr = 0; }\n")
    draw = (source / "src/ps2/hw/ps2_hw_tile_draw.inc").read_text()
    if "tile_core_fast_count(" in draw:
        base += "\n#define TILE_CORE_FAST_COST 1\n"
    if "tile_box_count(" in draw:
        base += "\n#define TILE_BOX_COST 1\n"
    (work / "base_hosttest.c").write_text(base)
    ran = compile_run(source, work, HERE / "tile_stream_cost_hosttest.c", a.sanitize)
    output = ran.stdout + ran.stderr
    passed = ran.returncode == 0 and "TILE COST COMPLETE" in output and "runtime error:" not in output and "ERROR: AddressSanitizer" not in output
    (work / "result.json").write_text(json.dumps({"source_root": str(source), "source_sha256": hashes,
        "sanitize": a.sanitize, "returncode": ran.returncode, "passed": passed, "emulator_used": False,
        "scope": __doc__, "output": output}, indent=2) + "\n")
    return int(not passed)


if __name__ == "__main__":
    raise SystemExit(main())
