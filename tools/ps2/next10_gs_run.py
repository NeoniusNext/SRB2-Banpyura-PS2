#!/usr/bin/env python3
"""Run a GS component only through run_pcsx2, with immutable input manifests."""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
REQUIRED = {
    ("EmuCore", "EnableExtraMemory"): "false",
    ("EmuCore", "EnablePatches"): "false", ("EmuCore", "EnableCheats"): "false",
    ("EmuCore", "EnableWideScreenPatches"): "false", ("EmuCore", "EnableNoInterlacingPatches"): "false",
    ("EmuCore/CPU", "FPU.DenormalsAreZero"): "true",
    ("EmuCore/CPU/Recompiler", "EnableEE"): "true", ("EmuCore/CPU/Recompiler", "EnableEECache"): "false",
    ("EmuCore/Speedhacks", "EECycleRate"): "0", ("EmuCore/Speedhacks", "EECycleSkip"): "0",
    ("EmuCore/GS", "Renderer"): "13", ("EmuCore/GS", "VsyncEnable"): "0",
    ("EmuCore/GS", "FrameLimitEnable"): "false",
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def critical_settings(path):
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    parser.read_string(path.read_text())
    # Only frontend window/table state is excluded; every global setting stays.
    result = {section: dict(parser[section]) for section in parser.sections()
              if section not in ("UI", "GameListTableView")}
    if path.name == "PCSX2.ini":
        for (section, key), value in REQUIRED.items():
            if parser.get(section, key, fallback="").strip().lower() != value:
                raise RuntimeError("GS fixture requires the recorded standard profile: " + section + "/" + key)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kind", choices=("high", "low"), required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()
    work = args.out.resolve(); work.mkdir(parents=True, exist_ok=True)
    low = args.kind == "low"
    elf = work / ("GS_LOW10.ELF" if low else "GS_NEXT10.ELF")
    marker = "GS LOW10 DONE" if low else "GS NEXT10 DONE"
    tool = "next10_gs_low" if low else "next10_gs_hosttest"
    patterns = ("*-graph.S", "*-graph.o", "*-graph.json", "low-fixture.o", "low-*.h", "shim.h", "ps2_hwd_dbg.h", "hw_model.h", "build-report.json",
                "linked-code-report.json", "independent-matrix.json") if low else (
                    "*-engine.S", "*-engine.o", "*-extraction.json", "*-production.c", "*-production.o", "fixture.o", "common.h", "ee-build-report.json", "expected-host-oracle.json")
    inputs = {elf, ROOT / ("tools/ps2/" + tool + ".c"), ROOT / ("tools/ps2/" + tool + ".py"),
              Path(__file__).resolve(), ROOT / "tools/ps2/run_pcsx2.py", Path("/workspace/.onboarding/emulator-env.sh")}
    for pattern in patterns: inputs.update(work.glob(pattern))
    inputs.update(ROOT / "src/ps2/hw" / name for name in ("ps2_hw_priv.inc", "ps2_hw_tex.inc", "ps2_hw_gs.inc"))
    before = {str(path): sha(path) for path in sorted(inputs)}
    archive = work / "pinned-run-inputs"
    archived = {}
    for name, digest in before.items():
        path = Path(name)
        relative = path.relative_to(ROOT) if path.is_relative_to(ROOT) else Path("external") / name.lstrip("/")
        dest = archive / relative; dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, dest)
        archived[str(dest.relative_to(work))] = digest
    (work / "pinned-run-inputs.json").write_text(json.dumps(archived, indent=2) + "\n")
    profile = Path(os.environ.get("SRB2_PCSX2_ROOT", "/workspace/.onboarding/pcsx2"))
    configs = sorted(profile.glob("slot*/**/*.ini"))
    config_before = {str(path): sha(path) for path in configs}
    critical_before = {str(path): critical_settings(path) for path in configs}
    if not any(path.name == "PCSX2.ini" for path in configs):
        raise RuntimeError("No complete standard PCSX2 profile found")
    runtimes = sorted(set(path for path in profile.glob("slot*/usr/bin/pcsx2-qt") if path.is_file()))
    runtime_before = {str(path): sha(path) for path in runtimes}
    for path in configs:
        dest = work / "run-config-before" / path.relative_to(profile)
        dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, dest)
    log = work / "run.log"
    command = [sys.executable, "-B", str(ROOT / "tools/ps2/run_pcsx2.py"), "--elf", str(elf),
               "--log", str(log), "--until", marker, "--timeout", str(args.timeout)]
    pre = {"command": command, "input_sha256": before, "config_sha256": config_before,
           "critical_settings": critical_before, "emulator_binary_sha256": runtime_before,
           "emulator_environment": {k: v for k, v in os.environ.items() if k.startswith(("PCSX2", "SRB2_PCSX2"))}}
    (work / "run-pre.json").write_text(json.dumps(pre, indent=2) + "\n")
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    (work / "orchestration.log").write_text(result.stdout + result.stderr)
    after = {str(path): sha(path) for path in sorted(inputs)}
    critical_after = {str(path): critical_settings(path) for path in configs if path.exists()}
    runtime_after = {str(path): sha(path) for path in runtimes}
    for path in configs:
        if path.exists():
            dest = work / "run-config-after" / path.relative_to(profile)
            dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, dest)
    post = {"command": command, "rc": result.returncode, "input_sha256": after,
            "all_frozen_inputs_unchanged": before == after, "log_sha256": sha(log) if log.exists() else None,
            "config_sha256": {str(path): sha(path) for path in configs if path.exists()},
            "critical_settings": critical_after, "critical_settings_unchanged": critical_before == critical_after,
            "emulator_binary_sha256": runtime_after, "emulator_binary_unchanged": runtime_before == runtime_after}
    (work / "run-post.json").write_text(json.dumps(post, indent=2) + "\n")
    if not low:
        (work / "run-metadata.json").write_text(json.dumps({
            "command": command, "rc": result.returncode, "elf_sha256_before": before[str(elf)],
            "elf_sha256_after": after[str(elf)], "log_sha256": post["log_sha256"]}, indent=2) + "\n")
    print(result.stdout + result.stderr)
    if before != after:
        raise SystemExit("Frozen GS native inputs changed during execution")
    if critical_before != critical_after or runtime_before != runtime_after:
        raise SystemExit("Critical emulator settings or binary changed during execution")
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
