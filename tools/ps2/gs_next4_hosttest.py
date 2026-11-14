"""Check frozen/current GS footprint and planning code in separate translation units."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"


def section(source, first, after):
    if source.count(first) != 1:
        raise RuntimeError("Missing or ambiguous production anchor: " + first)
    start = source.index(first)
    return source[start:source.index(after, start)]


def prepare(work, base, negative=0):
    work.mkdir(parents=True, exist_ok=True)
    current = ROOT / "src/ps2/hw"
    priv = (current / "ps2_hw_priv.inc").read_text()
    defs = section(priv, "#define PSM_CT32", "// DMA channel 2")
    common = ["#ifndef GS_NEXT4_COMMON_H\n#define GS_NEXT4_COMMON_H",
              "#ifdef GS_NEXT4_EE\n#include <tamtypes.h>\n#else\ntypedef unsigned int u32; typedef unsigned char u8;\n#endif",
              "_Static_assert(sizeof(u32) == 4, \"u32 must be 32 bits\");", defs]
    hashes = {}
    for tag, directory in [("base", base / "ps2/hw"), ("cur", current)]:
        tex = (directory / "ps2_hw_tex.inc").read_text()
        olddefs = section((directory / "ps2_hw_priv.inc").read_text(), "#define PSM_CT32", "// DMA channel 2")
        if defs != olddefs:
            raise RuntimeError("Shared pixel-format/VRAM definitions differ")
        hashes[tag] = {name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
                       for name in ("ps2_hw_tex.inc", "ps2_hw_priv.inc")}
        body = "\n".join([section(tex, "static const u8 blk_32_8", "// ---- record table"),
                           section(tex, "static int tex_nosplit;", "// statistics: kind of upload")])
        if tag == "cur":
            mutations = {
                1: ("by = 3;", "by = 4;"),
                2: ("((tbw + 1) >> 1)", "(tbw >> 1)"),
                3: ("(h >> 5) * tbw", "(h >> 6) * tbw"),
                4: ("maxblk + 1;", "maxblk;"),
            }
            if negative:
                old, new = mutations[negative]
                if old not in body:
                    raise RuntimeError("Missing negative-control anchor: " + old)
                body = body.replace(old, new)
        declarations = [f"u32 {tag}_footprint(int psm, u32 w, u32 h, u32 tbw);",
                        f"int {tag}_plan(int psm, u32 w, u32 h, u32 cap, int mode, u32 rows, u32 *dx, u32 *dy);",
                        f"u32 {tag}_plan_blocks(int psm, u32 w, u32 h, u32 dx, u32 dy);"]
        common += declarations
        wrappers = f"""
__attribute__((noinline)) u32 {tag}_footprint(int psm, u32 w, u32 h, u32 tbw)
{{ return tex_footprint(psm, w, h, tbw); }}
__attribute__((noinline)) int {tag}_plan(int psm, u32 w, u32 h, u32 cap, int mode, u32 rows, u32 *dx, u32 *dy)
{{
    tex_nosplit = mode & 1;
    tex_split_rows = rows;
    return tex_plan(psm, w, h, cap, mode >> 1, dx, dy);
}}
__attribute__((noinline)) u32 {tag}_plan_blocks(int psm, u32 w, u32 h, u32 dx, u32 dy)
{{ return tex_plan_blocks(psm, w, h, dx, dy); }}
"""
        (work / (tag + "-production.c")).write_text('#include "common.h"\n' + body + wrappers)
    common.append("#endif\n")
    (work / "common.h").write_text("\n".join(common))
    (work / "source-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")


def run(command, log, expected=0):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True)
    output = result.stdout + result.stderr
    log.write_text(" ".join(map(str, command)) + "\n" + output)
    if result.returncode != expected or "runtime error:" in output:
        print(output)
        raise SystemExit("Unexpected result; see " + str(log))
    return output


def host(work, args, sanitize=False, expected=0):
    binary = work / ("test-sanitize" if sanitize else "test")
    flags = ["-std=gnu17", "-O2", "-fwrapv", "-fno-lto", "-Wall", "-Wextra", "-Werror"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    run([args.cc, *flags, "-I" + str(work), work / "base-production.c", work / "cur-production.c",
         TOOLS / "gs_next4_hosttest.c", "-o", binary], work / (binary.name + "-build.log"))
    return run([binary], work / (binary.name + ".log"), expected)


def summarize(log, output):
    rows = {}
    for line in log.read_text().splitlines():
        m = re.search(r"GSNEXT4BENCH kind=(\d+) psm=(\d+) calls=(\d+) rep=(\d+) base=(\d+) current=(\d+)", line)
        if m:
            kind, psm, calls, rep, base, cur = map(int, m.groups())
            row = rows.setdefault(kind, {"kind": kind, "psm": psm, "calls": calls, "base": [], "current": []})
            row["base"].append(base)
            row["current"].append(cur)
    result = []
    for row in rows.values():
        if len(row["base"]) != 9 or len(row["current"]) != 9:
            raise RuntimeError("Incomplete benchmark repetitions")
        a, b = statistics.median(row["base"]), statistics.median(row["current"])
        result.append({**row, "baseline_median": a, "current_median": b, "change_percent": (b / a - 1) * 100})
    if not result or "GS NEXT4 DONE" not in log.read_text():
        raise RuntimeError("Missing completed benchmark")
    output.write_text(json.dumps(result, indent=2) + "\n")
    for row in result:
        print(f"kind={row['kind']} psm={row['psm']:02x} {row['baseline_median']} -> {row['current_median']} ({row['change_percent']:.2f}%)")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--base-src", type=Path)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--ee-build", action="store_true")
    ap.add_argument("--ee-only", action="store_true")
    ap.add_argument("--summarize", type=Path)
    args = ap.parse_args()
    work = args.out.resolve()
    if args.summarize:
        summarize(args.summarize, work / "benchmark-summary.json")
        return
    if not args.base_src:
        ap.error("--base-src is required for source extraction")
    prepare(work, args.base_src.resolve())
    if not args.ee_only:
        print(host(work, args).strip())
        if args.sanitize:
            print(host(work, args, True).strip())
        if args.negative_controls:
            for negative in range(1, 5):
                subdir = work / ("negative-" + str(negative))
                prepare(subdir, args.base_src.resolve(), negative)
                output = host(subdir, args, expected=1)
                if "GS NEXT4 FAIL" not in output:
                    raise SystemExit("Negative control did not report a mismatch")
                print("negative=" + str(negative) + " detected: " + output.strip().splitlines()[-1])
    if args.ee_build or args.ee_only:
        sys.path.insert(0, str(TOOLS))
        import build as B
        # Each production version stays in its own non-LTO TU: no cross-version
        # identical-function folding, constant arguments or shared optimization.
        flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-flto")]
        flags += ["-fno-lto", "-Werror", "-DGS_NEXT4_EE", "-I" + str(work)]
        sources = [work / "base-production.c", work / "cur-production.c", TOOLS / "gs_next4_hosttest.c"]
        objects = []
        for source in sources:
            obj = work / (source.stem + ".o")
            objects.append(obj)
            run([B.CC, *flags, *B.INCS, "-c", source, "-o", obj], work / (source.stem + "-ee-build.log"))
        binary = work / "GS_NEXT4.ELF"
        run([B.CC, *B.LDFLAGS, *objects, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "ee-link.log")
        run([B.CC.parent / "mips64r5900el-ps2-elf-objdump", "-d", binary], work / "ee-disassembly.txt")
        run([B.CC.parent / "mips64r5900el-ps2-elf-nm", "-S", "--size-sort", binary], work / "ee-symbols.txt")
        (work / "ee-build-report.json").write_text(json.dumps({
            "elf_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "elf_bytes": binary.stat().st_size,
            "compiler": str(B.CC), "flags": flags, "separate_translation_units": True,
            "fixture_sha256": hashlib.sha256((TOOLS / "gs_next4_hosttest.c").read_bytes()).hexdigest(),
            "source_sha256": json.loads((work / "source-sha256.json").read_text()),
        }, indent=2) + "\n")
        print("Built " + str(binary) + "; run_pcsx2.py --until 'GS NEXT4 DONE'")


if __name__ == "__main__":
    main()
