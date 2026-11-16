#!/usr/bin/env python3
"""One official, single-profile PCSX2 run with immutable settings evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
HERE = Path(__file__).resolve().parent
ROOT = next(p for p in HERE.parents if (p / "tools/ps2/build.py").is_file())
sys.path.insert(0, str(ROOT / "tools/ps2"))
from next12_runtime import run_verified
from next10_gs_run import critical_settings

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path); ap.add_argument("--args", default="")
    a = ap.parse_args(); out = a.out.resolve(); original_elf = a.elf.resolve()
    profile = Path(os.environ.get("SRB2_PCSX2_ROOT", "/workspace/.onboarding/pcsx2"))
    configs = sorted(profile.glob("slot*/**/PCSX2.ini"))
    if os.environ.get("SRB2_PCSX2_SLOTS") != "1" or not configs or any(critical_settings(p).get("EmuCore", {}).get("EnableExtraMemory") != "false" for p in configs):
        raise SystemExit("Use the unchanged standard 32 MiB single-slot emulator environment")
    stage = out.parent / (out.name + "-host"); stage.mkdir(parents=True, exist_ok=False)
    elf = stage / original_elf.name
    shutil.copyfile(original_elf, elf); shutil.copyfile(original_elf.parent / "build-report.json", stage / "build-report.json")
    command = [sys.executable, str(ROOT / "tools/ps2/run_pcsx2.py"), "--elf", str(elf), "--log", str(out / "pcsx2.log"), "--timeout", "180", "--until", "TN COMPLETE", "--args", a.args]
    p = run_verified(command, out, (elf, elf.parent / "build-report.json", Path(__file__)))
    boot = (out / "pcsx2.log").read_text(errors="replace")
    lines = [line.split("] ", 1)[-1] for line in boot.splitlines() if "TN " in line or "H0 " in line]
    (out / "native.txt").write_text("\n".join(lines) + "\n")
    complete = re.search(r"TN COMPLETE checks=(\d+) failures=(\d+)", boot)
    failures = [line for line in lines if "H0 FAIL " in line]
    post = json.loads((out / "run-post.json").read_text())
    checked_lines = [line for line in lines if "H0 PASS " in line or "H0 FAIL " in line]
    profiles_32 = all(profile.get("EmuCore", {}).get("EnableExtraMemory") == "false" for profile in post["critical_settings"].values())
    report = {"scope": "Actual PS2 EE code and GS-rendered complete-frame readbacks in PCSX2, standard 32 MiB profile; not physical PS2.", "original_elf": str(original_elf), "private_host_stage": str(stage),
        "command": command, "wrapper_rc": p.returncode, "completed": bool(complete),
        "checks": int(complete[1]) if complete else None, "failures": failures,
        "failure_count": int(complete[2]) if complete else None,
        "all_frozen_inputs_unchanged": post["all_frozen_inputs_unchanged"],
        "all_non_UI_settings_unchanged": post["all_non_UI_settings_unchanged"],
        "emulator_binary_unchanged": post["emulator_binary_unchanged"],
        "profile_32MiB": profiles_32, "single_emulator_slot": json.loads((out / "run-pre.json").read_text())["environment"].get("SRB2_PCSX2_SLOTS") == "1",
        "check_totals_consistent": complete is not None and int(complete[1]) == len(checked_lines) and int(complete[2]) == len(failures),
        "pixels": [line for line in lines if "TN PIXELS " in line],
        "coordinates": [line for line in lines if "TN COORDS " in line],
        "stencil": [line for line in lines if "TN STENCIL " in line],
        "stencil_gradient": [line for line in lines if "TN STENCILGRAD " in line],
        "images": {str(path): {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in sorted(elf.parent.glob("*.rgba"))},
        "passed": p.returncode == 0 and complete is not None and int(complete[2]) == 0 and profiles_32}
    (out / "native-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("completed", "checks", "failure_count", "failures", "all_frozen_inputs_unchanged", "all_non_UI_settings_unchanged")}, indent=2))
    return int(not report["passed"])
if __name__ == "__main__": raise SystemExit(main())
