#!/usr/bin/env python3
"""Prove interleaved VRAM ranges; time complete default-LTO high/free helpers."""
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
from next8_gs_codecheck import canonical_relocations, function_bytes, symbols
from next9_gs_hosttest import patched

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
H_BYTES = 36896
NAMES = ("vram_alloc_hi", "vram_free")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def section(text, first, after):
    if text.count(first) != 1:
        raise RuntimeError("Missing/ambiguous source anchor: " + first)
    start = text.index(first)
    return text[start:text.index(after, start)]


def prepare(work, base, negative=0, current_src=None):
    work.mkdir(parents=True, exist_ok=True)
    current = (current_src or ROOT / "src") / "ps2/hw"
    before = (base / "ps2/hw/ps2_hw_tex.inc").read_text()
    after = (current / "ps2_hw_tex.inc").read_text()
    # The reverse operation preserves exact indentation in each of the six loops.
    reverse = re.sub(r"^(\t+)H\.free_range\[j\] = H\.free_range\[j ([+-]) 1\];$",
                     lambda m: m[1] + "H.free_start[j] = H.free_start[j " + m[2] + " 1];\n" +
                     m[1] + "H.free_len[j] = H.free_len[j " + m[2] + " 1];", after, flags=re.M)
    reverse = re.sub(r"H\.free_range\[([^\]]+)\]\.start", r"H.free_start[\1]", reverse)
    reverse = re.sub(r"H\.free_range\[([^\]]+)\]\.len", r"H.free_len[\1]", reverse)
    if reverse != before:
        raise RuntimeError("Range-layout reversal does not restore the complete frozen texture source")
    priv = (current / "ps2_hw_priv.inc").read_text()
    old_priv = (base / "ps2/hw/ps2_hw_priv.inc").read_text()
    declaration = ("\t// Keep each range word-aligned so list shifts copy its unchanged start/length together.\n"
                   "\tstruct { u16 start, len; } __attribute__((aligned(4))) free_range[VRAM_RANGES];")
    if priv.replace(declaration, "\tu16 free_start[VRAM_RANGES], free_len[VRAM_RANGES];") != old_priv:
        raise RuntimeError("Unexpected private layout edit")
    gs = (current / "ps2_hw_gs.inc").read_text()
    reverse_gs = gs.replace("H.free_range[0].start", "H.free_start[0]").replace("H.free_range[0].len", "H.free_len[0]")
    if reverse_gs != (base / "ps2/hw/ps2_hw_gs.inc").read_text():
        raise RuntimeError("Unexpected GS setup edit")
    untouched = {}
    for path in sorted(current.iterdir()):
        old = base / "ps2/hw" / path.name
        if path.is_file() and path.name not in ("ps2_hw_tex.inc", "ps2_hw_priv.inc", "ps2_hw_gs.inc"):
            if old.read_bytes() != path.read_bytes():
                raise RuntimeError("Unexpected other GS source edit: " + path.name)
            untouched[path.name] = sha(path)
    common = """#ifndef NEXT10_GS_COMMON_H
#define NEXT10_GS_COMMON_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NEXT10_GS_EE
#include <tamtypes.h>
#else
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#endif
#define VRAM_BLOCKS 16384u
#define VRAM_RANGES 8193
#define H_BYTES 36896
typedef struct {
    u8 prefix[2288];
    u16 free_start[VRAM_RANGES], free_len[VRAM_RANGES];
    int free_n; u32 used_blocks; u8 suffix[1828];
} __attribute__((aligned(16))) base_state_t;
typedef struct {
    u8 prefix[2288];
    struct { u16 start, len; } __attribute__((aligned(4))) free_range[VRAM_RANGES];
    int free_n; u32 used_blocks; u8 suffix[1828];
} __attribute__((aligned(16))) cur_state_t;
typedef union { base_state_t base; cur_state_t cur; u8 bytes[H_BYTES]; } state_t;
_Static_assert(sizeof(base_state_t) == H_BYTES && sizeof(cur_state_t) == H_BYTES, "whole state size");
_Static_assert(offsetof(base_state_t, free_start) == 2288 && offsetof(cur_state_t, free_range) == 2288, "range offset");
_Static_assert(offsetof(base_state_t, free_n) == 35060 && offsetof(cur_state_t, free_n) == 35060, "count offset");
_Static_assert(offsetof(base_state_t, used_blocks) == 35064 && offsetof(cur_state_t, used_blocks) == 35064, "counter offset");
extern state_t base_H, cur_H, H;
void capture_abort(void) __attribute__((noreturn));
void capture_output(const char *, ...);
#define CONS_ERROR 0
#define CONS_Alert(level, ...) capture_output(__VA_ARGS__)
#define abort capture_abort
u32 base_src_vram_alloc(u32,u32); u32 cur_src_vram_alloc(u32,u32);
u32 base_src_vram_alloc_hi(u32,u32); u32 cur_src_vram_alloc_hi(u32,u32);
void base_src_vram_free(u32,u32); void cur_src_vram_free(u32,u32);
u32 base_engine_vram_alloc_hi(u32,u32); u32 cur_engine_vram_alloc_hi(u32,u32);
void base_engine_vram_free(u32,u32); void cur_engine_vram_free(u32,u32);
#endif
"""
    (work / "common.h").write_text(common)
    for tag, source in (("base", before), ("cur", after)):
        body = section(source, "static u32 vram_alloc(u32 n", "// ---- footprint")
        if tag == "cur" and negative:
            changes = {
                1: ("H.free_range[j] = H.free_range[j - 1];", "H.free_range[j] = H.free_range[j];"),
                2: ("H.free_range[i + 1].len = tail;", "H.free_range[i + 1].len = tail ^ 1u;"),
                3: ("H.used_blocks += n;", "H.used_blocks += n + 1u;"),
                4: ("H.free_range[i - 1].len += H.free_range[i].len;", "H.free_range[i - 1].len += H.free_range[i].len - 1u;"),
                5: ("H.free_range[j] = H.free_range[j + 1];", "H.free_range[j] = H.free_range[j];"),
                6: ("return s;", "return s ^ 1u;"),
            }
            old, new = changes[negative]
            if old not in body:
                raise RuntimeError("Missing mutation anchor")
            body = body.replace(old, new, 1)
        body = re.sub(r"\bH\b", tag + "_H." + tag, body)
        body = re.sub(r"static (u32|void) (vram_\w+)\(", lambda m: m[1] + " " + tag + "_src_" + m[2] + "(", body)
        (work / (tag + "-production.c")).write_text('#include "common.h"\n' + body)
    dump(work / "source-audit.json", {
        "baseline_texture_sha256": hashlib.sha256(before.encode()).hexdigest(),
        "current_texture_sha256": sha(current / "ps2_hw_tex.inc"),
        "reversal_restores_all_three_complete_frozen_production_files": True,
        "all_other_GS_sources_unchanged": untouched,
        "six_range_copy_loops": after.count("H.free_range[j] = H.free_range[j "),
        "range_capacity": 8193, "VRAM_blocks": 16384,
        "old_and_new_range_payload_bytes": 32772,
        "unchanged_actual_EE_layout_offsets": {"ranges": 2288, "free_n": 35060,
                                               "used_blocks": 35064, "palette": 35264, "H_bytes": H_BYTES},
        "wide_arithmetic_allocation_order_and_failure_policy_unchanged": True,
    })


def run(command, log, expected=0):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True)
    output = result.stdout + result.stderr
    log.write_text(" ".join(map(str, command)) + "\n" + output)
    if result.returncode != expected or "runtime error:" in output:
        print(output)
        raise RuntimeError("Unexpected result: " + str(log))
    return output


def host(work, cc, sanitize=False, expected=0, short=False):
    binary = work / ("test-short" if short else "test-sanitize" if sanitize else "test")
    flags = ["-std=gnu17", "-O2", "-fno-lto", "-Wall", "-Wextra", "-Werror"]
    if short:
        flags += ["-DNEXT10_GS_SHORT"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    run([cc, *flags, "-I" + str(work), work / "base-production.c", work / "cur-production.c",
         TOOLS / "next10_gs_hosttest.c", "-o", binary], work / (binary.name + "-build.log"))
    return run([binary], work / (binary.name + ".log"), expected)


def streams(obj):
    result = {}
    table = symbols(obj)
    for name in NAMES:
        names = [s for s in obj.sections if re.fullmatch(r"\.text\." + name + r"(?:\.lto_priv\.\d+)?", s)]
        if len(names) != 1:
            raise RuntimeError("Missing/ambiguous actual complete helper: " + name)
        section_name = names[0]
        actual_name = section_name[len(".text."):]
        records = []
        for item in obj.relocations.get(section_name, []):
            target = item["target"]
            canonical = re.sub(r"\.lto_priv\.\d+", "", target["name"])
            if canonical.startswith(".rodata.vram_free"):
                canonical = "diagnostic"
            elif canonical == "I_OutputMsg":
                canonical = "capture_output"
            elif canonical == "abort":
                canonical = "capture_abort"
            if canonical not in ("H", "memmove", "diagnostic", "capture_output", "capture_abort") or item["type"] not in (4, 5, 6):
                raise RuntimeError("Unexpected allocator relocation: " + repr(item))
            if canonical == "H" and (target["size"] != H_BYTES or target["value"]):
                raise RuntimeError("Actual H layout changed")
            records.append({"offset": item["offset"], "type": item["type"], "target": canonical,
                            "actual_target": target["name"], "addend": item["explicit_addend"]})
        result[name] = {"name": actual_name, "bytes": obj.content(obj.sections[section_name]),
                        "body_bytes": table[actual_name]["bytes"], "relocations": records}
    return result


def emit(work, path, tag):
    obj = Object(path)
    selected = streams(obj)
    lines = [".set noreorder", ".set noat"]
    for name, data in selected.items():
        label = tag + "_engine_" + name
        lines += ['.section .text.' + label + ',"ax",@progbits', ".balign 8", ".globl " + label,
                  ".type " + label + ",@function", label + ":"]
        lines += [".word 0x%08x" % word for word in struct.unpack("<" + "I" * (len(data["bytes"]) // 4), data["bytes"])]
        for item in data["relocations"]:
            target = tag + "_diagnostic" if item["target"] == "diagnostic" else item["target"]
            lines += [".reloc " + label + "+" + str(item["offset"]) + ",R_MIPS_" +
                      {4: "26", 5: "HI16", 6: "LO16"}[item["type"]] + "," + target + "+" + str(item["addend"])]
        lines += [".size " + label + "," + str(data["body_bytes"])]
    diagnostic = next(r["actual_target"] for r in selected["vram_free"]["relocations"] if r["target"] == "diagnostic")
    original = obj.content(obj.sections[diagnostic])
    if original != b"PS2 GS: VRAM free-list capacity exhausted\n\0":
        raise RuntimeError("Actual capacity diagnostic changed")
    lines += ['.section .rodata.' + tag + '_diagnostic,"a",@progbits', ".balign 8", ".globl " + tag + "_diagnostic", tag + "_diagnostic:"]
    lines += [".byte " + ",".join(str(v) for v in original)]
    source = work / (tag + "-engine.S")
    source.write_text("\n".join(lines) + "\n")
    dump(work / (tag + "-extraction.json"), {"engine": str(path), "sha256": sha(path),
         "functions": {name: {**{k: v for k, v in data.items() if k != "bytes"},
                              "section_bytes": len(data["bytes"]), "code_sha256": hashlib.sha256(data["bytes"]).hexdigest()}
                       for name, data in selected.items()}})
    return source


def verify_embedded(engine, embedded, tag):
    obj = Object(embedded)
    reports = {}
    for name, data in streams(Object(engine)).items():
        sec = ".text." + tag + "_engine_" + name
        records = [{"offset": r["offset"], "type": r["type"], "target": r["target"]["name"],
                    "addend": r["explicit_addend"]} for r in obj.relocations.get(sec, [])]
        expected = [{k: v for k, v in r.items() if k != "actual_target"} for r in data["relocations"]]
        for r in expected:
            if r["target"] == "diagnostic":
                r["target"] = tag + "_diagnostic"
        if obj.content(obj.sections[sec]) != data["bytes"] or expected != records:
            raise RuntimeError("Complete assembler code/ordered RELA extraction mismatch")
        reports[name] = {"all_section_bytes_and_ordered_RELA_records_identical": True,
                         "section_bytes": len(data["bytes"]), "body_bytes": data["body_bytes"], "ordered_relocations": records}
    return reports


def ee(work, base_obj, cur_obj, host_oracle):
    import build as B
    flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-flto")]
    flags += ["-fno-lto", "-Werror", "-DNEXT10_GS_EE", "-DNEXT10_GS_SHORT", "-I" + str(work)]
    objects, reports = [], {}
    for tag, path in (("base", base_obj), ("cur", cur_obj)):
        source = emit(work, path, tag)
        target = source.with_suffix(".o")
        run([B.CC, *flags, *B.INCS, "-c", source, "-o", target], work / (tag + "-engine-build.log"))
        reports[tag] = verify_embedded(path, target, tag)
        objects.append(target)
        target = work / (tag + "-production.o")
        run([B.CC, *flags, *B.INCS, "-c", work / (tag + "-production.c"), "-o", target], work / (tag + "-production-build.log"))
        objects.append(target)
    fixture = work / "fixture.o"
    run([B.CC, *flags, *B.INCS, "-c", TOOLS / "next10_gs_hosttest.c", "-o", fixture], work / "fixture-build.log")
    binary = work / "GS_NEXT10.ELF"
    run([B.CC, *B.LDFLAGS, *objects, fixture, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "fixture-link.log")
    dump(work / "ee-build-report.json", {"elf_sha256": sha(binary), "elf_bytes": binary.stat().st_size,
         "compiler": str(B.CC), "flags": flags, "complete_embedded_sections": reports,
         "fixture_sha256": sha(TOOLS / "next10_gs_hosttest.c"),
         "scope": "complete actual high/free allocator helpers and actual newlib memmove; capacity diagnostic/abort are capture-only error hooks, excluded from timings"})
    bind_oracle(work, host_oracle)
    print("Built " + str(binary))


def bind_oracle(work, host_oracle):
    oracle = json.loads(host_oracle.read_text())
    built = json.loads((work / "ee-build-report.json").read_text())
    if oracle["fixture_sha256"] != sha(TOOLS / "next10_gs_hosttest.c") or built["fixture_sha256"] != oracle["fixture_sha256"]:
        raise RuntimeError("Host/native fixture source hashes differ")
    if not oracle["sanitized"] or oracle["mutations"] != list(range(1, 7)):
        raise RuntimeError("Host sanitizer/executed mutation proof is incomplete")
    if built["elf_sha256"] != sha(work / "GS_NEXT10.ELF"):
        raise RuntimeError("Built native fixture ELF changed")
    dump(work / "expected-host-oracle.json", oracle)


def bind(work, engines, binaries, fixture, negatives=False):
    timed = Object(fixture)
    timed_syms = symbols(timed)
    reports = {}
    for tag in ("base", "cur"):
        obj, elf = Object(engines[tag]), Object(binaries[tag])
        actual_syms = symbols(elf)
        diagnostic_name = next(r["actual_target"] for r in streams(obj)["vram_free"]["relocations"] if r["target"] == "diagnostic")
        diagnostic = obj.content(obj.sections[diagnostic_name])
        candidates = []
        for header in elf.headers:
            if header[1] == 8 or not (header[2] & 2):
                continue
            body = elf.content(header)
            at = 0
            while True:
                at = body.find(diagnostic, at)
                if at < 0:
                    break
                candidates.append(header[3] + at)
                at += 1
        if len(candidates) != 1:
            raise RuntimeError("Expected one exact complete merged capacity diagnostic in actual ELF")
        actual_syms[diagnostic_name] = {"address": candidates[0], "bytes": len(diagnostic)}
        result = {}
        for name, data in streams(obj).items():
            addrs = dict(timed_syms)
            addrs["diagnostic"] = addrs[tag + "_diagnostic"]
            expected = patched(data["bytes"], data["relocations"], addrs)
            actual = patched(data["bytes"], data["relocations"], actual_syms, actual=True)
            def span(binary, symbol_table, symbol_name):
                symbol = symbol_table[symbol_name]
                header = binary.headers[symbol["section"]]
                at = symbol["address"] - header[3]
                return binary.content(header)[at:at + len(data["bytes"])]
            if actual != span(elf, actual_syms, data["name"]) or expected != span(timed, timed_syms, tag + "_engine_" + name):
                raise RuntimeError("Entire resolved real/fixture helper binding failed")
            words = struct.unpack("<" + "I" * (data["body_bytes"] // 4), data["bytes"][:data["body_bytes"]])
            frames = sorted(set(65536 - (w & 65535) for w in words if w >> 16 == 0x27bd and w & 0x8000))
            result[name] = {"body_bytes": data["body_bytes"], "section_bytes": len(data["bytes"]),
                            "stack_frame_bytes": frames, "ordered_relocations": data["relocations"],
                            "complete_actual_and_fixture_resolved_bytes_match_including_section_padding": True}
            if negatives and tag == "cur":
                code = bytearray(data["bytes"]); code[0] ^= 1
                moved = copy.deepcopy(data["relocations"]); moved[0]["offset"] += 4
                addend = copy.deepcopy(data["relocations"]); addend[0]["addend"] += 65536
                for changed, relas in ((code, data["relocations"]), (data["bytes"], moved), (data["bytes"], addend)):
                    try:
                        same = patched(changed, relas, addrs) == expected
                    except RuntimeError:
                        same = False
                    if same:
                        raise RuntimeError("Executed code-binding mutation escaped")
        if actual_syms[next(r["actual_target"] for r in streams(obj)["vram_alloc_hi"]["relocations"] if r["target"] == "H")]["bytes"] != H_BYTES or timed_syms["H"]["bytes"] != H_BYTES:
            raise RuntimeError("Actual/fixture H size changed")
        # memmove is a real timed dependency, so compare its whole linked SDK body.
        sdk = function_bytes(elf, actual_syms, "memmove")
        if sdk != function_bytes(timed, timed_syms, "memmove"):
            raise RuntimeError("Actual linked SDK memmove differs from timing fixture")
        sdk_words = struct.unpack("<" + "I" * (len(sdk) // 4), sdk)
        for word in sdk_words:
            opcode, rs, rt, function = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 63
            if (opcode in (2, 3) or (opcode == 1 and rt in (16, 17, 18, 19)) or
                    (opcode == 0 and (function in (9, 12) or (function == 8 and rs != 31)))):
                raise RuntimeError("SDK memmove has a call/jump/syscall requiring additional closure proof")
        reports[tag] = {"engine_sha256": sha(engines[tag]), "elf_sha256": sha(binaries[tag]),
                        "functions": result, "actual_newlib_memmove_bytes": len(sdk),
                        "actual_newlib_memmove_sha256": hashlib.sha256(sdk).hexdigest(),
                        "exact_merged_capacity_diagnostic_address": candidates[0],
                        "exact_merged_capacity_diagnostic_bytes": len(diagnostic),
                        "complete_timed_SDK_closure_is_this_verified_leaf": True,
                        "SDK_leaf_has_no_calls_nonreturn_jumps_or_syscalls": True}
    for name in ("fill_idx", "fill_ap88", "fill_ia88", "fill_ap88_ct32", "fill_ct32", "state_flush"):
        before, after = Object(engines["base"]), Object(engines["cur"])
        old = next(s for s in before.sections if re.fullmatch(r"\.text\." + name + r"(?:\.lto_priv\.\d+)?", s))
        new = next(s for s in after.sections if re.fullmatch(r"\.text\." + name + r"(?:\.lto_priv\.\d+)?", s))
        if before.content(before.sections[old]) != after.content(after.sections[new]) or canonical_relocations(before, old) != canonical_relocations(after, new):
            raise RuntimeError("Earlier upload/state helper changed: " + name)
    reports["earlier_complete_upload_and_state_helpers_unchanged"] = True
    reports["binding_mutations_detected_per_helper"] = ["instruction bit", "moved relocation", "explicit addend"] if negatives else []
    dump(work / "linked-code-report.json", reports)
    print("Complete DEFAULT-LTO allocator/SDK/real-ELF bindings passed.")


def exact_result(content):
    found = re.findall(r"GS NEXT10 EXACT cases=(\d+) allocations=(\d+) frees=(\d+) defensive=(\d+) state_bytes=(\d+) digest=([0-9a-f]{8})", content)
    if len(found) != 1:
        raise RuntimeError("Missing/ambiguous exactness result")
    return {"cases": int(found[0][0]), "allocations": int(found[0][1]), "frees": int(found[0][2]),
            "defensive": int(found[0][3]), "state_bytes": int(found[0][4]), "digest": found[0][5]}


def parse_timing(content, oracle):
    if content.count("GS NEXT10 DONE") != 1 or "GS NEXT10 FAIL" in content:
        raise RuntimeError("Native exactness/completion failed")
    exact = exact_result(content)
    if exact != oracle["short"]:
        raise RuntimeError("Native deterministic corpus differs from independent host corpus")
    rows = {}
    pattern = r"NEXT10 TIMING row=(\d+) op=(\d+) count=(\d+) kind=(\d+) cache=(\d+) rep=(\d+) old=(\d+) new=(\d+) iters=(\d+)"
    records = re.findall(pattern, content)
    if len(records) != 144 * 9 or len(records) != content.count("NEXT10 TIMING row="):
        raise RuntimeError("Missing, malformed or extra timing record")
    expected = [(op, count, kind, cache) for cache in range(2) for op in (1, 2)
                for count in (0, 1, 2, 3, 7, 16, 31, 64, 127, 512, 2048, 8192) for kind in range(3)]
    for values in records:
        row, op, count, kind, cache, rep, old, new, iters = map(int, values)
        if (not 0 <= row < 144 or (op, count, kind, cache) != expected[row] or not 0 <= rep < 9 or iters != 8 or
                not 0 < old < 0x80000000 or not 0 < new < 0x80000000):
            raise RuntimeError("Prescribed timing parameters/counter changed")
        entry = rows.setdefault(row, {"op": op, "count": count, "kind": kind, "cache": cache, "iters": iters, "samples": {}})
        if rep in entry["samples"] or any(entry[k] != v for k, v in (("op", op), ("count", count), ("kind", kind), ("cache", cache), ("iters", iters))):
            raise RuntimeError("Duplicate/inconsistent timing row")
        entry["samples"][rep] = [old, new]
    if set(rows) != set(range(144)) or any(set(r["samples"]) != set(range(9)) for r in rows.values()):
        raise RuntimeError("Missing prescribed timing row/repetition")
    for value in rows.values():
        value["old_median"] = statistics.median(p[0] for p in value["samples"].values())
        value["new_median"] = statistics.median(p[1] for p in value["samples"].values())
        value["percent"] = (value["new_median"] / value["old_median"] - 1) * 100
    old = sum(r["old_median"] for r in rows.values()); new = sum(r["new_median"] for r in rows.values())
    result = {"exact": exact, "host_corpus_identical": True,
              "rows": rows, "old_sum": old, "new_sum": new, "percent": (new / old - 1) * 100,
              "faster": sum(r["new_median"] < r["old_median"] for r in rows.values()),
              "ties": sum(r["new_median"] == r["old_median"] for r in rows.values()),
              "slower": sum(r["new_median"] > r["old_median"] for r in rows.values())}
    return result


def summarize(work, log, negative=False):
    oracle = json.loads((work / "expected-host-oracle.json").read_text())
    if not oracle["sanitized"] or oracle["mutations"] != list(range(1, 7)):
        raise RuntimeError("Independent host sanitizer/mutation proof incomplete")
    wrapper = json.loads((work / "run-metadata.json").read_text())
    if (wrapper["rc"] or wrapper["elf_sha256_before"] != wrapper["elf_sha256_after"] or
            wrapper["elf_sha256_after"] != sha(work / "GS_NEXT10.ELF") or wrapper["log_sha256"] != sha(log)):
        raise RuntimeError("Wrapper/input/ELF/log provenance differs")
    content = log.read_text(); result = parse_timing(content, oracle)
    if (work / "run-pre.json").exists():
        pre = json.loads((work / "run-pre.json").read_text()); post = json.loads((work / "run-post.json").read_text())
        if "critical_settings" in pre and (pre["critical_settings"] != post["critical_settings"] or
                not post["critical_settings_unchanged"] or not post["emulator_binary_unchanged"] or
                pre["input_sha256"] != post["input_sha256"] or not post["all_frozen_inputs_unchanged"]):
            raise RuntimeError("Critical non-UI emulator settings/binary/native inputs changed")
        result["complete_pre_post_non_UI_settings_and_binary_recorded"] = "critical_settings" in pre
    controls = []
    if negative:
        first = re.search(r"NEXT10 TIMING row=0[^\n]*", content)[0]
        variants = [content.replace(first, "", 1), content + "\n" + first,
                    content.replace("row=0 op=1 count=0", "row=0 op=1 count=1", 1),
                    content.replace("GS NEXT10 DONE", "GS NEXT10 INCOMPLETE", 1),
                    content + "\nGS NEXT10 FAIL injected\n",
                    content.replace("digest=" + oracle["short"]["digest"], "digest=00000000", 1),
                    content.replace("iters=8", "iters=7", 1), content.replace("old=", "old=bad", 1)]
        directory = work / "parser-negative-controls"; directory.mkdir(exist_ok=True)
        for i, variant in enumerate(variants, 1):
            (directory / (str(i) + ".log")).write_text(variant)
            try:
                parse_timing(variant, oracle)
            except RuntimeError as error:
                controls.append({"mutation": i, "rejected": str(error)})
            else:
                raise RuntimeError("High/free report-parser mutation escaped")
    result["parser_negative_controls"] = controls
    dump(work / "benchmark-summary.json", result)
    print(json.dumps({k: v for k, v in result.items() if k != "rows"}))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--base-src", type=Path, default=ROOT / "build/nov15-next10/baseline-src/src")
    ap.add_argument("--current-src", type=Path, default=ROOT / "src")
    ap.add_argument("--host-oracle", type=Path, default=ROOT / "build/nov15-next10/gs/host/host-report.json")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--ee-only", action="store_true")
    ap.add_argument("--bind-host-oracle", action="store_true")
    ap.add_argument("--baseline-engine", type=Path)
    ap.add_argument("--current-engine", type=Path)
    ap.add_argument("--baseline-elf", type=Path)
    ap.add_argument("--current-elf", type=Path)
    ap.add_argument("--fixture-elf", type=Path)
    ap.add_argument("--summarize", type=Path)
    args = ap.parse_args()
    work = args.out.resolve(); work.mkdir(parents=True, exist_ok=True)
    if args.summarize:
        summarize(work, args.summarize, args.negative_controls)
        return
    if args.bind_host_oracle:
        bind_oracle(work, args.host_oracle)
        print("Bound current host/native exactness corpora.")
        return
    prepare(work, args.base_src.resolve(), current_src=args.current_src.resolve())
    if args.fixture_elf:
        bind(work, {"base": args.baseline_engine, "cur": args.current_engine},
             {"base": args.baseline_elf, "cur": args.current_elf}, args.fixture_elf, args.negative_controls)
    elif args.ee_only:
        ee(work, args.baseline_engine, args.current_engine, args.host_oracle)
    else:
        full = host(work, args.cc); short = host(work, args.cc, short=True)
        print(full.strip()); print(short.strip())
        report = {"fixture_sha256": sha(TOOLS / "next10_gs_hosttest.c"),
                  "full": exact_result(full), "short": exact_result(short), "sanitized": False, "mutations": []}
        if args.sanitize:
            sanitized = host(work, args.cc, sanitize=True)
            print(sanitized.strip())
            if exact_result(sanitized) != report["full"]:
                raise RuntimeError("Sanitized corpus differs")
            report["sanitized"] = True
        if args.negative_controls:
            for n in range(1, 7):
                sub = work / ("negative-" + str(n))
                prepare(sub, args.base_src.resolve(), n, args.current_src.resolve())
                output = host(sub, args.cc, expected=1)
                if "GS NEXT10 FAIL" not in output:
                    raise RuntimeError("Executed source mutation escaped")
                print("Mutation " + str(n) + " detected")
                report["mutations"].append(n)
        dump(work / "host-report.json", report)


if __name__ == "__main__":
    main()
