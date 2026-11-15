#!/usr/bin/env python3
"""Prepare the exact old header overlay in an isolated eleventh-pass snapshot.

The complete 602-file source baseline is verified before it is copied. No
production source is edited, and no EE compiler or emulator is started.
"""
import argparse
import hashlib
import json
from pathlib import Path

from next9_io_variant import BASE_SHA, CANDIDATE_SHA, prepare

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def write_once(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        assert path.read_bytes() == data, "different existing artifact: " + str(path)
    else:
        path.write_bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next11/baseline-src")
    ap.add_argument("--manifest", type=Path, default=ROOT / "tools/ps2/next11_baseline.json")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next11/io/candidate-src")
    a = ap.parse_args()
    a.baseline, a.out = a.baseline.resolve(), a.out.resolve()
    assert a.out.is_relative_to(ROOT / "build") and a.out != a.baseline
    baseline = json.loads(a.manifest.read_text())
    assert len(baseline["source_sha256"]) == 602
    assert baseline["source_sha256"]["src/w_pack.c"] == BASE_SHA
    for relative, expected in baseline["source_sha256"].items():
        assert sha(a.baseline / relative) == expected, "frozen source differs: " + relative
    candidate = prepare(a.out.parent / "overlay/w_pack.c", a.baseline / "src/w_pack.c")
    copied = {}
    for directory in ("src", "tools"):
        for source in sorted((a.baseline / directory).rglob("*")):
            if not source.is_file() or "__pycache__" in source.parts:
                continue
            relative = source.relative_to(a.baseline)
            data = candidate.read_bytes() if relative.as_posix() == "src/w_pack.c" else source.read_bytes()
            write_once(a.out / relative, data)
            copied[relative.as_posix()] = hashlib.sha256(data).hexdigest()
    configs = {}
    for renderer in ("software", "hardware"):
        source = ROOT / "build/nov15-next11/baseline" / renderer / "gen/config.h"
        relative = Path("build") / renderer / "gen/config.h"
        write_once(a.out / relative, source.read_bytes())
        configs[renderer] = {"source": str(source), "snapshot": str(a.out / relative), "sha256": sha(source)}
    host = ROOT / "build/host-gen/config.h"
    write_once(a.out / "build/host-gen/config.h", host.read_bytes())
    configs["host"] = {"source": str(host), "sha256": sha(host)}
    assert sum(name.startswith("src/") for name in copied) == 602
    changes = [name for name, old in baseline["source_sha256"].items() if copied[name] != old]
    assert changes == ["src/w_pack.c"] and copied[changes[0]] == CANDIDATE_SHA
    report = {"baseline_revision": baseline["revision"], "baseline_manifest": str(a.manifest.resolve()),
              "baseline_manifest_sha256": sha(a.manifest), "baseline_source": str(a.baseline),
              "snapshot": str(a.out), "source_files": 602, "changed_sources": changes,
              "baseline_pack_sha256": BASE_SHA, "candidate_pack_sha256": CANDIDATE_SHA,
              "snapshot_sha256": copied, "generated_configs": configs,
              "production_sources_edited": False, "ee_compiler_invoked": False,
              "checker_sha256": sha(Path(__file__))}
    write_once(a.out / "manifest.json", (json.dumps(report, indent=2) + "\n").encode())
    print(json.dumps({key: report[key] for key in ("snapshot", "source_files", "changed_sources", "candidate_pack_sha256")}))


if __name__ == "__main__":
    main()
