#!/usr/bin/env python3
"""Prove map-twin poisoning using actual full-source-to-mip cache generation."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent; ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from transparency_hosttest import prepare, execute


def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument("--source-root", type=Path, default=ROOT); ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(); source = a.source_root.resolve(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    report = {"scope": __doc__, "source_root": str(source), "fixture_sha256": hashlib.sha256((HERE / "transparency_map_twin_hosttest.c").read_bytes()).hexdigest(), "variants": {}}
    for name, fixed in (("baseline", False), ("fixed", True)):
        work = out / name; inputs = prepare(work, source, fixed)
        (work / "fixture_base.c").write_bytes((HERE / "transparency_hosttest.c").read_bytes())
        (work / "fixture.c").write_bytes((HERE / "transparency_map_twin_hosttest.c").read_bytes())
        results = {mode: execute(work, sanitize, packed) for mode, sanitize, packed in (("normal", False, False), ("sanitize", True, False), ("packed", False, True), ("packed_sanitize", True, True))}
        for r in results.values():
            if r["rows"] != results["normal"]["rows"] or r["summary"] != results["normal"]["summary"]: raise RuntimeError("Map sequence ABI/sanitizer mismatch")
        report["variants"][name] = {"source_inputs": inputs, "results": results}
    before = report["variants"]["baseline"]["results"]["normal"]; after = report["variants"]["fixed"]["results"]["normal"]
    if before["summary"]["rgba_bad"] != 2 or after["summary"]["rgba_bad"] or after["summary"]["poisoned"]: raise RuntimeError("Actual map sequence did not expose/repair poisoning")
    for old, new in zip(before["rows"], after["rows"]):
        for key in ("case", "step", "w", "h", "ff", "index_crc", "expected_rgba_crc"):
            if old[key] != new[key]: raise RuntimeError("Map texture bytes or oracle changed")
    report["index_crc_pairs_exact"] = len(after["rows"])
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"baseline": before["summary"], "fixed": after["summary"], "index_crc_pairs_exact": report["index_crc_pairs_exact"], "out": str(out / "report.json")}, indent=2))


if __name__ == "__main__": main()
