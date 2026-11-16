#!/usr/bin/env python3
"""Compare complete texture-manager behavior before/after record compaction.

The real driver sources are compiled independently; normalized metadata, upload
packets, source-cache locks and free-list/owner state are compared byte for byte.
The optional EE compilation measures the actual 32-bit target record ABI.
"""
import argparse
import filecmp
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

import hw_hosttest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/ps2/hw"
FILES = ("ps2_hw_priv.inc", "ps2_hw_regs.inc", "ps2_hw_xform.inc",
         "ps2_hw_pal.inc", "ps2_hw_tex.inc", "ps2_hwd_dbg.h", "ps2_hw_fx2.h")
MEMBERS = "owner blk nblk w h tw th tbw psm clut wrapx wrapy nearest_only baked screen notcc region npot_x npot_y used pin stamp seq done ws wgt prev next uw vh sxs sys sw sh dx dy capi pad2 fp share sec sec_next has255 shareable dmapend pad3 imgws lodfr lod psplit pnext ppar pt0 pt1 ptk".split()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    log.write_text(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError("Command failed; see " + str(log))
    return result


def section(text, first, after):
    if text.count(first) != 1:
        raise RuntimeError("Missing or ambiguous source anchor: " + first)
    start = text.index(first)
    return text[start:text.index(after, start)]


def prepare(work, reference, mutation=None):
    work.mkdir(parents=True)
    hw_hosttest.make_shim(work)
    (work / "hw_model.h").write_text((ROOT / "src/hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
    sources = {}
    for name in (*FILES, "ps2_hw_draw.inc"):
        path = SOURCE / name
        text = subprocess.check_output(["git", "show", reference + ":" + str(path.relative_to(ROOT))], cwd=ROOT).decode() if reference else path.read_text()
        sources[str(path.relative_to(ROOT))] = hashlib.sha256(text.encode()).hexdigest()
        text = text.replace('#include "../../doomtype.h"', '#include "shim.h"')
        if name == "ps2_hw_priv.inc" and mutation:
            old, new = {
                "address": ("\tu16 blk;", "\tu8 blk;"),
                "length": ("\tu16 nblk;", "\tu8 nblk;"),
                "dimensions": ("\tu16 uw, vh;", "\tu8 uw, vh;"),
                "frame": ("\tu32 done;", "\tu8 done;"),
            }[mutation]
            if text.count(old) != 1:
                raise RuntimeError("Mutation anchor changed: " + old)
            text = text.replace(old, new)
        if name == "ps2_hw_draw.inc":
            text = section(text, "static void tex_regs(", "static void lru_touch(")
            name = "texture-registers.inc"
        (work / name).write_text(text)
    return sources


def layout(work, compiler, native):
    source = '#include <stddef.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <math.h>\n#include "shim.h"\n#include "ps2_hwd_dbg.h"\n#define CONS_WARNING 1\n#define CONS_Alert(...) ((void)0)\n#include "ps2_hw_priv.inc"\n'
    expressions = ["sizeof(texrec_t)", "_Alignof(texrec_t)"]
    for name in MEMBERS:
        expressions.extend(["offsetof(texrec_t," + name + ")", "sizeof(((texrec_t *)0)->" + name + ")"])
    source += '__attribute__((used,section(".record_layout"))) const unsigned int texture_record_layout[] = {' + ",".join(expressions) + "};\n"
    cfile = work / ("layout-ee.c" if native else "layout-host.c")
    obj = cfile.with_suffix(".o")
    cfile.write_text(source)
    command = [str(compiler), "-std=gnu17", "-O2", "-DPS2_PROFILE", "-I" + str(work), "-c", str(cfile), "-o", str(obj)]
    run(command, obj.with_suffix(".log"), cwd=ROOT)
    data = obj.read_bytes()
    if data[:6] not in (b"\x7fELF\x01\x01", b"\x7fELF\x02\x01"):
        raise RuntimeError("Expected a little-endian ELF layout object")
    bits = data[4]
    if bits == 1:
        off = struct.unpack_from("<I", data, 32)[0]
        size, count, names = struct.unpack_from("<HHH", data, 46)
        headers = [struct.unpack_from("<IIIIIIIIII", data, off + i * size) for i in range(count)]
    else:
        off = struct.unpack_from("<Q", data, 40)[0]
        size, count, names = struct.unpack_from("<HHH", data, 58)
        headers = [struct.unpack_from("<IIQQQQIIQQ", data, off + i * size) for i in range(count)]
    string_header = headers[names]
    strings = data[string_header[4]:string_header[4] + string_header[5]]
    record = [h for h in headers if strings[h[0]:].split(b"\0", 1)[0] == b".record_layout"]
    if len(record) != 1 or record[0][5] != 4 * len(expressions):
        raise RuntimeError("Missing complete target layout")
    values = struct.unpack_from("<" + "I" * len(expressions), data, record[0][4])
    return {"object_sha256": sha(obj), "sizeof": values[0], "alignment": values[1],
            "members": {name: {"offset": values[2 + i * 2], "size": values[3 + i * 2]} for i, name in enumerate(MEMBERS)}}


def execute(work, sanitizer, expect_failure=False):
    name = "sanitize" if sanitizer else "plain"
    binary = work / name
    flags = ["-std=gnu17", "-O2", "-fwrapv", "-ffp-contract=off", "-DPS2_PROFILE",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
             "-Wno-unused-but-set-variable", "-Wno-unused-parameter", "-I" + str(work)]
    if sanitizer:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    if expect_failure:
        # Corrupt field widths deliberately contradict the fixture's boundary
        # constants; retain those warnings and require an executed failure too.
        flags += ["-Wno-error=overflow", "-Wno-error=type-limits"]
    run(["gcc", *flags, str(ROOT / "tools/ps2/texture_record_hosttest.c"), "-o", str(binary), "-lm"], work / (name + "-build.log"), cwd=ROOT)
    result = subprocess.run([str(binary), str(work / (name + ".bin"))], cwd=ROOT, capture_output=True, text=True)
    output = result.stdout + result.stderr
    (work / (name + ".log")).write_text(output)
    bad = result.returncode or "runtime error:" in output or "ERROR: AddressSanitizer" in output
    if expect_failure:
        if not bad or "Texture record proof failed:" not in output:
            raise RuntimeError("Mutation did not produce an executed semantic failure")
        return {"detected": True, "failure": output.strip()}
    if bad:
        raise RuntimeError("Texture record proof failed; see " + str(work / (name + ".log")))
    return {"summary": json.loads(result.stdout), "snapshot_sha256": sha(work / (name + ".bin"))}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--reference", default="e10593b0d")
    ap.add_argument("--out", type=Path, default=ROOT / "build/texture-record")
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--ee-cc", type=Path)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    compiler = args.ee_cc
    if not compiler:
        found = shutil.which("mips64r5900el-ps2-elf-gcc")
        if found:
            compiler = Path(found)
        elif os.environ.get("PS2DEV"):
            compiler = Path(os.environ["PS2DEV"]) / "ee/bin/mips64r5900el-ps2-elf-gcc"
    headers = ("src/hardware/hw_defs.h", "src/hardware/hw_data.h", "src/doomtype.h", "src/hardware/hw_drv.h", "src/hardware/hw_model.h")
    for path in headers:
        if (ROOT / path).read_bytes() != subprocess.check_output(["git", "show", args.reference + ":" + path], cwd=ROOT):
            raise RuntimeError("Shared shim input differs from reference: " + path)
    report = {"reference": subprocess.check_output(["git", "rev-parse", args.reference], cwd=ROOT, text=True).strip(),
              "fixture_sha256": sha(ROOT / "tools/ps2/texture_record_hosttest.c"),
              "runner_sha256": sha(Path(__file__).resolve()),
              "shim_inputs": {path: sha(ROOT / path) for path in headers}, "variants": {}}
    if compiler:
        report["EE_compiler"] = {"path": str(compiler), "sha256": sha(compiler),
                                 "version": subprocess.check_output([str(compiler), "--version"], text=True).splitlines()[0]}
    for name, reference in (("reference", args.reference), ("compact", None)):
        work = out / name
        inputs = prepare(work, reference)
        item = {"source_inputs": inputs, "host_layout": layout(work, "gcc", False)}
        if compiler:
            item["ee_layout"] = layout(work, compiler, True)
        item["plain"] = execute(work, False)
        item["sanitize"] = execute(work, True)
        if item["plain"] != item["sanitize"] or not filecmp.cmp(work / "plain.bin", work / "sanitize.bin", shallow=False):
            raise RuntimeError("Plain/sanitized normalized behavior differs")
        report["variants"][name] = item
    before, after = report["variants"].values()
    if before["plain"] != after["plain"] or not filecmp.cmp(out / "reference/plain.bin", out / "compact/plain.bin", shallow=False):
        raise RuntimeError("Reference/compact normalized behavior differs")
    report["complete_snapshot_byte_comparisons"] = 3
    for abi in ("host_layout", "ee_layout"):
        if abi in before:
            if before[abi]["sizeof"] - after[abi]["sizeof"] != 8:
                raise RuntimeError("Expected exactly eight fewer record bytes: " + abi)
    if args.negative_controls:
        report["negative_controls"] = {}
        for mutation in ("address", "length", "dimensions", "frame"):
            work = out / ("mutation-" + mutation)
            prepare(work, None, mutation)
            report["negative_controls"][mutation] = execute(work, False, True)
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"reference_bytes": before.get("ee_layout", before["host_layout"])["sizeof"], "compact_bytes": after.get("ee_layout", after["host_layout"])["sizeof"], **after["plain"], "negative_controls": len(report.get("negative_controls", {})), "report": str(out / "report.json")}, indent=2))


if __name__ == "__main__":
    main()
