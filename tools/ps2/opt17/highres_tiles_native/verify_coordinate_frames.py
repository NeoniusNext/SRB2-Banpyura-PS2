#!/usr/bin/env python3
"""Decode opaque grayscale source coordinates from actual native GS frames."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--frames", type=Path, required=True); ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(); report = {"scope": "Independent decode of four complete actual GS P8 readbacks per variant (Xlow, Xhigh, Ylow, Yhigh). Compared native full resident versus native tiled source coordinates, circular wrap distance, and opaque coverage. This bounds sampling differences; original strict RGB reports remain unchanged.", "source": [1024, 512], "frame": [320, 200], "cases": [], "passed": True}
    for shape in (0, 3):
        coords, covers, recipes, inconsistent, artifacts = [], [], 0, 0, {}
        for variant in (0, 1):
            parts, masks = [], []
            for component in range(4):
                p = a.frames / f"tn-coordinate-shape{shape}-part{component}-v{variant}-320x200.rgba"
                raw = np.fromfile(p, dtype="<u4").reshape(200, 320)
                mask = (raw & 0xffffff) != 0x352b1d
                red = (raw & 255).astype(np.int32)
                recipes += int((mask & ((red != ((raw >> 8) & 255)) | (red != ((raw >> 16) & 255)) | ((raw >> 24) != 128))).sum())
                parts.append(red); masks.append(mask)
                artifacts[str(p.resolve())] = hashlib.sha256(p.read_bytes()).hexdigest()
            cover = masks[0]
            inconsistent += sum(int((other != cover).sum()) for other in masks[1:])
            coords.append((parts[0] | (parts[1] << 8), parts[2] | (parts[3] << 8))); covers.append(cover)
        both = covers[0] & covers[1]; dx = np.abs(coords[0][0] - coords[1][0]); dy = np.abs(coords[0][1] - coords[1][1])
        invalid = int(sum((cover & ((xy[0] >= 1024) | (xy[1] >= 512))).sum() for cover, xy in zip(covers, coords)))
        dx = np.minimum(dx, 1024 - dx); dy = np.minimum(dy, 512 - dy)
        row = {"shape": shape, "compared_frame_pixels": 64000, "covered_in_both": int(both.sum()), "missing_coverage": int((covers[0] & ~covers[1]).sum()), "extra_coverage": int((~covers[0] & covers[1]).sum()), "component_coverage_inconsistent": inconsistent, "bad_grayscale_or_alpha_recipe": recipes, "invalid_decoded_coordinates": invalid, "changed_source_coordinates": int((both & ((dx != 0) | (dy != 0))).sum()), "max_circular_dx": int(dx[both].max()), "max_circular_dy": int(dy[both].max()), "beyond_one_source_texel": int((both & ((dx > 1) | (dy > 1))).sum()), "artifact_sha256": artifacts}
        row["passed"] = all(row[k] == 0 for k in ("missing_coverage", "extra_coverage", "component_coverage_inconsistent", "bad_grayscale_or_alpha_recipe", "invalid_decoded_coordinates", "beyond_one_source_texel"))
        report["passed"] &= row["passed"]; report["cases"].append(row)
    a.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "cases": [{k: v for k, v in row.items() if k != "artifact_sha256"} for row in report["cases"]]}, indent=2))
    return int(not report["passed"])
if __name__ == "__main__": raise SystemExit(main())
