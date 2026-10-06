"""Exact production map linking and pooled sector linebuffer memory regression."""
import argparse
import re
from pathlib import Path
from memory_hosttest import ROOT, build_run


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=str(ROOT / "build/ps2-memory-continuation/link"))
    args = p.parse_args()
    out = Path(args.out).resolve()
    if not out.parent.is_dir():
        p.error("output parent must already exist")
    out.mkdir(exist_ok=True)
    text = (ROOT / "src/p_setup.c").read_text(encoding="utf-8")
    source = out / "link_actual.inc"
    source.write_text(text[text.index("static void P_LinkMapData(void)"):text.index("// For maps in binary format, add multi-tags")], encoding="utf-8")
    outputs = []
    defs = ['/DMEMORY_LINK_SOURCE="' + source.as_posix() + '"']
    for name, flags in [("reference", ["/DPS2_NOOPT_sectorpool"]), ("pool", [])]:
        outputs.append(build_run(out, name, "memory_link_hosttest.c", defs + flags))
    rows = [[m.groups() for m in re.finditer(r"(\S+) hash=(\d+) entries=(\d+) used=(\d+) blocks=(\d+)", o)] for o in outputs]
    assert len(rows[0]) == len(rows[1]) == 5
    for old, new in zip(*rows):
        assert old[:3] == new[:3], (old, new)
        assert int(new[3]) <= int(old[3]), (old, new)
        assert int(new[4]) == (1 if int(new[2]) else 0), new
    print("PASS exact sector lines/count/order, subsectors and sound origins; pooled PU_LEVEL lifetime")


if __name__ == "__main__":
    main()
