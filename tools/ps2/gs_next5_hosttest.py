"""Compare the actual frozen/current AP_88 row fillers in separate translation units."""
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


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(work, base, negative=0):
    work.mkdir(parents=True, exist_ok=True)
    current = ROOT / "src/ps2/hw"
    tex = (current / "ps2_hw_tex.inc").read_text()
    original = (base / "ps2/hw/ps2_hw_tex.inc").read_text()
    # Bounded source audit: the entire production file must reduce to the frozen
    # source by removing the pure helper and restoring these two expressions.
    helper = section(tex, "// Gather the low byte of each of four 16-bit texels", "// AP_88 -> index")
    restored = tex.replace(helper, "")
    restored = restored.replace("u32 idx = ap88_gather(v);",
                                "u32 idx = (u32)(v & 0xFFu) | (u32)((v >> 8) & 0xFF00u) | (u32)((v >> 16) & 0xFF0000u) | (u32)((v >> 24) & 0xFF000000u);")
    restored = restored.replace("const u32 a = ap88_gather(av);",
                                "const u32 a = (u32)(av & 0xFFu) | (u32)((av >> 8) & 0xFF00u) | (u32)((av >> 16) & 0xFF0000u) | (u32)((av >> 24) & 0xFF000000u);")
    if restored != original:
        raise RuntimeError("Production file contains changes beyond the audited AP_88 gather replacement")
    if len(re.findall(r"\bfill_ap88\b", tex)) != 2 or len(re.findall(r"\bap88_gather\b", tex)) != 3:
        raise RuntimeError("AP_88 caller/helper count changed; audit must be revisited")
    context = section(tex, "typedef struct\n{\n\tconst u8 *src;", "// 8-bit index data")
    common = ["#ifndef GS_NEXT5_COMMON_H\n#define GS_NEXT5_COMMON_H",
              "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>",
              "#ifdef GS_NEXT5_EE\n#include <tamtypes.h>\n#else\ntypedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;\n#endif",
              "#define FX3_NOFILL 1u\n#define HWDBG_COMPOSE 16777216u",
              "extern u32 ps2hwd_fx2, ps2hwd_dbg_flags;\nint gs_next5_print(const char *, ...);\n#define CONS_Printf gs_next5_print",
              context]
    hashes = {}
    for tag, directory in [("base", base / "ps2/hw"), ("cur", current)]:
        source = directory / "ps2_hw_tex.inc"
        t = source.read_text()
        if context != section(t, "typedef struct\n{\n\tconst u8 *src;", "// 8-bit index data"):
            raise RuntimeError("Shared row context/source selection changed")
        first = "static inline u32 ap88_gather(" if "static inline u32 ap88_gather(" in t else "static void fill_ap88("
        body = section(t, first, "// AP_88 with partial alpha")
        if tag == "cur" and negative:
            old, new = {
                1: ("return (u32)packed;", "return (u32)packed ^ 1u;"),
                2: ("const u64 av = v >> 8;", "const u64 av = v >> 7;"),
                3: ("*s8++", "(*s8++ ^ 0x100u)"),
                4: ("const u32 x4 = c->W2 & ~3u;", "const u32 x4 = (c->W2 + 3u) & ~3u;"),
            }[negative]
            if old not in body:
                raise RuntimeError("Missing negative anchor: " + old)
            body = body.replace(old, new)
            if negative == 1:
                body = body.replace("return (u32)(v & 0xFFu)", "return (u32)(v & 0xFEu)")
        wrappers = f"\n__attribute__((noinline)) void {tag}_fill(fillctx_t *ctx, u32 row, u8 *dst)\n{{ fill_ap88(ctx, row, dst); }}\n"
        (work / (tag + "-production.c")).write_text('#include "common.h"\n' + body + wrappers)
        common.append(f"void {tag}_fill(fillctx_t *, u32, u8 *);")
        hashes[tag] = {"ps2_hw_tex.inc": sha(source)}
    common.append("#endif\n")
    (work / "common.h").write_text("\n".join(common))
    (work / "source-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    (work / "caller-audit.json").write_text(json.dumps({
        "source_sha256": hashes, "full_file_restores_exact_baseline": True,
        "helper_sha256": hashlib.sha256(helper.encode()).hexdigest(),
        "formula": "output byte j = (v >> (16*j)) & 255, j=0..3",
        "instruction": "PPACB packs the low byte of each input halfword; only low 32 output bits are consumed",
        "domain": "all u64 bit patterns; no dependence on the unused high 64 bits of the EE input register",
        "callers": ["fill_ap88 index bytes: v is the existing bounded 64-bit load",
                    "fill_ap88 alpha bytes: av is v >> 8, exactly as in the frozen implementation"],
        "bounds": ["Existing fast branch requires dx=0 and 8-byte source alignment",
                   "Existing x4=W2&~3u bounds the four-texel load; all remaining texels use the unchanged scalar tail",
                   "Existing FX3_NOFILL/debug predicates and aligned/unaligned stores remain byte-identical",
                   "The only fill_ap88 selection is tex_upload's GL_TEXFMT_AP_88 case; upload_bands calls the selected row callback",
                   "tex_upload, upload_bands, source row/column functions and all packet/format/planner code are unchanged"],
        "memory": "No new persistent data, BSS, heap, scratch, VRAM, input reads or output writes",
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
         TOOLS / "gs_next5_hosttest.c", "-o", binary], work / (binary.name + "-build.log"))
    return run([binary], work / (binary.name + ".log"), expected)


def summarize(log, output):
    rows = {}
    for line in log.read_text().splitlines():
        m = re.search(r"GSNEXT5BENCH kind=(\d+) width=(\d+) mode=(\d+) dst=(\d+) src=(\d+) dx=(\d+) fx=(\d+) calls=(\d+) rep=(\d+) base=(\d+) current=(\d+)", line)
        if m:
            kind, width, mode, dst, src, dx, fx, calls, rep, old, cur = map(int, m.groups())
            row = rows.setdefault(kind, dict(kind=kind, width=width, mode=mode, dst=dst, src=src, dx=dx, fx=fx,
                                            calls=calls, base=[], current=[]))
            row["base"].append(old)
            row["current"].append(cur)
    result = []
    for row in rows.values():
        if len(row["base"]) != 9 or len(row["current"]) != 9:
            raise RuntimeError("Incomplete benchmark repetitions")
        a, b = statistics.median(row["base"]), statistics.median(row["current"])
        result.append({**row, "baseline_median": a, "current_median": b, "change_percent": (b / a - 1) * 100})
    if not result or "GS NEXT5 DONE" not in log.read_text():
        raise RuntimeError("Missing completed benchmark")
    output.write_text(json.dumps(result, indent=2) + "\n")
    for row in result:
        print(f"kind={row['kind']} width={row['width']} mode={row['mode']} dst={row['dst']} "
              f"{row['baseline_median']} -> {row['current_median']} ({row['change_percent']:.3f}%)")


def code_size(work):
    symbols = (work / "ee-symbols.txt").read_text()
    disassembly = (work / "ee-disassembly.txt").read_text()
    result = {}
    for tag in ("base", "cur"):
        name = tag + "_fill"
        match = re.search(r"^([0-9a-f]+) ([0-9a-f]+) T " + name + r"$", symbols, re.M)
        body = disassembly[disassembly.index("<" + name + ">:"):]
        body = body[:body.index("\n\n")]
        result[tag] = {
            "symbol_bytes": int(match[2], 16),
            "conditional_stack_frames": sorted(set(int(v) for v in re.findall(r"addiu\s+sp,sp,-(\d+)", body))),
            "ppacb_instructions": len(re.findall(r"\bppacb\b", body)),
            "object_sha256": sha(work / (tag + "-production.o")),
            "unchanged_debug_bss_bytes": 8,
        }
    result.update(production_source_sha256=sha(ROOT / "src/ps2/hw/ps2_hw_tex.inc"),
                  static_data_delta_bytes=0, new_heap_or_vram_bytes=0, section_padding_bytes_each=4,
                  scope="Separate non-LTO production TU wrappers; full-engine LTO sizes are integration results.")
    (work / "code-size-report.json").write_text(json.dumps(result, indent=2) + "\n")


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
                result = host(subdir, args, expected=1)
                if "GS NEXT5 FAIL" not in result:
                    raise SystemExit("Negative control did not report a mismatch")
                print("negative=" + str(negative) + " detected: " + result.strip().splitlines()[-1])
    if args.ee_only or args.ee_build:
        sys.path.insert(0, str(TOOLS))
        import build as B
        flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-flto")]
        flags += ["-fno-lto", "-Werror", "-DGS_NEXT5_EE", "-I" + str(work)]
        objects = []
        for source in [work / "base-production.c", work / "cur-production.c", TOOLS / "gs_next5_hosttest.c"]:
            obj = work / (source.stem + ".o")
            objects.append(obj)
            run([B.CC, *flags, *B.INCS, "-c", source, "-o", obj], work / (source.stem + "-ee-build.log"))
        binary = work / "GS_NEXT5.ELF"
        run([B.CC, *B.LDFLAGS, *objects, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "ee-link.log")
        for tool, opts, filename in [("objdump", ["-d"], "ee-disassembly.txt"),
                                     ("nm", ["-S", "--size-sort"], "ee-symbols.txt"),
                                     ("size", ["-A"], "ee-sections.txt")]:
            run([B.CC.parent / ("mips64r5900el-ps2-elf-" + tool), *opts, binary], work / filename)
        run([B.CC.parent / "mips64r5900el-ps2-elf-size", "-A", *objects[:2]], work / "ee-object-sections.txt")
        code_size(work)
        (work / "ee-build-report.json").write_text(json.dumps({
            "elf_sha256": sha(binary), "elf_bytes": binary.stat().st_size, "compiler": str(B.CC),
            "flags": flags, "separate_translation_units": True,
            "fixture_sha256": sha(TOOLS / "gs_next5_hosttest.c"),
            "source_sha256": json.loads((work / "source-sha256.json").read_text()),
        }, indent=2) + "\n")
        print("Built " + str(binary) + "; run_pcsx2.py --until 'GS NEXT5 DONE'")


if __name__ == "__main__":
    main()
