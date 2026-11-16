#!/usr/bin/env python3
"""Compare the actual baseline/current parallelogram cull and build an EE probe.

The probe includes the real complete driver state, cull type and pixel-centre
predicate. --ee builds only: the one shared emulator slot is managed separately.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from hw_front_cull_hosttest import ROOT, function

BASE = "604cfd150c5240142d8cb5367950b53d987916d1"
SOURCE = "src/ps2/hw/ps2_hw_fx2.inc"


def header(negative=None):
    baseline = subprocess.check_output(["git", "show", BASE + ":" + SOURCE], cwd=ROOT, text=True)
    current = (ROOT / SOURCE).read_text()
    old = function(baseline, "PS2HWD_ParaHidden").replace("PS2HWD_ParaHidden", "ReferenceParaHidden")
    new = function(current, "PS2HWD_ParaHidden")
    if function(baseline, "PS2HWD_CullSetup") != function(current, "PS2HWD_CullSetup"):
        raise RuntimeError("unrelated view setup changed")
    frontend = (ROOT / "src/hardware/hw_main.c").read_text()
    old_frontend = subprocess.check_output(["git", "show", BASE + ":src/hardware/hw_main.c"], cwd=ROOT, text=True)
    for caller in ("HWR_FX_QuadHidden", "HWR_PS2_SpriteHidden"):
        if function(frontend, caller) != function(old_frontend, caller):
            raise RuntimeError("parallelogram construction/caller changed: " + caller)
    if negative == "lost-eye-guard":
        anchor = "if (c[k][3] < w2)"
        assert new.count(anchor) == 1
        new = new.replace(anchor, "if (c[0][3] < w2)")
    elif negative == "mixed-outcodes":
        anchor = "c[0][k] < -c[0][3] && c[1][k] < -c[1][3]\n\t\t\t&& c[2][k] < -c[2][3] && c[3][k] < -c[3][3]"
        assert new.count(anchor) == 1
        new = new.replace(anchor, anchor.replace("&&", "||"))
    elif negative == "lost-far-plane":
        anchor = "for (k = 0; k < 3; k++)"
        assert new.count(anchor) == 1
        new = new.replace(anchor, "for (k = 0; k < 2; k++)")
    return old + "\n" + new


def run(command, directory, name, expected=0):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    (directory / (name + ".log")).write_text(result.stdout + result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}:\n{result.stdout}{result.stderr}")
    return result


def analyze_ee(work, log_path):
    report = json.loads((work / "report.json").read_text())
    elf = Path(report["ee_build"]["elf"])
    if hashlib.sha256(elf.read_bytes()).hexdigest() != report["ee_build"]["elf_sha256"]:
        raise RuntimeError("native ELF no longer matches its build report")
    if hashlib.sha256((ROOT / SOURCE).read_bytes()).hexdigest() != report["source_sha256"][SOURCE]:
        raise RuntimeError("production helper changed after the native build")
    log = log_path.read_text(errors="replace")
    if "PARACULL DONE" not in log or "PARACULL FAIL" in log:
        raise RuntimeError("native run did not complete successfully")
    exact = re.search(r"PARACULL PASS queries=(\d+) hidden=(\d+) retained=(\d+) state=(\d+) cull=(\d+)", log)
    if not exact:
        raise RuntimeError("native exact replay result missing")
    rows = []
    for match in re.finditer(r"PARACULL BENCH pattern=(\S+) queries=(\d+) reference=(\d+) current=(\d+)", log):
        pattern, queries, reference, current = match.groups()
        reference, current = int(reference), int(current)
        rows.append({"pattern": pattern, "queries": int(queries), "reference": reference,
                     "current": current, "percent": (current / reference - 1.0) * 100.0})
    if len(rows) != 12:
        raise RuntimeError("native benchmark rows missing")
    report["ee_build"]["launched"] = True
    commands = json.loads((work / "ee/commands.json").read_text())
    nm = Path(commands[0][0]).with_name(Path(commands[0][0]).name.replace("gcc", "nm"))
    symbols = run([nm, "-S", elf], work / "ee", "symbols").stdout
    assembly = (work / "ee/disassembly.log").read_text()
    sizes = {}
    for name in ("ReferenceParaHidden", "PS2HWD_ParaHidden"):
        symbol = re.search(r"^[0-9a-f]+ ([0-9a-f]+) [tT] " + name + r"$", symbols, re.M)
        body = re.search(r"^[0-9a-f]+ <" + name + r">:\n([\s\S]*?)(?=\n[0-9a-f]+ <|\Z)", assembly, re.M)
        if not symbol or not body:
            raise RuntimeError("native function symbol missing: " + name)
        frames = re.findall(r"addiu\s+sp,sp,-(\d+)", body[1])
        sizes[name] = {"symbol_bytes": int(symbol[1], 16), "stack_bytes": max(map(int, frames), default=0)}
    report["ee"] = {"passed": True, "queries": int(exact[1]), "hidden": int(exact[2]),
                    "retained": int(exact[3]), "driver_state_bytes": int(exact[4]),
                    "cull_type_bytes": int(exact[5]), "rows": rows,
                    "log_sha256": hashlib.sha256(log_path.read_bytes()).hexdigest(),
                    "micro_assembly": sizes, "compile_flags": commands[0][1:commands[0].index("-c")],
                    "scope": "COP0 Count, 8192 equal calls per row, minimum of seven repetitions in alternating order; no FPS/physical-hardware claim"}
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("EE PASS", report["ee"]["queries"], "exact cases;", len(rows), "timing rows")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next16/para")
    ap.add_argument("--ee", action="store_true", help="build, do not launch, the native EE probe")
    ap.add_argument("--skip-sanitizers", action="store_true")
    ap.add_argument("--analyze-ee-log", type=Path, help="attach a completed native run to its existing frozen build report")
    args = ap.parse_args()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if args.analyze_ee_log:
        analyze_ee(work, args.analyze_ee_log.resolve())
        return
    (work / "config.h").write_text("#define SRB2_COMP_OPTIMIZED 1\n")
    cc = shutil.which("gcc") or shutil.which("cc")
    dependencies = [SOURCE, "src/ps2/hw/ps2_hw_priv.inc", "src/ps2/hw/ps2_hw_fx2.h", "src/ps2/hw/ps2_hw_quad.h"]
    report = {"baseline": BASE, "source_sha256": {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in dependencies}, "scope": "actual complete baseline/current parallelogram cull; real driver state/type/pixel-centre headers", "runs": []}
    variants = [("normal", None, [])]
    variants += [(name, name, []) for name in ("lost-eye-guard", "mixed-outcodes", "lost-far-plane")]
    if not args.skip_sanitizers:
        variants.append(("sanitized", None, ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    for name, negative, flags in variants:
        directory = work / name
        directory.mkdir(exist_ok=True)
        (directory / "hw_para_cull_under_test.h").write_text(header(negative))
        exe = directory / "test"
        command = [cc, "-std=gnu2x", "-O2", "-g", "-fwrapv", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-DPS2_PROFILE", "-DHWRENDER", "-DCMAKECONFIG", *flags, "-I", str(directory), "-I", str(work), "-I", str(ROOT / "src"), str(ROOT / "tools/ps2/hw_para_cull_hosttest.c"), "-o", str(exe), "-lm"]
        (directory / "commands.json").write_text(json.dumps(command, indent=2) + "\n")
        run(command, directory, "build")
        result = run([exe], directory, "run", 1 if negative else 0)
        print(name, "PASS", result.stdout.strip(), flush=True)
        report["runs"].append({"name": name, "passed": True, "exit": result.returncode, "stdout": result.stdout})
    if args.ee:
        os.environ["SRB2_PS2_HW"] = "1"
        import build as B
        directory = work / "ee"
        directory.mkdir(exist_ok=True)
        (directory / "hw_para_cull_under_test.h").write_text(header())
        obj, elf = directory / "test.o", directory / "PARACULL.ELF"
        flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP")]
        flags += B.UNIT_FLAGS.get("src/ps2/hw/ps2_hwd.c", [])
        commands = [[B.CC, *flags, "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-DHWRENDER", "-DPARA_CULL_NATIVE", "-I", str(directory), "-I", str(work), *B.INCS, "-c", ROOT / "tools/ps2/hw_para_cull_hosttest.c", "-o", obj], [B.CC, *B.LDFLAGS, obj, "-o", elf, "-ldebug", "-lpatches", "-lm"]]
        (directory / "commands.json").write_text(json.dumps([[str(c) for c in command] for command in commands], indent=2) + "\n")
        for i, command in enumerate(commands):
            run(command, directory, "build-" + str(i))
        disassembler = B.DEV / ("ee/bin/mips64r5900el-ps2-elf-objdump" + B.EXE)
        run([disassembler, "-dr", elf], directory, "disassembly")
        report["ee_build"] = {"elf": str(elf), "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(), "launched": False}
        print("Built", elf, flush=True)
    report["passed"] = True
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
