#!/usr/bin/env python3
"""Build isolated actual EE/GS tile witnesses. Does not run PCSX2."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = next(p for p in HERE.parents if (p / "tools/ps2/build.py").is_file())
sys.path.insert(0, str(ROOT / "tools/ps2"))
import build as B

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def run(cmd, log):
    r = subprocess.run(list(map(str, cmd)), cwd=ROOT, env=B.ENV, capture_output=True, text=True)
    log.write_text(" ".join(map(str, cmd)) + "\n" + r.stdout + r.stderr)
    if r.returncode: raise RuntimeError(str(log) + "\n" + r.stdout + r.stderr)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--no-halo", action="store_true")
    a = ap.parse_args(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = a.source.resolve(); root = out / "source"
    shutil.copytree(source / "src", root / "src")
    original_driver_hashes = {str(p.relative_to(root)): sha(p) for p in sorted((root / "src/ps2/hw").glob("*")) if p.is_file()}
    shutil.copyfile(HERE / "driver.inc", out / "driver.inc")
    shutil.copyfile(HERE / "main.inc", out / "main.inc")
    driver = root / "src/ps2/hw/ps2_hwd.c"
    driver_text = re.sub(r'^#include "[^"\n]*/native/[^"\n]*driver\.inc"\n?', '', driver.read_text(), flags=re.M)
    driver.write_text(driver_text + '\n#include "' + str(out / "driver.inc") + '"\n')
    stencil = (HERE / "stencil_driver.inc").exists()
    if stencil:
        shutil.copyfile(HERE / "stencil_driver.inc", out / "stencil_driver.inc")
        driver.write_text(driver.read_text() + '\n#include "' + str(out / "stencil_driver.inc") + '"\n')
        if (HERE / "stencil_gradient.inc").exists():
            shutil.copyfile(HERE / "stencil_gradient.inc", out / "stencil_gradient.inc")
            driver.write_text(driver.read_text() + '\n#include "' + str(out / "stencil_gradient.inc") + '"\n')
    halo_layout = None
    if a.no_halo:
        p = root / "src/ps2/hw/ps2_hw_tile.inc"; s = p.read_text()
        if s.count('dy -= height / 4;') == s.count('dx -= width / 4;') == 1:
            halo_layout = "dynamic rectangular width/2 by height/2 core, quarter-size halo"
            s = s.replace('dy -= height / 4;', 'dy -= height / 4;\n\t\tif (dy < 0) dy = 0;\n\t\telse if (dy >= height / 2) dy = height / 2 - 1;')
            s = s.replace('dx -= width / 4;', 'dx -= width / 4;\n\t\t\tif (dx < 0) dx = 0;\n\t\t\telse if (dx >= width / 2) dx = width / 2 - 1;')
        elif s.count('dy -= extent / 4;') == s.count('dx -= extent / 4;') == 1:
            # Applies to both the 128-indexed and 64-direct region windows.
            # Keep the rolled physical address and replace only halo samples.
            halo_layout = "dynamic extent/2 core, extent/4 halo"
            s = s.replace('dy -= extent / 4;', 'dy -= extent / 4;\n\t\tif (dy < 0) dy = 0;\n\t\telse if (dy >= extent / 2) dy = extent / 2 - 1;')
            s = s.replace('dx -= extent / 4;', 'dx -= extent / 4;\n\t\t\tif (dx < 0) dx = 0;\n\t\t\telse if (dx >= extent / 2) dx = extent / 2 - 1;')
        elif s.count('dy -= TILE_SIZE / 4;') == s.count('dx -= TILE_SIZE / 4;') == 1:
            # Rolled 32-core window: replace every neighboring texel with
            # the nearest core edge, preserving the physical address map.
            halo_layout = "fixed 32 core, 16 halo"
            s = s.replace('dy -= TILE_SIZE / 4;', 'dy -= TILE_SIZE / 4;\n\t\tif (dy < 0) dy = 0;\n\t\telse if (dy >= TILE_SIZE / 2) dy = TILE_SIZE / 2 - 1;')
            s = s.replace('dx -= TILE_SIZE / 4;', 'dx -= TILE_SIZE / 4;\n\t\t\tif (dx < 0) dx = 0;\n\t\t\telse if (dx >= TILE_SIZE / 2) dx = TILE_SIZE / 2 - 1;')
        elif s.count('c->y + (int)row - 1') == s.count('c->x + x - 1') == 1:
            halo_layout = "fixed 62 core, 1 halo"
            s = s.replace('c->y + (int)row - 1', 'c->y + ((int)row < 1 ? 0 : (int)row > TILE_CORE ? TILE_CORE - 1 : (int)row - 1)')
            s = s.replace('c->x + x - 1', 'c->x + (x < 1 ? 0 : x > TILE_CORE ? TILE_CORE - 1 : x - 1)')
        else:
            raise RuntimeError("Halo negative-control anchors changed; update it mechanically for the candidate layout")
        p.write_text(s)
    fixture = (ROOT / "tools/ps2/hw_test.c").read_text().split("static void test_layout(void)", 1)[0]
    fixture = fixture.replace("D.pfnGClipRect(0, 0, SW, SH, 0.9f);", "D.pfnGClipRect(0, 0, INFO.screen_w, INFO.screen_h, 0.9f);")
    fixture = fixture.replace("D.pfnDrawPolygon(s, v, 4, flags);", "if (arg(\"triangles\")) { FOutVector second[3] = {v[0], v[2], v[3]}; D.pfnDrawPolygon(s, v, 3, flags); D.pfnDrawPolygon(s, second, 3, flags); } else D.pfnDrawPolygon(s, v, 4, flags);")
    fixture += '\n#include "' + str(out / "main.inc") + '"\n'
    cfile = out / "native.c"; cfile.write_text(fixture)
    B.gen_config()
    shutil.copytree(B.GEN, out / "gen")
    flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-O2", "-O3", "-G0", "-G8", "-DNOHW")]
    flags += ["-O3", "-G8", "-flto", "-DHWRENDER", "-Werror", "-MMD", "-MP", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable"]
    if stencil: flags.append("-DTN_STENCIL_PROBE")
    if "tile_core_fast_count(" in (root / "src/ps2/hw/ps2_hw_tile_draw.inc").read_text():
        flags.append("-DTN_ONECORE_PROBE")
    if "tile_general_extent(" in (root / "src/ps2/hw/ps2_hw_tile.inc").read_text():
        flags.append("-DTN_DYNAMIC_TILE_PROBE")
    if "tile_cache_try_borrow(" in (root / "src/ps2/hw/ps2_hw_tile.inc").read_text():
        flags.append("-DTN_BORROW_PROBE")
    incs = ["-I" + str(root / "src"), "-I" + str(root / "src/ps2"), "-I" + str(out / "gen"), *[flag for flag in B.INCS if flag != "-I" + str(B.GEN)]]
    objects = []
    for path in (cfile, driver, ROOT / "tools/ps2/gs_list_ee_stubs.c"):
        obj = out / (path.stem + ".o")
        run([B.CC, *flags, *incs, "-c", path, "-o", obj], out / (path.stem + ".build.log")); objects.append(obj)
    for stem in ("ps2_hw_vu1", "ps2_hw_vu1_spr"):
        obj = out / (stem + "_vsm.o")
        run([B.DEV / "dvp/bin/dvp-as", root / "src/ps2/hw/vu1" / (stem + ".vsm"), "-o", obj], out / (stem + ".build.log")); objects.append(obj)
    merged = out / "native-lto.o"
    run([B.CC, "-r", "-nostdlib", "-flto", "-flinker-output=nolto-rel", "-O3", "-G8", "-mno-abicalls", "-ffunction-sections", "-fdata-sections", *objects, "-o", merged], out / "lto.log")
    elf = out / "TILES17.ELF"
    run([B.CC, *B.LDFLAGS, merged, "-o", elf, "-Wl,-Map=" + str(out / "TILES17.map"), "-lgskit", "-ldmakit", "-ldebug", "-lpatches", "-lm"], out / "link.log")
    inputs = {str(p.relative_to(root)): sha(p) for p in sorted((root / "src/ps2/hw").glob("*")) if p.is_file()}
    dependencies = set()
    for dep in out.glob("*.d"):
        first = dep.read_text().replace("\\\n", " ").splitlines()[0]
        for name in shlex.split(first.split(": ", 1)[1]):
            p = Path(name); dependencies.add(p if p.is_absolute() else ROOT / p)
    report = {"scope": "Build-only actual EE/GS fixture. Full-frame rendered readbacks; no emulator execution in builder.", "source": str(source), "no_halo_negative_control": a.no_halo, "elf": str(elf), "elf_sha256": sha(elf), "elf_bytes": elf.stat().st_size, "flags": flags, "shipping_driver_source_sha256_before_fixture_hooks": original_driver_hashes, "driver_source_sha256": inputs, "compiled_dependency_sha256": {str(p.resolve()): sha(p) for p in sorted(dependencies)}, "fixture_sha256": {str(p): sha(p) for p in (Path(__file__), out / "driver.inc", out / "main.inc", cfile)}}
    report["halo_negative_control_layout"] = halo_layout
    (out / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("elf", "elf_sha256", "elf_bytes", "no_halo_negative_control")}, indent=2))
if __name__ == "__main__": main()
