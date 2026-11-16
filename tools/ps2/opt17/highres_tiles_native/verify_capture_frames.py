#!/usr/bin/env python3
"""Verify complete saved native GS CT32 identity and continuous-alpha wipes."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--frames", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True); a = ap.parse_args()
    report = {"scope": "Host verification of complete actual native GS readbacks. Identity compares all CT32 RGB and alpha bits; wipe expectation derives independently from the two actual rendered source frames and the GS Ad=64 equation. This is not a CPU tile model.", "modes": [], "passed": True}
    for w, h in ((640, 400), (640, 480), (640, 512)):
        paths = {"a": a.frames / f"tn-highres-original-a-{w}x{h}.rgba", "b": a.frames / f"tn-highres-original-b-{w}x{h}.rgba", "identity": a.frames / f"tn-highres-restored-identity-{w}x{h}.rgba", "wipe": a.frames / f"tn-highres-wipe-{w}x{h}-{w}x{h}.rgba"}
        arrays = {k: np.fromfile(p, dtype="<u4").reshape(h, w) for k, p in paths.items()}
        expected = np.full((h, w), 0x80000000, dtype=np.uint32)
        for c in range(3):
            ca = ((arrays["a"] >> (8 * c)) & 255).astype(np.int16)
            cb = ((arrays["b"] >> (8 * c)) & 255).astype(np.int16)
            expected |= ((ca + ((cb - ca) * 64 // 128)).astype(np.uint32) << (8 * c))
        row = {"visible": [w, h], "compared_pixels_per_check": w * h, "bytes_per_CT32_frame": w * h * 4,
            "identity_different_RGBA_pixels": int((arrays["a"] != arrays["identity"]).sum()),
            "wipe_different_RGBA_pixels": int((arrays["wipe"] != expected).sum()),
            "source_alpha_values": np.unique(arrays["a"] >> 24).tolist(),
            "wipe_alpha_values": np.unique(arrays["wipe"] >> 24).tolist(),
            "artifacts": {k: {"path": str(p.resolve()), "sha256": sha(p)} for k, p in paths.items()}}
        row["passed"] = row["identity_different_RGBA_pixels"] == row["wipe_different_RGBA_pixels"] == 0
        report["passed"] &= row["passed"]; report["modes"].append(row)
    a.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "modes": [{k: row[k] for k in ("visible", "compared_pixels_per_check", "identity_different_RGBA_pixels", "wipe_different_RGBA_pixels")} for row in report["modes"]]}, indent=2))
    return int(not report["passed"])
if __name__ == "__main__": raise SystemExit(main())
