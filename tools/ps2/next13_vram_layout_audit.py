#!/usr/bin/env python3
"""Read-only exact GS layout/deduplication census for pass 13.

These are storage opportunities, not simultaneous residency, safe usable pool
growth, adopted changes, or measured savings. No compiler/emulator is invoked.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[2]
REVISION = "353cc94103aeb38cef87d26151c779302135d0e9"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", type=Path, default=ROOT / "build/nov15-next13/vram-clear/stages/packed-v1/baseline/src")
    ap.add_argument("--asset", type=Path, default=Path("/workspace/.onboarding/srb2-assets/srb2.pk3"))
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    source, out = args.src.resolve(), args.out.resolve()
    if not out.is_relative_to(ROOT / "build/nov15-next13") or out.exists():
        ap.error("--out must be a new path within build/nov15-next13/")
    inputs = {}
    for name in ("ps2_hw_tex.inc", "ps2_hw_priv.inc", "ps2_hw_gs.inc", "ps2_hw_draw.inc", "ps2_hw_plan.inc", "ps2_hw_vu1.inc", "ps2_hw_screen.inc", "ps2_hwd.c"):
        path = source / "ps2/hw" / name
        inputs["src/ps2/hw/" + name] = sha(path.read_bytes())
    tex = (source / "ps2/hw/ps2_hw_tex.inc").read_text()
    priv = (source / "ps2/hw/ps2_hw_priv.inc").read_text()
    table_text = re.search(r"static const u8 blk_32_8\[4\]\[8\] = (.*?);", tex)[1]
    flat = [int(n) for n in re.findall(r"\d+", table_text)]
    table = [flat[i:i + 8] for i in range(0, 32, 8)]
    slots = int(re.search(r"^#define CLUT_SLOTS (\d+)", priv, re.M)[1])
    clut_blocks = int(re.search(r"^#define CLUT_BLOCKS (\d+)", priv, re.M)[1])
    clut_payload = slots * clut_blocks * 256
    clut_reserved = (clut_payload + 8191) // 8192 * 8192
    budgets = []
    for mode, w, h in (("PAL actual",320,256),("NTSC",320,224),("NTSC full",640,448),("PAL full",640,512),("480p full",640,480)):
        for fb32 in (True, False):
            fb = w // 64 * ((h + (31 if fb32 else 63)) // (32 if fb32 else 64)) * 8192
            z = w // 64 * ((h + 31) // 32) * 8192
            pool = 4194304 - 2 * fb - z - clut_reserved
            assert 2 * fb + z + clut_reserved + pool == 4194304
            budgets.append({"mode":mode,"width":w,"height":h,"framebuffer_format":"CT32" if fb32 else "CT16S",
                            "each_framebuffer_reserved_bytes":fb,"each_framebuffer_unused_padding_bytes":fb-w*h*(4 if fb32 else 2),
                            "Z24_reserved_bytes":z,"Z24_unused_spatial_padding_bytes":z-w*h*4,
                            "Z24_high_byte_index_capacity_ceiling":z//4,"CLUT_reserved_bytes":clut_reserved,"ordinary_pool_bytes":pool})
    holes = []
    for w,h in ((16,1024),(32,1024),(64,1024),(256,1),(64,1536),(128,128)):
        pieces = [min(h,1024)] + ([h-1024] if h>1024 else [])
        span_total, occupied_total = 0, 0
        for piece_h in pieces:
            tbw = ((w+127)//128)*2
            occupied = set()
            for y in range(0,piece_h,16):
                for x in range(0,w,16):
                    occupied.add(((y//64)*(tbw//2)+x//128)*32+table[(y//16)%4][(x//16)%8])
            span = max(occupied)+1
            if len(pieces)>1 and piece_h==1024:
                span = (span+31)//32*32
            occupied_total += len(occupied)
            span_total += span
        holes.append({"PSM":"T8","width":w,"height":h,"reserved_span_blocks":span_total,
                      "occupied_blocks":occupied_total,"internal_hole_bytes":(span_total-occupied_total)*256,
                      "note":"Moving another image into these holes requires changed block ownership/allocation. This is not a safe reclaimed quota."})
    groups = {}
    with zipfile.ZipFile(args.asset) as archive:
        for item in sorted(archive.infolist(), key=lambda item:item.filename):
            side=math.isqrt(item.file_size)
            if not item.filename.startswith("Flats/") or item.is_dir() or side not in (16,32,64,128,256,512,1024) or side*side!=item.file_size:
                continue
            data=archive.read(item)
            if data.startswith(b"\x89PNG\r\n\x1a\n"):
                continue
            key=(side,sha(data))
            if key in groups:
                # Hashes only index candidates. Every duplicate is byte-verified.
                assert groups[key][0]==data
                groups[key][1].append(item.filename)
            else:
                groups[key]=[data,[item.filename]]
    duplicates=[{"width":side,"height":side,"sha256":digest,"names":names,
                 "redundant_payload_bytes":side*side*(len(names)-1)}
                for (side,digest),(data,names) in sorted(groups.items()) if len(names)>1]
    result={"revision":REVISION,"source":str(source),"source_sha256":inputs,"budgets":budgets,
            "CLUT_images":slots,"CLUT_payload_bytes":clut_payload,"CLUT_rounding_gap_bytes":clut_reserved-clut_payload,
            "block_hole_examples":holes,"asset_sha256":sha(args.asset.read_bytes()),
            "raw_square_flat_count":sum(len(names) for data,names in groups.values()),
            "duplicate_groups":len(duplicates),"redundant_raw_flat_images":sum(len(row["names"])-1 for row in duplicates),
            "redundant_raw_flat_payload_bytes":sum(row["redundant_payload_bytes"] for row in duplicates),"duplicates":duplicates,
            "adopted_VRAM_saving_bytes":0,"production_sources_edited":False,"compiler_invocations":0,"emulator_launches":0,
            "scope":"Read-only opportunity arithmetic and full-byte duplicate census. No simultaneous residency or safe usable pool increase established.",
            "rejection":"Any usable ordinary-pool growth or changed live image ownership feeds nonmonotonic quality budgets/evictions. Retaining unavailable old quota is not counted as saving. Z24 alias transfers also require GS manual4.2.4 page-break workarounds."}
    out.mkdir(parents=True)
    (out/"report.json").write_text(json.dumps(result,indent=2)+"\n")
    (out/Path(__file__).name).write_bytes(Path(__file__).read_bytes())
    print(json.dumps({k:v for k,v in result.items() if k not in ("source_sha256","budgets","duplicates")},indent=2))


if __name__=="__main__":
    main()
