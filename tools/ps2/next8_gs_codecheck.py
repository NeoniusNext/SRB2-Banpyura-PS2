#!/usr/bin/env python3
"""Bind the measured full-LTO GS row code to actual final Hardware ELFs.

Checks every instruction byte after applying only the original HI16/LO16
relocations, and verifies unchanged complete PSMT8 kernels separately.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
from next8_gs_hosttest import NAMES, H_BYTES, Object, dump, sha, stream, symbol_sizes


def symbols(elf):
    result = {}
    for table in elf.headers:
        if table[1] != 2:
            continue
        strings = elf.content(elf.headers[table[6]])
        for offset in range(table[4], table[4] + table[5], table[9]):
            item = struct.unpack_from("<IIIBBH", elf.data, offset)
            name = elf.string(strings, item[0])
            if name:
                result[name] = {"address": item[1], "bytes": item[2], "section": item[5], "info": item[3]}
    return result


def function_bytes(elf, syms, name):
    symbol = syms[name]
    section = elf.headers[symbol["section"]]
    offset = symbol["address"] - section[3]
    if offset < 0 or offset + symbol["bytes"] > section[5]:
        raise RuntimeError("Invalid linked symbol bounds")
    return elf.content(section)[offset:offset + symbol["bytes"]]


def patched(data, relocations, H_address):
    result = bytearray(data)
    for reloc in relocations:
        word = struct.unpack_from("<I", data, reloc["offset"])[0]
        if word & 65535:
            raise RuntimeError("Unexpected encoded addend in RELA instruction")
        absolute = H_address + reloc["addend"]
        if reloc["type"] == 5:
            immediate = (absolute + 32768) >> 16
        elif reloc["type"] == 6:
            immediate = absolute
        else:
            raise RuntimeError("Unexpected relocation type")
        struct.pack_into("<I", result, reloc["offset"], (word & 0xffff0000) | (immediate & 65535))
    return bytes(result)


def bind(engine, binary, fixture, tag):
    obj, elf, timed = Object(engine), Object(binary), Object(fixture)
    syms, timed_syms = symbols(elf), symbols(timed)
    H_names = [name for name, sym in syms.items() if re.fullmatch(r"H(?:\.lto_priv\.\d+)?", name) and sym["bytes"] == H_BYTES]
    if len(H_names) != 1 or timed_syms["H"]["bytes"] != H_BYTES:
        raise RuntimeError("Actual and fixture H layouts differ")
    report = {"engine_object": str(engine), "engine_sha256": sha(engine), "elf": str(binary), "elf_sha256": sha(binary), "H_address": hex(syms[H_names[0]]["address"]), "functions": {}}
    for name in NAMES:
        data = stream(obj, name)
        body_bytes = symbol_sizes(obj)[name]
        if any(data["bytes"][body_bytes:]):
            raise RuntimeError("Expected only zero alignment padding after function")
        expected = patched(data["bytes"], data["relocations"], syms[H_names[0]]["address"])[:body_bytes]
        actual = function_bytes(elf, syms, name)
        timed_expected = patched(data["bytes"], data["relocations"], timed_syms["H"]["address"])[:body_bytes]
        timed_actual = function_bytes(timed, timed_syms, tag + "_" + name)
        if expected != actual or timed_expected != timed_actual:
            raise RuntimeError("Complete linked function differs from measured object: " + tag + ":" + name)
        words = struct.unpack("<" + "I" * (len(data["bytes"]) // 4), data["bytes"])
        # No JR/return's delay-slot frame manipulation, or any stack adjustment.
        if any((word >> 16) == 0x27bd for word in words):
            raise RuntimeError("Unexpected helper stack frame")
        backedges = []
        for index, word in enumerate(words[:body_bytes // 4]):
            if word >> 26 != 5:  # BNE; these helpers each have one pixel-loop branch.
                continue
            displacement = struct.unpack("<h", struct.pack("<H", word & 65535))[0]
            if displacement < 0:
                loop_start = index + 1 + displacement
                backedges.append({"setup_instructions": loop_start, "pixel_loop_instructions_including_branch_delay": index + 2 - loop_start, "epilogue_instructions": body_bytes // 4 - index - 2})
        if len(backedges) != 1:
            raise RuntimeError("Expected one complete pixel loop")
        report["functions"][name] = {"object_section_bytes": len(data["bytes"]), "object_symbol_bytes": body_bytes, "zero_alignment_padding_bytes": len(data["bytes"]) - body_bytes, "actual_elf_bytes": len(actual), "fixture_elf_bytes": len(timed_actual), "all_complete_instructions_identical_after_original_relocations": True, "ordered_relocations": data["relocations"], "stack_frame_bytes": 0, "positive_width_instruction_structure": backedges[0], "object_code_sha256": hashlib.sha256(data["bytes"]).hexdigest()}
    return report


def canonical_relocations(obj, section):
    records = []
    for record in obj.relocations.get(section, []):
        record = copy.deepcopy(record)
        record.pop("relocation_section")
        for key in ("name", "section"):
            record["target"][key] = re.sub(r"\.lto_priv\.\d+$", "", record["target"][key])
        records.append(record)
    return records


def unaffected(before, after):
    result = {}
    for name in ("fill_idx", "fill_ap88", "fill_ia88"):
        section = ".text." + name
        old, new = before.content(before.sections[section]), after.content(after.sections[section])
        old_reloc, new_reloc = canonical_relocations(before, section), canonical_relocations(after, section)
        if old != new or old_reloc != new_reloc:
            raise RuntimeError("Unaffected production row helper changed: " + name)
        words = struct.unpack("<" + "I" * (len(old) // 4), old)
        ppacb = sum((word & 0xfc0007ff) == 0x700006c8 for word in words)
        result[name] = {"bytes": len(old), "all_complete_instructions_and_relocations_unchanged": True, "ordered_relocations": old_reloc, "code_sha256": hashlib.sha256(old).hexdigest(), "ppacb_sites": ppacb}
    if result["fill_ap88"]["ppacb_sites"] != 3:
        raise RuntimeError("Expected all three preceding AP88 PPACB sites")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--baseline-engine", type=Path, required=True)
    parser.add_argument("--current-engine", type=Path, required=True)
    parser.add_argument("--baseline-elf", type=Path, required=True)
    parser.add_argument("--current-elf", type=Path, required=True)
    parser.add_argument("--fixture-elf", type=Path, required=True)
    parser.add_argument("--negative-controls", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {"baseline": bind(args.baseline_engine, args.baseline_elf, args.fixture_elf, "base"), "current": bind(args.current_engine, args.current_elf, args.fixture_elf, "cur"), "unaffected": unaffected(Object(args.baseline_engine), Object(args.current_engine))}
    if args.negative_controls:
        obj = Object(args.current_engine)
        selected = stream(obj, NAMES[0])
        original = patched(selected["bytes"], selected["relocations"], 0x512380)
        bad_code = bytearray(selected["bytes"])
        bad_code[0] ^= 1
        bad_reloc = copy.deepcopy(selected["relocations"])
        bad_reloc[0]["offset"] += 4
        bad_addend = copy.deepcopy(selected["relocations"])
        bad_addend[0]["addend"] += 65536
        for code, relocs in ((bad_code, selected["relocations"]), (selected["bytes"], bad_reloc), (selected["bytes"], bad_addend)):
            try:
                changed = patched(code, relocs, 0x512380)
            except RuntimeError:
                continue
            if changed == original:
                raise RuntimeError("Linked binding mutation escaped")
        result["negative_controls"] = ["instruction byte", "relocation location", "explicit relocation addend"]
    dump(args.out / "linked-code-report.json", result)
    print("All complete baseline/current LTO helper streams bind exactly to the actual Hardware and fixture ELFs; PSMT8 kernels and IA88 unchanged.")


if __name__ == "__main__":
    main()
