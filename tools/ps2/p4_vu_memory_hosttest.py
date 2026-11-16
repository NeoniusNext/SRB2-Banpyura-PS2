#!/usr/bin/env python3
"""Execute actual boot/constant packets and reject overlapping VU data writes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import hw_hosttest
from plan_scratch_opt16_hosttest import function

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(source, out):
    out.mkdir(parents=True, exist_ok=False)
    hw_hosttest.ROOT = source.parent
    hw_hosttest.make_shim(out)
    (out / "hw_model.h").write_text((source / "hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
    inputs = {}
    for name in ("ps2_hw_priv.inc", "ps2_hwd_dbg.h", "ps2_hw_regs.inc", "ps2_hw_vif.inc"):
        p = source / "ps2/hw" / name
        inputs[str(p)] = sha(p)
        (out / name).write_text(p.read_text().replace('#include "../../doomtype.h"', '#include "shim.h"'))
    vu_path = source / "ps2/hw/ps2_hw_vu1.inc"
    vu = vu_path.read_text()
    inputs[str(vu_path)] = sha(vu_path)
    gs = (source / "ps2/hw/ps2_hw_gs.inc").read_text()
    inputs[str(source / "ps2/hw/ps2_hw_gs.inc")] = sha(source / "ps2/hw/ps2_hw_gs.inc")
    assembly_path = source / "ps2/hw/vu1/ps2_hw_vu1.vsm"
    sprite_path = source / "ps2/hw/vu1/ps2_hw_vu1_spr.vsm"
    for path in (assembly_path, sprite_path):
        inputs[str(path)] = sha(path)
    equ = dict(re.findall(r"\.equ\s+(\w+),\s*(\d+)\b", assembly_path.read_text()))
    sprite = dict(re.findall(r"\.equ\s+(\w+),\s*(\d+)\b", sprite_path.read_text()))
    constants = "\n".join("#define M_" + n + " " + equ[n] for n in ("PL_VEC", "PL_BITS", "P4_TEX2", "OUT0", "OUT1", "LISTA", "LISTB", "LISTC"))
    constants += "\n#define M_RGBTMP " + sprite["RGBTMP"] + "\n#define M_DBG " + sprite["DBG"] + "\n"
    prefix = vu[vu.index("#define VU_MAXVERT"):vu.index("static inline u32 vu_entry")]
    boot = function(vu, "vu1_boot")
    barrier = '__asm__ volatile("sync.l");'
    if boot.count(barrier) != 1:
        raise RuntimeError("Unexpected boot architecture boundary")
    boot = boot.replace(barrier, "(void)0; /* host hardware barrier boundary */", 1)
    body = constants + "\n" + prefix + function(vu, "vu_entry") + boot
    body += "\n" + function(gs, "dma_tag")
    body += "\n".join(function(vu, name) for name in ("vu_room", "vu_tag_slot", "vu_next_run", "vu_consts_send"))
    (out / "executed.inc").write_text(body)
    shutil.copyfile(ROOT / "tools/ps2/p4_vu_memory_hosttest.c", out / "fixture.c")
    shutil.copyfile(Path(__file__), out / "driver.py")
    return inputs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--expect-collision", action="store_true", help="Require the retained QW28 source to fail the executed plane-table preservation check")
    a = ap.parse_args()
    source, out = a.src.resolve(), a.out.resolve()
    inputs = prepare(source, out)
    results = {}
    for sanitize in (False, True):
        name = "sanitize" if sanitize else "plain"
        flags = ["-std=gnu17", "-O2", "-fwrapv", "-ffunction-sections", "-fdata-sections", "-DPS2", "-DPS2_PROFILE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-but-set-variable", "-I" + str(out)]
        if sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        command = ["gcc", *flags, out / "fixture.c", "-Wl,--gc-sections", "-lm", "-o", out / name]
        built = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        (out / (name + "-build.log")).write_text(" ".join(map(str, command)) + "\n" + built.stdout + built.stderr)
        if built.returncode:
            raise RuntimeError("Build failed: " + str(out / (name + "-build.log")))
        ran = subprocess.run([str(out / name)], capture_output=True, text=True)
        (out / (name + ".log")).write_text(ran.stdout + ran.stderr)
        if a.expect_collision:
            failure = "VU P4 memory proof failed: P4 constant send preserves all twelve boot plane quadwords"
            if ran.returncode != 1 or failure not in ran.stderr or "runtime error:" in ran.stderr or "ERROR: AddressSanitizer" in ran.stderr:
                raise RuntimeError("Expected executed plane collision did not occur")
            results[name] = {"expected_collision_detected": True, "exit": ran.returncode, "failure": ran.stderr.strip()}
            continue
        if ran.returncode or "runtime error:" in ran.stderr:
            raise RuntimeError("VU memory proof failed: " + str(out / (name + ".log")))
        results[name] = json.loads(ran.stdout)
    report = {"source_inputs": inputs, "fixture_sha256": sha(out / "fixture.c"), "driver_sha256": sha(out / "driver.py"), "results": results, "expected_collision": a.expect_collision, "boundaries": "Host VIF packet interpretation; boot microcode bytes, hardware register/barrier and queue execution are substituted. Actual boot/constant-packet C bodies and VIF macros execute."}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(out / "report.json"), "results": results}, indent=2))


if __name__ == "__main__":
    main()
