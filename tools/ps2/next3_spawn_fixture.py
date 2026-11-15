"""Make identical MAP99 rooms with guaranteed compact/dense spawn arguments."""
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
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov14-next3/spawn-addons")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    text, vertices, lines = textmap(4, 3, 99)
    nodes, _ = znodes(4, 3, vertices, lines)
    soc = ("Level 99\nLevelName = Spawn Snapshot Rooms\nTypeOfLevel = Race,Match,Coop\n"
           "Act = 1\nNoZone = 1\nSkyNum = 1\nMusic = NONE\nNextLevel = 1\n\n")
    for name, dense in (("SP_COMPACT", False), ("SP_FIXED", True)):
        source = re.sub(r"(linedef\s*\{)", r"\1\n\targ9 = 2147483647;", text) if dense else text
        data = wad([("MAP99", b""), ("TEXTMAP", source.encode()), ("ZNODES", nodes), ("ENDMAP", b"")])
        path = args.out / (name + ".pk3")
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("Maps/MAP99.wad", data)
            z.writestr("SOC/SPAWN.soc", soc.replace("\n", "\r\n"))
        expected = udmf_ref.expect_wad(data)
        expected["spawn_args"] = {"lines": len(lines), "nonzero": len(lines) if dense else 0,
                                  "ee_fixed_bytes": 60 * len(lines),
                                  "ee_chosen_bytes": (60 if dense else 24) * len(lines)}
        (args.out / (name + ".expected.json")).write_text(json.dumps(expected, indent=1) + "\n")
        print(path, expected["spawn_args"])


if __name__ == "__main__":
    main()
