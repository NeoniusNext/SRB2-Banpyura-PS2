#!/usr/bin/env python3
"""Actual-source proof of recovery after the existing 64 eviction attempts."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

import p4_opt16_hosttest as P
from plan_scratch_opt16_hosttest import function

ROOT = Path(__file__).resolve().parents[2]
BASELINE_REF = "604cfd150c5240142d8cb5367950b53d987916d1"
RECOVERY = '''	// The final victim may have made room; small oldest-first victims may
	// also leave only scattered holes. Finish one existing safe window.
	b = hi ? vram_alloc_hi(n, align) : vram_alloc(n, align);
	if (b)
		return b;
	w = best_window(n, align);
	if (w == NOWIN)
		return 0;
	for (e = w; e < w + n;)
	{
		u16 o = blk_owner[e];

		if (!o)
		{
			e++;
			continue;
		}
		e = H.rec[o - 1].blk + H.rec[o - 1].nblk;
		tex_drop(o - 1, 1);
	}
	return hi ? vram_alloc_hi(n, align) : vram_alloc(n, align);
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def candidate(text, baseline_text):
    current = function(text, "vram_alloc_evicting")
    old = function(baseline_text, "vram_alloc_evicting")
    tail = "\treturn 0;\n}\n"
    if not old.endswith(tail):
        raise RuntimeError("Allocator tail anchor changed")
    new = old[:-len(tail)] + RECOVERY + "}\n"
    if not new.startswith(old[:-len(tail)]):
        raise RuntimeError("Original 64-attempt policy changed")
    if current not in (old, new):
        raise RuntimeError("Allocator differs from the literal pinned baseline plus the exact reviewed recovery tail")
    return text.replace(current, new, 1), old, new, current


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", type=Path, default=ROOT / "src")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--baseline-src", type=Path, help="Pinned baseline src; default exports complete 604cfd source into the fresh output")
    ap.add_argument("--planner-proof", type=Path, default=ROOT / "docs/research/opt16/PLAN_SCRATCH_HOST_RESULTS_20261116.json")
    a = ap.parse_args(); source = a.src.resolve(); out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    if a.baseline_src:
        baseline = a.baseline_src.resolve()
    else:
        raw = subprocess.check_output(["git", "archive", BASELINE_REF, "src"], cwd=ROOT)
        with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
            archive.extractall(out / "reference-source", filter="data")
        baseline = out / "reference-source/src"
    proof = a.planner_proof.resolve()
    texture = (source / "ps2/hw/ps2_hw_tex.inc").read_text()
    changed, old_fn, new_fn, current_fn = candidate(texture, (baseline / "ps2/hw/ps2_hw_tex.inc").read_text())
    frozen = out / "source"; shutil.copytree(source, frozen / "src")
    (frozen / "src/ps2/hw/ps2_hw_tex.inc").write_text(changed)
    runs = {}; modes = ["old", "fixed"] + (["no-recovery", "unsafe-window"] if a.negative_controls else [])
    for mode in modes:
        work = out / mode
        P.prepare(work, source, baseline, 0, proof)
        registers = work / "texture-registers.inc"
        registers.write_text(registers.read_text() + function((source / "ps2/hw/ps2_hw_draw.inc").read_text(), "lru_touch"))
        body = new_fn if mode in ("fixed", "unsafe-window") else old_fn
        if mode == "unsafe-window":
            pos = body.rfind("\tw = best_window(n, align);")
            if pos < 0: raise RuntimeError("Missing recovery window mutation")
            body = body[:pos] + body[pos:].replace("w = best_window(n, align);", "w = (H.pool_base + align - 1) & ~(align - 1);", 1)
        pos = len(old_fn) - len("\treturn 0;\n}\n")
        body = body[:pos] + "\talloc_snapshot();\n" + body[pos:]
        (work / "ps2_hw_tex.inc").write_text(texture.replace(current_fn, body, 1))
        shutil.copyfile(ROOT / "tools/ps2/p4_opt16_hosttest.c", work / "production-fixture.inc")
        shutil.copyfile(ROOT / "tools/ps2/vram_recovery_opt16_hosttest.c", work / "fixture.c")
        modes_out = {}
        for sanitize in (False, True):
            if mode in ("no-recovery", "unsafe-window") and sanitize: continue
            name = "sanitize" if sanitize else "plain"
            flags = ["-std=gnu17", "-O2", "-fwrapv", "-ffp-contract=off", "-ffunction-sections", "-fdata-sections", "-DPS2", "-DPS2_PROFILE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-but-set-variable", "-I" + str(work), "-DRECOVERY_EXPECTED=" + str(int(mode != "old"))]
            if sanitize: flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
            command = ["gcc", *flags, str(work / "fixture.c"), "-Wl,--gc-sections", "-lm", "-o", str(work / name)]
            built = subprocess.run(command, capture_output=True, text=True)
            (work / (name + "-build.log")).write_text(" ".join(command) + "\n" + built.stdout + built.stderr)
            if built.returncode: raise RuntimeError("Build failed: " + str(work / (name + "-build.log")))
            ran = subprocess.run([str(work / name)], capture_output=True, text=True)
            (work / (name + ".log")).write_text(ran.stdout + ran.stderr)
            if mode in ("no-recovery", "unsafe-window"):
                if ran.returncode != 1 or "VRAM recovery proof failed:" not in ran.stderr: raise RuntimeError("Mutation escaped")
                modes_out[name] = {"executed_failure": ran.stderr.strip()}
            else:
                if ran.returncode or "runtime error:" in ran.stderr: raise RuntimeError("Proof failed: " + str(work / (name + ".log")))
                modes_out[name] = json.loads(ran.stdout)
        runs[mode] = modes_out
    for mode in ("old", "fixed"):
        if runs[mode]["plain"] != runs[mode]["sanitize"]: raise RuntimeError("Sanitizer behavior differs")
    for old, new in zip(runs["old"]["plain"]["cases"], runs["fixed"]["plain"]["cases"]):
        if old["name"] != new["name"] or old["prefix_hash"] != new["prefix_hash"]: raise RuntimeError("Original 64 attempts differ")
    report = {"source_texture_sha256": hashlib.sha256(texture.encode()).hexdigest(), "candidate_texture_sha256": hashlib.sha256(changed.encode()).hexdigest(), "baseline_source": str(baseline), "baseline_texture_sha256": sha(baseline / "ps2/hw/ps2_hw_tex.inc"), "literal_pinned_baseline_allocator_prefix_matches": True, "source_already_contains_reviewed_recovery": current_fn == new_fn, "original_64_attempt_source_prefix_unchanged": True, "actual_64_attempt_states_equal": True, "runs": runs, "candidate_source": str(frozen), "production_written": False}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(out / "report.json"), "candidate_source": str(frozen), "runs": {k:v["plain"] for k,v in runs.items()}}, indent=2))


if __name__ == "__main__": main()
