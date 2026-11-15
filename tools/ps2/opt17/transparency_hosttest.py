#!/usr/bin/env python3
"""Prove P8 canonical-key classification over real cached-mip transitions.

Compiles the complete frozen texture driver, then compiles an isolated minimal
fix. The baseline must expose wrong logical RGBA and the fix must preserve all
uploaded index CRCs while repairing each wrong keyed CLUT selection. No shipping
source is edited. Raw-flat cases exercise the actual streamed first-level path;
map cases probe the same data-cache contract from a prepared valid mip entry.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import hw_hosttest

FILES = ("ps2_hw_priv.inc", "ps2_hw_regs.inc", "ps2_hw_xform.inc",
         "ps2_hw_pal.inc", "ps2_hw_tex.inc", "ps2_hwd_dbg.h", "ps2_hw_fx2.h")
OLD = """\t\t\t\telse
\t\t\t\t{
\t\t\t\t\tif (!m->ps2_h255)
\t\t\t\t\t\tm->ps2_h255 = (m->ps2_twin && m->ps2_twin->ps2_h255) ? m->ps2_twin->ps2_h255 : (UINT8)(scan_has_ff(data, dsrc ? ssw * ssh : n) ? 2 : 1);
\t\t\t\t\thas255 = m->ps2_h255 == 2;
\t\t\t\t}"""
NEW = """\t\t\t\telse if (dsrc)
\t\t\t\t{
\t\t\t\t\t// A cached mip can lose index 255 even when the original has holes.
\t\t\t\t\t// Its classification belongs to this upload, never to the full source.
\t\t\t\t\thas255 = scan_has_ff(data, ssw * ssh);
\t\t\t\t}
\t\t\t\telse
\t\t\t\t{
\t\t\t\t\tif (!m->ps2_h255)
\t\t\t\t\t\tm->ps2_h255 = (m->ps2_twin && m->ps2_twin->ps2_h255) ? m->ps2_twin->ps2_h255 : (UINT8)(scan_has_ff(data, n) ? 2 : 1);
\t\t\t\t\thas255 = m->ps2_h255 == 2;
\t\t\t\t}"""


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def run(cmd, log):
    result = subprocess.run(list(map(str, cmd)), cwd=ROOT, capture_output=True, text=True)
    log.write_text(" ".join(map(str, cmd)) + "\n" + result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Command failed: {log}\n{result.stdout}{result.stderr}")
    return result.stdout


def prepare(work, source_root, fixed):
    work.mkdir(parents=True, exist_ok=False)
    saved_root = hw_hosttest.ROOT
    try:
        hw_hosttest.ROOT = source_root
        hw_hosttest.make_shim(work)
    finally:
        hw_hosttest.ROOT = saved_root
    # Preserve private source-representation helpers that follow GLMipmap_t in
    # the integrated header. P8 fixtures leave their flags clear, while the
    # complete current texture manager still compiles its real AP88 branches.
    data_header = (source_root / "src/hardware/hw_data.h").read_text()
    after_type = data_header.index("typedef struct GLMipmap_s GLMipmap_t;") + len("typedef struct GLMipmap_s GLMipmap_t;")
    extension_end = data_header.index("//\n// Level textures", after_type)
    with (work / "shim.h").open("a") as shim:
        shim.write(data_header[after_type:extension_end])
    (work / "fixture.c").write_bytes((HERE / "transparency_hosttest.c").read_bytes())
    (work / "hw_model.h").write_text((source_root / "src/hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
    inputs = {name: sha(source_root / name) for name in
              ("src/hardware/hw_defs.h", "src/hardware/hw_data.h", "src/doomtype.h", "src/hardware/hw_drv.h", "src/hardware/hw_model.h")}
    for name in FILES:
        path = source_root / "src/ps2/hw" / name
        inputs[str(path.relative_to(source_root))] = sha(path)
        text = path.read_text()
        if name == "ps2_hw_tex.inc":
            # A regression run can start from either the unfixed source or the
            # reviewed integrated source. Reverse only this exact correction
            # for the isolated negative control; retain every other change.
            if text.count(OLD) == 1 and not text.count(NEW):
                baseline = text
            elif text.count(NEW) == 1 and not text.count(OLD):
                baseline = text.replace(NEW, OLD)
            else:
                raise RuntimeError("Reviewed classification block missing or ambiguous")
            text = baseline.replace(OLD, NEW) if fixed else baseline
        (work / name).write_text(text.replace('#include "../../doomtype.h"', '#include "shim.h"'))
    draw = (source_root / "src/ps2/hw/ps2_hw_draw.inc").read_text()
    a = draw.index("static void tex_regs(")
    b = draw.index("static void lru_touch(", a)
    (work / "texture-registers.inc").write_text(draw[a:b])
    inputs["src/ps2/hw/ps2_hw_draw.inc"] = sha(source_root / "src/ps2/hw/ps2_hw_draw.inc")
    return inputs


def execute(work, sanitize, packed):
    name = ("packed" if packed else "normal") + ("-sanitize" if sanitize else "")
    flags = ["-std=gnu17", "-O2", "-fwrapv", "-ffp-contract=off", "-DPS2_PROFILE",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
             "-Wno-unused-but-set-variable", "-Wno-unused-parameter", "-I" + str(work)]
    if packed:
        flags += ["-DPS2"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    binary = work / name
    run(["gcc", *flags, work / "fixture.c", "-o", binary, "-lm"], work / (name + "-build.log"))
    output = run([binary], work / (name + ".log"))
    if "runtime error:" in output or "ERROR: AddressSanitizer" in output:
        raise RuntimeError("Sanitizer failure: " + str(work / (name + ".log")))
    rows = []
    for line in output.splitlines():
        if line.startswith("T17 case="):
            rows.append(dict(item.split("=", 1) for item in line.split()[1:]))
    match = re.search(r"T17 COMPLETE observations=(\d+) rgba_bad=(\d+) poisoned=(\d+) checks=(\d+)", output)
    if not match or len(rows) != int(match[1]):
        raise RuntimeError("Incomplete executed fixture: " + str(work))
    return {"rows": rows, "summary": dict(zip(("observations", "rgba_bad", "poisoned", "checks"), map(int, match.groups()))),
            "binary_sha256": sha(binary)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, default=ROOT)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    source = args.source_root.resolve(); out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"scope": __doc__.strip(), "source_root": str(source), "fixture_sha256": sha(HERE / "transparency_hosttest.c"),
              "runner_sha256": sha(Path(__file__).resolve()), "negative_control": "Baseline reverses only the exact reviewed canonical-classification fix when input already contains it.", "variants": {}}
    for variant, fixed in (("baseline", False), ("fixed", True)):
        work = out / variant
        inputs = prepare(work, source, fixed)
        results = {name: execute(work, sanitize, packed) for name, sanitize, packed in
                   (("normal", False, False), ("sanitize", True, False), ("packed", False, True), ("packed_sanitize", True, True))}
        for item in results.values():
            if item["rows"] != results["normal"]["rows"] or item["summary"] != results["normal"]["summary"]:
                raise RuntimeError("ABI/sanitizer behavior differs: " + variant)
        report["variants"][variant] = {"source_inputs": inputs, "results": results}
    before = report["variants"]["baseline"]["results"]["normal"]
    after = report["variants"]["fixed"]["results"]["normal"]
    if not before["summary"]["rgba_bad"] or not before["summary"]["poisoned"]:
        raise RuntimeError("Baseline failed to expose executed wrong transparency")
    if after["summary"]["rgba_bad"] or after["summary"]["poisoned"]:
        raise RuntimeError("Isolated fix did not repair complete mixed sequence")
    for a, b in zip(before["rows"], after["rows"]):
        for key in ("case", "step", "w", "h", "ff", "index_crc", "expected_rgba_crc"):
            if a[key] != b[key]:
                raise RuntimeError("Fix changed texture data/reference semantics: " + key)
    report["index_crc_pairs_exact"] = len(after["rows"])
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"baseline": before["summary"], "fixed": after["summary"],
                      "index_crc_pairs_exact": report["index_crc_pairs_exact"], "report": str(out / "report.json")}, indent=2))


if __name__ == "__main__":
    main()
