#!/usr/bin/env python3
"""Actual complete PlanEnd/imm_prepare accounting, including carried full-size proxies."""
import argparse
import json
from pathlib import Path
from tile_host_common import HERE, ROOT, compile_run, prepare, replace_once


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, default=ROOT)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative", choices=("old-charge", "carried-cap"))
    a = ap.parse_args()
    source, work = a.source_root.resolve(), a.out.resolve()
    hashes = prepare(source, work)
    plan = (source / "src/ps2/hw/ps2_hw_plan.inc").read_text().split("// ---- sprites that draw nothing")[0]
    plan = plan.replace(" tex_want(", " actual_tex_want(").replace(" imm_prepare(", " actual_imm_prepare(")
    if a.negative:
        old = "if (image->used && image->tiled == 1 && !image->nblk)" if a.negative == "old-charge" else "if (k->ps2_full_fr && plan_stamp() - k->ps2_full_fr < PLAN_FULL_KEEP && !plan_blocks(k, 0))"
        plan = replace_once(plan, old, "if (0)")
    (work / "actual-plan.inc").write_text(plan)
    ran = compile_run(source, work, HERE / "tile_planner_hosttest.c", a.sanitize)
    output = ran.stdout + ran.stderr
    passed = ran.returncode == 0 and "PLAN COMPLETE checks=40 failed=0" in output
    matched = passed if not a.negative else ran.returncode == 1 and "PLAN FAIL" in output
    matched = matched and "runtime error:" not in output and "ERROR: AddressSanitizer" not in output
    (work / "result.json").write_text(json.dumps({"source_root": str(source), "source_sha256": hashes,
        "sanitize": a.sanitize, "negative": a.negative, "returncode": ran.returncode,
        "matched_expectation": matched, "emulator_used": False, "output": output}, indent=2) + "\n")
    return int(not matched)


if __name__ == "__main__":
    raise SystemExit(main())
