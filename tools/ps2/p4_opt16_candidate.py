#!/usr/bin/env python3
"""Replay the retained P4 prototype onto an isolated, pinned current source tree.

It never writes production src/. The default export bounds packing to explicit
full-size mode; exporting a tree is not a substitute for its recorded checks.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
PROTOTYPE_REF = "85bd9716b1ed114d46f95831c64a87623038eeaa"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(args, cwd=ROOT):
    return subprocess.run(list(map(str, args)), cwd=cwd, check=True,
                          capture_output=True)


def export(ref, dest):
    archive = command(["git", "archive", ref, "src"]).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        for member in stream.getmembers():
            if member.name.startswith("/") or ".." in Path(member.name).parts:
                raise RuntimeError("Unsafe source archive path")
        stream.extractall(dest, filter="data")


def manifest(source):
    return {str(p.relative_to(source)): sha(p)
            for p in sorted((source / "src").rglob("*")) if p.is_file()}


def replace(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise RuntimeError("Missing or ambiguous bounded-mode anchor: " + old)
    path.write_text(text.replace(old, new, 1))


def bound_mode(baseline, candidate):
    hw = candidate / "src/ps2/hw"
    replace(hw / "ps2_hw_tex.inc", """	// Only immutable P8 map/flat sources with the ordinary wrap policy. Other
	// formats, debug/cap/budget modes, split/oversized and illegal RRW widths
	// keep the complete preceding T8 path. The selected level is still planned
	// as T8, so packing does not introduce a new LOD or fidelity policy.
""", """	// Exact packing is confined to the explicitly selected full-size mode.
	// Its frame/immediate planners are off and no cap or budget feeds back
	// into stored levels. Only a complete original P8 source can enter here:
	// DMA retirement cannot select a different non-associative mip source.
""")
    replace(hw / "ps2_hw_tex.inc",
            "&& !H.cap_adapt && !H.tex_cap_blocks && !cfg.tex_max_bytes && !ps2hwd_dbg_flags)",
            "&& !H.cap_adapt && !H.tex_cap_blocks && !cfg.tex_max_bytes\n"
            "\t\t&& ps2hwd_dbg_flags == HWDBG_NOPLAN && !lv && !lvdec && !dsrc)")
    replace(hw / "ps2_hw_tex.inc",
            "can_p4 = p4_band(data, lvdec ? (sw >> lv) * (sh >> lv) : dsrc ? ssw * ssh : n, &csa);",
            "can_p4 = p4_band(data, n, &csa);")
    replace(hw / "ps2_hw_tex.inc", "if (can_p4 && !split && !(W2 & 7u)",
            "if (can_p4 && !split && !dx && !dy && W2 == sw && H2 == sh && !(W2 & 7u)")
    # P4 never participates in the active planner; keep the old exact function.
    original = (baseline / "src/ps2/hw/ps2_hw_plan.inc").read_text()
    first, after = "static int plan_too_fine(", "// ---- immediate mode"
    old = (hw / "ps2_hw_plan.inc").read_text()
    a, b = old.index(first), old.index(after, old.index(first))
    x, y = original.index(first), original.index(after, original.index(first))
    (hw / "ps2_hw_plan.inc").write_text(old[:a] + original[x:y] + old[b:])
    replace(hw / "ps2_hw_plan.inc", "\t*visible = 1;\n\tif (imm_level >= 0)",
            "\t*visible = 1;\n\tif (ps2hwd_dbg_flags & HWDBG_NOPLAN)\n"
            "\t\treturn 0; // full-size mode also rejects any stale same-frame hint\n"
            "\tif (imm_level >= 0)")
    replace(hw / "ps2_hwd.c", """void PS2HWD_SetTrace(int frame, int dbg_flags)
{
	trace_frame = frame;
""", """void PS2HWD_SetTrace(int frame, int dbg_flags)
{
#ifndef PS2_NOOPT_P4_BAND
	if (H.up && ((ps2hwd_dbg_flags ^ dbg_flags) & HWDBG_NOPLAN))
	{
		int i;

		// Consume every old draw/reference before changing its quality mode.
		ov_flush_all();
		dma_fence();
		H.imm_tex = NULL;
		for (i = 0; i < plan_n; i++)
			if (plan_info[i].key)
				plan_info[i].key->ps2_planfr = 0;
		plan_reset();
		// A newly selected quality mode starts with the initialization policy,
		// not reserve/thrash history measured with the preceding format set.
		H.plan_reserve = H.unplanned_cur = H.plan_last = H.plan_exact = 0;
		H.plan_pct = H.plan_calm = H.thrash_n = H.thrash_blk = 0;
		for (i = 0; i < H.rec_n; i++)
			if (H.rec[i].used)
			{
				H.rec[i].imgws = 0; // existing T8 images may recharge the new mode's reserve this frame
				if (H.rec[i].psm == PSM_T4)
					tex_drop(img_of(i), 0); // owners and every plain/keyed view
			}
	}
#endif
	trace_frame = frame;
""")
    replace(hw / "ps2_hwd.c", "\tplan_nofid = 0;\n",
            "\tif (M_CheckParm(\"-hwfulltex\"))\n\t{\n"
            "\t\tps2hwd_dbg_flags |= HWDBG_NOPLAN; // explicit full-size mode; default planning stays on\n"
            "\t\tCONS_Printf(\"HWD full-size textures selected (-hwfulltex = -hwdbg 33554432)\\n\");\n\t}\n"
            "\tplan_nofid = 0;\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ref", default="604cfd150c5240142d8cb5367950b53d987916d1",
                    help="Pre-P4 source to replay; pinned because HEAD already contains the adopted change")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--unbounded-prototype", action="store_true",
                    help="Retain rejected prototype eligibility for isolated comparison")
    a = ap.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / "build") or out.exists():
        ap.error("--out must be a fresh child of build/")
    out.mkdir(parents=True)
    ref = command(["git", "rev-parse", a.ref]).stdout.decode().strip()
    baseline, candidate = out / "baseline", out / "candidate"
    baseline.mkdir()
    export(ref, baseline)
    shutil.copytree(baseline, candidate)
    original, prototype = out / "prototype-base", out / "prototype"
    original.mkdir()
    export(PROTOTYPE_REF, original)
    shutil.copytree(original, prototype)
    patch = ROOT / "tools/ps2/next12_p4_candidate.patch"
    result = command(["patch", "--batch", "-p1", "-i", patch], prototype)
    (out / "prototype-patch.log").write_bytes(result.stdout + result.stderr)
    changes = []
    for old in sorted((original / "src").rglob("*")):
        if not old.is_file():
            continue
        relative = old.relative_to(original)
        proposed = prototype / relative
        if old.read_bytes() == proposed.read_bytes():
            continue
        current = candidate / relative
        merge = command(["git", "merge-file", "-p", current, old, proposed])
        current.write_bytes(merge.stdout)
        changes.append(str(relative))
    if not a.unbounded_prototype:
        bound_mode(baseline, candidate)
    # Preserve the official fixture/build tools from the same current commit.
    tools_archive = command(["git", "archive", ref, "tools/ps2"]).stdout
    for dest in (baseline, candidate):
        with tarfile.open(fileobj=io.BytesIO(tools_archive)) as stream:
            stream.extractall(dest, filter="data")
    for name, tree in (("baseline", baseline), ("candidate", candidate)):
        (out / (name + "-manifest.json")).write_text(
            json.dumps(manifest(tree), indent=2) + "\n")
    patch_result = subprocess.run(["diff", "-ruN", baseline / "src", candidate / "src"],
                                  capture_output=True)
    if patch_result.returncode != 1:
        raise RuntimeError("Expected a nonempty candidate source diff")
    (out / "adapted.patch").write_bytes(patch_result.stdout)
    report = {"source_ref": ref, "prototype_ref": PROTOTYPE_REF,
              "prototype_patch_sha256": sha(patch), "changed": changes,
              "candidate_patch_sha256": sha(out / "adapted.patch"),
              "production_src_written": False,
              "bounded_full_size_mode": not a.unbounded_prototype,
              "adoption": "Pending native proof; default planner mode remains T8."}
    (out / "prepare-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
