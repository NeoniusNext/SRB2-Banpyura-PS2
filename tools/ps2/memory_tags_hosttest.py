"""Exhaustive singleton/multitag production semantics and 32-bit map ABI regression."""
import argparse
import re
from pathlib import Path
from memory_hosttest import ROOT, build_run


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=str(ROOT / "build/ps2-memory-continuation/tags"))
    args = p.parse_args()
    out = Path(args.out).resolve()
    if not out.parent.is_dir():
        p.error("output parent must already exist")
    out.mkdir(exist_ok=True)
    text = (ROOT / "src/taglist.c").read_text(encoding="utf-8")
    source = out / "tags_actual.inc"
    source.write_text(text[text.index("void Tag_Add ("):text.index("/// Search for an element inside a global taggroup.")], encoding="utf-8")
    outputs = []
    defs = ['/DMEMORY_TAGS_SOURCE="' + source.as_posix() + '"']
    for name, flags in [("reference", ["/DPS2_NOOPT_taginline"]), ("inline", [])]:
        outputs.append(build_run(out, name, "memory_tags_hosttest.c", defs + flags))
    rows = [re.search(r"singletons hash=(\d+) used=(\d+) blocks=(\d+) allocs=(\d+)\noperations hash=(\d+)", o).groups() for o in outputs]
    assert rows[0][0] == rows[1][0] and rows[0][4] == rows[1][4]
    saved = int(rows[0][1]) - int(rows[1][1])
    assert 65536 * 32 <= saved <= 65536 * 32 + 64  # final arena-tail/header rounding
    assert int(rows[0][2]) == 65537 and int(rows[1][2]) == 1
    print(f"PASS all 65536 tags and multitag/remove semantics exact; singleton savings {saved} B, 65536 blocks")


if __name__ == "__main__":
    main()
