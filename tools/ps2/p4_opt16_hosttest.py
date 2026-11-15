#!/usr/bin/env python3
"""Read and exercise complete P4 uploads, mode transitions and policy rejection.

--src may select an isolated snapshot or production source. Sources are never
written. DMA/engine services are host boundaries; optional EE compilation checks
the private record ABI. No emulator is launched by this runner.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import hw_hosttest
import texture_record_hosttest
import plan_scratch_opt16_hosttest

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def target_layout(work, source, baseline_source, compiler):
    results = {}
    original_members = texture_record_hosttest.MEMBERS[:]
    for tag, tree in (("baseline", baseline_source), ("candidate", source)):
        target = work / ("layout-" + tag)
        target.mkdir()
        hw_hosttest.ROOT = tree.parent
        hw_hosttest.make_shim(target)
        (target / "hw_model.h").write_text((tree / "hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
        for name in ("ps2_hw_priv.inc", "ps2_hwd_dbg.h"):
            (target / name).write_text((tree / "ps2/hw" / name).read_text().replace('#include "../../doomtype.h"', '#include "shim.h"'))
        try:
            if tag == "candidate":
                texture_record_hosttest.MEMBERS = ["csa" if name == "pad2" else name for name in original_members]
            results[tag] = texture_record_hosttest.layout(target, compiler, True)
        finally:
            texture_record_hosttest.MEMBERS = original_members
    before, after = results.values()
    members = after["members"].copy()
    members["pad2"] = members.pop("csa")
    if before["sizeof"] != after["sizeof"] or before["alignment"] != after["alignment"] or before["members"] != members:
        raise RuntimeError("P4 changes the target record ABI")
    return {"variants": results, "all_previous_member_offsets_and_widths_preserved": True,
            "record_payload_growth_bytes": 0, "compiler": str(compiler),
            "compiler_sha256": sha(compiler)}


def section(text, first, after):
    if text.count(first) != 1:
        raise RuntimeError("Missing or ambiguous source anchor: " + first)
    start = text.index(first)
    return text[start:text.index(after, start)]


def prepare(work, source, baseline_source, mutation, planner_proof):
    work.mkdir(parents=True, exist_ok=True)
    hw_hosttest.ROOT = source.parent
    hw_hosttest.make_shim(work)
    (work / "hw_model.h").write_text((source / "hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
    inputs = {}
    for name in ("doomtype.h", "hardware/hw_defs.h", "hardware/hw_data.h", "hardware/hw_drv.h", "hardware/hw_model.h"):
        inputs[name] = sha(source / name)
    for name in ("ps2_hw_priv.inc", "ps2_hw_regs.inc", "ps2_hw_xform.inc", "ps2_hw_pal.inc", "ps2_hw_tex.inc", "ps2_hwd_dbg.h", "ps2_hw_fx2.h"):
        path = source / "ps2/hw" / name
        inputs[str(path.relative_to(source))] = sha(path)
        text = path.read_text().replace('#include "../../doomtype.h"', '#include "shim.h"')
        if name == "ps2_hw_tex.inc" and mutation in (1, 2, 3, 6):
            old, new = {
                1: ("(src[src_col(c, x)] & 15u)", "(src[src_col(c, x)] & 14u)"),
                2: ("lastpage = (h >> 7) * ((tbw + 1) >> 1) + (w >> 7);", "lastpage = (h >> 6) * ((tbw + 1) >> 1) + (w >> 7);"),
                3: ("rowbytes = W2 >> 1;", "rowbytes = W2;"),
                6: ("for (i = 1; i < n; i++)\n\t\tif ((src[i] & 0xF0u) != high)", "for (i = 1; i + 1 < n; i++)\n\t\tif ((src[i] & 0xF0u) != high)"),
            }[mutation]
            if mutation == 6 and old not in text:
                old = "for (; i < n; i++)\n\t\tif ((src[i] & 0xF0u) != high)"
                new = "for (; i + 1 < n; i++)\n\t\tif ((src[i] & 0xF0u) != high)"
            if text.count(old) != 1:
                raise RuntimeError("Missing exact source mutation: " + old)
            text = text.replace(old, new, 1)
        (work / name).write_text(text)
    draw = (source / "ps2/hw/ps2_hw_draw.inc").read_text()
    inputs["ps2/hw/ps2_hw_draw.inc"] = sha(source / "ps2/hw/ps2_hw_draw.inc")
    registers = section(draw, "static void tex_regs(", "static void lru_touch(")
    if mutation in (4, 5):
        old, new = {
            4: ("((u64)PSM_T8 << 20) | ((u64)cbp << 37) | (1ull << 61)", "((u64)PSM_T4 << 20) | ((u64)cbp << 37) | (1ull << 61)"),
            5: ("t0 |= (u64)r->csa << 56;", "t0 |= 0;"),
        }[mutation]
        if registers.count(old) != 1:
            raise RuntimeError("Missing exact register mutation: " + old)
        registers = registers.replace(old, new, 1)
    (work / "texture-registers.inc").write_text(registers)
    plan_path = source / "ps2/hw/ps2_hw_plan.inc"
    plan = plan_path.read_text()
    inputs["ps2/hw/ps2_hw_plan.inc"] = sha(plan_path)
    policy = section(plan, "static int plan_active;", "// ---- OPT11 round 2")
    policy += section(plan, "static inline int plan_bin(", "// the plan record of texture")
    policy += section(plan, "static int plan_lmax(", "// The images drawn outside")
    policy += section(plan, "#define IMM_MAX", "// the engine drops its textures")
    policy += section(plan, "void PS2HWD_PlanEnd(", "static int imm_level")
    original_path = baseline_source / "ps2/hw/ps2_hw_plan.inc"
    original = original_path.read_text()
    original_policy = section(original, "static int plan_active;", "// ---- OPT11 round 2")
    original_policy += section(original, "static inline int plan_bin(", "// the plan record of texture")
    original_policy += section(original, "static int plan_lmax(", "// The images drawn outside")
    original_policy += section(original, "#define IMM_MAX", "// the engine drops its textures")
    original_policy += section(original, "void PS2HWD_PlanEnd(", "static int imm_level")
    if policy != original_policy:
        if not planner_proof:
            raise RuntimeError("Changed planner requires the explicit reviewed scratch-equivalence proof")
        proof = json.loads(planner_proof.read_text())
        if not proof.get("passed") or not all(run["passed"] for run in proof["runs"]):
            raise RuntimeError("Planner scratch proof did not pass all positive/negative runs")
        for name in ("plan_lmax", "PS2HWD_PlanEnd"):
            actual = plan_scratch_opt16_hosttest.digest(plan_scratch_opt16_hosttest.function(plan, name))
            if actual != proof["extracted_functions"][name]["candidate"]:
                raise RuntimeError("Actual planner body differs from separately executed scratch proof: " + name)
        normalized = policy
        edits = (
            ("#define PLAN_LEVEL_MAX 5 // plan_lmax bounds every entry's level and coarsening cap\n", ""),
            ("int lv = PLAN_LEVEL_MAX;", "int lv = 5;"),
            ("static u32 blk_l[ENT_MAX][PLAN_LEVEL_MAX + 1]; // only lx..cap are read; lx + 1 is used only while lx < cap", "static u32 blk_l[ENT_MAX][8]; // blocks of an entry at level l (filled for lx..cap + 1 when the plan is over budget)"),
            ("for (l = lx[i]; l <= cap[i]; l++)", "for (l = lx[i]; l <= cap[i] + 1 && l < 8; l++)"),
        )
        for expected, old in edits:
            if normalized.count(expected) != 1:
                raise RuntimeError("Reviewed scratch edit is missing or ambiguous: " + expected)
            normalized = normalized.replace(expected, old, 1)
        if normalized != original_policy:
            raise RuntimeError("Planner contains a change beyond the four reviewed scratch-only edits")
        inputs["separate_planner_equivalence_proof_sha256"] = sha(planner_proof)
        inputs["separate_planner_equivalence_proof"] = str(planner_proof)
    (work / "texture-policy.inc").write_text(policy)
    mode = section(plan, "static void plan_reset(", "// one texture is deleted")
    mode += section(plan, "static int imm_level", "// an image stored at level")
    hwd_path = source / "ps2/hw/ps2_hwd.c"
    mode += section(hwd_path.read_text(), "static int trace_frame", "#define TRACING()")
    (work / "texture-mode.inc").write_text(mode)
    inputs["ps2/hw/ps2_hwd.c"] = sha(hwd_path)
    inputs["executed_literal_policy_sha256"] = hashlib.sha256(policy.encode()).hexdigest()
    inputs["baseline_policy_sha256"] = hashlib.sha256(original_policy.encode()).hexdigest()
    return inputs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", type=Path, default=ROOT / "build/nov15-opt16/p4-bounded-v4/candidate/src")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-opt16/p4-bounded-v4/host")
    ap.add_argument("--mutation", type=int, default=0, choices=range(7))
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--corpus", type=Path, help="Full immutable input from next12_p4_corpus.py")
    ap.add_argument("--ee-cc", type=Path, help="Compile the complete before/after target record layout")
    ap.add_argument("--baseline-src", type=Path, help="Pinned P8 source used for ABI/policy comparison")
    ap.add_argument("--planner-proof", type=Path, help="Explicit separately executed proof for the four reviewed scratch-only edits")
    args = ap.parse_args()
    source = args.src.resolve()
    baseline_source = (args.baseline_src.resolve() if args.baseline_src else source.parent.parent / "baseline/src")
    work = args.out.resolve()
    inputs = prepare(work, source, baseline_source, args.mutation, args.planner_proof.resolve() if args.planner_proof else None)
    fixture = work / "fixture.c"
    shutil.copyfile(ROOT / "tools/ps2/p4_opt16_hosttest.c", fixture)
    shutil.copyfile(Path(__file__), work / "driver.py")
    results = {}
    for sanitize in (False, True):
        name = "sanitize" if sanitize else "plain"
        binary = work / name
        flags = ["-std=gnu17", "-O2", "-fwrapv", "-ffp-contract=off", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-parameter", "-DPS2", "-DPS2_PROFILE", "-I" + str(work)]
        if sanitize:
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        command = ["gcc", *flags, str(fixture), "-Wl,--gc-sections", "-o", str(binary), "-lm"]
        build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (work / (name + "-build.log")).write_text(" ".join(command) + "\n" + build.stdout + build.stderr)
        if build.returncode:
            raise RuntimeError("Host build failed: " + str(work / (name + "-build.log")))
        result = subprocess.run([str(binary), *([str(args.corpus.resolve())] if args.corpus else [])], cwd=ROOT, capture_output=True, text=True)
        output = result.stdout + result.stderr
        (work / (name + ".log")).write_text(output)
        if result.returncode or "runtime error:" in output or "ERROR: AddressSanitizer" in output:
            raise RuntimeError("Host proof failed: " + str(work / (name + ".log")))
        results[name] = json.loads(result.stdout)
    report = {"source": str(source), "source_inputs": inputs, "fixture_sha256": sha(fixture), "runner_sha256": sha(work / "driver.py"), "mutation": args.mutation, "results": results, "no_EE_compiler_or_emulator": True}
    if args.ee_cc:
        report["target_record_layout"] = target_layout(work, source, baseline_source, args.ee_cc.resolve())
        report["no_EE_compiler_or_emulator"] = False
        report["emulator_invoked"] = False
    if args.corpus:
        report["full_corpus_sha256"] = sha(args.corpus.resolve())
    # Texture/CLUT texels alone did not expose the original QW28 collision.
    # Every positive replay must also execute the actual boot/constant packets.
    vu_work = work / "vu-memory"
    vu_command = [sys.executable, str(ROOT / "tools/ps2/p4_vu_memory_hosttest.py"),
                  "--src", str(source), "--out", str(vu_work)]
    vu_run = subprocess.run(vu_command, cwd=ROOT, capture_output=True, text=True)
    (work / "vu-memory-driver.log").write_text(vu_run.stdout + vu_run.stderr)
    if vu_run.returncode:
        raise RuntimeError("VU data-map proof failed: " + str(work / "vu-memory-driver.log"))
    report["VU_data_map"] = {"report": str(vu_work / "report.json"),
                             "report_sha256": sha(vu_work / "report.json"),
                             "results": json.loads((vu_work / "report.json").read_text())["results"]}
    if args.negative_controls:
        report["negative_controls"] = []
        for mutation in range(1, 7):
            out = work / ("mutation" + str(mutation))
            command = [sys.executable, str(Path(__file__).resolve()), "--src", str(source), "--out", str(out), "--mutation", str(mutation)]
            command += ["--baseline-src", str(baseline_source)]
            if args.planner_proof:
                command += ["--planner-proof", str(args.planner_proof.resolve())]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            (work / ("mutation" + str(mutation) + "-driver.log")).write_text(result.stdout + result.stderr)
            log = out / "plain.log"
            if result.returncode == 0 or not log.is_file() or "P4 proof failed:" not in log.read_text():
                raise RuntimeError("Mutation did not produce an executed semantic failure: " + str(mutation))
            report["negative_controls"].append({"mutation": mutation, "executed_failure": log.read_text().strip()})
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    summary = {"report": str(work / "report.json"), "results": results,
               "detected_negative_controls": len(report.get("negative_controls", []))}
    if "target_record_layout" in report:
        summary["record_payload_growth_bytes"] = report["target_record_layout"]["record_payload_growth_bytes"]
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
