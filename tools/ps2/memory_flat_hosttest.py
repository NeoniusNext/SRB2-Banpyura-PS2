"""Exact raw-flat bytes/owners, cache rebuild, and duplicate-payload peak regression."""
import argparse
import re
import subprocess
from pathlib import Path
from memory_hosttest import ROOT, build_run


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=str(ROOT / "build/ps2-memory-continuation/flat"))
    args = p.parse_args()
    out = Path(args.out).resolve()
    if not out.parent.is_dir():
        p.error("output parent must already exist")
    out.mkdir(exist_ok=True)
    wad = (ROOT / "src/w_wad.c").read_text(encoding="utf-8")
    cache = out / "lumpcache_actual.inc"
    cache.write_text(wad[wad.index("void *W_CacheLumpNumPwad("):wad.index("void *W_CacheLumpNum(lumpnum_t")], encoding="utf-8")
    textures = (ROOT / "src/r_textures.c").read_text(encoding="utf-8")
    flat = out / "flat_actual.inc"
    flat.write_text(textures[textures.index("UINT8 *R_GetFlatForTexture("):textures.index("//\n// R_GetTextureNum")], encoding="utf-8")
    defs = ['/DMEMORY_LUMPCACHE_SOURCE="' + cache.as_posix() + '"',
            '/DMEMORY_FLAT_SOURCE="' + flat.as_posix() + '"']
    outputs = []
    for name, flags in [("reference", ["/DNO_PNG_LUMPS", "/DPS2_NOOPT_flattransfer"]), ("transfer", ["/DNO_PNG_LUMPS"])]:
        outputs.append(build_run(out, name, "memory_flat_hosttest.c", defs + flags))
    rows = [[m.groups() for m in re.finditer(r"flat-(\d+) hash=(\d+) peak=(\d+) allocs=(\d+)", result)] for result in outputs]
    assert len(rows[0]) == len(rows[1]) == 3
    for old, new in zip(*rows):
        assert old[:2] == new[:2], (old, new)
        assert int(new[2]) * 2 == int(old[2]), (old, new)
        assert int(new[3]) == 1 and int(old[3]) == 2, (old, new)
    for name, code in [("reference", 2), ("transfer", 0)]:
        result = subprocess.run([str(out / name / "test.exe"), "100000"], capture_output=True, text=True, timeout=90)
        (out / name / "tight.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == code, result.stdout + result.stderr
    print("PASS exact flat bytes, detached/retargeted owners, cache rebuild; 64KiB flat fits 100000-byte arena")


if __name__ == "__main__":
    main()
