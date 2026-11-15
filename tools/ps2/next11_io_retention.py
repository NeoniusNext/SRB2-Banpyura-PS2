#!/usr/bin/env python3
"""Run frozen complete pack/index/device and recovery-readiness host suites.

The copied historical tools derive their ROOT from the isolated candidate
snapshot. They therefore compile that reader and its frozen project headers.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--snapshot", type=Path, default=ROOT / "build/nov15-next11/io/candidate-src")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next11/io/retention")
    ap.add_argument("--pak", type=Path, default=ROOT / "build/pak")
    ap.add_argument("--assets", type=Path, default=Path("/workspace/.onboarding/srb2-assets"))
    ap.add_argument("--python", type=Path, default=Path("/workspace/.onboarding/assets-venv/bin/python3"))
    a = ap.parse_args()
    a.out, a.snapshot = a.out.resolve(), a.snapshot.resolve()
    assert a.out.is_relative_to(ROOT / "build")
    a.out.mkdir(parents=True, exist_ok=True)
    manifest_path = a.snapshot / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    hashes = {str(a.snapshot / name): expected for name, expected in manifest["snapshot_sha256"].items()}
    hashes.update({str(a.snapshot / "build" / mode / "gen/config.h"): row["sha256"]
                   for mode, row in manifest["generated_configs"].items() if mode != "host"})
    hashes[str(a.snapshot / "build/host-gen/config.h")] = manifest["generated_configs"]["host"]["sha256"]
    assert all(sha(Path(path)) == expected for path, expected in hashes.items())
    env = dict(os.environ, PS2DEV="/workspace/.onboarding/ps2dev-v2/ps2dev",
               PS2SDK="/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk", PYTHONDONTWRITEBYTECODE="1")
    jobs = [
        ("pack-corpus", [a.python, "-B", a.snapshot / "tools/ps2/test_pack_reader.py", "--pak", a.pak.resolve(),
                         "--src", a.assets.resolve(), "--out", a.out / "pack-corpus"], 240),
        ("pack-ready-asan", [a.python, "-B", a.snapshot / "tools/ps2/next7_pack_ready_hosttest.py",
                             "--sanitize", "--negative-controls", "--out", a.out / "pack-ready-asan"], 90),
    ]
    result = {"snapshot_manifest_sha256": sha(manifest_path), "reader_sha256": sha(a.snapshot / "src/w_pack.c"),
              "checker_sha256": sha(Path(__file__)), "runs": {}}
    for name, command, timeout in jobs:
        command = list(map(str, command))
        p = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout)
        log = a.out / (name + ".log")
        log.write_text(p.stdout + p.stderr)
        assert p.returncode == 0, (name, p.returncode, p.stdout[-3000:], p.stderr[-3000:])
        if name == "pack-corpus":
            assert "TOTAL differences/failures: 0" in p.stdout and "9 packs/archives/sidecars hashed before and after, 0 changed" in p.stdout
        result["runs"][name] = {"command": command, "returncode": p.returncode, "log_sha256": sha(log),
            "evidence_sha256": {str(path.relative_to(a.out / name)): sha(path)
                                for path in sorted((a.out / name).rglob("*")) if path.is_file()}}
        print(name, "passed", flush=True)
    assert all(sha(Path(path)) == expected for path, expected in hashes.items()), "frozen snapshot changed"
    result["snapshot_unchanged"] = True
    result["ee_compiler_invoked"] = False
    (a.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
