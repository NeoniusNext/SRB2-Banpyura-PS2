"""x86 host test of the GS renderer driver's pure logic: transform, clipping, vertex/GIF packing, register values,
VRAM pool, texture conversion and upload packets. The driver's own source (src/ps2/hw/ps2_hw_*.inc) is compiled as it is;
the engine type definitions come from the real headers (sections copied verbatim into the work directory).

python tools/ps2/hw_hosttest.py --negative-controls [--out build/agent-c-hw/host]
Every group of checks has a negative control (a deliberate mutation of the expected values or of the code under test): the
test executable must report a FAIL for exactly that group.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")
HW = ROOT / "src/ps2/hw"


def section(text, start, end, include_end=True):
    if text.count(start) != 1:
        raise RuntimeError(f"anchor not unique/missing: {start!r}")
    a = text.index(start)
    b = text.index(end, a)
    return text[a:b + (len(end) if include_end else 0)]


def make_shim(work):
    defs = (ROOT / "src/hardware/hw_defs.h").read_text()
    data = (ROOT / "src/hardware/hw_data.h").read_text()
    dtype = (ROOT / "src/doomtype.h").read_text()
    drv = (ROOT / "src/hardware/hw_drv.h").read_text()
    parts = [
        "#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n",
        "typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32; typedef int32_t INT32; typedef int boolean;\n",
        "#define ATTRPACK\n",
        re.search(r"^#define SCREENVERTS \d+", drv, re.M)[0] + "\n",
        section(dtype, "typedef struct\n{\n\tUINT8 red;", "typedef union FColorRGBA RGBA_t;"),
        section(defs, "typedef long            FINT;", "typedef unsigned char   FBOOLEAN;"),
        section(defs, "// RGBA Color components with float type", "typedef struct FRGBAFloat FRGBAFloat;"),
        section(defs, "typedef struct\n{\n\tFLOAT       x,y,z;           // position", "} FTransform;"),
        section(defs, "typedef struct\n{\n\tFLOAT       x,y,z;\n\tFLOAT       s;", "} FOutVector;"),
        section(defs, "typedef struct vbo_vertex_s", "} gl_sky_t;"),
        section(defs, "// Flags describing how to render a polygon", "typedef enum hwdscreentexture hwdscreentexture_t;"),
        section(data, "typedef enum GLTextureFormat_e", "typedef struct GLMipmap_s GLMipmap_t;"),
         "typedef struct GSGLOBAL GSGLOBAL;\ntypedef int8_t s8;\ntypedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;\n",
        '#include "hw_model.h"\n',
    ]
    (work / "shim.h").write_text("\n".join(parts), encoding="utf-8")


def copy_sources(work):
    """Copies of the driver's .inc/.h files with the two things MSVC cannot take changed; the code under test is untouched."""
    for name in ["ps2_hw_priv.inc", "ps2_hw_regs.inc", "ps2_hw_xform.inc", "ps2_hw_light.inc", "ps2_hw_tex.inc", "ps2_hw_draw.inc", "ps2_hw_model.inc", "ps2_hw_screen.inc", "ps2_hwd_dbg.h"]:
        text = (HW / name).read_text()
        text = text.replace('#include "../../doomtype.h"', '#include "shim.h"')
        text = re.sub(r"__attribute__\(\(aligned\(\d+\)\)\)", "", text)
        (work / name).write_text(text, encoding="utf-8")
    (work / "hw_model.h").write_text((ROOT / "src/hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ''), encoding="utf-8")


def build(work, arch):
    bat = work / f"build-{arch}.bat"
    exe = f"hw_hosttest-{arch}.exe"
    lines = ["@echo off", f'call "{VCVARS}" {arch} >nul 2>&1', "if errorlevel 1 exit /b 1",
             subprocess.list2cmdline(["cl", "/nologo", "/std:c17", "/O2", "/W3", "/WX", "/D_CRT_SECURE_NO_WARNINGS", "/D_USE_MATH_DEFINES", "/DPS2_PROFILE",
                                      "/I" + str(work), str(ROOT / "tools/ps2/hw_hosttest.c"), "/Fe:" + exe, "/Fo:hw_hosttest-" + arch + ".obj"])]
    bat.write_text("\n".join(lines) + "\n", encoding="utf-8")
    r = subprocess.run(["cmd", "/c", str(bat)], cwd=work, capture_output=True, text=True, encoding="oem", errors="replace")
    (work / f"build-{arch}.log").write_text(r.stdout + r.stderr, encoding="utf-8")
    if r.returncode:
        print(r.stdout + r.stderr)
        raise RuntimeError("build failed")
    return work / exe


def run(exe, args=()):
    r = subprocess.run([str(exe), *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build/agent-c-hw/host"))
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--arch", action="append", default=None)
    a = ap.parse_args()
    work = Path(a.out).resolve()
    work.mkdir(parents=True, exist_ok=True)
    make_shim(work)
    copy_sources(work)
    ok = True
    for arch in a.arch or ["x64", "x86"]:
        exe = build(work, arch)
        code, out = run(exe)
        (work / f"test-{arch}.log").write_text(out, encoding="utf-8")
        print(f"[{arch}] exit={code}\n{out}")
        if code:
            ok = False
        if a.negative_controls and arch == "x64":
            count = re.search(r"^HT negctl-count (\d+)", out, re.M)
            if count is None:
                print("host test did not complete; negative controls skipped")
                ok = False
                continue
            nneg = int(count[1])
            for n in range(1, nneg + 1):
                code, out = run(exe, [f"neg={n}"])
                (work / f"test-{arch}-neg{n}.log").write_text(out, encoding="utf-8")
                fails = re.findall(r"^HT FAIL (\S+)", out, re.M)
                want = re.search(r"^HT negctl (\d+) expects (\S+)", out, re.M)
                allowed = {1: {"matrices", "vertex_packing", "clipping", "triangle_lists"},
                           3: {"clipping", "triangle_lists"}}.get(n, {want[2]} if want else set())
                good = code != 0 and want is not None and want[2] in fails and set(fails) <= allowed and "HT COMPLETE" in out
                print(f"[{arch}] negative control {n}: exit={code}, failing groups {fails}, expected {want[2] if want else '?'} -> {'ok' if good else 'NOT RED'}")
                if not good:
                    ok = False
    print("HOST TEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
