#!/usr/bin/env python3
"""Census exact raw stock flats; emit immutable full-texel host input under build/."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def census(asset, out):
    asset, out = asset.resolve(), out.resolve()
    if not out.is_relative_to(ROOT / "build"):
        raise RuntimeError("Corpus output must stay inside build/")
    out.mkdir(parents=True, exist_ok=True)
    records, rows = [], []
    with zipfile.ZipFile(asset) as archive:
        for item in sorted(archive.infolist(), key=lambda x: x.filename):
            if not item.filename.startswith("Flats/") or item.is_dir():
                continue
            side = math.isqrt(item.file_size)
            if side not in (16, 32, 64, 128, 256, 512, 1024) or side * side != item.file_size:
                continue
            data = archive.read(item)
            if data.startswith(b"\x89PNG\r\n\x1a\n"):
                continue
            high = data[0] >> 4
            eligible = all(value >> 4 == high for value in data)
            band = high if eligible else 0xffffffff
            records.append(struct.pack("<IIII", side, side, band, len(data)) + data)
            rows.append({"name": item.filename, "width": side, "height": side, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "single_band": high if eligible else None, "has255": 255 in data})
    payload = b"P4CORP12" + struct.pack("<I", len(records)) + b"".join(records)
    path = out / "full-texels.bin"
    if path.exists() and path.read_bytes() != payload:
        raise RuntimeError("Refusing to overwrite a different immutable corpus")
    path.write_bytes(payload)
    selected = [row for row in rows if row["single_band"] is not None]
    report = {"asset": str(asset), "asset_sha256": hashlib.sha256(asset.read_bytes()).hexdigest(), "raw_square_flats": len(rows), "single_band_flats": len(selected), "single_band_source_bytes": sum(row["bytes"] for row in selected), "potential_half_payload_bytes": sum(row["bytes"] // 2 for row in selected), "scope": "Corpus opportunity only; not simultaneous residency, policy invariance, native GS validation, or an adopted saving.", "full_texels": str(path), "full_texels_sha256": hashlib.sha256(payload).hexdigest(), "rows": rows}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--asset", type=Path, default=Path("/workspace/.onboarding/srb2-assets/srb2.pk3"))
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next12/p4/corpus")
    args = ap.parse_args()
    report = census(args.asset, args.out)
    print(json.dumps({key: value for key, value in report.items() if key != "rows"}, indent=2))


if __name__ == "__main__":
    main()
