"""Prove and time the actual preceding/current CT32 row fillers in separate TUs."""
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
NAMES = ("fill_ap88_ct32", "fill_ct32", "fill_ia88")
AFTER = ("// RGBA -> CT32", "// ALPHA_INTENSITY_88", "// why the last tex_upload")
SNAPSHOT = "// tex_upload owns this immutable context on its stack; output is in the DMA ring.\n\tconst fillctx_t c = *(const fillctx_t *)ctx;"


def section(t, first, after):
    if t.count(first) != 1:
        raise RuntimeError("Missing/ambiguous production anchor: " + first)
    start = t.index(first)
    return t[start:t.index(after, start)]


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def prepare(work, base, negative=0):
    work.mkdir(parents=True, exist_ok=True)
    paths = {"base": base / "ps2/hw/ps2_hw_tex.inc", "cur": ROOT / "src/ps2/hw/ps2_hw_tex.inc"}
    texts = {k: p.read_text() for k, p in paths.items()}
    restored = texts["cur"]
    for name, after in zip(NAMES, AFTER):
        first = "static void " + name + "("
        old = section(texts["base"], first, after)
        cur = section(texts["cur"], first, after)
        expected = old.replace("fillctx_t *c = ctx;", SNAPSHOT).replace("c->", "c.")
        expected = expected.replace("src_row(c,", "src_row(&c,").replace("src_col(c,", "src_col(&c,")
        if expected != cur:
            raise RuntimeError("Unexpected production change in " + name)
        restored = restored.replace(cur, old)
        if len(re.findall(r"\b" + name + r"\b", texts["cur"])) != 2:
            raise RuntimeError("Caller count changed: " + name)
    if restored != texts["base"]:
        raise RuntimeError("Production source changes exceed three immutable-context snapshots")
    context = section(texts["base"], "typedef struct\n{\n\tconst u8 *src;", "// 8-bit index data")
    alpha = section(texts["base"], "static inline u32 a8_to_gs(", "static inline u32 pal_hash(")
    common = ["#ifndef GS_NEXT6_COMMON_H\n#define GS_NEXT6_COMMON_H",
              "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>",
              "#ifdef GS_NEXT6_EE\n#include <tamtypes.h>\n#else\ntypedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;\n#endif",
              "typedef struct { u32 pal[256]; } hwfixture_t;\nextern hwfixture_t H;", context, alpha]
    for tag, t in texts.items():
        if context != section(t, "typedef struct\n{\n\tconst u8 *src;", "// 8-bit index data"):
            raise RuntimeError("Context or source-address helpers changed")
        if alpha != section(t, "static inline u32 a8_to_gs(", "static inline u32 pal_hash("):
            raise RuntimeError("Alpha helper changed")
        code = []
        for name, after in zip(NAMES, AFTER):
            body = section(t, "static void " + name + "(", after)
            if tag == "cur" and negative:
                old, new = {
                    1: (SNAPSHOT, "static fillctx_t c; static int once; if (!once) { c = *(const fillctx_t *)ctx; once = 1; }"),
                    2: ("a8_to_gs(", "(1u ^ a8_to_gs("),
                    3: ("src_col(&c, x)", "src_col(&c, x + 1)"),
                    4: ("x < c.W2", "x <= c.W2"),
                    5: ("src_row(&c, row)", "src_row(&c, 0)"),
                }[negative]
                if negative == 2:
                    body = body.replace("a8_to_gs(p[1])", "(1u ^ a8_to_gs(p[1]))")
                    body = body.replace("a8_to_gs(t >> 24)", "(1u ^ a8_to_gs(t >> 24))")
                    body = body.replace("a8_to_gs(t[1])", "(1u ^ a8_to_gs(t[1]))")
                else:
                    if old not in body:
                        raise RuntimeError("Missing negative anchor")
                    body = body.replace(old, new)
                    if negative == 5:
                        body = body.replace("{\n", "{\n\t(void)row;\n", 1)
            code.append(body)
            wrapper = tag + "_" + name
            code.append("__attribute__((noinline)) void " + wrapper + "(fillctx_t *c, u32 row, u8 *dst)\n{ " + name + "(c, row, dst); }\n")
            common.append("void " + wrapper + "(fillctx_t *, u32, u8 *);")
        (work / (tag + "-production.c")).write_text('#include "common.h"\n' + "\n".join(code))
    common.append("#endif\n")
    (work / "common.h").write_text("\n".join(common))
    hashes = {k: sha(p) for k, p in paths.items()}
    (work / "source-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    (work / "caller-audit.json").write_text(json.dumps({
        "source_sha256": hashes, "whole_file_restores_exact_frozen_source": True,
        "changed_functions": list(NAMES), "only_change": "immutable per-call fill context copied by value",
        "callers": ["Each fill function is selected once in tex_upload's corresponding format case",
                    "tex_upload owns fc on its stack, clears it and sets source/dimensions/decimation before upload_bands",
                    "upload_bands is the sole caller of these selected callbacks; its other caller uses fill_screen",
                    "upload_bands obtains destination through pk_alloc before invoking fill synchronously for each row",
                    "The split-image case updates source/H2 only between complete upload_bands calls"],
        "alias_and_lifetime": ["fc lives on tex_upload's stack; H.ring entries are separate memalign allocations",
                               "The destination is ring payload h[6] plus rowbytes*y; it cannot overwrite fc",
                               "fill calls no callback, allocation, scheduler, palette mutation or DMA submission",
                               "The ring's existing ownership/space checks occur before fill; no new retained pointer or lifetime",
                               "Source and palette contents are still read for each texel in the same original order",
                               "Snapshot is per call, never cached across rows/textures/palette changes"],
        "unchanged": ["source row/column and alpha formulas", "pixel order, formats, dimensions, planner, palette lookup",
                      "row writes, transfer sizes, packet bytes, DMA ordering, capacity, heap, VRAM and persistent state"],
        "domain": "all existing positive dimensions and tex_plan decimation shifts; original alignment requirements",
    }, indent=2) + "\n")


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
         TOOLS / "gs_next6_hosttest.c", "-o", binary], work / (binary.name + "-build.log"))
    return run([binary], work / (binary.name + ".log"), expected)


def summarize(log, output):
    rows = {}
    for line in log.read_text().splitlines():
        m = re.search(r"GSNEXT6BENCH kind=(\d+) format=(\d+) width=(\d+) src=(\d+) dst=(\d+) dx=(\d+) dy=(\d+) row=(\d+) calls=(\d+) rep=(\d+) base=(\d+) current=(\d+)", line)
        if m:
            kind, fmt, width, src, dst, dx, dy, row, calls, rep, old, cur = map(int, m.groups())
            item = rows.setdefault(kind, dict(kind=kind, format=fmt, width=width, src=src, dst=dst, dx=dx, dy=dy, row=row, calls=calls, base=[], current=[]))
            item["base"].append(old)
            item["current"].append(cur)
    result = []
    for row in rows.values():
        if len(row["base"]) != 9 or len(row["current"]) != 9:
            raise RuntimeError("Incomplete benchmark repetitions")
        a, b = statistics.median(row["base"]), statistics.median(row["current"])
        result.append({**row, "baseline_median": a, "current_median": b, "change_percent": (b / a - 1) * 100})
    if not result or "GS NEXT6 DONE" not in log.read_text():
        raise RuntimeError("Missing completed benchmark")
    output.write_text(json.dumps(result, indent=2) + "\n")
    for row in result:
        print(f"kind={row['kind']} format={row['format']} width={row['width']} dx={row['dx']} dy={row['dy']} {row['baseline_median']} -> {row['current_median']} ({row['change_percent']:.4f}%)")


def code_size(work):
    symbols = (work / "ee-symbols.txt").read_text()
    disassembly = (work / "ee-disassembly.txt").read_text()
    sections = (work / "ee-object-sections.txt").read_text()
    result = {}
    for tag in ("base", "cur"):
        result[tag] = {}
        for name in NAMES:
            symbol = tag + "_" + name
            match = re.search(r"^([0-9a-f]+) ([0-9a-f]+) T " + symbol + r"$", symbols, re.M)
            body = disassembly[disassembly.index("<" + symbol + ">:"):]
            body = body[:body.index("\n\n")]
            loop_loads = []
            insns = list(re.finditer(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\w+)\s+([^\n]+)", body, re.M))
            for insn in insns:
                target = re.search(r",([0-9a-f]+) <" + symbol + r"\+0x[0-9a-f]+>", insn[3])
                if insn[2].startswith("b") and target and int(target[1], 16) < int(insn[1], 16):
                    loop = "\n".join(i[0] for i in insns if int(target[1], 16) <= int(i[1], 16) <= int(insn[1], 16) + 4)
                    loop_loads.append(len(re.findall(r"\blw\s+\w+,(?:12|20)\(a0\)", loop)))
            if len(loop_loads) != 1 or loop_loads[0] != (2 if tag == "base" else 0):
                raise RuntimeError("Unexpected context-load code generation in " + symbol)
            result[tag][name] = {"symbol_bytes": int(match[2], 16),
                                 "stack_frames": sorted(set(int(v) for v in re.findall(r"addiu\s+sp,sp,-(\d+)", body))),
                                 "context_loads_per_pixel_loop": loop_loads[0]}
        result[tag]["object_sha256"] = sha(work / (tag + "-production.o"))
        start = sections.index(str(work / (tag + "-production.o")) + "  :")
        object_sections = sections[start:]
        object_sections = object_sections[:object_sections.index("Total")]
        result[tag]["object_text_section_bytes"] = sum(int(v) for v in re.findall(r"^\.text\.\S+\s+(\d+)", object_sections, re.M))
        result[tag]["object_data_bss_bytes"] = sum(int(v) for v in re.findall(r"^\.(?:data|bss|sdata|sbss)\s+(\d+)", object_sections, re.M))
    result.update(production_source_sha256=sha(ROOT / "src/ps2/hw/ps2_hw_tex.inc"),
                  data_bss_delta_bytes=0, new_heap_or_vram_bytes=0,
                  scope="Separate non-LTO wrappers; full-engine LTO measured separately")
    (work / "code-size-report.json").write_text(json.dumps(result, indent=2) + "\n")


def inspect_linked(work, baseline, current):
    sys.path.insert(0, str(TOOLS))
    import build as B
    work.mkdir(parents=True, exist_ok=True)
    result = {"production_source_sha256": sha(ROOT / "src/ps2/hw/ps2_hw_tex.inc"), "functions": {}}
    for tag, binary in (("base", baseline), ("cur", current)):
        symbols = run([B.CC.parent / "mips64r5900el-ps2-elf-nm", "-S", "--size-sort", binary], work / (tag + "-engine-symbols.txt"))
        result[tag + "_elf"] = {"path": str(binary), "sha256": sha(binary)}
        for name in NAMES:
            match = re.search(r"^([0-9a-f]+) ([0-9a-f]+) [Tt] " + name + r"$", symbols, re.M)
            if not match:
                raise RuntimeError("Missing/renamed actual linked helper: " + name)
            address, size = int(match[1], 16), int(match[2], 16)
            output = work / (tag + "-engine-" + name + "-disassembly.txt")
            body = run([B.CC.parent / "mips64r5900el-ps2-elf-objdump", "-d", "--start-address=" + str(address), "--stop-address=" + str(address + size), binary], output)
            loop_loads = []
            insns = list(re.finditer(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\w+)\s+([^\n]+)", body, re.M))
            for insn in insns:
                target = re.search(r",([0-9a-f]+) <" + name + r"\+0x[0-9a-f]+>", insn[3])
                if insn[2].startswith("b") and target and int(target[1], 16) < int(insn[1], 16):
                    loop = "\n".join(i[0] for i in insns if int(target[1], 16) <= int(i[1], 16) <= int(insn[1], 16) + 4)
                    loop_loads.append(len(re.findall(r"\blw\s+\w+,(?:12|20)\(a0\)", loop)))
            if len(loop_loads) != 1 or loop_loads[0] != (2 if tag == "base" else 0):
                raise RuntimeError("Unexpected actual linked context-load code generation in " + name)
            result["functions"].setdefault(name, {})[tag] = {
                "address": hex(address), "symbol_bytes": size,
                "stack_frames": sorted(set(int(v) for v in re.findall(r"addiu\s+sp,sp,-(\d+)", body))),
                "context_loads_per_pixel_loop": loop_loads[0],
                "disassembly_sha256": sha(output),
            }
    (work / "linked-engine-report.json").write_text(json.dumps(result, indent=2) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--base-src", type=Path)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--ee-only", action="store_true")
    ap.add_argument("--ee-build", action="store_true")
    ap.add_argument("--summarize", type=Path)
    ap.add_argument("--linked-base", type=Path)
    ap.add_argument("--linked-current", type=Path)
    args = ap.parse_args()
    work = args.out.resolve()
    if args.linked_base or args.linked_current:
        if not args.linked_base or not args.linked_current:
            ap.error("Both --linked-base and --linked-current are required")
        inspect_linked(work, args.linked_base.resolve(), args.linked_current.resolve())
        return
    if args.summarize:
        summarize(args.summarize, work / "benchmark-summary.json")
        return
    if not args.base_src:
        ap.error("--base-src is required")
    prepare(work, args.base_src.resolve())
    if not args.ee_only:
        print(host(work, args).strip())
        if args.sanitize:
            print(host(work, args, True).strip())
        if args.negative_controls:
            for negative in range(1, 6):
                subdir = work / ("negative-" + str(negative))
                prepare(subdir, args.base_src.resolve(), negative)
                result = host(subdir, args, expected=1)
                if "GS NEXT6 FAIL" not in result:
                    raise SystemExit("Negative control did not report mismatch")
                print("negative=" + str(negative) + " detected: " + result.strip().splitlines()[-1])
    if args.ee_only or args.ee_build:
        sys.path.insert(0, str(TOOLS))
        import build as B
        flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-flto")]
        flags += ["-fno-lto", "-Werror", "-DGS_NEXT6_EE", "-I" + str(work)]
        objects = []
        for source in [work / "base-production.c", work / "cur-production.c", TOOLS / "gs_next6_hosttest.c"]:
            obj = work / (source.stem + ".o")
            objects.append(obj)
            run([B.CC, *flags, *B.INCS, "-c", source, "-o", obj], work / (source.stem + "-ee-build.log"))
        binary = work / "GS_NEXT6.ELF"
        run([B.CC, *B.LDFLAGS, *objects, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "ee-link.log")
        for tool, opts, filename in [("objdump", ["-d"], "ee-disassembly.txt"), ("nm", ["-S", "--size-sort"], "ee-symbols.txt"), ("size", ["-A"], "ee-sections.txt")]:
            run([B.CC.parent / ("mips64r5900el-ps2-elf-" + tool), *opts, binary], work / filename)
        run([B.CC.parent / "mips64r5900el-ps2-elf-size", "-A", *objects[:2]], work / "ee-object-sections.txt")
        code_size(work)
        (work / "ee-build-report.json").write_text(json.dumps({"elf_sha256": sha(binary), "elf_bytes": binary.stat().st_size,
            "compiler": str(B.CC), "flags": flags, "separate_translation_units": True,
            "fixture_sha256": sha(TOOLS / "gs_next6_hosttest.c"), "source_sha256": json.loads((work / "source-sha256.json").read_text())}, indent=2) + "\n")
        print("Built " + str(binary) + "; run_pcsx2.py --until 'GS NEXT6 DONE'")


if __name__ == "__main__":
    main()
