"""Compare actual old/new blockmap output, zone peaks, and a tight-RAM regression."""
import argparse
import re
import subprocess
from pathlib import Path
from memory_hosttest import ROOT, build_run


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=str(ROOT / "build/ps2-memory-continuation/blockmap"))
    p.add_argument("--compact", action="store_true")
    p.add_argument("--negative-controls", action="store_true")
    args = p.parse_args()
    out = Path(args.out).resolve()
    if not out.parent.is_dir():
        p.error("output parent must already exist")
    out.mkdir(exist_ok=True)
    setup = (ROOT / "src/p_setup.c").read_text(encoding="utf-8")
    source = out / "blockmap_actual.inc"
    source.write_text(setup[setup.index("static boolean LineInBlock("):setup.index("#ifndef PS2_PROFILE\n// PK3 version", setup.index("static boolean LineInBlock("))], encoding="utf-8")
    geometry = (ROOT / "src/p_maputl.c").read_text(encoding="utf-8")
    geom = out / "geometry_actual.inc"
    geom.write_text(geometry[geometry.index("INT32 P_PointOnLineSide("):geometry.index("//\n// P_PointOnDivlineSide")], encoding="utf-8")
    defs = ['/DMEMORY_BLOCKMAP_SOURCE="' + source.as_posix() + '"',
            '/DMEMORY_GEOMETRY_SOURCE="' + geom.as_posix() + '"']
    if args.compact:
        compact = out / "compact_actual.inc"
        compact.write_text(setup[setup.index("static void P_CompactBlockmap("):setup.index("static boolean P_LoadMapFromFile(")].removesuffix("#endif\n\n"), encoding="utf-8")
        iterator = out / "iterator_actual.inc"
        iterator.write_text(geometry[geometry.index("boolean P_BlockLinesIterator("):geometry.index("boolean P_BlockThingsIterator(")], encoding="utf-8")
        defs += ['/DMEMORY_BLOCKMAP16_SOURCE="' + compact.as_posix() + '"', '/DMEMORY_BLOCKMAP_ITERATOR_SOURCE="' + iterator.as_posix() + '"']
    outputs = []
    for name, flags in [("reference", ["/DPS2_NOOPT_blockmap"]), ("two-pass", [])]:
        outputs.append(build_run(out, name, "memory_blockmap_hosttest.c", defs + flags))
    rows = []
    for result in outputs:
        rows.append([m.groups() for m in re.finditer(r"(\S+) words=(\d+) hash=(\d+) peak=(\d+) allocs=(\d+)", result)])
    assert len(rows[0]) == len(rows[1]) == 54
    for old, new in zip(*rows):
        assert old[:3] == new[:3], (old, new)
        assert int(new[3]) <= int(old[3]), (old, new)
    budget = 150000
    for name, code in [("reference", 2), ("two-pass", 0)]:
        result = subprocess.run([str(out / name / "test.exe"), str(budget)], capture_output=True, text=True, timeout=90)
        (out / name / "tight.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == code, result.stdout + result.stderr
    print("PASS 54 exact blockmap streams; 150000-byte dense map fits two-pass and reference fails cleanly")
    if args.negative_controls:
        if not args.compact: p.error("--negative-controls needs --compact")
        corrupt = out / "compact_corrupt.inc"
        actual = compact.read_text(encoding="utf-8")
        broken = actual.replace("UINT16 value = (UINT16)blockmaplump[i];",
            "UINT16 value = (UINT16)(blockmaplump[i] == 0 ? 1 : blockmaplump[i]);")
        assert actual != broken
        corrupt.write_text(broken, encoding="utf-8")
        negative_defs = [('/DMEMORY_BLOCKMAP16_SOURCE="' + corrupt.as_posix() + '"') if d.startswith('/DMEMORY_BLOCKMAP16_SOURCE=') else d for d in defs]
        try:
            build_run(out, "negative-line-zero", "memory_blockmap_hosttest.c", negative_defs)
        except RuntimeError as error:
            assert "FAIL: exact iterator linedef order" in str(error), error
        else:
            raise AssertionError("line-zero corruption escaped exact iterator comparison")
        print("PASS negative control: changed linedef zero rejected by exact callback stream")


if __name__ == "__main__":
    main()
