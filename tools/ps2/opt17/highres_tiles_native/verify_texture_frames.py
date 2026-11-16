#!/usr/bin/env python3
"""Compare every bit in paired complete native GS texture frames."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np

def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument("--frames", type=Path, required=True); ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(); rows = []
    for source in sorted(a.frames.glob("tn-texture-*-v0-320x200.rgba")):
        tiled = source.with_name(source.name.replace("-v0-", "-v1-"))
        ref = np.fromfile(source, dtype="<u4"); got = np.fromfile(tiled, dtype="<u4")
        if ref.size != 64000 or got.shape != ref.shape: raise ValueError("Incomplete native frame pair")
        delta = np.abs((ref >> 24).astype(np.int16) - (got >> 24).astype(np.int16))
        rows.append({"case": source.name.split("-v0-")[0], "compared": int(ref.size), "different_RGBA": int((ref != got).sum()), "different_alpha": int((delta != 0).sum()), "max_alpha_delta": int(delta.max()), "source_readback_sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "tiled_readback_sha256": hashlib.sha256(tiled.read_bytes()).hexdigest()})
    if not rows: raise ValueError("No paired native texture readbacks")
    report = {"scope": "Complete actual native GS CT32 readbacks, including all alpha bits; no CPU tile model. Original strict reference differences remain recorded.", "cases": rows, "total_compared": sum(r["compared"] for r in rows), "different_alpha": sum(r["different_alpha"] for r in rows), "different_RGBA": sum(r["different_RGBA"] for r in rows)}
    a.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "cases"}, indent=2))
if __name__ == "__main__": main()
