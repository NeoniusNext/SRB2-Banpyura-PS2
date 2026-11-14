"""Extract frozen/current complete tilted drawers into a separate external-global TU.

No drawing, texture sampling, lighting or slope conversion is reimplemented.
The EE functions TU deliberately uses no LTO, preventing fixture-specific alias
proofs from turning the frozen reference into the same optimized algorithm.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
NAMES = ["R_DrawTiltedTranslucentSpan_8", "R_DrawTiltedWaterSpan_8",
         "R_DrawTiltedSplat_8", "R_DrawTiltedTranslucentSplat_8",
         "R_DrawTiltedFloorSprite_8", "R_DrawTiltedTranslucentFloorSprite_8"]


def function(source, name):
    start = re.search(r"(?:static )?(?:void|inline UINT32) " + name + r"\(", source).start()
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: " " * len(m[0]), source, flags=re.S)
    begin = masked.index("{", start)
    depth, end = 1, begin + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return source[start:end] + "\n"


def prepare(work, base):
    work.mkdir(parents=True, exist_ok=True)
    hashes = {}
    for variant, directory in [("reference", base), ("candidate", ROOT / "src")]:
        source = (directory / "r_draw8.c").read_text()
        prefix = ""
        if "#define TILT_FLAT_INDEX" in source:
            prefix = source[source.index("#ifdef PS2_OPT_DRAW\n#define TILT_FLAT_INDEX"):
                            source.index("void R_DrawTiltedTranslucentSpan_8(void)")]
        body = prefix + "\n".join("__attribute__((noinline, noipa, externally_visible))\n"
                                   + function(source, name) for name in NAMES)
        for name in NAMES:
            body = re.sub(r"\b" + name + r"\b", variant + "_" + name, body)
        body += "\n#undef TILT_FLAT_INDEX\n#undef TILT_LIGHT\n#undef TILT_TRANSMAP\n"
        (work / (variant + ".inc")).write_text(body)
        hashes[variant] = hashlib.sha256(source.encode()).hexdigest()
    source = (base / "r_draw.c").read_text()
    a = source.index("static INT32 tiltlighting")
    b = source.index("// =========================================================================", a)
    lighting = source[a:b].replace("static INT32 tiltlighting", "INT32 tiltlighting", 1)
    (work / "lighting.inc").write_text(lighting)
    (work / "functions.c").write_text('#include "next3_render_tilted_stub.h"\n#include "lighting.inc"\n'
                                       '#include "reference.inc"\n#include "candidate.inc"\n')
    candidate = (work / "candidate.inc").read_text()
    # Mutate actual extracted source lookups, preserving array expressions.
    # Balance each mutated subscript by parsing its original square brackets.
    result, pos = [], 0
    for match in re.finditer(r"\bsource\[", candidate):
        start = match.start()
        end, depth = match.end(), 1
        while depth:
            depth += (candidate[end] == "[") - (candidate[end] == "]")
            end += 1
        result.extend([candidate[pos:start], "(1 ^ ", candidate[start:end], ")"])
        pos = end
    result.append(candidate[pos:])
    broken = "".join(result)
    if broken == candidate:
        raise SystemExit("Actual-lookup negative control did not mutate source")
    (work / "candidate-negative.inc").write_text(broken)
    (work / "functions-negative.c").write_text((work / "functions.c").read_text().replace(
        '"candidate.inc"', '"candidate-negative.inc"'))
    (work / "source-hashes.json").write_text(json.dumps(hashes, indent=2) + "\n")


def run(command, log, check=True):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, cwd=ROOT)
    log.write_text(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    if check and result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(f"Command failed ({result.returncode}); see {log}")
    return result


def host(work, args):
    results = {}
    for policy, extra in [("float", []), ("double", ["-DPS2_NOOPT_SLOPE"]),
                          ("noopt-draw", ["-DPS2_NOOPT_DRAW"]), ("noopt", ["-DPS2_NOOPT"])]:
        binary = work / policy
        flags = [args.cc, "-O2", "-std=gnu17", "-fwrapv", "-ffp-contract=off", "-DPS2_PROFILE",
                 "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", *extra,
                 "-I" + str(work), "-I" + str(TOOLS), "-I" + str(ROOT / "src")]
        run([*flags, TOOLS / "next3_render_tilted_hosttest.c", work / "functions.c", "-o", binary],
            work / (policy + "-build.log"))
        output = run([binary], work / (policy + "-test.log")).stdout
        results[policy] = output.strip()
        print(policy + ": " + output.strip(), flush=True)
        if args.negative_controls:
            result = run([binary, "broken-pixel"], work / (policy + "-negative-pixel.log"), False)
            if result.returncode != 1:
                raise SystemExit(policy + " pixel corruption not detected")
            negative = work / (policy + "-negative")
            run([*flags, TOOLS / "next3_render_tilted_hosttest.c", work / "functions-negative.c", "-o", negative],
                work / (policy + "-negative-build.log"))
            result = run([negative], work / (policy + "-negative-lookup.log"), False)
            if result.returncode != 1:
                raise SystemExit(policy + " actual-lookup mutation not detected")
            print(policy + ": detected pixel and actual-lookup negative controls", flush=True)
        if args.benchmark and policy == "float":
            run([binary, "bench"], work / "host-bench.log")
    if args.sanitize:
        binary = work / "asan"
        flags = [args.cc, "-O1", "-g", "-std=gnu17", "-fwrapv", "-ffp-contract=off", "-DPS2_PROFILE",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                 "-I" + str(work), "-I" + str(TOOLS), "-I" + str(ROOT / "src")]
        run([*flags, TOOLS / "next3_render_tilted_hosttest.c", work / "functions.c", "-o", binary],
            work / "asan-build.log")
        output = run([binary], work / "asan-test.log").stdout
        if "runtime error:" in (work / "asan-test.log").read_text():
            raise SystemExit("Sanitizer reported undefined behavior")
        print("asan: " + output.strip(), flush=True)
        results["asan"] = output.strip()
    (work / "host-report.json").write_text(json.dumps(results, indent=2) + "\n")


def ee(work):
    sys.path.insert(0, str(TOOLS))
    import build as B
    flags = [f for f in B.CFLAGS if not f.startswith("-MMD") and f not in ("-MP", "-flto")]
    objects = []
    for name, source in [("functions", work / "functions.c"),
                         ("driver", TOOLS / "next3_render_tilted_hosttest.c")]:
        obj = work / (name + ".o")
        run([B.CC, *flags, "-Werror", "-DNEXT3_TILTED_EE", "-I" + str(TOOLS),
             "-I" + str(work), *B.INCS, "-c", source, "-o", obj], work / ("ee-" + name + "-build.log"))
        objects.append(obj)
    binary = work / "TILTED.ELF"
    run([B.CC, *B.LDFLAGS, *objects, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "ee-link.log")
    print("Built " + str(binary), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--base-src", type=Path, required=True)
    p.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    p.add_argument("--negative-controls", action="store_true")
    p.add_argument("--sanitize", action="store_true")
    p.add_argument("--benchmark", action="store_true")
    p.add_argument("--ee-build", action="store_true")
    p.add_argument("--ee-only", action="store_true")
    a = p.parse_args()
    work = a.out.resolve()
    prepare(work, a.base_src.resolve())
    if not a.ee_only:
        host(work, a)
    if a.ee_build or a.ee_only:
        ee(work)


if __name__ == "__main__":
    main()
