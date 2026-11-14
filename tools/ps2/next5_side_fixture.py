"""Make paired UDMF add-ons selecting compact and fixed side snapshots."""
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
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov14-next5/side-addons")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    text, vertices, lines = textmap(4, 3, 99)
    nodes, _ = znodes(4, 3, vertices, lines)
    soc = ("Level 99\nLevelName = Side Snapshot Rooms\nTypeOfLevel = Race,Match,Coop\n"
           "Act = 1\nNoZone = 1\nSkyNum = 1\nMusic = NONE\nNextLevel = 1\n\n")
    # Two signed offsets plus three valid texture IDs reach exact EE equality:
    # 4 + 4*5 = 24 bytes. That fixture must retain the fixed representation.
    dense = ('\n offsetx = 64;\n offsety = -32;\n texturetop = "GFZBRICK";'
             '\n texturebottom = "GFZROCK2";\n texturemiddle = "GFZCRACK";\n')
    for name, fixed in (("SIDE_COMPACT", False), ("SIDE_FIXED", True)):
        def dense_side(match):
            sector = re.search(r"sector\s*=\s*\d+;", match[0])[0]
            return "sidedef { " + sector + dense + "}\n"
        source = re.sub(r"sidedef\s*\{[^}]*\}", dense_side, text) if fixed else text
        data = wad([("MAP99", b""), ("TEXTMAP", source.encode()), ("ZNODES", nodes), ("ENDMAP", b"")])
        path = args.out / (name + ".pk3")
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("Maps/MAP99.wad", data)
            z.writestr("SOC/SIDES.soc", soc.replace("\n", "\r\n"))
        expected = udmf_ref.expect_wad(data)
        sideblocks = re.findall(r"sidedef\s*\{[^}]*\}", source)
        n = len(sideblocks)
        count = sum(len(re.findall(r'texture(?:top|bottom|middle)\s*=\s*"[^"-]+"', s))
                    + len(re.findall(r'offset[xy]\s*=\s*-?[1-9]\d*', s)) for s in sideblocks)
        expected["side_snapshot"] = {"sides": n, "changed_words": count, "fixed": fixed,
                                     "ee_before": n * 24, "ee_after": min(n * 24, n * 4 + count * 4)}
        (args.out / (name + ".expected.json")).write_text(json.dumps(expected, indent=1) + "\n")
        print(path, expected["side_snapshot"])


if __name__ == "__main__":
    main()
