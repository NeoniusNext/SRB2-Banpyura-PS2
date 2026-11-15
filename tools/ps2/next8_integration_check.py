"""Compare complete eighth-pass gameplay frames and loader hashes on 32 MiB EE.

Uses the existing emulator wrapper only. Baseline ELFs must be built from the
frozen preceding source; no BIOS or game assets are supplied by this script.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
SCENARIOS = (("sw-fog-stereo", "MAP01"), ("sw-map11", "MAP11"),
             ("hw-fog-stereo", "MAP01"), ("hw-map02", "MAP02"))


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inspect(run, name):
    summary = json.loads((run / "summary.json").read_text())
    boot = (run / "boot.txt").read_text(errors="replace")
    errors = []
    if summary["rc"] or summary["ram"] != 32:
        errors.append("run failed or RAM configuration differs")
    if re.search(r"I_Error|HEAP CHECK FAILED|Out of memory|ERROR: Not enough memory|^ERROR:", boot, re.M):
        errors.append("fatal/heap error")
    warnings = [line for line in boot.splitlines() if "WARNING:" in line]
    if warnings:
        errors.append("unexpected warning")
    if "-hw-" in name and ("HWE startup renderer=Hardware chosen=2 hardware=1" not in boot
                           or "Switching to the Software renderer" in boot):
        errors.append("native Hardware rendering absent")
    if "fog-stereo" in name and not all(marker in boot for marker in (
            "NEXT5 NPOT SPRITES READY", "NEXT7 FOG SOLID READY", "type=3", "NEXT5 STEREO SFX event=4")):
        errors.append("scripted drawing/MIDI/stereo requests absent")
    underruns = list(map(int, re.findall(r"underruns=(\d+)", boot)))
    if any(underruns) or any(map(int, re.findall(r"(?:hmis|herr)=(\d+)", boot))):
        errors.append("audio underrun/hash error")
    if name.endswith("spawn-175") and (not summary["done"] or not summary["zstat"]
            or summary["zstat"][-1].get("levelframes") != "175"):
        errors.append("175-frame completion absent")
    checkpoints = {}
    for key, values in re.findall(r"^\[zck\]\s+(\S+)\s+(.*)$", boot, re.M):
        checkpoints[key] = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", values)}
    return {"rc": summary["rc"], "elf_sha256": sha(run / "SRB2.ELF"),
            "captures": {path.name: sha(path) for path in sorted(run.glob("vidshot-*.ppm"))},
            "lhash": re.findall(r"^LHASH .+$", boot, re.M), "zck": checkpoints,
            "zstat": summary["zstat"], "warnings": warnings, "audio_underruns": underruns,
            "errors": errors}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next8")
    ap.add_argument("--baseline-sw", type=Path, default=ROOT / "build/nov15-next8/baseline/software/SRB2.ELF")
    ap.add_argument("--baseline-hw", type=Path, default=ROOT / "build/nov15-next8/baseline/hardware/SRB2.ELF")
    ap.add_argument("--software", type=Path, default=ROOT / "build/ps2/SRB2.ELF")
    ap.add_argument("--hardware", type=Path, default=ROOT / "build/nov15-next8/hardware/SRB2.ELF")
    ap.add_argument("--pak", type=Path, default=ROOT / "build/pak")
    ap.add_argument("--npot-addon", type=Path, default=ROOT / "build/nov14-next5/npot-addons/NX5_FLOOR.pk3")
    ap.add_argument("--fog-addon", type=Path, default=ROOT / "build/nov14-next7/fog-addons/NX7_FOG.pk3")
    ap.add_argument("--analyze-only", action="store_true")
    ap.add_argument("--reuse-baseline", action="store_true",
                    help="inspect existing base-* runs against the exact ELF/args/assets/fixtures instead of rerunning them")
    a = ap.parse_args()
    a.out = a.out.resolve()
    if not a.out.is_relative_to(ROOT / "build") or a.out == ROOT / "build":
        ap.error("--out must be a child of the repository build directory")
    a.out.mkdir(parents=True, exist_ok=True)
    if not a.analyze_only and not a.npot_addon.is_file():
        subprocess.run([sys.executable, "-B", str(TOOLS / "next5_render_npot_fixture.py"),
                        "--out", str(a.npot_addon.parent)], cwd=ROOT, check=True)
    if not a.analyze_only and not a.fog_addon.is_file():
        subprocess.run([sys.executable, "-B", str(TOOLS / "next7_render_fog_fixture.py"),
                        "--out", str(a.fog_addon.parent)], cwd=ROOT, check=True)
    entries = []
    for scenario, mapname in SCENARIOS:
        hardware = scenario.startswith("hw-")
        for version, elf in (("base", a.baseline_hw if hardware else a.baseline_sw),
                             ("new", a.hardware if hardware else a.software)):
            name = version + "-" + scenario
            cmd = [sys.executable, "-B", str(TOOLS / "opt_run.py"), "--name", name,
                   "--elf", str(elf.resolve()), "--pak", str(a.pak.resolve()),
                   "--out", str(a.out / "runs"), "--map", mapname, "--timeout", "180",
                   "--until", "VIDSHOT COMPLETE 3"]
            scene = "fog-stereo" in scenario
            if scene:
                cmd += ["--home-file", str(a.npot_addon.resolve()) + "=NX5_FLOOR.pk3",
                        "--home-file", str(TOOLS / "next5_audio_integration.lua") + "=NX5_AUDIO.lua",
                        "--home-file", str(a.fog_addon.resolve()) + "=NX7_FOG.pk3"]
            cmd += ["--", "-singletics"]
            if scene:
                cmd += ["-keepspawn", "-nodigmusic"]
            cmd += ["-loadprof", "-loadhash", "-zck", "-zheap", "35", "-astat"]
            if scene:
                cmd += ["-file", "host:/.srb2/NX5_FLOOR.pk3", "host:/.srb2/NX7_FOG.pk3", "host:/.srb2/NX5_AUDIO.lua"]
            cmd += ["-vidshot", "l20,l70,l140"]
            if hardware:
                cmd += ["-renderer", "Hardware"]
            entries.append((name, elf.resolve(), cmd))
    for version, elf in (("base", a.baseline_sw), ("new", a.software)):
        name = version + "-sw-map11-spawn-175"
        entries.append((name, elf.resolve(), [sys.executable, "-B", str(TOOLS / "opt_run.py"),
            "--name", name, "--elf", str(elf.resolve()), "--pak", str(a.pak.resolve()),
            "--out", str(a.out / "runs"), "--map", "MAP11", "--timeout", "180", "--",
            "-singletics", "-keepspawn", "-loadprof", "-loadhash", "-zck", "-zheap", "1",
            "-zstack", "-zquit", "175", "-astat"]))
    for version, elf in (("base", a.baseline_sw), ("new", a.software)):
        name = version + "-sw-map11-pressure-frame"
        entries.append((name, elf.resolve(), [sys.executable, "-B", str(TOOLS / "opt_run.py"),
            "--name", name, "--elf", str(elf.resolve()), "--pak", str(a.pak.resolve()),
            "--out", str(a.out / "runs"), "--map", "MAP11", "--timeout", "180",
            "--until", "VIDSHOT COMPLETE 1", "--", "-singletics", "-keepspawn",
            "-loadprof", "-loadhash", "-zck", "-zheap", "1", "-zstack", "-astat",
            "-vidshot", "l175"]))
    asset_names = sorted(p.name for p in a.pak.glob("*.PAK"))
    if (a.pak / "FINEACON.DAT").is_file():
        asset_names.append("FINEACON.DAT")
    if not asset_names:
        ap.error("--pak contains no stock asset files")
    asset_hashes = {name: sha(a.pak / name) for name in asset_names}
    report = {"runs": {}, "pairs": [], "errors": [], "asset_sha256": asset_hashes}
    for name, elf, cmd in entries:
        run = a.out / "runs" / name
        reused_baseline = a.reuse_baseline and name.startswith("base-")
        if not a.analyze_only and not reused_baseline:
            if run.exists():
                ap.error("Run already exists; choose a fresh --out or use --analyze-only: " + str(run))
            print("START", name, flush=True)
            result = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
            (a.out / (name + ".orchestration.log")).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(name + ": wrapper exit " + str(result.returncode))
        item = inspect(run, name)
        item["reused_baseline"] = reused_baseline
        if item["elf_sha256"] != sha(elf):
            item["errors"].append("staged ELF differs from selected binary")
        for asset, expected_hash in asset_hashes.items():
            staged = run / asset
            if not staged.is_file() or (not staged.samefile(a.pak / asset)
                                       and sha(staged) != expected_hash):
                item["errors"].append("staged stock asset differs: " + asset)
        mapname = cmd[cmd.index("--map") + 1]
        expected_args = ["-config", "reference.cfg", "-nolog", "-noendtxt",
                         "-skipintro", "-warp", mapname, *cmd[cmd.index("--") + 1:]]
        if (run / "ps2args").read_text().splitlines() != expected_args:
            item["errors"].append("staged engine arguments differ")
        if "fog-stereo" in name:
            for source, target in ((a.npot_addon, "NX5_FLOOR.pk3"),
                                   (TOOLS / "next5_audio_integration.lua", "NX5_AUDIO.lua"),
                                   (a.fog_addon, "NX7_FOG.pk3")):
                if (run / ".srb2" / target).read_bytes() != source.read_bytes():
                    item["errors"].append("staged scene fixture differs: " + target)
        report["runs"][name] = item
        report["errors"] += [name + ": " + error for error in item["errors"]]
    for scenario, _ in (*SCENARIOS, ("sw-map11-pressure-frame", "MAP11")):
        before, after = (report["runs"][version + "-" + scenario] for version in ("base", "new"))
        expected_frames = 1 if scenario.endswith("pressure-frame") else 3
        equal_frames = len(before["captures"]) == expected_frames and before["captures"] == after["captures"]
        equal_hashes = bool(before["lhash"]) and before["lhash"] == after["lhash"]
        if not equal_frames or not equal_hashes:
            report["errors"].append(scenario + ": whole-frame/loader-hash mismatch")
        report["pairs"].append({"scenario": scenario, "captures": len(before["captures"]),
            "captures_equal": equal_frames, "lhash_equal": equal_hashes,
            "loaded_used_delta": after["zck"]["map-loaded"]["used"] - before["zck"]["map-loaded"]["used"]})
    stress_base = report["runs"]["base-sw-map11-spawn-175"]
    stress_new = report["runs"]["new-sw-map11-spawn-175"]
    if not stress_base["lhash"] or stress_base["lhash"] != stress_new["lhash"]:
        report["errors"].append("stress: loader-hash mismatch")
    report["stress"] = {"lhash_equal": stress_base["lhash"] == stress_new["lhash"],
                        "baseline": stress_base["zstat"], "current": stress_new["zstat"]}
    (a.out / "next8-integration-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"runs": len(report["runs"]), "pairs": report["pairs"], "errors": report["errors"]}, indent=2))
    return bool(report["errors"])


if __name__ == "__main__":
    raise SystemExit(main())
