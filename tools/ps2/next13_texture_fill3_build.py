#!/usr/bin/env python3
"""Freeze all accepted sources plus only the row-filler candidate overlay."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from next13_texture_fill3_variant import ROOT, PATH, rewrite


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def hashes(root):
    return {str(p.relative_to(root)): sha(p) for p in sorted((root / "src").rglob("*")) if p.is_file()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--base-src", type=Path, default=ROOT / "build/nov15-next12/final/source/src")
    p.add_argument("--candidate-src", type=Path, default=ROOT / "build/nov15-next13/texture-fill3-classic/overlay-src")
    p.add_argument("--freeze-only", action="store_true")
    args = p.parse_args()
    work = args.out.resolve()
    snapshot = work / "measured-source"
    before = hashes(args.base_src.resolve().parent)
    if not snapshot.exists():
        baseline = (args.base_src / PATH).read_text()
        if (args.candidate_src / PATH).read_text() != rewrite(baseline):
            raise RuntimeError("Overlay is not exactly the dec_level variant")
        shutil.copytree(args.base_src, snapshot / "src")
        shutil.copy2(args.candidate_src / PATH, snapshot / "src" / PATH)
        (snapshot / "tools/ps2").mkdir(parents=True)
        names = ("build.py", "sources.txt", "sources_hw.txt", "opt_units.txt")
        for name in names:
            shutil.copy2(ROOT / "tools/ps2" / name, snapshot / "tools/ps2" / name)
        frozen = hashes(snapshot)
        changed = [name for name in before if before[name] != frozen[name]]
        if changed != ["src/" + str(PATH)] or len(frozen) != 602 or before != hashes(args.base_src.resolve().parent):
            raise RuntimeError("Complete baseline/overlay isolation failed")
        (work / "candidate-source-freeze.json").write_text(json.dumps({
            "baseline_src": str(args.base_src.resolve()), "source_root": str(snapshot),
            "source_file_count": len(frozen), "source_sha256": frozen,
            "only_baseline_differences": changed,
            "tools_sha256": {name: sha(snapshot / "tools/ps2" / name) for name in names},
        }, indent=2) + "\n")
    manifest = json.loads((work / "candidate-source-freeze.json").read_text())
    if manifest["source_sha256"] != hashes(snapshot):
        raise RuntimeError("Immutable candidate source changed")
    if args.freeze_only:
        print("Froze complete 602-file candidate: " + str(snapshot))
        return
    if (work / "candidate-hardware").exists():
        raise RuntimeError("Fresh actual candidate build required")
    environment = dict(os.environ)
    extra = {k: v for k, v in environment.items() if k.startswith("SRB2_PS2_")}
    if extra:
        raise RuntimeError("Default whole build rejects experiment environment: " + repr(extra))
    environment.update(SRB2_PS2_HW="1", SRB2_PS2_OUT=str(work / "candidate-hardware"))
    command = [sys.executable, "-B", str(snapshot / "tools/ps2/build.py"), "--jobs", "1"]
    result = subprocess.run(command, cwd=ROOT, env=environment, text=True, capture_output=True)
    (work / "candidate-full-build.log").write_text(result.stdout + result.stderr)
    print((result.stdout + result.stderr)[-2000:])
    if result.returncode:
        raise RuntimeError("Whole default-LTO candidate build failed")


if __name__ == "__main__":
    main()
