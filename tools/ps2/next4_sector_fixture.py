"""Make paired UDMF add-ons selecting compact and fixed sector snapshots."""
import argparse
import json
from pathlib import Path
import re
import zipfile

from make_udmf_map import textmap, wad, znodes
import udmf_ref

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov14-next4/sector-addons")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    text, vertices, lines = textmap(4, 3, 99)
    nodes, _ = znodes(4, 3, vertices, lines)
    soc = ("Level 99\nLevelName = Sector Snapshot Rooms\nTypeOfLevel = Race,Match,Coop\n"
           "Act = 1\nNoZone = 1\nSkyNum = 1\nMusic = NONE\nNextLevel = 1\n\n")
    # Sixteen safe, accepted nondefault scalar fields reach the EE equality
    # threshold: 40 + 4*16 = 104 bytes, so the old fixed array is retained.
    dense = """
    xpanningfloor = 1.25;
    ypanningfloor = -1.25;
    xpanningceiling = 2.5;
    ypanningceiling = -2.5;
    xscalefloor = 1.25;
    yscalefloor = 0.75;
    xscaleceiling = 1.5;
    yscaleceiling = 0.5;
    rotationfloor = 17.0;
    rotationceiling = 359.0;
    lightfloor = 144;
    lightceiling = 176;
    lightfloorabsolute = true;
    lightceilingabsolute = true;
    gravity = 1.03125;
    flipspecial_nofloor = true;
"""
    for name, fields in (("SECT_COMPACT", ""), ("SECT_FIXED", dense)):
        source = re.sub(r"(sector\s*\{)", lambda m: m[1] + fields, text)
        data = wad([("MAP99", b""), ("TEXTMAP", source.encode()), ("ZNODES", nodes), ("ENDMAP", b"")])
        path = args.out / (name + ".pk3")
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("Maps/MAP99.wad", data)
            z.writestr("SOC/SECTOR.soc", soc.replace("\n", "\r\n"))
        expected = udmf_ref.expect_wad(data)
        expected["sector_snapshot"] = {"sectors": 12, "changed_words": 12 * (16 if fields else 0),
                                       "ee_before": 12 * 104, "ee_after": 12 * (104 if fields else 40)}
        (args.out / (name + ".expected.json")).write_text(json.dumps(expected, indent=1) + "\n")
        print(path, expected["sector_snapshot"])


if __name__ == "__main__":
    main()
