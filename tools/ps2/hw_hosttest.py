"""Host test of the GS renderer driver's pure logic: transform, clipping, vertex/GIF packing, register values,
VRAM pool, texture conversion and upload packets. The driver's own source (src/ps2/hw/ps2_hw_*.inc) is compiled as it is;
the engine type definitions come from the real headers (sections copied verbatim into the work directory).

python tools/ps2/hw_hosttest.py --negative-controls [--out build/agent-c-hw/host]
Windows uses MSVC x64 and x86; other hosts use $CC (or gcc/clang/cc), native architecture.
--cc selects a GCC-compatible compiler; --arch x64 / --arch x86 request -m64 / -m32.
Each listed negative control deliberately mutates the reference or code under test.
The executable must fail its named group, with only the documented dependent groups allowed to fail too.
"""
import argparse
import json
import os
import re
import shlex
import shutil
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
         "typedef struct GSGLOBAL GSGLOBAL;\ntypedef int8_t s8; typedef int16_t s16; typedef int32_t s32;\ntypedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;\n",
        '#include "hw_model.h"\n',
    ]
    (work / "shim.h").write_text("\n".join(parts), encoding="utf-8")


def copy_sources(work, msvc=True):
    """Copy include dependencies; keep GCC alignment attributes on compatible hosts."""
    for source in sorted([*HW.glob("*.inc"), *HW.glob("*.h")]):
        text = source.read_text()
        text = text.replace('#include "../../doomtype.h"', '#include "shim.h"')
        if msvc:
            text = re.sub(r"__attribute__\(\(aligned\(\d+\)\)\)", "", text)
        (work / source.name).write_text(text, encoding="utf-8")
    (work / "hw_model.h").write_text((ROOT / "src/hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ''), encoding="utf-8")


def build_msvc(work, arch):
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


def build_native(work, arch, cc):
    exe = work / (f"hw_hosttest-{arch}" + (".exe" if os.name == "nt" else ""))
    # The included production translation unit has unused engine entry points and
    # diagnostics. Keep all other warnings fatal, especially missing declarations,
    # format errors and bounds/overflow diagnostics.
    command = [*cc, "-std=gnu17", "-O2", "-g", "-fwrapv", "-ffp-contract=off",
               "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
               "-Wno-unused-parameter", "-Wno-unused-but-set-variable", "-DPS2_PROFILE",
               "-I" + str(work), "-I" + str(ROOT / "src/hardware")]
    if arch != "native":
        command.append({"x64": "-m64", "x86": "-m32"}[arch])
    command += [str(ROOT / "tools/ps2/hw_hosttest.c"), "-o", str(exe), "-lm"]
    (work / f"build-{arch}-command.json").write_text(json.dumps(command, indent=2) + "\n", encoding="utf-8")
    r = subprocess.run(command, cwd=work, capture_output=True, text=True)
    (work / f"build-{arch}.log").write_text(r.stdout + r.stderr, encoding="utf-8")
    if r.returncode:
        print(r.stdout + r.stderr)
        raise RuntimeError("build failed")
    return exe


def run(exe, args=()):
    r = subprocess.run([str(exe), *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build/agent-c-hw/host"))
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--arch", action="append", choices=("native", "x64", "x86"))
    ap.add_argument("--cc", help="GCC-compatible compiler command (default: $CC or gcc/clang/cc on non-Windows)")
    a = ap.parse_args()
    msvc = os.name == "nt" and a.cc is None
    arches = a.arch or (["x64", "x86"] if msvc else ["native"])
    if msvc and "native" in arches:
        ap.error("MSVC requires --arch x64 or --arch x86")
    cc = []
    if not msvc:
        compiler = a.cc or os.environ.get("CC") or next((c for c in ("gcc", "clang", "cc") if shutil.which(c)), None)
        if not compiler:
            ap.error("no host C compiler found; use --cc")
        cc = shlex.split(compiler)
        if not cc or not shutil.which(cc[0]):
            ap.error("host compiler not found: " + compiler)
    work = Path(a.out).resolve()
    work.mkdir(parents=True, exist_ok=True)
    make_shim(work)
    copy_sources(work, msvc=msvc)
    ok = True
    for arch in arches:
        exe = build_msvc(work, arch) if msvc else build_native(work, arch, cc)
        code, out = run(exe)
        (work / f"test-{arch}.log").write_text(out, encoding="utf-8")
        print(f"[{arch}] exit={code}\n{out}")
        complete = re.search(r"^HT COMPLETE groups=(\d+) failed=(\d+)$", out, re.M)
        group_count = int(complete[1]) if complete else 0
        if (code or not complete or group_count == 0 or int(complete[2]) != 0
                or re.search(r"^HT FAIL ", out, re.M)
                or len(re.findall(r"^HT PASS ", out, re.M)) != group_count):
            ok = False
        if a.negative_controls:
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
                done = re.search(r"^HT COMPLETE groups=(\d+) failed=(\d+)$", out, re.M)
                good = (code == 1 and want is not None and int(want[1]) == n
                        and want[2] in fails and set(fails) <= allowed
                        and done is not None and int(done[1]) == group_count and int(done[2]) == len(fails))
                print(f"[{arch}] negative control {n}: exit={code}, failing groups {fails}, expected {want[2] if want else '?'} -> {'ok' if good else 'NOT RED'}")
                if not good:
                    ok = False
    print("HOST TEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
