#!/usr/bin/env python3
"""Lossless full-frame PNG views and spatial metrics of paired GS readbacks."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--frames", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True); a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    report = {"scope": "Every input is a complete native GS CT32 readback. PNGs preserve the complete original dimensions; absolute-difference RGB values are unscaled.", "cases": {}}
    for label, stem in (("p8-nearest", "texture-f0-1024x512-s0-l0"), ("p8-perspective", "texture-f0-1024x512-s3-l1"), ("rgba-bilinear", "texture-f2-512x256-s2-l1"), ("rgba-bilinear-aligned", "texture-f2-512x256-s6-l1"), ("p8-bilinear-aligned", "texture-f0-1024x512-s6-l1"), ("p8-minified-nearest", "texture-f0-1024x512-s7-l0"), ("p8-minified-bilinear", "texture-f0-1024x512-s7-l1"), ("p8-steep-minified-nearest", "texture-f0-1024x512-s8-l0"), ("p8-steep-minified-bilinear", "texture-f0-1024x512-s8-l1")):
        paths = [a.frames / f"tn-{stem}-v{v}-320x200.rgba" for v in (0, 1)]
        if not all(p.exists() for p in paths): continue
        raw = [np.fromfile(p, dtype="<u4").reshape(200, 320) for p in paths]
        rgb = [np.stack([(r >> (8 * c)) & 255 for c in range(3)], axis=2).astype(np.uint8) for r in raw]
        diff = np.abs(rgb[1].astype(np.int16) - rgb[0].astype(np.int16)).astype(np.uint8)
        err = diff.max(axis=2); ys, xs = np.where(err != 0)
        images = {}
        for name, arr in (("resident", rgb[0]), ("tiled", rgb[1]), ("absolute-rgb-difference", diff)):
            path = a.out / f"{label}-{name}.png"; Image.fromarray(arr).save(path); images[name] = str(path.resolve())
        row = {"input_paths": list(map(str, paths)), "size": [320, 200], "images": images,
            "different_rgb_pixels": int((err != 0).sum()), "beyond_3_pixels": int((err > 3).sum()), "max_channel_error": int(err.max()),
            "mean_absolute_channel_error": float(diff.mean()), "nonzero_bounds_xyxy_inclusive": [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())] if len(xs) else None,
            "channel_max": [int(diff[:, :, c].max()) for c in range(3)],
            "row_difference_counts": (err != 0).sum(axis=1).tolist(), "column_difference_counts": (err != 0).sum(axis=0).tolist(),
            "rgb_error_histogram": {str(i): int(n) for i, n in enumerate(np.bincount(err.ravel(), minlength=256)) if n}}
        if label == "p8-nearest":
            xx, yy = np.meshgrid(np.arange(320), np.arange(200))
            sx = np.arange(62, 1024, 62) * 320.0 / 1024.0 - 0.5
            sy = np.arange(62, 512, 62) * 200.0 / 512.0 - 0.5
            distance = np.minimum(np.min(np.abs(xx[:, :, None] - sx), axis=2), np.min(np.abs(yy[:, :, None] - sy), axis=2))
            row["differences_within_1_screen_pixel_of_tile_cut"] = int(((err != 0) & (distance <= 1)).sum())
            row["differences_more_than_2_screen_pixels_from_tile_cut"] = int(((err != 0) & (distance > 2)).sum())
            neighbor = np.zeros((200, 320), dtype=bool)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if not dx and not dy: continue
                    shifted = np.roll(rgb[0], (dy, dx), axis=(0, 1))
                    neighbor |= (rgb[1] == shifted).all(axis=2)
            row["different_pixels_matching_an_8_neighbor_resident_rgb"] = int(((err != 0) & neighbor).sum())
        report["cases"][label] = row
    (a.out / "spatial-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({name: {k: row[k] for k in ("different_rgb_pixels", "beyond_3_pixels", "max_channel_error", "nonzero_bounds_xyxy_inclusive", "images")} for name, row in report["cases"].items()}, indent=2))
if __name__ == "__main__": main()
