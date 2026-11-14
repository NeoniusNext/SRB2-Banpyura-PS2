"""Compare complete shade/translated sprite drawers against a frozen source tree.

Optionally build a same-binary EE benchmark. Extracted drawers are compiled in a
separate non-LTO translation unit, against the same external global declarations
as the engine, to prevent fixture-specific buffer alias/address specialization.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
NAMES = ["R_DrawShadeColumn_8", "R_DrawTranslatedColumn_8",
         "R_DrawTranslatedTranslucentColumn_8"]


def function(source, name):
    start = source.index("void " + name + "(void)")
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
        source = (directory / "r_draw8.c").read_text()
        body = "\n".join("__attribute__((noinline, noipa, externally_visible))\n" + function(source, name)
                         for name in NAMES)
        for name in NAMES:
            body = re.sub(r"\b" + name + r"\b", variant + "_" + name, body)
        (work / (variant + ".inc")).write_text(body)
    (work / "functions.c").write_text('#include "next2_render_columns_stub.h"\n#include "reference.inc"\n#include "candidate.inc"\n')
    candidate = (work / "candidate.inc").read_text()
    broken = re.sub(r"\b(?:source|dc_source)\[[^\]]+\]", lambda m: "((UINT8)(" + m[0] + " ^ 1))", candidate)
    if broken == candidate:
        raise SystemExit("Actual-lookup negative control did not mutate source")
    (work / "candidate-negative.inc").write_text(broken)
    (work / "functions-negative.c").write_text((work / "functions.c").read_text().replace('"candidate.inc"', '"candidate-negative.inc"'))


def run(command, log):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, cwd=ROOT)
    log.write_text(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(f"Command failed ({result.returncode}); see {log}")
    return result.stdout


def host(work, args):
    results = {}
    for policy, extra in [("profile", []), ("noopt", ["-DPS2_NOOPT"]),
                          ("rangecheck", ["-DRANGECHECK"])]:
        binary = work / policy
        flags = [args.cc, "-O2", "-std=gnu17", "-fwrapv", "-ffp-contract=off", "-DPS2_PROFILE",
                 "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", *extra,
                 "-I" + str(work), "-I" + str(TOOLS), "-I" + str(ROOT / "src")]
        run([*flags, TOOLS / "next2_render_columns_hosttest.c", work / "functions.c", "-o", binary], work / (policy + "-build.log"))
        output = run([binary], work / (policy + "-test.log"))
        results[policy] = output.strip()
        print(policy + ": " + output.strip())
        if args.negative_controls:
            result = subprocess.run([str(binary), "broken-pixel"], capture_output=True, text=True)
            (work / (policy + "-negative.log")).write_text(result.stdout + result.stderr)
            if result.returncode != 1:
                raise SystemExit(policy + " negative control was not detected")
            print(policy + ": detected broken-pixel")
            negative = work / (policy + "-negative")
            run([*flags, TOOLS / "next2_render_columns_hosttest.c", work / "functions-negative.c", "-o", negative], work / (policy + "-negative-build.log"))
            result = subprocess.run([str(negative)], capture_output=True, text=True)
            (work / (policy + "-negative-lookup.log")).write_text(result.stdout + result.stderr)
            if result.returncode != 1:
                raise SystemExit(policy + " actual-lookup mutation was not detected")
            print(policy + ": detected actual-lookup mutation")
        if args.benchmark and policy == "profile":
            run([binary, "bench"], work / "host-bench.log")
    if args.sanitize:
        binary = work / "asan"
        flags = [args.cc, "-O1", "-g", "-std=gnu17", "-fwrapv", "-ffp-contract=off", "-DPS2_PROFILE",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I" + str(work), "-I" + str(TOOLS), "-I" + str(ROOT / "src")]
        run([*flags, TOOLS / "next2_render_columns_hosttest.c", work / "functions.c", "-o", binary], work / "asan-build.log")
        output = run([binary], work / "asan-test.log")
        if "runtime error:" in (work / "asan-test.log").read_text():
            raise SystemExit("Sanitizer reported undefined behavior")
        print("asan: " + output.strip())
        results["asan"] = output.strip()
    (work / "host-report.json").write_text(json.dumps(results, indent=2) + "\n")


def ee(work):
    import sys
    sys.path.insert(0, str(TOOLS))
    import build as B
    flags = [f for f in B.CFLAGS if not f.startswith("-MMD") and f != "-MP"]
    obj = work / "columns.o"
    functions = work / "functions.o"
    run([B.CC, *[f for f in flags if f != "-flto"], "-Werror", "-I" + str(TOOLS), "-I" + str(work),
         *B.INCS, "-c", work / "functions.c", "-o", functions], work / "ee-functions-build.log")
    run([B.CC, *flags, "-Werror", "-DNEXT2_COLUMNS_EE", "-I" + str(work),
         *B.INCS, "-c", TOOLS / "next2_render_columns_hosttest.c", "-o", obj], work / "ee-build.log")
    binary = work / "COLUMNS.ELF"
    run([B.CC, *B.LDFLAGS, obj, functions, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "ee-link.log")
    print("Built " + str(binary) + "; run through run_pcsx2.py --until 'COLUMNS DONE'")


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
