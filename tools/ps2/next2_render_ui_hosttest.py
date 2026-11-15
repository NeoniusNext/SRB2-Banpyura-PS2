"""Compare complete cropped-patch drawers with frozen-current-baseline source.

Generates the unchanged flag/type declarations and pixel helpers from the source
tree. The separately compiled actual drawers preserve scaling/crop arithmetic.
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
NAME = "V_DrawCroppedPatch"
HELPERS = ["standardpdraw", "mappedpdraw", "translucentpdraw", "transmappedpdraw"]


def function(source, name):
    start = re.search(r"(?:static )?(?:inline )?(?:UINT8|UINT32|UINT64|void) " + name + r"\(", source).start()
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
    vflags = (base / "v_video.h").read_text()
    flags = "\n".join(line for line in vflags.splitlines()
                      if re.match(r"#define V_(?:PARAMMASK|SCALEPATCHMASK|NOSCALEPATCH|SMALLSCALEPATCH|MEDSCALEPATCH|ALPHASHIFT|ALPHAMASK|BLENDSHIFT|BLENDMASK|FLIP|SNAPTOTOP|SNAPTOBOTTOM|SNAPTOLEFT|SNAPTORIGHT|NOSCALESTART|PERPLAYER)\s", line))
    structs = (base / "r_defs.h").read_text()
    start = structs.index("typedef struct\n{\n\tunsigned topdelta;")
    end = structs.index("} column_t;", start) + len("} column_t;")
    types = structs[start:end] + "\n"
    start = structs.index("typedef struct\n{\n\tINT16 width, height;")
    end = structs.index("} patch_t;", start) + len("} patch_t;")
    types += structs[start:end] + "\n"
    screen = (base / "screen.h").read_text()
    start = screen.index("typedef struct viddef_s")
    end = screen.index("} viddef_t;", start) + len("} viddef_t;")
    types += screen[start:end] + "\n"
    declarations = []
    for variant, directory in [("reference", base), ("candidate", ROOT / "src")]:
        source = (directory / "v_video.c").read_text()
        start = source.index("static UINT8 hudplusalpha")
        end = source.index("static inline UINT8 standardpdraw", start)
        body = source[start:end] + "\n".join(function(source, n) for n in HELPERS)
        draw = function(source, NAME)
        body += "__attribute__((noinline, noipa, externally_visible))\n" + draw
        body += "const UINT8 *ui_cmap(void) { return v_colormap; }\nconst UINT8 *ui_trans(void) { return v_translevel; }\n"
        for n in HELPERS + [NAME, "hudplusalpha", "hudminusalpha", "v_colormap", "v_translevel", "ui_cmap", "ui_trans"]:
            body = re.sub(r"\b" + n + r"\b", variant + "_" + n, body)
        declarations.append(draw[:draw.index("{")].replace(NAME, variant + "_" + NAME) + ";\n"
                            + f"const UINT8 *{variant}_ui_cmap(void);\nconst UINT8 *{variant}_ui_trans(void);\n")
        (work / (variant + ".inc")).write_text(body)
    header = '#ifndef NEXT2_UI_GLOBALS_H\n#define NEXT2_UI_GLOBALS_H\n#include <stddef.h>\n#include "m_fixed.h"\n#define DNWH void *\n#define BASEVIDWIDTH 320\n#define BASEVIDHEIGHT 200\n' + flags + "\n" + types
    header += """
enum { render_soft = 1, render_opengl = 2, render_none = 3 };
typedef struct { int dummy; } testplayer_t;
extern INT32 rendermode, splitscreen, displayplayer, st_translucency;
extern INT32 ps2mh_recording;
extern testplayer_t players[4], *stplyr;
extern viddef_t vid;
extern UINT8 *screens[5];
UINT8 *R_GetBlendTable(int style, INT32 alphalevel);
boolean PS2MenuHints_NoteCropped(fixed_t x, fixed_t y, fixed_t hs, fixed_t vs, INT32 flags, const patch_t *patch, fixed_t w, fixed_t h);
void HWR_DrawCroppedPatch(patch_t *patch, fixed_t x, fixed_t y, fixed_t hs, fixed_t vs, INT32 flags, const UINT8 *cm, fixed_t sx, fixed_t sy, fixed_t w, fixed_t h);
""" + "\n".join(declarations) + "\n#endif\n"
    (work / "ui_globals.h").write_text(header)
    math_source = (base / "m_fixed.c").read_text()
    math_body = "\n".join(function(math_source, n) for n in ["PS2_Divlu", "PS2_MulU32", "PS2_FixedDivMag"])
    (work / "functions.c").write_text('#include "ui_globals.h"\n#ifdef PS2_OPT_MATH\n' + math_body
                                      + '\n#endif\n#include "reference.inc"\n#include "candidate.inc"\n')
    candidate = (work / "candidate.inc").read_text()
    broken = candidate.replace("source[ofs>>FRACBITS]", "((UINT8)(source[ofs>>FRACBITS] ^ 1))")
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
    for policy, extra in [("profile", []), ("noopt", ["-DPS2_NOOPT"]), ("hardware", ["-DHWRENDER"]),
                          ("ps2hooks", ["-DPS2"])]:
        binary = work / policy
        flags = [args.cc, "-O2", "-std=gnu17", "-fwrapv", "-DPS2_PROFILE", "-Wall", "-Wextra", "-Werror", *extra,
                 "-I" + str(work), "-I" + str(ROOT / "src")]
        run([*flags, TOOLS / "next2_render_ui_hosttest.c", work / "functions.c", "-o", binary], work / (policy + "-build.log"))
        output = run([binary], work / (policy + "-test.log"))
        results[policy] = output.strip()
        print(policy + ": " + output.strip())
        if args.negative_controls:
            result = subprocess.run([str(binary), "broken-pixel"], capture_output=True, text=True)
            (work / (policy + "-negative.log")).write_text(result.stdout + result.stderr)
            if result.returncode != 1:
                raise SystemExit(policy + " negative control was not detected")
            negative = work / (policy + "-negative")
            run([*flags, TOOLS / "next2_render_ui_hosttest.c", work / "functions-negative.c", "-o", negative], work / (policy + "-negative-build.log"))
            result = subprocess.run([str(negative)], capture_output=True, text=True)
            (work / (policy + "-negative-lookup.log")).write_text(result.stdout + result.stderr)
            if result.returncode != 1:
                raise SystemExit(policy + " actual-lookup mutation was not detected")
    if args.sanitize:
        binary = work / "asan"
        flags = [args.cc, "-O1", "-g", "-std=gnu17", "-fwrapv", "-DPS2_PROFILE", "-fsanitize=address,undefined",
                 "-fno-omit-frame-pointer", "-I" + str(work), "-I" + str(ROOT / "src")]
        run([*flags, TOOLS / "next2_render_ui_hosttest.c", work / "functions.c", "-o", binary], work / "asan-build.log")
        output = run([binary], work / "asan-test.log")
        if "runtime error:" in (work / "asan-test.log").read_text():
            raise SystemExit("Sanitizer reported undefined behavior")
        print("asan: " + output.strip())
        results["asan"] = output.strip()
    (work / "host-report.json").write_text(json.dumps(results, indent=2) + "\n")


def ee(work):
    sys.path.insert(0, str(TOOLS))
    import build as B
    flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-flto")]
    functions, obj = work / "functions.o", work / "ui.o"
    run([B.CC, *[f for f in flags if f != "-flto"], "-Werror", "-I" + str(work), *B.INCS,
         "-c", work / "functions.c", "-o", functions], work / "ee-functions-build.log")
    run([B.CC, *flags, "-Werror", "-DNEXT2_UI_EE", "-I" + str(work), *B.INCS,
         "-c", TOOLS / "next2_render_ui_hosttest.c", "-o", obj], work / "ee-driver-build.log")
    binary = work / "UI.ELF"
    run([B.CC, *B.LDFLAGS, obj, functions, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "ee-link.log")
    print("Built " + str(binary) + "; run through run_pcsx2.py --until 'UI DONE'")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--base-src", type=Path, required=True)
    p.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    p.add_argument("--negative-controls", action="store_true")
    p.add_argument("--sanitize", action="store_true")
    p.add_argument("--ee-only", action="store_true")
    p.add_argument("--ee-build", action="store_true")
    args = p.parse_args()
    work = args.out.resolve()
    prepare(work, args.base_src.resolve())
    if not args.ee_only:
        host(work, args)
    if args.ee_only or args.ee_build:
        ee(work)


if __name__ == "__main__":
    main()
