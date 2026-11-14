#!/usr/bin/env python3
"""Prove the two CT32 source-column inductions, then time real engine code.

Host code comes from the frozen/current complete production source. Native
timing instead embeds each complete default-LTO engine function section and
its original relocations; no replacement C compilation supplies timed code.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
sys.path.insert(0, str(TOOLS))
from audio_storage_next7_finalbind import Object

NAMES = ("fill_ap88_ct32", "fill_ct32")
INDUCTION = """\t// Advance the unsigned source column once per texel; the centre sample is unchanged.
\tconst u32 step = 1u << c.dx;
\tu32 sx = step >> 1;
\tfor (x = 0; x < c.W2; x++, sx += step)"""
PAL_OFFSET = 35264
H_BYTES = 36896


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def section(text, first, after):
    if text.count(first) != 1:
        raise RuntimeError("Missing/ambiguous source anchor: " + first)
    start = text.index(first)
    return text[start:text.index(after, start)]


def function(text, name):
    start = text.index("static void " + name + "(")
    return text[start:text.index("\n}\n", start) + 3]


def prepare(work, base, negative=0):
    work.mkdir(parents=True, exist_ok=True)
    paths = {"base": base / "ps2/hw/ps2_hw_tex.inc", "cur": ROOT / "src/ps2/hw/ps2_hw_tex.inc"}
    texts = {tag: path.read_text() for tag, path in paths.items()}
    restored = texts["cur"]
    for name in NAMES:
        old, new = function(texts["base"], name), function(texts["cur"], name)
        expected = old.replace("\tfor (x = 0; x < c.W2; x++)", INDUCTION).replace("src_col(&c, x)", "sx")
        if expected != new:
            raise RuntimeError("Unexpected production change: " + name)
        restored = restored.replace(new, old)
        counts = [len(re.findall(r"\b" + name + r"\b", text)) for text in texts.values()]
        if counts[0] != counts[1]:
            raise RuntimeError("Changed caller count: " + name)
    if restored != texts["base"]:
        raise RuntimeError("Texture source changes exceed the two exact induction rewrites")
    ctx = section(texts["base"], "typedef struct\n{\n\tconst u8 *src;", "// 8-bit index data")
    alpha = section(texts["base"], "static inline u32 a8_to_gs(", "static inline u32 pal_hash(")
    for text in texts.values():
        if ctx != section(text, "typedef struct\n{\n\tconst u8 *src;", "// 8-bit index data") or alpha != section(text, "static inline u32 a8_to_gs(", "static inline u32 pal_hash("):
            raise RuntimeError("Context/address/alpha helpers changed")
    header = """#ifndef NEXT8_GS_COMMON_H
#define NEXT8_GS_COMMON_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NEXT8_GS_EE
#include <tamtypes.h>
#else
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#endif
typedef struct {
#ifdef NEXT8_GS_LINKED
    u8 prefix[35264];
#endif
    u32 pal[256];
#ifdef NEXT8_GS_LINKED
    u8 suffix[608];
#endif
} hwfixture_t;
extern hwfixture_t H;
""" + ctx + alpha
    for tag, text in texts.items():
        bodies = []
        for name in NAMES:
            body = function(text, name)
            if tag == "cur" and negative:
                before, after = {
                    1: ("u32 sx = step >> 1;", "static u32 sx; static int once; if (!once) { sx = step >> 1; once = 1; }"),
                    2: ("1u << c.dx;", "2u << c.dx;"),
                    3: ("step >> 1;", "step >> 2;"),
                    4: ("<< 24)", "<< 23)"),
                    5: ("src_row(&c, row)", "src_row(&c, 0)"),
                    6: ("x < c.W2;", "x <= c.W2;"),
                }[negative]
                if before not in body:
                    raise RuntimeError("Missing mutation anchor")
                body = body.replace(before, after)
                if negative == 5:
                    body = body.replace("{\n", "{\n\t(void)row;\n", 1)
            body = body.replace("static void " + name + "(", "void " + tag + "_" + name + "(")
            bodies.append(body)
            header += "void " + tag + "_" + name + "(void *, u32, u8 *);\n"
        (work / (tag + "-production.c")).write_text('#include "common.h"\n' + "\n".join(bodies))
    header += "#endif\n"
    (work / "common.h").write_text(header)
    caller_hashes = {}
    for name in ("ps2_hwd.c", "ps2_hw_gs.inc", "ps2_hw_priv.inc"):
        before, after = base / "ps2/hw" / name, ROOT / "src/ps2/hw" / name
        if before.read_bytes() != after.read_bytes():
            raise RuntimeError("Caller, ring, or layout changed: " + name)
        caller_hashes[name] = sha(after)
    dump(work / "caller-audit.json", {
        "source_sha256": {tag: sha(path) for tag, path in paths.items()},
        "whole_texture_file_restores_frozen_source": True,
        "changed_functions": list(NAMES), "identical_caller_source_sha256": caller_hashes,
        "change": "Unsigned source-column induction; same row, centre sample, pixel read, alpha, palette and store",
        "production_domain": "Positive source axes <=16384; planned upload axes <=1024; dx/dy <=10; no added format/dimension/capacity limits",
        "integer_proof": "sx_0=2^dx>>1, sx_(x+1)=sx_x+2^dx, so sx_x=(x<<dx)+(2^dx>>1) modulo2^32; x/dx valid for the original expressions",
        "alias_and_lifetime": ["Original per-call fillctx_t snapshot retained", "Only an unsigned integer induction variable advances in C; no C pointer is formed beyond the last accessed source texel", "tex_upload owns its context on the stack; DMA output comes from separately allocated memalign rings", "The same selected source texels and palette entries are read in order, then the same destination words are stored", "No callbacks, allocations, DMA submissions or palette mutations occur inside these two fillers"],
        "unchanged": ["all other texture functions", "PSMT8 kernels and PPACB", "formats, keys, partial alpha and palette interpretation", "source/destination alignment requirements", "upload and draw packets, DMA ordering, allocator, scratch, heap and VRAM"],
    })


def run(command, log, expected=0):
    proc = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True)
    output = proc.stdout + proc.stderr
    log.write_text(" ".join(map(str, command)) + "\n" + output)
    if proc.returncode != expected or "runtime error:" in output:
        print(output)
        raise SystemExit("Unexpected result: " + str(log))
    return output


def host(work, cc, sanitize=False, expected=0):
    binary = work / ("test-sanitize" if sanitize else "test")
    flags = ["-std=gnu17", "-O2", "-fwrapv", "-fno-lto", "-Wall", "-Wextra", "-Werror"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    run([cc, *flags, "-I" + str(work), work / "base-production.c", work / "cur-production.c", TOOLS / "next8_gs_hosttest.c", "-o", binary], work / (binary.name + "-build.log"))
    return run([binary], work / (binary.name + ".log"), expected)


def relocations(obj, name):
    records = []
    for record in obj.relocations.get(".text." + name, []):
        target = record["target"]
        if record["type"] not in (5, 6) or not re.fullmatch(r"H(?:\.lto_priv\.\d+)?", target["name"]):
            raise RuntimeError("Unexpected engine helper relocation: " + repr(record))
        if record["explicit_addend"] is None:
            raise RuntimeError("Expected explicit RELA addend")
        if target["value"] or target["size"] != H_BYTES:
            raise RuntimeError("Unexpected full-engine H symbol layout")
        records.append({"offset": record["offset"], "type": record["type"], "target": "H", "addend": record["explicit_addend"]})
    if len(records) != (2 if name == "fill_ap88_ct32" else 0):
        raise RuntimeError("Unexpected helper relocation count")
    return records


def stream(obj, name):
    return {"bytes": obj.content(obj.sections[".text." + name]), "relocations": relocations(obj, name)}


def symbol_sizes(obj):
    sizes = {}
    for table in obj.headers:
        if table[1] != 2:
            continue
        strings = obj.content(obj.headers[table[6]])
        for offset in range(table[4], table[4] + table[5], table[9]):
            item = struct.unpack_from("<IIIBBH", obj.data, offset)
            sizes[obj.string(strings, item[0])] = item[2]
    return sizes


def emit_engine(work, path, tag):
    obj = Object(path)
    sizes = symbol_sizes(obj)
    lines = [".set noreorder", ".set noat"]
    report = {"engine_lto_object": str(path), "sha256": sha(path), "functions": {}}
    for name in NAMES:
        data = stream(obj, name)
        symbol = tag + "_" + name
        lines += ['.section .text.' + symbol + ',"ax",@progbits', ".balign 8", ".globl " + symbol, ".type " + symbol + ",@function", symbol + ":"]
        words = struct.unpack("<" + "I" * (len(data["bytes"]) // 4), data["bytes"])
        lines += [".word 0x%08x" % word for word in words]
        for reloc in data["relocations"]:
            lines += [".reloc " + symbol + "+" + str(reloc["offset"]) + ",R_MIPS_" + ("HI16" if reloc["type"] == 5 else "LO16") + ",H+" + str(reloc["addend"])]
        lines += [".size " + symbol + "," + str(sizes[name])]
        report["functions"][name] = {"section_bytes": len(data["bytes"]), "symbol_bytes": sizes[name], "instruction_sha256": hashlib.sha256(data["bytes"]).hexdigest(), "ordered_relocations": data["relocations"]}
    source = work / (tag + "-engine.S")
    source.write_text("\n".join(lines) + "\n")
    dump(work / (tag + "-engine-extraction.json"), report)
    return source


def verify_extracted(work, engine_path, embedded_path, tag):
    engine, embedded = Object(engine_path), Object(embedded_path)
    result = {}
    for name in NAMES:
        actual_name = tag + "_" + name
        before = stream(engine, name)
        section_name = ".text." + actual_name
        after = embedded.content(embedded.sections[section_name])
        relocs = [{"offset": r["offset"], "type": r["type"], "target": r["target"]["name"], "addend": r["explicit_addend"]} for r in embedded.relocations.get(section_name, [])]
        if after != before["bytes"] or relocs != before["relocations"]:
            raise RuntimeError("Embedded complete code/relocations differ: " + actual_name)
        result[name] = {"complete_text_and_relocations_identical": True, "bytes": len(after), "relocations": relocs}
    dump(work / (tag + "-embedded-code-check.json"), result)


def ee(work, base_obj, cur_obj):
    import build as B
    flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-flto")]
    flags += ["-fno-lto", "-Werror", "-DNEXT8_GS_EE", "-DNEXT8_GS_LINKED", "-I" + str(work)]
    objects = []
    for tag, engine in (("base", base_obj), ("cur", cur_obj)):
        source = emit_engine(work, engine, tag)
        target = source.with_suffix(".o")
        run([B.CC, *flags, *B.INCS, "-c", source, "-o", target], work / (tag + "-engine-build.log"))
        verify_extracted(work, engine, target, tag)
        objects.append(target)
    target = work / "next8_gs_hosttest.o"
    run([B.CC, *flags, *B.INCS, "-c", TOOLS / "next8_gs_hosttest.c", "-o", target], work / "fixture-ee-build.log")
    objects.append(target)
    binary = work / "GS_NEXT8.ELF"
    run([B.CC, *B.LDFLAGS, *objects, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "fixture-ee-link.log")
    for tool, opts, filename in (("objdump", ["-d"], "ee-disassembly.txt"), ("nm", ["-S", "--size-sort"], "ee-symbols.txt"), ("size", ["-A"], "ee-sections.txt")):
        run([B.CC.parent / ("mips64r5900el-ps2-elf-" + tool), *opts, binary], work / filename)
    dump(work / "ee-build-report.json", {"elf_sha256": sha(binary), "elf_bytes": binary.stat().st_size, "compiler": str(B.CC), "flags": flags, "source_fixture_sha256": sha(TOOLS / "next8_gs_hosttest.c"), "timed_code": "Complete exact text and RELA streams from actual full default-LTO Hardware engine objects", "baseline_object_sha256": sha(base_obj), "current_object_sha256": sha(cur_obj), "H_layout": {"pal_offset": PAL_OFFSET, "bytes": H_BYTES, "prefix_suffix_canaries": True}})
    print("Built " + str(binary))


def summarize(work, log):
    rows = {}
    pattern = r"GSNEXT8BENCH kind=(\d+) format=(\d+) width=(\d+) src=(\d+) dst=(\d+) dx=(\d+) dy=(\d+) row=(\d+) calls=(\d+) rep=(\d+) base=(\d+) current=(\d+)"
    text = log.read_text()
    for match in re.finditer(pattern, text):
        kind, fmt, width, src, dst, dx, dy, row, calls, rep, before, after = map(int, match.groups())
        item = rows.setdefault(kind, dict(kind=kind, format=fmt, width=width, src=src, dst=dst, dx=dx, dy=dy, row=row, calls=calls, base=[], current=[]))
        if rep != len(item["base"]):
            raise RuntimeError("Unexpected repetition order")
        item["base"].append(before)
        item["current"].append(after)
    if len(rows) != 84 or "GS NEXT8 DONE" not in text or "GS NEXT8 FAIL" in text:
        raise RuntimeError("Incomplete or failed native proof/timing")
    result = []
    for item in rows.values():
        if len(item["base"]) != 9:
            raise RuntimeError("Incomplete repetitions")
        before, after = statistics.median(item["base"]), statistics.median(item["current"])
        result.append({**item, "baseline_median": before, "current_median": after, "change_percent": (after / before - 1) * 100})
    dump(work / "benchmark-summary.json", result)
    total_a = sum(r["baseline_median"] for r in result)
    total_b = sum(r["current_median"] for r in result)
    report = {"rows": len(result), "faster": sum(r["current_median"] < r["baseline_median"] for r in result), "equal": sum(r["current_median"] == r["baseline_median"] for r in result), "slower": sum(r["current_median"] > r["baseline_median"] for r in result), "baseline_sum": total_a, "current_sum": total_b, "change_percent": (total_b / total_a - 1) * 100, "proof": re.search(r"GS NEXT8 PASS cases=(\d+) pixels=(\d+)", text).groups(), "columns": int(re.search(r"GS NEXT8 COLUMNS count=(\d+)", text)[1]), "scope": "Exact complete engine-LTO helper calls in a bounded fixture; not whole uploads, game-weighted FPS or physical-console timing"}
    dump(work / "component-report.json", report)
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--base-src", type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--ee-only", action="store_true")
    parser.add_argument("--baseline-engine", type=Path)
    parser.add_argument("--current-engine", type=Path)
    parser.add_argument("--summarize", type=Path)
    args = parser.parse_args()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if args.summarize:
        summarize(work, args.summarize.resolve())
        return
    if not args.base_src:
        parser.error("--base-src is required")
    prepare(work, args.base_src.resolve())
    if not args.ee_only:
        outputs = {"ordinary": host(work, args.cc)}
        print(outputs["ordinary"].strip())
        if args.sanitize:
            outputs["asan_ubsan"] = host(work, args.cc, True)
            print(outputs["asan_ubsan"].strip())
        controls = []
        if args.negative_controls:
            for negative in range(1, 7):
                subdir = work / ("negative-" + str(negative))
                prepare(subdir, args.base_src.resolve(), negative)
                result = host(subdir, args.cc, expected=1)
                if "GS NEXT8 FAIL" not in result:
                    raise RuntimeError("Mutation did not report mismatch")
                line = result.strip().splitlines()[-1]
                controls.append({"negative": negative, "exit_code": 1, "detected": line})
                print("negative=" + str(negative) + " detected: " + line)
        dump(work / "host-report.json", {"runs": {name: {"cases": int(re.search(r"GS NEXT8 PASS cases=(\d+)", output)[1]), "pixels": int(re.search(r"pixels=(\d+)", output)[1]), "column_arithmetic_cases": int(re.search(r"GS NEXT8 COLUMNS count=(\d+)", output)[1])} for name, output in outputs.items()}, "negative_controls": controls, "fixture_sha256": sha(TOOLS / "next8_gs_hosttest.c"), "script_sha256": sha(Path(__file__)), "production_source_sha256": sha(ROOT / "src/ps2/hw/ps2_hw_tex.inc")})
    if args.ee_only:
        if not args.baseline_engine or not args.current_engine:
            parser.error("Native timing requires both actual engine_lto.o objects")
        ee(work, args.baseline_engine.resolve(), args.current_engine.resolve())


if __name__ == "__main__":
    main()
