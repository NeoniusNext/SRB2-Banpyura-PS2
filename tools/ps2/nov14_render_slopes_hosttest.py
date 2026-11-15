"""Compare actual frozen/current NPOT slope drawers; optionally build an EE bench.

The generated reference is extracted from --base-src, never from the candidate.
Both use the same preexisting slope precision policy. The fixture compares every
framebuffer/canary byte after each draw and checks the public ds_x1 side effect.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
NAMES = [
    "R_DrawTiltedSpan_NPO2_8",
    "R_DrawTiltedTranslucentSpan_NPO2_8",
    "R_DrawTiltedSplat_NPO2_8",
    "R_DrawTiltedTranslucentSplat_NPO2_8",
    "R_DrawTiltedFloorSprite_NPO2_8",
    "R_DrawTiltedTranslucentFloorSprite_NPO2_8",
    "R_DrawTiltedWaterSpan_NPO2_8",
]


def function(source, name):
    start = re.search(r"(?:static )?(?:void|inline UINT32) " + name + r"\(", source).start()
    # Mask strings/comments before counting braces; preserve their byte offsets.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: " " * len(m[0]), source, flags=re.S)
    begin = masked.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return source[start:end] + "\n"


def prepare(work, base):
    work.mkdir(parents=True, exist_ok=True)
    for variant, directory in [("reference", base), ("candidate", ROOT / "src")]:
        source = (directory / "r_draw8_npo2.c").read_text()
        prefix = ""
        if variant == "candidate":
            prefix = "#ifdef PS2_OPT_DRAW\n" + function(source, "R_NPO2SlopeDividers") + "#endif\n"
        body = prefix + "\n".join(function(source, name) for name in NAMES)
        for name in NAMES:
            body = re.sub(r"\b" + name + r"\b", variant + "_" + name, body)
        (work / (variant + ".inc")).write_text(body)
    source = (base / "r_draw.c").read_text()
    start = source.index("static INT32 tiltlighting")
    end = source.index("// =========================================================================", start)
    (work / "lighting.inc").write_text(source[start:end])


def run_logged(command, log):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, cwd=ROOT)
    log.write_text(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(f"Command failed ({result.returncode}); see {log}")
    return result.stdout


def host(work, args):
    results = {}
    for policy, extra in [("float", []), ("double", ["-DPS2_NOOPT_SLOPE"]),
                          ("noopt", ["-DPS2_NOOPT"])]:
        binary = work / policy
        command = [args.cc, "-O2", "-std=gnu17", "-fwrapv", "-ffp-contract=off", "-DPS2_PROFILE",
                   "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", *extra,
                   "-I" + str(work), "-I" + str(ROOT / "src"),
                   TOOLS / "nov14_render_slopes_hosttest.c", "-o", binary, "-lm"]
        run_logged(command, work / (policy + "-build.log"))
        output = run_logged([binary], work / (policy + "-test.log"))
        print(policy + ": " + output.strip())
        results[policy] = output
        if args.negative_controls:
            for negative in ["broken-pixel", "broken-divider"]:
                result = subprocess.run([str(binary), negative], capture_output=True, text=True)
                (work / (policy + "-" + negative + ".log")).write_text(result.stdout + result.stderr)
                if result.returncode != 1:
                    raise SystemExit(policy + " " + negative + " control not detected")
                print(policy + ": detected " + negative)
        for axis in ["width", "height"]:
            for variant in ["reference", "candidate"]:
                invalid = "zero-" + axis + "-" + variant
                result = subprocess.run([str(binary), invalid], capture_output=True, text=True)
                (work / (policy + "-" + invalid + ".log")).write_text(result.stdout + result.stderr)
                if result.returncode != 2 or "divider must be != 0" not in result.stderr:
                    raise SystemExit(policy + " " + invalid + " changed the libdivide error path")
        if args.benchmark and policy == "float":
            output = run_logged([binary, "bench"], work / "host-bench.log")
            print("host benchmark: " + str(output.count("SLOPEBENCH ")) + " rows; see " + str(work / "host-bench.log"))
    if args.sanitize:
        binary = work / "asan"
        command = [args.cc, "-O1", "-g", "-std=gnu17", "-fwrapv", "-ffp-contract=off",
                   "-DPS2_PROFILE", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-I" + str(work), "-I" + str(ROOT / "src"),
                   TOOLS / "nov14_render_slopes_hosttest.c", "-o", binary, "-lm"]
        run_logged(command, work / "asan-build.log")
        output = run_logged([binary], work / "asan-test.log")
        if "runtime error:" in (work / "asan-test.log").read_text():
            raise SystemExit("Undefined behavior sanitizer reported an error")
        print("asan: " + output.strip())
    (work / "host-report.json").write_text(json.dumps(results, indent=2) + "\n")


def ee(work):
    sys.path.insert(0, str(TOOLS))
    import build as B
    flags = [f for f in B.CFLAGS if not f.startswith("-MMD") and f != "-MP"]
    obj = work / "slopes.o"
    run_logged([B.CC, *flags, "-Werror", "-DNOV14_SLOPES_EE", "-I" + str(work), *B.INCS,
                "-c", TOOLS / "nov14_render_slopes_hosttest.c", "-o", obj], work / "ee-build.log")
    binary = work / "SLOPES.ELF"
    run_logged([B.CC, *B.LDFLAGS, obj, "-o", binary, "-ldebug", "-lpatches", "-lm"],
               work / "ee-link.log")
    print("Built EE benchmark " + str(binary) + "; run through run_pcsx2.py --until 'SLOPES DONE'")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--base-src", type=Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--ee-build", action="store_true")
    parser.add_argument("--ee-only", action="store_true")
    args = parser.parse_args()
    work = args.out.resolve()
    prepare(work, args.base_src.resolve())
    if not args.ee_only:
        host(work, args)
    if args.ee_build or args.ee_only:
        ee(work)


if __name__ == "__main__":
    main()
