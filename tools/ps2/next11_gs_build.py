#!/usr/bin/env python3
"""Freeze all 602 baseline sources plus only the one GS candidate overlay."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def hashes(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted((root / "src").rglob("*")) if p.is_file()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next11/gs")
    ap.add_argument("--base-src", type=Path, default=ROOT / "build/nov15-next11/baseline-src/src")
    ap.add_argument("--candidate-src", type=Path, default=ROOT / "src",
                    help="Overlay source root; use the preserved measured-source/src after rejection")
    ap.add_argument("--freeze-only", action="store_true")
    args = ap.parse_args()
    work = args.out.resolve()
    snapshot = work / "measured-source"
    candidate_src = args.candidate_src.resolve()
    if not snapshot.exists():
        baseline_root = args.base_src.resolve().parent
        before = hashes(baseline_root)
        shutil.copytree(args.base_src, snapshot / "src")
        overlays = {}
        for name in ("ps2_hw_tex.inc",):
            relative = "src/ps2/hw/" + name
            overlay = candidate_src / relative.removeprefix("src/")
            overlays[relative] = hashlib.sha256(overlay.read_bytes()).hexdigest()
            shutil.copy2(overlay, snapshot / relative)
        (snapshot / "tools/ps2").mkdir(parents=True)
        for name in ("build.py", "sources.txt", "sources_hw.txt", "opt_units.txt"):
            shutil.copy2(ROOT / "tools/ps2" / name, snapshot / "tools/ps2" / name)
        frozen = hashes(snapshot)
        differences = {name for name in before if before[name] != frozen[name]}
        if differences != set(overlays) or before != hashes(baseline_root) or any(
                overlays[name] != hashlib.sha256((candidate_src / name.removeprefix("src/")).read_bytes()).hexdigest() for name in overlays):
            raise RuntimeError("Baseline/overlay changed or isolation failed during the complete snapshot")
        (work / "candidate-source-freeze.json").write_text(json.dumps({
            "source_root": str(snapshot), "source_file_count": len(frozen),
            "source_sha256": frozen,
            "baseline_src": str(args.base_src.resolve()), "only_baseline_differences": sorted(differences),
            "overlay_source_root": str(candidate_src),
            "tools_sha256": {name: hashlib.sha256((snapshot / "tools/ps2" / name).read_bytes()).hexdigest()
                             for name in ("build.py", "sources.txt", "sources_hw.txt", "opt_units.txt")},
        }, indent=2) + "\n")
    manifest = json.loads((work / "candidate-source-freeze.json").read_text())
    if manifest["source_sha256"] != hashes(snapshot):
        raise RuntimeError("Frozen candidate source changed")
    if args.freeze_only:
        print("Froze " + str(manifest["source_file_count"]) + " source files: " + str(snapshot))
        return
    environment = dict(os.environ, SRB2_PS2_HW="1", SRB2_PS2_OUT=str(work / "candidate-hardware"))
    overrides = {key: environment[key] for key in environment if key.startswith("SRB2_PS2_")
                 and key not in ("SRB2_PS2_HW", "SRB2_PS2_OUT")}
    if overrides:
        raise RuntimeError("Default build requires no experiment overrides: " + repr(overrides))
    command = [sys.executable, "-B", str(snapshot / "tools/ps2/build.py"), "--jobs", "1"]
    result = subprocess.run(command, cwd=ROOT, env=environment, text=True, capture_output=True)
    (work / "candidate-full-build.log").write_text(result.stdout + result.stderr)
    print((result.stdout + result.stderr)[-2000:])
    if result.returncode:
        raise RuntimeError("Default full-LTO candidate build failed")


if __name__ == "__main__":
    main()
