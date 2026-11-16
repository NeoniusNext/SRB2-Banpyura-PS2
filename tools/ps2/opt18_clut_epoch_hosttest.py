#!/usr/bin/env python3
"""Reproduce a ramp eviction invalidating the VU1 light-row residency promise.

Compile the actual production CLUT packet/cache builders and prow_ensure/prow_blk
with a bounded host ring. Colour formatters supply distinct synthetic images:
the test concerns slot lifetime, not the independently tested lighting equation.
The old source must leave a ramp image certified as a light row; the fixed source
must regenerate that row and preserve fast cache hits. No emulator is used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FIX = "\tH.clut_epoch++; // a ramp can replace a light-table row cached by the VU1 path\n"
INPUTS = (
    "src/ps2/hw/ps2_hw_priv.inc",
    "src/ps2/hw/ps2_hw_regs.inc",
    "src/ps2/hw/ps2_hw_tex.inc",
    "src/ps2/hw/ps2_hw_draw.inc",
    "src/ps2/hw/ps2_hw_vu1.inc",
    "tools/ps2/opt18_clut_epoch_hosttest.c",
    "tools/ps2/opt18_clut_epoch_hosttest.py",
)


def section(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]


def run(command, log):
    result = subprocess.run(list(map(str, command)), cwd=ROOT,
                            capture_output=True, text=True)
    log.write_text(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {log}\n"
                           + result.stdout + result.stderr)
    return result.stdout


def build_source(source_root, fixed):
    # Cache builders end at the next section, whose exact prose is unrelated
    # to this regression. Keep every function body in that interval unchanged.
    hw = source_root / "src/ps2/hw"
    private = (hw / "ps2_hw_priv.inc").read_text()
    regs = (hw / "ps2_hw_regs.inc").read_text()
    tex = (hw / "ps2_hw_tex.inc").read_text()
    draw = (hw / "ps2_hw_draw.inc").read_text()
    vu = (hw / "ps2_hw_vu1.inc").read_text()
    if draw.count(FIX) != 1:
        raise RuntimeError("Missing or ambiguous reviewed ramp epoch fix")
    if not fixed:
        draw = draw.replace(FIX, "", 1)
    definitions = (
        section(private, "#define GSR_PRIM", "// DMA channel 2")
        + section(private, "#define HWGIF_TAG", "#define PS2HWD_MAXPOLY")
        + section(private, "#define CLUT_FIXED", "// OPT8 (PS2-HW-24)")
        + section(tex, "enum\n{\n\tCK_OPAQUE", "// the 256 entries")
    )
    bodies = (
        section(regs, "static inline void ad_set", "// FRAME_1")
        + section(tex, "// Reserve a complete 16x16 CT32 CLUT transfer.", "// CSM1 swaps")
        + section(tex, "static void clut_swizzle", "enum\n{\n\tCK_OPAQUE")
        + section(tex, "static int clut_palette_dependent", "// ---- upload")
    )
    return definitions, bodies, draw, vu


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs_sha256 = {
        name: hashlib.sha256((args.source_root / name).read_bytes()).hexdigest()
        for name in INPUTS
    }
    fixture_path = ROOT / "tools/ps2/opt18_clut_epoch_hosttest.c"
    fixture = fixture_path.read_text()
    results = {}
    for fixed in (False, True):
        tag = "fixed" if fixed else "old-negative"
        definitions, bodies, draw, vu = build_source(args.source_root, fixed)
        body = (
            bodies
            + section(draw, "static u32 ramp_clut", "static void ov_flush(void)\n{")
            + section(vu, "static inline u32 prow_param", "static inline int cm_row")
        )
        generated = fixture.replace("/* PRODUCTION_DEFINITIONS */", definitions)
        generated = generated.replace("/* PRODUCTION_FUNCTIONS */", body)
        source = out / f"{tag}.c"
        source.write_text(generated)
        for sanitize in (False, True):
            suffix = "-sanitize" if sanitize else ""
            binary = out / (tag + suffix)
            flags = ["-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-function", "-Wno-unused-variable",
                     f"-DEXPECT_REPAIR={int(fixed)}"]
            if sanitize:
                flags += ["-g", "-fsanitize=address,undefined",
                          "-fno-omit-frame-pointer", "-no-pie"]
            run(["gcc", *flags, source, "-o", binary], out / (binary.name + "-build.log"))
            output = run([binary], out / (binary.name + ".log"))
            results[binary.name] = json.loads(output)
    if inputs_sha256 != {
        name: hashlib.sha256((args.source_root / name).read_bytes()).hexdigest()
        for name in INPUTS
    }:
        raise RuntimeError("Regression inputs changed during compilation or execution")
    report = {
        "schema": "ps2-opt18-clut-epoch-host-v1",
        "inputs_sha256": inputs_sha256,
        "production_functions": ["clut_alloc", "clut_swizzle", "clut_build_into",
                                 "clut_get", "ramp_clut", "prow_param", "prow_blk", "prow_ensure"],
        "fixture_boundaries": ["bounded host ring allocation and overlay flush counter",
                               "distinct synthetic light/ramp images replace colour equations",
                               "GS CLUT transfer interpreter applies actual emitted image payloads"],
        "negative_control": "remove only the reviewed missing ramp epoch increment",
        "results": results,
    }
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
