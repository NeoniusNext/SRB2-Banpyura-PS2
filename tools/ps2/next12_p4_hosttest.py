#!/usr/bin/env python3
"""Exercise isolated P4 source, complete uploads, palette-band selection and fallbacks.

This does not launch EE tools or an emulator. Production ROOT/src is never read.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import hw_hosttest

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def section(text, first, after):
    if text.count(first) != 1:
        raise RuntimeError("Missing or ambiguous source anchor: " + first)
    start = text.index(first)
    return text[start:text.index(after, start)]


def prepare(work, source, mutation):
    work.mkdir(parents=True, exist_ok=True)
    hw_hosttest.ROOT = source.parent
    hw_hosttest.make_shim(work)
    (work / "hw_model.h").write_text((source / "hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
    inputs = {}
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
    original_path = ROOT / "build/nov15-next12/baseline-src/src/ps2/hw/ps2_hw_plan.inc"
    original = original_path.read_text()
    original_policy = section(original, "static int plan_active;", "// ---- OPT11 round 2")
    original_policy += section(original, "static inline int plan_bin(", "// the plan record of texture")
    original_policy += section(original, "static int plan_lmax(", "// The images drawn outside")
    original_policy += section(original, "#define IMM_MAX", "// the engine drops its textures")
    original_policy += section(original, "void PS2HWD_PlanEnd(", "static int imm_level")
    if policy != original_policy:
        raise RuntimeError("Counterexample must execute the complete literal unchanged baseline policy")
    (work / "texture-policy.inc").write_text(policy)
    inputs["literal_unchanged_policy_sha256"] = hashlib.sha256(policy.encode()).hexdigest()
    return inputs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", type=Path, default=ROOT / "build/nov15-next12/p4-src/src")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next12/p4/host")
    ap.add_argument("--mutation", type=int, default=0, choices=range(7))
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--corpus", type=Path, help="Full immutable input from next12_p4_corpus.py")
    args = ap.parse_args()
    source = args.src.resolve()
    work = args.out.resolve()
    inputs = prepare(work, source, args.mutation)
    results = {}
    for sanitize in (False, True):
        name = "sanitize" if sanitize else "plain"
        binary = work / name
        flags = ["-std=gnu17", "-O2", "-fwrapv", "-ffp-contract=off", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-parameter", "-DPS2_PROFILE", "-I" + str(work)]
        if sanitize:
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        command = ["gcc", *flags, str(ROOT / "tools/ps2/next12_p4_hosttest.c"), "-Wl,--gc-sections", "-o", str(binary), "-lm"]
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
    report = {"source": str(source), "source_inputs": inputs, "fixture_sha256": sha(ROOT / "tools/ps2/next12_p4_hosttest.c"), "mutation": args.mutation, "results": results, "no_EE_compiler_or_emulator": True}
    if args.corpus:
        report["full_corpus_sha256"] = sha(args.corpus.resolve())
    if args.negative_controls:
        report["negative_controls"] = []
        for mutation in range(1, 7):
            out = work / ("mutation" + str(mutation))
            command = [sys.executable, str(Path(__file__).resolve()), "--src", str(source), "--out", str(out), "--mutation", str(mutation)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            (work / ("mutation" + str(mutation) + "-driver.log")).write_text(result.stdout + result.stderr)
            log = out / "plain.log"
            if result.returncode == 0 or not log.is_file() or "P4 proof failed:" not in log.read_text():
                raise RuntimeError("Mutation did not produce an executed semantic failure: " + str(mutation))
            report["negative_controls"].append({"mutation": mutation, "executed_failure": log.read_text().strip()})
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
