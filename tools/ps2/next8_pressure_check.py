"""Extend the unchanged MAP11 memory-pressure trace for both exact ELFs."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from next8_integration_check import inspect, sha, ROOT, TOOLS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov15-next8")
    parser.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next8/baseline/software/SRB2.ELF")
    parser.add_argument("--software", type=Path, default=ROOT / "build/ps2/SRB2.ELF")
    parser.add_argument("--pak", type=Path, default=ROOT / "build/pak")
    parser.add_argument("--frames", type=int, default=350)
    parser.add_argument("--analyze-only", action="store_true")
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / "build") or out == ROOT / "build" or args.frames <= 0:
        parser.error("choose a build child directory and a positive frame count")
    out.mkdir(parents=True, exist_ok=True)
    game_args = ["-singletics", "-keepspawn", "-loadprof", "-loadhash", "-zck", "-zheap", "1",
                 "-zstack", "-zquit", str(args.frames), "-astat"]
    expected_args = ["-config", "reference.cfg", "-nolog", "-noendtxt", "-skipintro", "-warp", "MAP11", *game_args]
    names = sorted(p.name for p in args.pak.glob("*.PAK")) + ["FINEACON.DAT"]
    assets = {name: sha(args.pak / name) for name in names}
    report = {"frames": args.frames, "runs": {}, "errors": [], "asset_sha256": assets}
    for version, elf in (("base", args.baseline), ("new", args.software)):
        name = version + "-sw-map11-spawn-" + str(args.frames)
        run = out / "runs" / name
        if not args.analyze_only:
            if run.exists():
                parser.error("run exists: choose another --out or --analyze-only")
            command = [sys.executable, "-B", str(TOOLS / "opt_run.py"), "--name", name,
                       "--elf", str(elf.resolve()), "--pak", str(args.pak.resolve()),
                       "--out", str(out / "runs"), "--map", "MAP11", "--timeout", "300", "--", *game_args]
            print("START", name, flush=True)
            process = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            (out / (name + ".orchestration.log")).write_text(process.stdout + process.stderr)
            if process.returncode:
                raise RuntimeError(name + ": wrapper exit " + str(process.returncode))
        item = inspect(run, name)
        if item["elf_sha256"] != sha(elf) or (run / "ps2args").read_text().splitlines() != expected_args:
            item["errors"].append("staged ELF/arguments differ")
        for asset, digest in assets.items():
            staged = run / asset
            if not staged.is_file() or (not staged.samefile(args.pak / asset) and sha(staged) != digest):
                item["errors"].append("staged asset differs: " + asset)
        if not item["zstat"] or item["zstat"][-1].get("levelframes") != str(args.frames):
            item["errors"].append("requested frame count not reached")
        report["runs"][version] = item
        report["errors"] += [name + ": " + error for error in item["errors"]]
    report["lhash_equal"] = bool(report["runs"]["base"]["lhash"]) and report["runs"]["base"]["lhash"] == report["runs"]["new"]["lhash"]
    if not report["lhash_equal"]:
        report["errors"].append("loader hashes differ")
    (out / ("next8-pressure-" + str(args.frames) + "-report.json")).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"frames": args.frames, "errors": report["errors"], "lhash_equal": report["lhash_equal"]}))
    return bool(report["errors"])


if __name__ == "__main__":
    raise SystemExit(main())
