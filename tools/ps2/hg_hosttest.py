"""x86 (gcc, Linux) host test of the OPT10 HG geometry paths of the GS renderer driver: the driver's own sources (src/ps2/hw/ps2_hw_*.inc) are
compiled as they are (engine type definitions from the real headers, as tools/ps2/hw_hosttest.py does) and the new fast paths are compared with the
paths they replace:
  water   - water_fast (one sweep, ps2_hw_water.inc) against emit_water_poly (band by band), same ripple bands (-hwdbg WATERPOL)
  plancache - begin_draw with the plan cache against begin_draw_inner without it: identical GIF packets for random draw sequences
  clip    - clip_poly (per-plane distance arrays) against the plane-at-a-time reference: bit-identical vertices
  litclip - lit_fast_poly for clipped polygons (PS2-HW-62) against cut_and_emit (-hwdbg 8192): bit-identical GIF vertices
Every group has a negative control (neg=N: a deliberate mutation) that must turn it red.

python3 tools/ps2/hg_hosttest.py [--out build/hg-host] [--negative-controls]
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import hw_hosttest as T  # noqa: E402  (make_shim: the engine type definitions as a header)

HW = ROOT / "src/ps2/hw"


def copy_sources(work):
    for f in list(HW.glob("*.inc")) + list(HW.glob("*.h")):
        text = f.read_text()
        text = text.replace('#include "../../doomtype.h"', '#include "shim.h"')
        (work / f.name).write_text(text, encoding="utf-8")
    (work / "hw_model.h").write_text((ROOT / "src/hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ''), encoding="utf-8")


def build(work):
    exe = work / "hg_hosttest"
    cmd = ["gcc", "-std=gnu11", "-O2", "-g", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-misleading-indentation",
           "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-pointer-sign", "-Wno-stringop-overflow", "-Wno-array-bounds", "-DPS2_PROFILE", "-I" + str(work),
           str(ROOT / "tools/ps2/hg_hosttest.c"), "-o", str(exe), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (work / "build.log").write_text(r.stdout + r.stderr)
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit("build failed")
    return exe


def run(exe, args=()):
    r = subprocess.run([str(exe), *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build/hg-host"))
    ap.add_argument("--negative-controls", action="store_true")
    a = ap.parse_args()
    work = Path(a.out).resolve()
    work.mkdir(parents=True, exist_ok=True)
    T.make_shim(work)
    copy_sources(work)
    exe = build(work)
    code, out = run(exe)
    (work / "test.log").write_text(out)
    print(out)
    ok = code == 0
    if a.negative_controls:
        count = re.search(r"^HG negctl-count (\d+)", out, re.M)
        if count is None:
            print("host test did not complete; negative controls skipped")
            return 1
        for n in range(1, int(count[1]) + 1):
            code, out = run(exe, [f"neg={n}"])
            (work / f"test-neg{n}.log").write_text(out)
            fails = re.findall(r"^HG FAIL (\S+)", out, re.M)
            want = re.search(r"^HG negctl (\d+) expects (\S+)", out, re.M)
            good = code != 0 and want is not None and want[2] in fails and "HG COMPLETE" in out
            print(f"negative control {n}: exit={code}, failing groups {fails}, expected {want[2] if want else '?'} -> {'ok' if good else 'NOT RED'}")
            ok = ok and good
    print("HG HOST TEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
