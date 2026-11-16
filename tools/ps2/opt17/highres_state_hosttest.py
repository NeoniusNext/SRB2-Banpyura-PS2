"""Exercise actual video-size/viewport/capture code, without starting PCSX2.

The source root is explicit so a frozen renderer can be audited while unrelated
working-tree changes remain present. GS init/shutdown are interface stubs; size
selection, SetScreenSize, transforms, pool/capture code and cache keys are the
production source. Known capture limits are reported, not counted as coverage.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def function(text, name):
    match = re.search(r"^(?:static )?(?:inline )?\w+[ *]+" + re.escape(name) + r"\([^\n]*\)\n\{", text, re.M)
    if not match:
        raise RuntimeError("missing production function: " + name)
    depth = 0
    start = text.index("{", match.start())
    for pos in range(start, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if not depth:
                return text[match.start():pos + 1] + "\n"
    raise RuntimeError("unterminated production function: " + name)


def prepare(source, work, negative):
    spec = importlib.util.spec_from_file_location("hw_hosttest", ROOT / "tools/ps2/hw_hosttest.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    helper.ROOT = source
    helper.HW = source / "src/ps2/hw"
    work.mkdir(parents=True, exist_ok=True)
    helper.make_shim(work)
    helper.copy_sources(work, msvc=False)
    gs = (helper.HW / "ps2_hw_gs.inc").read_text()
    driver = (helper.HW / "ps2_hwd.c").read_text()
    vu = (helper.HW / "ps2_hw_vu1.inc").read_text()
    key_source = vu[vu.index("#define VK_N 32"):vu.index("// PS2-HW-111: the texture registers")]
    code = function(gs, "hw_choose_size") + function(driver, "view_defaults") + function(driver, "PS2HWD_SetScreenSize")
    if negative == "scale":
        code = code.replace("H.sy = (float)H.vh / (float)h;", "H.sy = 1.0f;")
    elif negative == "viewport":
        code = code.replace("viewport_set(0, 0, w, h);", "/* negative: stale viewport */")
    elif negative == "palette":
        code = code.replace("memcpy(H.pal, pal, sizeof pal);", "memset(H.pal, 0, sizeof pal);")
    (work / "size-production.inc").write_text(code)
    (work / "key-production.inc").write_text(key_source)
    plan = (helper.HW / "ps2_hw_plan.inc").read_text()
    (work / "reset-production.inc").write_text("static int plan_n, imm_ncur, imm_nprev, plan_active;\nstatic u32 plan_frame;\n" + function(plan, "plan_reset"))
    (work / "base_hosttest.c").write_text((ROOT / "tools/ps2/hw_hosttest.c").read_text())
    (work / "ps2_vmodes.h").write_text((source / "src/ps2/ps2_vmodes.h").read_text())
    paths = [source / "src/ps2/ps2_vmodes.h", helper.HW / "ps2_hwd.c", *sorted(helper.HW.glob("*.inc"))]
    (work / "source-sha256.json").write_text(json.dumps({str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}, indent=2) + "\n")


def run(source, work, cc, negative="", sanitize=False):
    prepare(source, work, negative)
    binary = work / "test"
    command = [*shlex.split(cc), "-std=gnu17", "-O2", "-g", "-fwrapv", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-but-set-variable", "-DPS2_PROFILE",
               "-I" + str(work), "-I" + str(source / "src/hardware"), str(Path(__file__).with_suffix(".c")), "-o", str(binary), "-lm"]
    if sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    built = subprocess.run(command, text=True, capture_output=True)
    (work / "build.log").write_text(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError("host build failed: " + str(work / "build.log"))
    result = subprocess.run([str(binary)], text=True, capture_output=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    output = result.stdout + result.stderr
    (work / "test.log").write_text(output)
    passed = result.returncode == 0 and "HIGHRES COMPLETE failed=0" in output and "runtime error:" not in output
    return {"negative": negative, "sanitize": sanitize, "rc": result.returncode, "passed": passed, "log": str(work / "test.log")}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov17-highres/host")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--sanitize", action="store_true")
    a = ap.parse_args()
    rows = [run(a.source_root.resolve(), a.out.resolve() / "normal", a.cc)]
    if a.sanitize:
        rows.append(run(a.source_root.resolve(), a.out.resolve() / "sanitize", a.cc, sanitize=True))
    if a.negative_controls:
        rows += [run(a.source_root.resolve(), a.out.resolve() / ("negative-" + name), a.cc, negative=name) for name in ("scale", "viewport", "palette")]
    print(json.dumps(rows, indent=2))
    (a.out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    return int(any(row["passed"] == bool(row["negative"]) for row in rows))


if __name__ == "__main__":
    raise SystemExit(main())
