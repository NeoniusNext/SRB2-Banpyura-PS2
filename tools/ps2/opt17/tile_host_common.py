"""Shared source preparation for the OPT17 tiled-source host fixtures."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def prepare(source, work):
    source, work = source.resolve(), work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("hw_hosttest", ROOT / "tools/ps2/hw_hosttest.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    helper.ROOT, helper.HW = source, source / "src/ps2/hw"
    helper.make_shim(work)
    helper.copy_sources(work, msvc=False)
    data = (source / "src/hardware/hw_data.h").read_text()
    start = data.index("#if defined(PS2) && defined(PS2_PROFILE)\n#define HWR_PS2_CPU_TILED_SOURCE")
    end = data.index("//\n// Level textures", start)
    shim = work / "shim.h"
    shim.write_text(shim.read_text() + "\n" + data[start:end])
    base = (ROOT / "tools/ps2/hw_hosttest.c").read_text()
    base = base.replace('#include "ps2_hw_tex.inc"',
                        'void HWR_PS2_RegenerateMipmap(GLMipmap_t *m) { (void)m; }\n#include "ps2_hw_tex.inc"')
    (work / "base_hosttest.c").write_text(base)
    paths = [*sorted((source / "src/ps2/hw").glob("*.inc")), source / "src/hardware/hw_data.h"]
    return {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}


def compile_run(source, work, fixture, sanitize=False):
    command = ["cc", "-std=gnu17", "-O2", "-g", "-fwrapv", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-but-set-variable",
               "-Wno-misleading-indentation", "-DPS2_PROFILE", "-DPS2", "-I" + str(work),
               "-I" + str(source / "src/hardware"), str(fixture), "-o", str(work / "test"), "-lm"]
    if sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    (work / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    built = subprocess.run(command, text=True, capture_output=True)
    (work / "build.log").write_text(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError("Host compile failed: " + str(work / "build.log"))
    ran = subprocess.run([str(work / "test")], text=True, capture_output=True,
                         env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    (work / "test.log").write_text(ran.stdout + ran.stderr)
    print(ran.stdout + ran.stderr, end="")
    return ran


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("Negative-control anchor missing or ambiguous: " + repr(old))
    return text.replace(old, new)
