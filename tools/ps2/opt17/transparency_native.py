#!/usr/bin/env python3
"""Build paired actual GS transparency witnesses; never starts an emulator."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent))
import build as B
from transparency_hosttest import OLD, NEW


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def run(cmd, log):
    r = subprocess.run(list(map(str, cmd)), cwd=ROOT, env=B.ENV, capture_output=True, text=True)
    log.write_text(" ".join(map(str, cmd)) + "\n" + r.stdout + r.stderr)
    if r.returncode:
        raise RuntimeError(str(log) + "\n" + r.stdout + r.stderr)


def replace_body(text, signature, body):
    pattern = re.escape(signature) + r"\n\{.*?^\}"
    result, n = re.subn(pattern, signature + "\n{\n" + body + "\n}", text, flags=re.M | re.S)
    if n != 1:
        raise RuntimeError("Boundary anchor missing/ambiguous: " + signature)
    return result


def prepare(work, source, fixed):
    root = work / "source"; (root / "src/ps2").mkdir(parents=True)
    for child in (source / "src").iterdir():
        if child.name != "ps2": (root / "src" / child.name).symlink_to(child, target_is_directory=child.is_dir())
    for child in (source / "src/ps2").iterdir():
        if child.name != "hw": (root / "src/ps2" / child.name).symlink_to(child, target_is_directory=child.is_dir())
    shutil.copytree(source / "src/ps2/hw", root / "src/ps2/hw")
    tex = root / "src/ps2/hw/ps2_hw_tex.inc"
    if fixed:
        text = tex.read_text()
        if text.count(OLD) != 1: raise RuntimeError("Fix anchor changed")
        tex.write_text(text.replace(OLD, NEW))
    driver = root / "src/ps2/hw/ps2_hwd.c"
    driver.write_text(driver.read_text() + '\n#include "' + str(HERE / "transparency_native_driver.inc") + '"\n')
    fixture = (ROOT / "tools/ps2/hw_test.c").read_text().split("static void test_layout(void)", 1)[0]
    fixture = fixture.replace("/* ---- stand-ins ---- */", "static const u8 *t17_source;\nstatic unsigned int t17_stream_reads;\n/* ---- stand-ins ---- */")
    fixture = replace_body(fixture, "void HWR_PS2_FreeData(void *p)", "\tfree(p);")
    fixture = replace_body(fixture, "void *HWR_PS2_StealData(GLMipmap_t *m, void **newuser)", "\tvoid *p = m->data; m->data = NULL; *newuser = p; return p;")
    fixture = replace_body(fixture, "void *HWR_PS2_AllocData(size_t bytes, void **newuser)", "\tvoid *p = memalign(64, bytes); *newuser = p; return p;")
    fixture = replace_body(fixture, "const u8 *HWR_PS2_FlatPin(const GLMipmap_t *m)", "\t(void)m; return t17_source;")
    fixture = replace_body(fixture, "boolean HWR_PS2_FlatStreamable(const GLMipmap_t *m)", "\treturn t17_source && m->regen_kind == 2 && m->width == 256 && m->height == 256;")
    fixture = replace_body(fixture, "boolean HWR_PS2_FlatRows(const GLMipmap_t *m, UINT32 row0, UINT32 nrows, UINT8 *dest)",
                           "\tif (!t17_source || row0 + nrows > m->height) return false;\n\tmemcpy(dest, t17_source + (size_t)row0 * m->width, (size_t)nrows * m->width);\n\tt17_stream_reads++; return true;")
    fixture = fixture.replace("D.pfnGClipRect(0, 0, SW, SH, 0.9f);", "D.pfnGClipRect(0, 0, INFO.screen_w, INFO.screen_h, 0.9f);")
    fixture += '\n#include "' + str(HERE / "transparency_native_main.inc") + '"\n'
    cfile = work / "transparency.c"; cfile.write_text(fixture)
    return root, cfile, driver


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, default=ROOT / "build/nov16-hwopt/candidate-v7/source")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(); out = args.out.resolve(); source = args.source_root.resolve()
    out.mkdir(parents=True, exist_ok=False); B.gen_config()
    flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-O2", "-O3", "-G0", "-G8", "-DNOHW")]
    flags += ["-O3", "-G8", "-flto", "-DHWRENDER", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable"]
    report = {"scope": "Build-only paired real EE/GS witness: actual streaming, mip source selection, CLUT VRAM readback, TEX0, keyed mask drawing and frame RGB readback before/after a real resolution restart. Allocation and raw-lump reads are fixture boundary services. No emulator run by this builder.",
              "source_root": str(source), "source_texture_sha256": sha(source / "src/ps2/hw/ps2_hw_tex.inc"),
              "fixture_inputs": {p.name: sha(p) for p in (Path(__file__), HERE / "transparency_native_driver.inc", HERE / "transparency_native_main.inc", HERE / "transparency_hosttest.py", ROOT / "tools/ps2/hw_test.c")},
              "variants": {}}
    for name, fixed in (("baseline", False), ("fixed", True)):
        work = out / name; work.mkdir()
        root, fixture, driver = prepare(work, source, fixed)
        includes = ["-I" + str(root / "src"), "-I" + str(root / "src/ps2"), *B.INCS]
        objects = []
        for path in (fixture, driver, ROOT / "tools/ps2/gs_list_ee_stubs.c"):
            obj = work / (path.stem + ".o")
            run([B.CC, *flags, *includes, "-c", path, "-o", obj], work / (path.stem + ".build.log")); objects.append(obj)
        for stem in ("ps2_hw_vu1", "ps2_hw_vu1_spr"):
            obj = work / (stem + "_vsm.o")
            run([B.DEV / "dvp/bin/dvp-as", root / "src/ps2/hw/vu1" / (stem + ".vsm"), "-o", obj], work / (stem + ".build.log")); objects.append(obj)
        merged = work / "native-lto.o"
        run([B.CC, "-r", "-nostdlib", "-flto", "-flinker-output=nolto-rel", "-O3", "-G8", "-mno-abicalls",
             "-ffunction-sections", "-fdata-sections", *objects, "-o", merged], work / "lto.log")
        elf = work / "T17.ELF"
        run([B.CC, *B.LDFLAGS, merged, "-o", elf, "-Wl,-Map=" + str(work / "T17.map"), "-lgskit", "-ldmakit", "-ldebug", "-lpatches", "-lm"], work / "link.log")
        report["variants"][name] = {"elf": str(elf), "elf_sha256": sha(elf), "elf_bytes": elf.stat().st_size,
                                    "texture_sha256": sha(root / "src/ps2/hw/ps2_hw_tex.inc"), "driver_sha256": sha(driver), "fixture_sha256": sha(fixture)}
    (out / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__": main()
