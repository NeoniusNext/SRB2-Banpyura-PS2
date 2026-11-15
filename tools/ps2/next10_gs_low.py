#!/usr/bin/env python3
"""Complete actual tex_upload/low-allocation fixture with full data/RELA binding."""
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
from next10_gs_hosttest import ROOT, TOOLS, dump, run, section, sha
import hw_hosttest

CODE = ("tex_upload", "tex_plan", "tex_footprint", "upload_bands", "rec_new", "fill_idx",
        "weights_prepare", "small_victim", "scr_spill_victim", "__wrap_memcpy", "__wrap_memset")
SPECIAL = {"H": "H", "TX": "TX", "cfg": "cfg", "V": "V", "VU": "VU",
           "blk_owner_p": "blk_owner_p", "ps2hwd_dbg_flags": "ps2hwd_dbg_flags",
           "resource_error_handler": "resource_error_handler", "tex_fail_why": "tex_fail_why"}
SDK = {"memcpy", "memmove", "memset"}


def canonical(name):
    return re.sub(r"\.lto_priv\.\d+", "", name)


def selected(obj):
    result = {}
    for name in CODE:
        found = [s for s in obj.sections if re.fullmatch(r"\.text\." + name + r"(?:\.lto_priv\.\d+)?", s)]
        if len(found) != 1:
            raise RuntimeError("Missing actual complete function: " + name)
        result[found[0]] = name
    return result


def bundle(work, path, tag):
    obj = Object(path)
    code = selected(obj)
    table = symbols(obj)
    gp0 = struct.unpack_from("<I", obj.content(obj.sections[".reginfo"]), 20)[0]
    nodes, labels, queue, local_aliases = {}, {}, list(code), {}
    def target(record):
        symbol = record["target"]
        name, sec = canonical(symbol["name"]), symbol["section"]
        if name in SPECIAL:
            return SPECIAL[name]
        if sec == ".bss.up_hist":
            return "low_up_hist"
        if sec in code:
            return tag + "_" + code[sec]
        if not sec:
            if name in ("memcpy", "memset"):
                return tag + "___wrap_" + name
            if name in ("__real_memcpy", "__real_memset"):
                return name[len("__real_"):]
            return name if name in SDK else "low_unexpected"
        if sec not in obj.sections:
            raise RuntimeError("Unexpected special section: " + sec)
        if obj.sections[sec][2] & 4:
            return "low_output" if name == "I_OutputMsg" else "low_unexpected"
        if sec not in labels:
            labels[sec] = tag + "_data_" + str(len(labels))
            queue.append(sec)
        if symbol["value"]:
            return labels[sec] + "_at_" + str(symbol["value"])
        return labels[sec]
    while queue:
        sec = queue.pop(0)
        if sec in nodes:
            continue
        records = []
        for original in obj.relocations.get(sec, []):
            if original["type"] not in (2, 4, 5, 6, 7) or original["explicit_addend"] is None:
                raise RuntimeError("Unsupported original allocator graph RELA")
            mapped_target = target(original)
            if original["type"] == 7 and original["target"]["info"] >> 4 == 0:
                local = mapped_target + "_local_gp"
                local_aliases[local] = mapped_target
                mapped_target = local
            records.append({"offset": original["offset"], "type": original["type"],
                            "addend": original["explicit_addend"], "target": mapped_target,
                            "original_target": original["target"]})
        label = tag + "_" + code[sec] if sec in code else labels[sec]
        nodes[sec] = {"label": label, "bytes": obj.sections[sec][5],
                      "body_bytes": table[sec[len('.text.'):]]["bytes"] if sec in code else obj.sections[sec][5],
                      "kind": "code" if sec in code else "data", "ordered_relocations": records,
                      "signature": obj.signature(sec)}
    lines = [".set noreorder", ".set noat", ".gpvalue " + str(gp0)]
    for sec, node in nodes.items():
        header, label = obj.sections[sec], node["label"]
        output_section = (".text." if sec in code else ".sbss." if sec.startswith(".sbss") else
                          ".sdata." if sec.startswith(".sdata") else ".bss." if header[1] == 8 else
                          ".data." if header[2] & 1 else ".rodata.") + label
        flags = "ax" if sec in code else "aw" if header[2] & 1 else "a"
        lines += ['.section ' + output_section + ',"' + flags + '",@' + ("nobits" if header[1] == 8 else "progbits"),
                  ".balign " + str(max(4, header[8])), ".globl " + label,
                  ".type " + label + ",@" + ("function" if sec in code else "object"), label + ":"]
        data = obj.content(header)
        if header[1] == 8:
            lines += [".space " + str(header[5])]
        else:
            for at in range(0, len(data), 32):
                lines += [".byte " + ",".join(str(v) for v in data[at:at + 32])]
        lines += [".size " + label + "," + str(node["body_bytes"])]
        for record in node["ordered_relocations"]:
            lines += [".reloc " + label + "+" + str(record["offset"]) + ",R_MIPS_" +
                      {2: "32", 4: "26", 5: "HI16", 6: "LO16", 7: "GPREL16"}[record["type"]] + "," +
                      record["target"] + "+" + str(record["addend"])]
    aliases = set()
    for node in nodes.values():
        for record in node["ordered_relocations"]:
            label = local_aliases.get(record["target"], record["target"])
            if "_at_" in label and label not in aliases:
                base, value = label.rsplit("_at_", 1)
                lines += [".globl " + label, ".set " + label + "," + base + "+" + value]
                aliases.add(label)
    for local, global_name in local_aliases.items():
        lines += [".local " + local, ".set " + local + "," + global_name]
    source = work / (tag + "-graph.S")
    source.write_text("\n".join(lines) + "\n")
    dump(work / (tag + "-graph.json"), {"engine": str(path), "engine_sha256": sha(path),
                                      "original_GP0": gp0, "preserved_local_GP_aliases": local_aliases, "nodes": nodes})
    return nodes, source


def native_header(work, current):
    hw_hosttest.make_shim(work)
    (work / "hw_model.h").write_text((ROOT / "src/hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
    shim = (work / "shim.h").read_text()
    shim = shim.replace("typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;", "#include <tamtypes.h>")
    (work / "shim.h").write_text(shim)
    private = (current / "ps2/hw/ps2_hw_priv.inc").read_text()
    dbg = (current / "ps2/hw/ps2_hwd_dbg.h").read_text().replace('#include "../../doomtype.h"', '#include "shim.h"')
    (work / "ps2_hwd_dbg.h").write_text(dbg)
    header = '#include "shim.h"\n#include "ps2_hwd_dbg.h"\n#include <kernel.h>\n'
    header += section(private, "#define GSR_PRIM", "// DMA channel 2")
    header += section(private, "typedef union\n{\n\tu64 d[2];", "static inline u32 cyc")
    header += section(private, "#define HWGIF_TAG", "#define PS2HWD_MAXPOLY")
    header += section(private, "static ps2hwd_config_t cfg", "#define NBUF").replace("static ps2hwd_config_t cfg", "ps2hwd_config_t cfg __attribute__((aligned(8)))")
    header += section(private, "#define NBUF", "// state of the stream")
    header += section(private, "// state of the stream", "int ps2hwd_dbg_flags;").replace("static struct\n{\n\tint up;", "struct\n{\n\tint up;")
    header += section(private, "typedef struct\n{\n\tu32 frames;", "enum\n{\n\tHW_SHADER").replace("static txstats_t TX;", "txstats_t TX __attribute__((aligned(8)));")
    header += """
int ps2hwd_dbg_flags;
u8 V[20] __attribute__((aligned(4)));
u8 VU[992] __attribute__((aligned(16)));
u16 (*blk_owner_p)[VRAM_BLOCKS];
void (*resource_error_handler)(const char *);
const char *tex_fail_why;
struct { const GLMipmap_t *m; u32 frame1; } low_up_hist[2048] __attribute__((aligned(8)));
_Static_assert(sizeof H == 36896 && offsetof(__typeof__(H), pal) == 35264, "whole production H/palette layout");
_Static_assert(sizeof(texrec_t) == 148 && sizeof(GLMipmap_t) == 76, "actual production record/mipmap ABI");
_Static_assert(sizeof TX == 224 && sizeof cfg == 56 && sizeof low_up_hist == 16384, "actual graph state ABI");
int base_tex_upload(GLMipmap_t *); int cur_tex_upload(GLMipmap_t *);
"""
    (work / "low-native.h").write_text(header)


def tables(work, graphs):
    declarations, entries = [], []
    for tag, nodes in graphs.items():
        for sec, node in nodes.items():
            if node["kind"] == "data" and node["signature"]["flags"] & 1:
                declarations.append("extern unsigned char " + node["label"] + "[];")
                entries.append("{" + node["label"] + "," + str(node["bytes"]) + "}")
    (work / "low-globals.h").write_text("\n".join(declarations) + "\n" +
        "static struct { unsigned char *address; unsigned bytes; } low_globals[] = {\n" + ",\n".join(entries) + "\n};\n")


def host_prepare(work, source, current, negative=0):
    work.mkdir(parents=True, exist_ok=True)
    hw_hosttest.make_shim(work)
    directory = source / "ps2/hw"
    for path in list(directory.glob("*.inc")) + list(directory.glob("*.h")):
        (work / path.name).write_text(path.read_text().replace('#include "../../doomtype.h"', '#include "shim.h"'))
    if negative:
        path = work / "ps2_hw_tex.inc"
        body = path.read_text()
        begin = body.index("static void upload_bands")
        if negative in (2, 5):
            end = body.index("// ---- texture data cache", begin)
            selected = body[begin:end]
            old, new = ({2: ("ad_set(&h[3], (u64)W2", "ad_set(&h[3], (u64)(W2 + 1)"),
                         5: ("H.serial++;", "H.serial += 2;")})[negative]
            if old not in selected:
                raise RuntimeError("Missing low packet/state mutation anchor")
            body = body[:begin] + selected.replace(old, new, 1) + body[end:]
        else:
            old, new = {1: ("return s;", "return s ^ 1u;"),
                        3: ("memcpy(dst, s, c.W2);", "memcpy(dst, s, c.W2 - 1u);"),
                        4: ("r->owner = m;", "r->owner = NULL;"),
                        6: ("tex_fail_why = \"ok\";", "tex_fail_why = \"ok\"; if (m->data) ((u8 *)m->data)[0] ^= 1u;")}[negative]
            if old not in body:
                raise RuntimeError("Missing low source mutation anchor")
            body = body.replace(old, new, 1)
        path.write_text(body)
    (work / "hw_model.h").write_text((ROOT / "src/hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
    fixture = (TOOLS / "hg_hosttest.c").read_text()
    if not current:
        fixture = fixture.replace("H.free_range[0].start", "H.free_start[0]").replace("H.free_range[0].len", "H.free_len[0]")
    (work / "hg-host.inc").write_text(fixture)
    header = """#include "shim.h"
#include "ps2_hw_fx2.h"
static void vu_chunk_flush(void);
void I_OutputMsg(const char *format, ...) { (void)format; }
void Z_SetRebuildCost(void **user, UINT32 cost) { (void)user; (void)cost; }
static void pk_ref_tail(const void *source, u32 bytes) { (void)source; (void)bytes; abort(); }
UINT32 hwr_texsig = 1;
#define main unused_historical_hg_main
#include "hg-host.inc"
#undef main
"""
    (work / "low-host.h").write_text(header)


def host_build(work, current, sanitize=False, expected=0):
    binary = work / ("test-sanitize" if sanitize else "test")
    flags = ["-std=gnu17", "-O2", "-ffp-contract=off", "-fwrapv", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-but-set-variable", "-Wno-misleading-indentation",
             "-Wno-sign-compare", "-Wno-pointer-sign", "-Wno-stringop-overflow", "-Wno-array-bounds", "-DPS2_PROFILE"]
    if current:
        flags += ["-DNEXT10_LOW_CURRENT"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    run(["gcc", *flags, "-I" + str(work), "-I" + str(ROOT / "src/hardware"), TOOLS / "next10_gs_low.c",
         "-Wl,--gc-sections", "-o", binary, "-lm"], work / (binary.name + "-build.log"))
    return run([binary], work / (binary.name + ".log"), expected)


def oracle_matrix():
    cases = []
    for count in (0, 1, 2, 7, 127, 8192):
        for di, (width, height, blocks) in enumerate(((1, 1, 1), (64, 16, 6))):
            alignment = 1
            while alignment < blocks and alignment < 32:
                alignment *= 2
            for kind in range(4):
                occupied = bytearray([1]) * 16384
                if count:
                    if count >= 8192 or kind == 2:
                        for i in range(count): occupied[2 * i + 1] = 0
                    elif kind in (0, 3):
                        first = 32 if kind == 0 else 3
                        occupied[first:first + blocks + alignment + 1] = bytes(blocks + alignment + 1)
                        for i in range(1, count): occupied[512 + 2 * i] = 0
                    else:
                        for i in range(count - 1): occupied[2 * i + 1] = 0
                        first = 2 * count + 3
                        occupied[first:first + blocks + alignment + 1] = bytes(blocks + alignment + 1)
                def runs(value):
                    result, at = [], 1
                    while at < 16384:
                        if occupied[at] != value: at += 1; continue
                        first = at
                        while at < 16384 and occupied[at] == value: at += 1
                        result.append((first, at))
                    return result
                records = len(runs(1)); block, returned = 0, -1
                for first, end in runs(0):
                    candidate = (first + alignment - 1) & -alignment
                    if candidate + blocks <= end:
                        block, returned = candidate, records
                        occupied[block:block + blocks] = bytes([1]) * blocks
                        break
                cases.append({"count": count, "dim": di, "kind": kind, "width": width, "height": height,
                              "blocks": blocks, "block": block, "returned": returned,
                              "free_n": len(runs(0)), "used": sum(occupied[1:]), "success": returned >= 0})
    digests = {}
    for repeat in (1, 2):
        digest = 2166136261
        for case in cases:
            for _ in range(repeat):
                digest = ((digest ^ case["block"] ^ case["blocks"] ^ (case["returned"] & 0xFFFFFFFF) ^
                           case["free_n"] ^ case["used"]) * 16777619) & 0xFFFFFFFF
        digests[repeat] = "%08x" % digest
    return {"cases": cases, "host_cases": len(cases), "native_cases": 2 * len(cases),
            "host_digest": digests[1], "native_digest": digests[2]}


def verify_object(original, embedded, nodes):
    obj, emitted = Object(original), Object(embedded)
    original_gp0 = struct.unpack_from("<I", obj.content(obj.sections[".reginfo"]), 20)[0]
    if struct.unpack_from("<I", emitted.content(emitted.sections[".reginfo"]), 20)[0] != original_gp0:
        raise RuntimeError("Original input GP0 metadata was not preserved")
    report = {}
    for sec, node in nodes.items():
        found = [s for s, header in emitted.sections.items() if s.endswith("." + node["label"]) and header[1] not in (4, 9)]
        if len(found) != 1:
            raise RuntimeError("Missing embedded graph node")
        output = found[0]
        if obj.content(obj.sections[sec]) != emitted.content(emitted.sections[output]) or obj.sections[sec][5] != emitted.sections[output][5]:
            raise RuntimeError("Complete graph bytes/size differ: " + sec)
        actual = [{"offset": r["offset"], "type": r["type"], "addend": r["explicit_addend"], "target": r["target"]["name"]}
                  for r in emitted.relocations.get(output, [])]
        expected = [{k: r[k] for k in ("offset", "type", "addend", "target")} for r in node["ordered_relocations"]]
        if actual != expected:
            raise RuntimeError("Ordered graph RELAs differ: " + sec)
        for before, after in zip(node["ordered_relocations"], emitted.relocations.get(output, [])):
            if before["type"] == 7:
                if before["original_target"]["info"] == 3:
                    for field in ("info", "value", "size"):
                        if before["original_target"][field] != after["target"][field]:
                            raise RuntimeError("Original local GPREL target metadata changed")
                elif after["target"]["info"] >> 4 != before["original_target"]["info"] >> 4:
                    raise RuntimeError("Original global GPREL binding changed")
        report[sec] = {"complete_section_bytes_and_ordered_RELA_identical": True, "bytes": node["bytes"], "relocations": len(actual)}
    return report


def restore_section_symbol_types(path, nodes, gp0):
    """GAS aliases inherit OBJECT; restore the input SECTION metadata verbatim.

    MIPS GPREL16 uses GP0 compensation for SECTION targets. No instruction or
    relocation field is edited, and verify_object checks all restored fields.
    """
    wanted = {r["target"]: r["original_target"] for node in nodes.values() for r in node["ordered_relocations"]
              if r["type"] == 7 and r["original_target"]["info"] == 3}
    obj = Object(path); result = bytearray(obj.data); seen = set()
    for header in obj.headers:
        if header[1] != 2:
            continue
        strings = obj.content(obj.headers[header[6]])
        for at in range(header[4], header[4] + header[5], header[9]):
            item = struct.unpack_from("<IIIBBH", result, at)
            name = obj.string(strings, item[0])
            if name in wanted:
                original = wanted[name]
                if item[1] != original["value"] or item[3] >> 4 != 0:
                    raise RuntimeError("Unexpected assembler local alias value/binding")
                struct.pack_into("<I", result, at + 8, original["size"])
                result[at + 12] = original["info"]
                seen.add(name)
    if seen != set(wanted):
        raise RuntimeError("Missing original local SECTION aliases")
    # .reloc contains raw instructions, so GAS does not infer the input GP0.
    struct.pack_into("<I", result, obj.sections[".reginfo"][4] + 20, gp0)
    path.write_bytes(result)


def build(work, engines):
    import build as B
    objects, reports = [], {}
    flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP", "-flto")]
    flags += ["-fno-lto", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-DNEXT10_LOW_EE", "-I" + str(work)]
    for tag, path in engines.items():
        source = work / (tag + "-graph.S")
        target = source.with_suffix(".o")
        run([B.CC, *flags, *B.INCS, "-c", source, "-o", target], work / (tag + "-graph-build.log"))
        graph = json.loads((work / (tag + "-graph.json")).read_text())
        nodes = graph["nodes"]
        restore_section_symbol_types(target, nodes, graph["original_GP0"])
        reports[tag] = verify_object(path, target, nodes)
        objects.append(target)
    fixture = work / "low-fixture.o"
    run([B.CC, *flags, *B.INCS, "-c", TOOLS / "next10_gs_low.c", "-o", fixture], work / "fixture-build.log")
    binary = work / "GS_LOW10.ELF"
    run([B.CC, *B.LDFLAGS, *objects, fixture, "-Wl,-Map=" + str(work / "GS_LOW10.map"),
         "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "fixture-link.log")
    dump(work / "build-report.json", {"elf_sha256": sha(binary), "fixture_sha256": sha(TOOLS / "next10_gs_low.c"),
                                      "flags": flags, "all_complete_embedded_nodes": reports})
    print("Built " + str(binary))


def linked_span(elf, syms, name, count):
    symbol = syms[name]
    header = elf.headers[symbol["section"]]
    at = symbol["address"] - header[3]
    if at < 0 or at + count > header[5]:
        raise RuntimeError("Full linked input-section span outside ELF")
    return elf.content(header)[at:at + count]


def map_addresses(path):
    result = {}
    pending = None
    for line in path.read_text().splitlines():
        parts = line.split()
        if parts and parts[0].startswith("."):
            pending = parts[0]
            values = parts[1:]
        else:
            values = parts
        if (pending and len(values) >= 3 and values[0].startswith("0x") and values[1].startswith("0x")
                and values[2].endswith("engine_lto.o")):
            if pending in result:
                raise RuntimeError("Duplicate actual input section map address")
            result[pending] = {"address": int(values[0], 16), "bytes": int(values[1], 16)}
            pending = None
    return result


def literal_candidates(elf, value):
    addresses = []
    if not value or value == b"\0":
        raise RuntimeError("Ambiguous empty merged literal")
    for header in elf.headers:
        if header[1] == 8 or not (header[2] & 2):
            continue
        body = elf.content(header); at = 0
        while True:
            at = body.find(value, at)
            if at < 0:
                break
            addresses.append(header[3] + at); at += 1
    if not addresses:
        raise RuntimeError("Missing complete merged literal: " + repr(value))
    return addresses


def resolve_original(obj, elf, syms, mapped, record, literals):
    target = record["original_target"]
    name, sec, addend = target["name"], target["section"], record["addend"]
    if not sec and name in ("memcpy", "memset"):
        return syms["__wrap_" + name]["address"] + addend
    if not sec and name in ("__real_memcpy", "__real_memset"):
        return syms[name[len("__real_"):]]["address"] + addend
    if sec in obj.sections and obj.sections[sec][2] & 32:  # SHF_STRINGS: linker merges individual strings.
        body = obj.content(obj.sections[sec]); at = target["value"] + addend
        if at < 0 or at >= len(body):
            raise RuntimeError("Merged literal RELA outside original input section")
        return literals[(sec, at)]["address"]
    if name in syms:
        return syms[name]["address"] + addend
    if sec.startswith(".text.") and sec[len(".text."):] in syms:
        return syms[sec[len(".text."):]]["address"] + target["value"] + addend
    if sec in mapped and mapped[sec]["bytes"] == obj.sections[sec][5]:
        return mapped[sec]["address"] + target["value"] + addend
    raise RuntimeError("Cannot independently resolve original RELA target: " + repr(record))


def relocate(data, records, address, gp, gp0):
    result = bytearray(data)
    for record in records:
        offset, kind = record["offset"], record["type"]
        word = struct.unpack_from("<I", data, offset)[0]
        absolute = address(record)
        if kind == 2:
            if word:
                raise RuntimeError("Unexpected encoded R32 addend")
            value = absolute & 0xFFFFFFFF
        elif kind == 4:
            if word & 0x3FFFFFF:
                raise RuntimeError("Unexpected encoded R26 addend")
            value = (word & 0xFC000000) | ((absolute >> 2) & 0x3FFFFFF)
        elif kind in (5, 6, 7):
            if word & 65535:
                raise RuntimeError("Unexpected encoded HI/LO/GP addend")
            bias = gp0 if kind == 7 and record["original_target"]["info"] >> 4 == 0 else 0
            immediate = ((absolute + 32768) >> 16) if kind == 5 else absolute if kind == 6 else absolute + bias - gp
            if kind == 7 and not -32768 <= immediate <= 32767:
                raise RuntimeError("GPREL target outside actual signed16 GP window")
            value = (word & 0xFFFF0000) | (immediate & 65535)
        else:
            raise RuntimeError("Unsupported RELA type")
        struct.pack_into("<I", result, offset, value)
    return bytes(result)


def bind(work, engines, binaries, fixture, negative=False):
    timed = Object(fixture); ts = symbols(timed)
    if json.loads((work / "build-report.json").read_text())["elf_sha256"] != sha(fixture):
        raise RuntimeError("Fixture ELF differs from its immutable build report")
    result = {}
    for tag in ("base", "cur"):
        graph = json.loads((work / (tag + "-graph.json")).read_text())
        obj, elf = Object(engines[tag]), Object(binaries[tag]); syms = symbols(elf)
        if json.loads((binaries[tag].parent / "build-report.json").read_text())["elf_sha256"] != sha(binaries[tag]):
            raise RuntimeError("Actual game ELF differs from its original build report")
        if graph["engine_sha256"] != sha(engines[tag]):
            raise RuntimeError("Graph was generated from a different actual engine")
        mapped = map_addresses(binaries[tag].with_suffix(".map"))
        for local, global_name in graph["preserved_local_GP_aliases"].items():
            if local not in ts:
                ts[local] = dict(ts[global_name])
        verified = {}
        for sec, node in graph["nodes"].items():
            data = obj.content(obj.sections[sec]); records = node["ordered_relocations"]
            if obj.sections[sec][1] == 8:
                if ts[node["label"]]["bytes"] != node["bytes"]:
                    raise RuntimeError("Embedded complete BSS node size changed")
                verified[sec] = {"complete_original_BSS_size": node["bytes"]}
                continue
            expected = relocate(data, records, lambda r: ts[r["target"]]["address"] + r["addend"], ts["_gp"]["address"], graph["original_GP0"])
            if expected != linked_span(timed, ts, node["label"], len(data)):
                raise RuntimeError("Complete resolved fixture code/data/RELA mismatch: " + sec)
            literals = {}
            if node["kind"] == "code":
                actual_code = linked_span(elf, syms, sec[len(".text."):], len(data))
                for record in records:
                    target = record["original_target"]; target_sec = target["section"]
                    if target_sec not in obj.sections or not (obj.sections[target_sec][2] & 32):
                        continue
                    key = (target_sec, target["value"] + record["addend"])
                    if key in literals:
                        continue
                    matching = [r for r in records if (r["original_target"]["section"], r["original_target"]["value"] + r["addend"]) == key]
                    high = next((r for r in matching if r["type"] == 5), None)
                    low = next((r for r in matching if r["type"] == 6), None)
                    if high is None or low is None:
                        raise RuntimeError("Merged string lacks complete original HI/LO address pair")
                    hw = struct.unpack_from("<I", actual_code, high["offset"])[0]
                    lw = struct.unpack_from("<I", actual_code, low["offset"])[0]
                    address = (((hw & 65535) << 16) + struct.unpack("<h", struct.pack("<H", lw & 65535))[0]) & 0xFFFFFFFF
                    literal = obj.content(obj.sections[target_sec])[key[1]:].split(b"\0", 1)[0] + b"\0"
                    candidates = literal_candidates(elf, literal)
                    alignment = obj.sections[target_sec][8]
                    if address not in candidates or address % alignment != key[1] % alignment:
                        raise RuntimeError("Concrete merged literal target bytes/alignment differ")
                    literals[key] = {"address": address, "bytes": len(literal), "source_offset": key[1],
                                     "source_section": key[0], "alignment": alignment,
                                     "exact_literal_hex": literal.hex(), "identical_merge_candidates": candidates}
            original = relocate(data, records, lambda r: resolve_original(obj, elf, syms, mapped, r, literals), syms["_gp"]["address"], graph["original_GP0"])
            if node["kind"] == "code":
                actual_name = sec[len(".text."):]
                if original != linked_span(elf, syms, actual_name, len(data)):
                    raise RuntimeError("Complete actual code+padding resolved byte mismatch: " + sec)
                words = struct.unpack("<" + "I" * (node["body_bytes"] // 4), data[:node["body_bytes"]])
                frames = sorted(set(65536 - (w & 65535) for w in words if w >> 16 == 0x27BD and w & 0x8000))
            elif obj.sections[sec][2] & 32:
                for value in data.split(b"\0"):
                    if value: literal_candidates(elf, value + b"\0")
                frames = []
            else:
                address = mapped.get(sec, {}).get("address")
                if address is None or mapped[sec]["bytes"] != len(data):
                    raise RuntimeError("Missing exact complete original data-section mapping: " + sec)
                found = None
                for h in elf.headers:
                    if h[1] != 8 and h[3] <= address and address + len(data) <= h[3] + h[5]:
                        found = elf.content(h)[address - h[3]:address - h[3] + len(data)]
                if original != found:
                    raise RuntimeError("Complete original resolved data node differs: " + sec)
                frames = []
            verified[sec] = {"body_bytes": node["body_bytes"], "full_input_section_bytes": len(data),
                             "original_and_fixture_resolved_bytes_identical_including_padding": True,
                             "ordered_RELA_count": len(records), "stack_frame_bytes": frames,
                             "concrete_merged_literal_targets": list(literals.values())}
            if negative and node["kind"] == "code":
                corrupted = bytearray(data); corrupted[0] ^= 1
                mutations = [(corrupted, records)]
                if records:
                    moved = copy.deepcopy(records); moved[0]["offset"] += 4
                    shifted = copy.deepcopy(records); shifted[0]["addend"] += 65536
                    mutations += [(data, moved), (data, shifted)]
                for candidate, relas in mutations:
                    try:
                        same = relocate(candidate, relas, lambda r: ts[r["target"]]["address"] + r["addend"], ts["_gp"]["address"], graph["original_GP0"]) == expected
                    except (RuntimeError, struct.error):
                        same = False
                    if same:
                        raise RuntimeError("Generic code/RELA mutation escaped")
        sdk = {}
        for name in ("memcpy", "memmove", "memset"):
            before, after = function_bytes(elf, syms, name), function_bytes(timed, ts, name)
            if before != after:
                raise RuntimeError("Full actual SDK body differs from fixture: " + name)
            sdk[name] = {"bytes": len(before), "sha256": hashlib.sha256(before).hexdigest()}
        for name, size, alignment in (("H", 36896, 16), ("TX", 224, 8), ("cfg", 56, 8), ("V", 20, 4), ("VU", 992, 16), ("low_up_hist", 16384, 8)):
            if ts[name]["bytes"] != size or ts[name]["address"] % alignment:
                raise RuntimeError("Complete shared state ABI/size/alignment mismatch: " + name)
        result[tag] = {"engine_sha256": sha(engines[tag]), "elf_sha256": sha(binaries[tag]),
                       "nodes": verified, "SDK_complete_bodies": sdk,
                       "original_gp": syms["_gp"]["address"], "fixture_gp": ts["_gp"]["address"]}
    dump(work / "linked-code-report.json", result)
    print("Complete actual low-upload graph, original data/literals, GP/R32/26/HI/LO RELAs and SDK bodies bind.")


def parse_timing(content, matrix):
    if content.count("GS LOW10 DONE") != 1 or "GS LOW10 FAIL" in content:
        raise RuntimeError("Missing exact completion or native failure")
    expected = "GS LOW10 EXACT cases=" + str(matrix["native_cases"]) + " digest=" + matrix["native_digest"]
    exact = re.findall(r"GS LOW10 EXACT cases=\d+ digest=[0-9a-f]{8}", content)
    if exact != [expected]:
        raise RuntimeError("Native case count/digest differs from independent host/Python matrix")
    pattern = r"LOW10 TIMING row=(\d+) count=(\d+) dim=(\d+) kind=(\d+) cache=(\d+) rep=(\d+) old=(\d+) new=(\d+)"
    records = re.findall(pattern, content)
    if len(records) != 96 * 9 or len(records) != content.count("LOW10 TIMING row="):
        raise RuntimeError("Missing, malformed or extra low timing row")
    rows = {}
    for raw in records:
        row, count, dim, kind, cache, rep, old, new = map(int, raw)
        if not 0 <= row < 96 or not 0 <= rep < 9 or not 0 < old < 0x80000000 or not 0 < new < 0x80000000:
            raise RuntimeError("Invalid prescribed row/repetition/counter")
        case = matrix["cases"][row % 48]
        if (count, dim, kind, cache) != (case["count"], case["dim"], case["kind"], row // 48):
            raise RuntimeError("Prescribed low row parameters changed")
        entry = rows.setdefault(row, {"count": count, "dim": dim, "kind": kind, "cold": bool(cache),
                                     "success": case["success"], "samples": {}})
        if rep in entry["samples"]:
            raise RuntimeError("Duplicate low row/repetition")
        entry["samples"][rep] = [old, new]
    if set(rows) != set(range(96)) or any(set(r["samples"]) != set(range(9)) for r in rows.values()):
        raise RuntimeError("Incomplete low row/repetition set")
    for entry in rows.values():
        entry["old_median"] = statistics.median(v[0] for v in entry["samples"].values())
        entry["new_median"] = statistics.median(v[1] for v in entry["samples"].values())
        entry["percent"] = (entry["new_median"] / entry["old_median"] - 1) * 100
    def total(selected):
        old = sum(r["old_median"] for r in selected); new = sum(r["new_median"] for r in selected)
        return {"rows": len(selected), "old_sum": old, "new_sum": new, "percent": (new / old - 1) * 100,
                "faster": sum(r["new_median"] < r["old_median"] for r in selected),
                "ties": sum(r["new_median"] == r["old_median"] for r in selected),
                "slower": sum(r["new_median"] > r["old_median"] for r in selected)}
    return {"exact_cases": matrix["native_cases"], "exact_digest": matrix["native_digest"], "rows": rows,
            "cohorts": {"all": total(list(rows.values())),
                        "successful_uploads": total([r for r in rows.values() if r["success"]]),
                        "unfit_with_controlled_diagnostic_capture": total([r for r in rows.values() if not r["success"]]),
                        "warm": total([r for r in rows.values() if not r["cold"]]),
                        "cache_flush_requested": total([r for r in rows.values() if r["cold"]])}}


def summarize(work, log, host_report, negative):
    matrix = oracle_matrix(); host = json.loads(host_report.read_text())
    build_report = json.loads((work / "build-report.json").read_text())
    if host["fixture_sha256"] != build_report["fixture_sha256"] or host["fixture_sha256"] != sha(TOOLS / "next10_gs_low.c"):
        raise RuntimeError("Host/native/actual fixture source hashes differ")
    expected_host = "GS LOW10 EXACT cases=" + str(matrix["host_cases"]) + " digest=" + matrix["host_digest"]
    for tag in ("base", "cur"):
        for mode in ("ordinary", "sanitized"):
            output = host["results"][tag][mode]
            if expected_host not in output or "GS LOW10 FAIL" in output:
                raise RuntimeError("Independent host/sanitizer matrix failed")
    if [r["mutation"] for r in host["controls"]] != list(range(1, 7)) or any("GS LOW10 FAIL" not in r["result"] for r in host["controls"]):
        raise RuntimeError("Executed independent low source mutations incomplete")
    pre = json.loads((work / "run-pre.json").read_text()); post = json.loads((work / "run-post.json").read_text())
    if post["rc"] or not post["all_frozen_inputs_unchanged"] or pre["input_sha256"] != post["input_sha256"] or post["log_sha256"] != sha(log):
        raise RuntimeError("Native wrapper failed or frozen run inputs/log changed")
    if "critical_settings" in pre and (pre["critical_settings"] != post["critical_settings"] or
            not post["critical_settings_unchanged"] or not post["emulator_binary_unchanged"]):
        raise RuntimeError("Critical non-UI emulator settings or binary changed")
    content = log.read_text(); report = parse_timing(content, matrix)
    controls = []
    if negative:
        first_line = re.search(r"LOW10 TIMING row=0[^\n]*", content)[0]
        changes = [content.replace(first_line, "", 1), content + "\n" + first_line,
                   content.replace("row=0 count=0", "row=0 count=1", 1),
                   content.replace("GS LOW10 DONE", "GS LOW10 INCOMPLETE", 1),
                   content + "\nGS LOW10 FAIL injected\n",
                   content.replace("digest=" + matrix["native_digest"], "digest=00000000", 1),
                   content.replace("old=", "old=bad", 1),
                   content.replace("rep=0", "rep=1", 1)]
        directory = work / "parser-negative-controls"; directory.mkdir(exist_ok=True)
        for i, changed in enumerate(changes, 1):
            (directory / (str(i) + ".log")).write_text(changed)
            try:
                parse_timing(changed, matrix)
            except RuntimeError as error:
                controls.append({"mutation": i, "rejected": str(error)})
            else:
                raise RuntimeError("Executed low report-parser mutation escaped")
    report["parser_negative_controls"] = controls
    report["complete_pre_post_non_UI_settings_and_binary_recorded"] = "critical_settings" in pre
    report["scope"] = "Complete actual default-LTO upload/low-allocation path and actual memory wrappers/SDK; successful and controlled diagnostic-boundary failures separately; no physical-console/cache or whole-game percentage claim."
    dump(work / "benchmark-summary.json", report)
    print(json.dumps(report["cohorts"], indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next10/gs/low")
    ap.add_argument("--baseline-engine", type=Path, default=ROOT / "build/nov15-next10/baseline/hardware/engine_lto.o")
    ap.add_argument("--current-engine", type=Path, default=ROOT / "build/nov15-next10/gs/candidate-hardware/obj/engine_lto.o")
    ap.add_argument("--current-src", type=Path, default=ROOT / "build/nov15-next10/gs/measured-source/src")
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--host", action="store_true")
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--bind", action="store_true")
    ap.add_argument("--baseline-elf", type=Path, default=ROOT / "build/nov15-next10/baseline/hardware/SRB2.ELF")
    ap.add_argument("--current-elf", type=Path, default=ROOT / "build/nov15-next10/gs/candidate-hardware/SRB2.ELF")
    ap.add_argument("--summarize", type=Path)
    ap.add_argument("--host-report", type=Path, default=ROOT / "build/nov15-next10/gs/low-host/host-report.json")
    args = ap.parse_args()
    work = args.out.resolve(); work.mkdir(parents=True, exist_ok=True)
    if args.summarize:
        summarize(work, args.summarize, args.host_report, args.negative_controls)
        return
    if args.host:
        results = {}
        for tag, source, current in (("base", ROOT / "build/nov15-next10/baseline-src/src", False), ("cur", args.current_src, True)):
            directory = work / tag
            host_prepare(directory, source, current)
            results[tag] = {"ordinary": host_build(directory, current)}
            if args.sanitize:
                results[tag]["sanitized"] = host_build(directory, current, True)
            print(results[tag]["ordinary"].strip())
        controls = []
        if args.negative_controls:
            for negative in range(1, 7):
                directory = work / ("negative-" + str(negative))
                host_prepare(directory, args.current_src, True, negative)
                output = host_build(directory, True, expected=1)
                if "GS LOW10 FAIL" not in output:
                    raise RuntimeError("Low source mutation escaped independent oracle")
                controls.append({"mutation": negative, "result": output.strip()})
                print("Low mutation " + str(negative) + " detected")
        oracle = oracle_matrix()
        for versions in results.values():
            for value in versions.values():
                if not re.search(r"GS LOW10 EXACT cases=" + str(oracle["host_cases"]) + " digest=" + oracle["host_digest"], value):
                    raise RuntimeError("Host complete low-upload corpus differs from independent Python oracle")
        dump(work / "host-report.json", {"fixture_sha256": sha(TOOLS / "next10_gs_low.c"), "results": results,
                                         "controls": controls, "independent_matrix": oracle})
        return
    engines = {"base": args.baseline_engine, "cur": args.current_engine}
    if args.bind:
        bind(work, engines, {"base": args.baseline_elf, "cur": args.current_elf}, work / "GS_LOW10.ELF", args.negative_controls)
        return
    graphs = {}
    for tag, path in engines.items():
        graphs[tag], _ = bundle(work, path, tag)
    native_header(work, args.current_src)
    tables(work, graphs)
    dump(work / "independent-matrix.json", oracle_matrix())
    if args.build:
        build(work, engines)


if __name__ == "__main__":
    main()
