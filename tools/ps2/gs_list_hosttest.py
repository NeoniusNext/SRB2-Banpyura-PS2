"""Compile the production indexed GIF encoders and compare native GS events.

The extracted production functions are copied verbatim, with minimal capture-only
state in the C fixture. This avoids the obsolete Windows-only full-driver shim.
"""
import argparse
import hashlib
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def section(source, start, end):
    if source.count(start) != 1:
        raise RuntimeError(f"Missing or ambiguous production anchor: {start}")
    first = source.index(start)
    return source[first:source.index(end, first)]


def extract(work):
    hw = ROOT / "src/ps2/hw"
    priv = (hw / "ps2_hw_priv.inc").read_text()
    draw = (hw / "ps2_hw_draw.inc").read_text()
    xform = (hw / "ps2_hw_xform.inc").read_text()
    model = (hw / "ps2_hw_model.inc").read_text()
    defs = (ROOT / "src/hardware/hw_defs.h").read_text()
    types = [
        section(priv, "typedef union\n{\n\tu64 d[2];", "static inline u32 cyc"),
        section(draw, "typedef struct\n{\n\tu64 alpha, test;", "// light ramp / fog specification"),
        section(xform, "typedef struct\n{\n\tfloat x, y, z, w;", "static inline int outcode"),
        section(draw, "typedef struct\n{\n\tint fmt; // 0 ST/", "static inline u32 fog_of_class"),
        section(model, "typedef struct\n{\n\ts32 xi, yi, zi;", "// the work areas"),
        section(defs, "typedef struct\n{\n\tFLOAT       x,y,z;\n\tFLOAT       s;", "} FOutVector;") + "} FOutVector;\n",
        section(draw, "typedef struct\n{\n\tfloat sb, tb; //", "static qw_t *ws_pack"),
        section(priv, "#define HWGIF_TAG", "#define PS2HWD_MAXPOLY"),
    ]
    code = [
        section(draw, "static u8 col_lut[256];", "#define TEST_BITS"),
        section(draw, "static inline float clampf", "// packing of one pass"),
        xform[xform.index("static inline int outcode"):],
        section(draw, "static inline qw_t *put_vertex", "// REGLIST sends native"),
        section(draw, "static inline u32 pass_prim", "// ---- cutting a convex polygon"),
        section(draw, "// ---- triangle lists", "// ---- clear ----"),
        section(model, "static inline void mdl_prepare_v", "// the GS state of a row"),
    ]
    (work / "production-types.h").write_text("\n".join(types))
    (work / "production-code.h").write_text("\n".join(code))
    hashes = {name: hashlib.sha256((hw / name).read_bytes()).hexdigest()
              for name in ("ps2_hw_draw.inc", "ps2_hw_model.inc")}
    (work / "production-sha256.txt").write_text(
        "".join(f"{digest}  {name}\n" for name, digest in hashes.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="build/gs-list-opt-host")
    ap.add_argument("--cc", default=shutil.which("cc") or "cc")
    ap.add_argument("--sanitize", action="store_true")
    args = ap.parse_args()
    work = Path(args.out).resolve()
    work.mkdir(parents=True, exist_ok=True)
    extract(work)
    exe = work / "gs_list_hosttest"
    command = [args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-ffp-contract=off", "-I", str(work),
               str(ROOT / "tools/ps2/gs_list_hosttest.c"), "-o", str(exe), "-lm"]
    if args.sanitize:
        command[2:2] = ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    result = subprocess.run(command, text=True, capture_output=True)
    (work / "build.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stdout + result.stderr)
        return 1
    passed = True
    for negative in range(6):
        result = subprocess.run([str(exe), str(negative)], text=True, capture_output=True)
        output = result.stdout + result.stderr
        (work / f"run-{negative}.log").write_text(output)
        expected = result.returncode == 0 if negative == 0 else (
            result.returncode == 1 and "mutated=1" in output and "FAIL" in output)
        print(f"negative={negative} {'PASS' if expected else 'FAIL'} " + output.strip().splitlines()[-1])
        passed &= expected
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
