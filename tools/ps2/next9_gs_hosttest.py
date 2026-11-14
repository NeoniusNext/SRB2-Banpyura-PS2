#!/usr/bin/env python3
"""Prove GS state-flush byte staging against an independent packet/state oracle.

Native mode embeds the complete default-LTO helper and original RELAs from
the actual engine objects, with capture-only completion hooks.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess

from audio_storage_next7_finalbind import Object
from next8_gs_codecheck import function_bytes, symbols

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
H_BYTES = 36896
NAME = "state_flush.lto_priv.0"
TARGETS = {"H": 36896, "V": 20, "VU": 992, "vu_chunk_end": None, "pk_flush": None}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def section(text, first, after):
    if text.count(first) != 1:
        raise RuntimeError("Ambiguous source anchor: " + first)
    start = text.index(first)
    return text[start:text.index(after, start)]


def function(text, name):
    return section(text, "static void " + name + "(", "\n}\n") + "\n}\n"


def prepare(work, base, negative=0):
    work.mkdir(parents=True, exist_ok=True)
    current = ROOT / "src/ps2/hw"
    before = (base / "ps2/hw/ps2_hw_draw.inc").read_text()
    after = (current / "ps2_hw_draw.inc").read_text()
    if after.replace("\tu8 regs[10];", "\tu32 regs[10];", 1) != before:
        raise RuntimeError("Unexpected production change beyond the one temporary array")
    priv = (current / "ps2_hw_priv.inc").read_text()
    gs = (current / "ps2_hw_gs.inc").read_text()
    regs = (current / "ps2_hw_regs.inc").read_text()
    unchanged = {}
    for name in ("ps2_hwd.c", "ps2_hw_priv.inc", "ps2_hw_gs.inc", "ps2_hw_regs.inc", "ps2_hw_vif.inc", "ps2_hw_vu1.inc", "ps2_hw_tex.inc", "ps2_hw_model.inc"):
        if (base / "ps2/hw" / name).read_bytes() != (current / name).read_bytes():
            raise RuntimeError("Caller/layout/ring/texture/model changed: " + name)
        unchanged[name] = sha(current / name)
    common = """#ifndef NEXT9_GS_COMMON_H
#define NEXT9_GS_COMMON_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NEXT9_GS_EE
#include <tamtypes.h>
#else
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#endif
"""
    common += section(priv, "#define GSR_PRIM", "// pixel formats")
    common += section(priv, "typedef union\n{\n\tu64 d[2];", "static inline u32 cyc")
    common += section(priv, "#define HWGIF_TAG", "#define PS2HWD_MAXPOLY")
    common += section(priv, "#define NBUF", "#define NSCR")
    common += section(priv, "// state of the stream as the GS knows it", "static struct\n{\n\tint up;")
    common += section(before, "typedef struct\n{\n\tu64 test, alpha, clamp, tex0", "static void state_flush")
    common += """
/* Actual target offsets, checked against linked symbol and instruction RELAs. */
typedef struct {
    u8 prefix[96];
    qw_t *ring[NBUF];
    int cur;
    u32 wr, run;
    u8 middle[36584 - 96 - NBUF * sizeof(qw_t *) - 12];
    gsregs_t gsr;
    u32 serial;
    u8 before_stats[44];
    struct { u32 state_writes; } st;
    u8 suffix[172];
} __attribute__((aligned(16))) hwfixture_t;
typedef struct { int on, ready, dmac, dmac_set, chunk_open; } vfixture_t;
typedef struct { u8 prefix[12]; int open; u8 suffix[976]; } vufixture_t;
_Static_assert(offsetof(hwfixture_t, ring) == 96, "ring offset");
_Static_assert(offsetof(hwfixture_t, gsr) == 36584, "state offset");
_Static_assert(offsetof(hwfixture_t, serial) == 36672, "serial offset");
_Static_assert(offsetof(hwfixture_t, st.state_writes) == 36720, "counter offset");
_Static_assert(sizeof(hwfixture_t) == 36896, "whole state layout");
_Static_assert(sizeof(vfixture_t) == 20 && sizeof(vufixture_t) == 992, "VU layouts");
extern hwfixture_t H;
extern vfixture_t V;
extern vufixture_t VU;
void pk_flush(void);
void vu_chunk_end(void);
static void vu_chunk_flush(void) { if (VU.open) vu_chunk_end(); }
#define CONS_ERROR 0
#define CONS_Alert(...) ((void)0)
void base_state_flush(const gsstate_t *);
void cur_state_flush(const gsstate_t *);
#endif
"""
    (work / "common.h").write_text(common)
    shared = section(gs, "static qw_t *pk_alloc(u32 n)", "// OPT9 (PS2-HW-32)")
    shared += section(gs, "static qw_t *ad_alloc(u32 n)", "// Wait until the GS")
    shared = section(regs, "static inline void ad_set", "// FRAME_1") + shared
    for tag, text in (("base", before), ("cur", after)):
        body = function(text, "state_flush")
        if tag == "cur" and negative:
            old, new = {
                1: ("regs[n++] = GSR_FRAME_1;", "regs[n++] = GSR_ZBUF_1;"),
                2: ("p[i], vals[i], regs[i]", "p[i], vals[i] ^ 1u, regs[i]"),
                3: ("|| (s->tex0 & cld_mask)", "|| 0"),
                4: ("s->use_fog &&", "0 && s->use_fog &&"),
                5: ("H.gsr.valid |= RV_FRAME;", "H.gsr.valid |= RV_FRAME | RV_TEST;"),
                6: ("i < n;", "i + 1 < n;"),
            }[negative]
            if old not in body:
                raise RuntimeError("Missing executed mutation anchor")
            body = body.replace(old, new)
        body = body.replace("static void state_flush(", "void " + tag + "_state_flush(")
        (work / (tag + "-production.c")).write_text('#include "common.h"\n' + shared + body)
    # Every staged value is a literal GS register address and every slot is bounded.
    body = function(after, "state_flush")
    addresses = re.findall(r"regs\[n\+\+\] = (GSR_\w+);", body)
    values = {name: int(re.search(r"#define " + name + r" (0x[0-9a-fA-F]+)", priv)[1], 16) for name in addresses}
    if len(addresses) != 10 or max(values.values()) > 255 or min(values.values()) < 0:
        raise RuntimeError("Byte staging capacity/range proof failed")
    dump(work / "source-audit.json", {
        "baseline_source_sha256": hashlib.sha256(before.encode()).hexdigest(),
        "current_source_sha256": sha(current / "ps2_hw_draw.inc"),
        "one_temporary_type_change_restores_entire_frozen_draw_file": True,
        "identical_caller_layout_ring_and_other_kernel_sources": unchanged,
        "maximum_staged_registers": len(addresses), "staged_register_addresses": values,
        "local_storage_bytes": {"before": 40, "after": 10},
        "allocation_state_update_and_packet_order_unchanged": True,
    })


def run(command, log, expected=0):
    process = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True)
    output = process.stdout + process.stderr
    log.write_text(" ".join(map(str, command)) + "\n" + output)
    if process.returncode != expected or "runtime error:" in output:
        print(output)
        raise RuntimeError("Unexpected result: " + str(log))
    return output


def host(work, cc, sanitize=False, expected=0, short=False):
    binary = work / ("test-native-corpus" if short else "test-sanitize" if sanitize else "test")
    flags = ["-std=gnu17", "-O2", "-fwrapv", "-fno-lto", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"]
    if short:
        flags += ["-DNEXT9_GS_SHORT"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    run([cc, *flags, "-I" + str(work), work / "base-production.c", work / "cur-production.c", TOOLS / "next9_gs_hosttest.c", "-o", binary], work / (binary.name + "-build.log"))
    return run([binary], work / (binary.name + ".log"), expected)


def stream(obj):
    sections = [name for name in obj.sections if re.fullmatch(r"\.text\.state_flush(?:\.lto_priv\.\d+)?", name)]
    if len(sections) != 1:
        raise RuntimeError("Missing/ambiguous complete LTO state_flush")
    section_name = sections[0]
    name = section_name[len(".text."):]
    records = []
    for item in obj.relocations.get(section_name, []):
        target = item["target"]
        canonical = re.sub(r"\.lto_priv\.\d+$", "", target["name"])
        if canonical not in TARGETS or item["type"] not in (4, 5, 6) or item["explicit_addend"] is None:
            raise RuntimeError("Unexpected relocation: " + repr(item))
        if target["value"] != 0 or (TARGETS[canonical] is not None and target["size"] != TARGETS[canonical]):
            raise RuntimeError("Unexpected actual state/target layout")
        records.append({"offset": item["offset"], "type": item["type"], "target": canonical,
                        "actual_target": target["name"], "addend": item["explicit_addend"]})
    table = symbols(obj)
    return {"name": name, "bytes": obj.content(obj.sections[section_name]),
            "body_bytes": table[name]["bytes"], "relocations": records}


def emit_engine(work, path, tag):
    selected = stream(Object(path))
    symbol = tag + "_state_flush"
    lines = [".set noreorder", ".set noat", '.section .text.' + symbol + ',"ax",@progbits',
             ".balign 8", ".globl " + symbol, ".type " + symbol + ",@function", symbol + ":"]
    lines += [".word 0x%08x" % word for word in struct.unpack("<" + "I" * (len(selected["bytes"]) // 4), selected["bytes"])]
    for item in selected["relocations"]:
        lines += [".reloc " + symbol + "+" + str(item["offset"]) + ",R_MIPS_" + {4: "26", 5: "HI16", 6: "LO16"}[item["type"]] + "," + item["target"] + "+" + str(item["addend"])]
    lines += [".size " + symbol + "," + str(selected["body_bytes"])]
    source = work / (tag + "-engine.S")
    source.write_text("\n".join(lines) + "\n")
    metadata = {key: value for key, value in selected.items() if key != "bytes"}
    dump(work / (tag + "-engine-extraction.json"), {**metadata, "engine_path": str(path), "engine_sha256": sha(path), "section_bytes": len(selected["bytes"]), "code_sha256": hashlib.sha256(selected["bytes"]).hexdigest()})
    return source


def verify_extracted(path, embedded, tag):
    selected, obj = stream(Object(path)), Object(embedded)
    section_name = ".text." + tag + "_state_flush"
    after = obj.content(obj.sections[section_name])
    records = [{"offset": r["offset"], "type": r["type"], "target": r["target"]["name"], "addend": r["explicit_addend"]} for r in obj.relocations.get(section_name, [])]
    before = [{key: value for key, value in r.items() if key != "actual_target"} for r in selected["relocations"]]
    if after != selected["bytes"] or records != before:
        raise RuntimeError("Embedded complete code/RELA stream changed")
    return {"entire_section_and_ordered_relas_identical": True, "section_bytes": len(after), "body_bytes": selected["body_bytes"], "ordered_relocations": records}


def ee(work, base_obj, cur_obj):
    import build as B
    flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-flto")]
    flags += ["-fno-lto", "-Werror", "-Wno-unused-function", "-DNEXT9_GS_EE", "-I" + str(work)]
    objects, reports = [], {}
    for tag, path in (("base", base_obj), ("cur", cur_obj)):
        source = emit_engine(work, path, tag)
        target = source.with_suffix(".o")
        run([B.CC, *flags, *B.INCS, "-c", source, "-o", target], work / (tag + "-engine-build.log"))
        reports[tag] = verify_extracted(path, target, tag)
        objects.append(target)
    fixture = work / "fixture.o"
    run([B.CC, *flags, *B.INCS, "-c", TOOLS / "next9_gs_hosttest.c", "-o", fixture], work / "fixture-build.log")
    binary = work / "GS_NEXT9.ELF"
    run([B.CC, *B.LDFLAGS, *objects, fixture, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "fixture-link.log")
    dump(work / "ee-build-report.json", {"elf_sha256": sha(binary), "elf_bytes": binary.stat().st_size,
         "compiler": str(B.CC), "flags": flags, "complete_embedded_sections": reports,
         "fixture_sha256": sha(TOOLS / "next9_gs_hosttest.c"), "completion_hooks": "capture-only VU/ring hooks; no hardware waits included"})
    print("Built " + str(binary))


def patched(data, records, addresses, actual=False):
    result = bytearray(data)
    for item in records:
        word = struct.unpack_from("<I", data, item["offset"])[0]
        target = item["actual_target"] if actual else item["target"]
        absolute = addresses[target]["address"] + item["addend"]
        if item["type"] == 4:
            if word & 0x3ffffff:
                raise RuntimeError("Unexpected encoded R26 addend")
            value = (word & 0xfc000000) | ((absolute >> 2) & 0x3ffffff)
        else:
            if word & 65535:
                raise RuntimeError("Unexpected encoded HI/LO addend")
            immediate = ((absolute + 32768) >> 16) if item["type"] == 5 else absolute
            value = (word & 0xffff0000) | (immediate & 65535)
        struct.pack_into("<I", result, item["offset"], value)
    return bytes(result)


def bind(work, base_obj, cur_obj, base_elf, cur_elf, fixture, negatives=False):
    timed = Object(fixture)
    timed_symbols = symbols(timed)
    report = {}
    for tag, engine, binary in (("base", base_obj, base_elf), ("cur", cur_obj, cur_elf)):
        obj, elf = Object(engine), Object(binary)
        selected, linked_symbols = stream(obj), symbols(elf)
        original = patched(selected["bytes"], selected["relocations"], linked_symbols, actual=True)[:selected["body_bytes"]]
        expected = patched(selected["bytes"], selected["relocations"], timed_symbols)[:selected["body_bytes"]]
        if original != function_bytes(elf, linked_symbols, selected["name"]) or expected != function_bytes(timed, timed_symbols, tag + "_state_flush"):
            raise RuntimeError("Entire resolved production/fixture helper differs")
        if linked_symbols[next(r["actual_target"] for r in selected["relocations"] if r["target"] == "H")]["bytes"] != H_BYTES or timed_symbols["H"]["bytes"] != H_BYTES:
            raise RuntimeError("Actual/fixture H layout changed")
        words = struct.unpack("<" + "I" * (selected["body_bytes"] // 4), selected["bytes"][:selected["body_bytes"]])
        frames = sorted(set(65536 - (word & 65535) for word in words if word >> 16 == 0x27bd and word & 0x8000))
        report[tag] = {"engine_sha256": sha(engine), "elf_sha256": sha(binary), "body_bytes": selected["body_bytes"],
                       "section_bytes": len(selected["bytes"]), "stack_frame_bytes": frames,
                       "ordered_relocations": selected["relocations"], "all_complete_resolved_bytes_match_both_real_and_fixture_elf": True}
        if negatives and tag == "cur":
            changed = bytearray(selected["bytes"])
            changed[0] ^= 1
            moved = copy.deepcopy(selected["relocations"])
            moved[0]["offset"] += 4
            addend = copy.deepcopy(selected["relocations"])
            addend[0]["addend"] += 65536
            for code, relocs in ((changed, selected["relocations"]), (selected["bytes"], moved), (selected["bytes"], addend)):
                try:
                    mismatch = patched(code, relocs, timed_symbols)[:selected["body_bytes"]] != expected
                except RuntimeError:
                    mismatch = True
                if not mismatch:
                    raise RuntimeError("Binary binding mutation escaped")
            report["negative_controls"] = ["instruction bit", "moved relocation", "explicit addend"]
    dump(work / "linked-code-report.json", report)
    print("Entire real DEFAULT-LTO and fixture helper bytes/RELA bindings passed.")


def summarize(work, log):
    text = log.read_text()
    if "GS NEXT9 DONE" not in text or "GS NEXT9 FAIL" in text:
        raise RuntimeError("Incomplete/failed target run")
    rows = {}
    for match in re.finditer(r"GSNEXT9BENCH kind=(\d+) valid=(\d+) dirty=(\d+) uses=(\d+) cld=(\d+) calls=(\d+) rep=(\d+) base=(\d+) current=(\d+)", text):
        kind, valid, dirty, uses, cld, calls, rep, old, new = map(int, match.groups())
        if kind >= 64:
            raise RuntimeError("Unexpected timing kind")
        config = kind % 16
        masks = (0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1023, 341, 682)
        expected = (0 if config == 14 else 1023, masks[config] if config < 14 else 0,
                    kind // 16, int(config == 15), 2048)
        if (valid, dirty, uses, cld, calls) != expected:
            raise RuntimeError("Timing configuration differs from the required grid")
        item = rows.setdefault(kind, dict(kind=kind, valid=valid, dirty=dirty, uses=uses, cld=cld, calls=calls, base=[], current=[]))
        if rep != len(item["base"]):
            raise RuntimeError("Unexpected repetition order")
        item["base"].append(old)
        item["current"].append(new)
    if set(rows) != set(range(64)) or any(len(item["base"]) != 9 for item in rows.values()):
        raise RuntimeError("Incomplete target timings")
    proof = re.search(r"GS NEXT9 PASS cases=(\d+) register_writes=(\d+)", text)
    if not proof or tuple(map(int, proof.groups())) != (37888, 229516):
        raise RuntimeError("Native proof totals differ from the pinned deterministic corpus")
    for item in rows.values():
        item["baseline_median"] = statistics.median(item["base"])
        item["current_median"] = statistics.median(item["current"])
        item["change_percent"] = 100 * (item["current_median"] / item["baseline_median"] - 1)
    result = list(rows.values())
    dump(work / "benchmark-summary.json", result)
    old, new = (sum(row[key] for row in result) for key in ("baseline_median", "current_median"))
    report = {"rows": len(result), "faster": sum(r["current_median"] < r["baseline_median"] for r in result),
              "equal": sum(r["current_median"] == r["baseline_median"] for r in result),
              "slower": sum(r["current_median"] > r["baseline_median"] for r in result),
              "baseline_sum": old, "current_sum": new, "change_percent": 100 * (new / old - 1),
              "proof": proof.groups(),
              "scope": "Complete actual engine-LTO state_flush calls with capture-only completion hooks, warm emulated EE; not whole-game FPS or physical-console timing"}
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
    parser.add_argument("--baseline-elf", type=Path)
    parser.add_argument("--current-elf", type=Path)
    parser.add_argument("--fixture-elf", type=Path)
    parser.add_argument("--summarize", type=Path)
    args = parser.parse_args()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if args.summarize:
        summarize(work, args.summarize.resolve())
    elif args.fixture_elf:
        bind(work, args.baseline_engine, args.current_engine, args.baseline_elf, args.current_elf, args.fixture_elf, args.negative_controls)
    else:
        if not args.base_src:
            parser.error("--base-src is required")
        prepare(work, args.base_src.resolve())
        if args.ee_only:
            if not args.baseline_engine or not args.current_engine:
                parser.error("Native mode requires actual engine_lto.o objects")
            ee(work, args.baseline_engine.resolve(), args.current_engine.resolve())
        else:
            outputs = {"ordinary": host(work, args.cc), "native_corpus_on_host": host(work, args.cc, short=True)}
            print(outputs["ordinary"].strip())
            print(outputs["native_corpus_on_host"].strip())
            if args.sanitize:
                outputs["asan_ubsan"] = host(work, args.cc, True)
                print(outputs["asan_ubsan"].strip())
            controls = []
            if args.negative_controls:
                for negative in range(1, 7):
                    directory = work / ("negative-" + str(negative))
                    prepare(directory, args.base_src.resolve(), negative)
                    output = host(directory, args.cc, expected=1)
                    if "GS NEXT9 FAIL" not in output:
                        raise RuntimeError("Mutation failed without reporting its oracle mismatch")
                    controls.append({"negative": negative, "exit_code": 1, "detected": output.strip().splitlines()[-1]})
                    print("Mutation detected: " + controls[-1]["detected"])
            dump(work / "host-report.json", {"outputs": outputs, "negative_controls": controls,
                 "fixture_sha256": sha(TOOLS / "next9_gs_hosttest.c"), "script_sha256": sha(Path(__file__))})


if __name__ == "__main__":
    main()
