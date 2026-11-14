#!/usr/bin/env python3
"""Bind the complete actual LTO cache helper and its resolved ELF instructions.

Read-only: verifies every relocation's patched bits, nonrelocated bits, cache
storage and frame. Optional whole-object isolation/opt-out and measured-to-final
checks include all ordered relocation records and their explicit addends.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from audio_storage_next7_finalbind import Object
from next9_io_codecheck import canonical, frame, symbols

ROOT = Path(__file__).resolve().parents[2]
HELPER = ".text.CheckLumpInCache"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def section_map(obj):
    result = {}
    for name, section in obj.sections.items():
        if not section[2] & 2:
            continue
        normal = name
        assert normal not in result, "colliding canonical section names: " + normal
        result[normal] = name
    return result


def relocs(obj, name):
    result = []
    seen = {"name": {}, "section": {}}
    for row in obj.relocations.get(name, []):
        target = dict(row["target"])
        for field in seen:
            original = target[field]
            normal = canonical(original)
            assert normal not in seen[field] or seen[field][normal] == original
            seen[field][normal] = original
            target[field] = normal
        result.append({"offset": row["offset"], "type": row["type"], "explicit_addend": row["explicit_addend"], "target": target})
    return result


def equal_section(a, aname, b, bname):
    return a.signature(aname) == b.signature(bname) and a.relocations.get(aname, []) == b.relocations.get(bname, [])


def sdk_bodies(elf):
    """Complete SDK leaves, masking only checked _ctype_+1 immediates."""
    syms = symbols(elf)
    ctype = syms["_ctype_"]
    assert ctype["bytes"] == 257
    section = elf.headers[ctype["section"]]
    start = ctype["address"] - section[3]
    table = elf.content(section)[start:start + 257]
    result = {"_ctype_": {"bytes": len(table), "sha256": hashlib.sha256(table).hexdigest(), "literal_hex": table.hex()}}
    for name, size, hi_offset, lo_offset, hi_opcode, lo_opcode in (
            ("strcasecmp", 144, 0, 8, 0x3C070000, 0x24E70000),
            ("strncasecmp", 168, 8, 16, 0x3C080000, 0x25080000)):
        symbol = syms[name]
        assert symbol["bytes"] == size
        header = elf.headers[symbol["section"]]
        start = symbol["address"] - header[3]
        data = elf.content(header)[start:start + size]
        high, low = (struct.unpack_from("<I", data, offset)[0] for offset in (hi_offset, lo_offset))
        assert high & 0xFFFF0000 == hi_opcode and low & 0xFFFF0000 == lo_opcode
        signed_low = struct.unpack("<h", struct.pack("<H", low & 0xFFFF))[0]
        address = ((high & 0xFFFF) * 65536 + signed_low) & 0xFFFFFFFF
        assert address == ctype["address"] + 1
        # These complete leaves have one address materialization, internal
        # relative branches and JR ra returns. They make no further calls.
        for offset in range(0, size, 4):
            word = struct.unpack_from("<I", data, offset)[0]
            assert word >> 26 not in (2, 3), "unexpected SDK absolute transfer"
            if word >> 26 == 15:
                assert offset == hi_offset, "unexpected SDK address materialization"
            if word >> 26 == 0 and word & 63 in (8, 9):
                assert word == 0x03E00008, "unexpected SDK indirect transfer"
        masked = bytearray(data)
        for offset in (hi_offset, lo_offset):
            word = struct.unpack_from("<I", masked, offset)[0]
            struct.pack_into("<I", masked, offset, word & 0xFFFF0000)
        result[name] = {"bytes": size, "complete_masked_body_sha256": hashlib.sha256(masked).hexdigest(),
                        "complete_masked_body_hex": masked.hex(),
                        "checked_literal": "_ctype_+1", "checked_literal_fields": [hi_offset, lo_offset],
                        "literal_address": address, "no_calls_or_other_address_materializations": True}
    return result


def equal_sdk(a, b):
    for name in ("_ctype_", "strcasecmp", "strncasecmp"):
        left, right = dict(a[name]), dict(b[name])
        left.pop("literal_address", None)
        right.pop("literal_address", None)
        assert left == right, "SDK body/data differs beyond exactly resolved _ctype_ address: " + name


def bind(obj, elf, exposed):
    osym, esym = symbols(obj), symbols(elf)
    names = {"CheckLumpInCache": "Engine_CheckLumpInCache" if exposed else "CheckLumpInCache",
             "lumpnumcache": "Engine_Cache" if exposed else "lumpnumcache",
             "lumpnumcacheindex": "Engine_Cursor" if exposed else "lumpnumcacheindex"}
    bodybytes = osym["CheckLumpInCache"]["bytes"]
    f = esym[names["CheckLumpInCache"]]
    assert f["bytes"] == bodybytes
    for name, expected in (("lumpnumcache", 2560), ("lumpnumcacheindex", 2)):
        assert osym[name]["bytes"] == esym[names[name]]["bytes"] == expected
    header = elf.headers[f["section"]]
    original = obj.content(obj.sections[HELPER])[:bodybytes]
    linked = elf.content(header)[f["address"] - header[3]:f["address"] - header[3] + bodybytes]
    masked_original, masked_linked = bytearray(original), bytearray(linked)
    input_gp = struct.unpack_from("<I", obj.content(obj.sections[".reginfo"]), 20)[0]
    resolved = []
    for row in obj.relocations[HELPER]:
        offset, kind, addend = row["offset"], row["type"], row["explicit_addend"]
        assert addend is not None and offset + 4 <= bodybytes and kind in (4, 5, 6, 7)
        target = row["target"]
        target_name = target["name"]
        if canonical(target["section"]) == ".bss.lumpnumcache":
            address = esym[names["lumpnumcache"]]["address"] + target["value"]
        elif canonical(target["section"]) == ".sbss.lumpnumcacheindex":
            address = esym[names["lumpnumcacheindex"]]["address"] + target["value"]
        else:
            assert not target["section"] and target_name in ("_ctype_", "strcasecmp", "strncasecmp")
            address = esym[target_name]["address"]
        word = struct.unpack_from("<I", linked, offset)[0]
        absolute = address + addend
        if kind == 4:
            assert word & 0x03FFFFFF == (absolute >> 2) & 0x03FFFFFF
            mask = 0xFC000000
        else:
            value = (absolute + 0x8000) >> 16 if kind == 5 else absolute
            if kind == 7:
                value += input_gp - esym["_gp"]["address"]
            assert word & 0xFFFF == value & 0xFFFF, (offset, kind, hex(word), hex(value))
            mask = 0xFFFF0000
        for data in (masked_original, masked_linked):
            unpatched = struct.unpack_from("<I", data, offset)[0]
            struct.pack_into("<I", data, offset, unpatched & mask)
        resolved.append({"offset": offset, "type": kind, "target": target_name,
                         "target_address": address, "explicit_addend": addend, "resolved_instruction": word})
    assert masked_original == masked_linked
    assert frame(original) == frame(linked)
    external_contents = {}
    for name in ("_ctype_", "strcasecmp", "strncasecmp"):
        symbol = esym[name]
        section = elf.headers[symbol["section"]]
        data = elf.content(section)[symbol["address"] - section[3]:][:symbol["bytes"]]
        external_contents[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    return {"symbol_bytes": bodybytes, "section_bytes": obj.sections[HELPER][5],
            "frame_bytes": frame(original), "cache_bytes": 2560, "cursor_bytes": 2,
            "linked_body_sha256": hashlib.sha256(linked).hexdigest(),
            "every_nonrelocated_instruction_bit_equal": True, "every_relocation_patch_verified": True,
            "resolved_relocations": resolved, "linked_external_contents": external_contents}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--object", type=Path, required=True)
    ap.add_argument("--elf", type=Path, required=True)
    ap.add_argument("--exposed", action="store_true")
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--measured", type=Path)
    ap.add_argument("--measured-elf", type=Path,
                    help="timed ELF whose complete SDK leaves/data must match modulo checked addresses")
    ap.add_argument("--optout", action="store_true")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    obj, elf = Object(a.object), Object(a.elf)
    result = {"object": str(a.object.resolve()), "object_sha256": sha(a.object),
              "elf": str(a.elf.resolve()), "elf_sha256": sha(a.elf), "binding": bind(obj, elf, a.exposed),
              "complete_sdk_bodies": sdk_bodies(elf),
              "checker_sha256": sha(Path(__file__))}
    if a.baseline:
        baseline = Object(a.baseline)
        before, after = section_map(baseline), section_map(obj)
        assert set(before) == set(after), "allocated section set changed"
        changed = [name for name in before if not equal_section(baseline, before[name], obj, after[name])]
        assert changed == ([] if a.optout else [HELPER]), changed
        result["isolation"] = {"baseline_sha256": sha(a.baseline), "unchanged_complete_allocated_sections": len(before) - len(changed),
                               "changed_sections": changed, "all_ordered_relocations_and_explicit_addends_compared": True,
                               "baseline_helper_section_bytes": baseline.sections[HELPER][5],
                               "baseline_helper_frame_bytes": frame(baseline.content(baseline.sections[HELPER]))}
    if a.measured:
        measured = Object(a.measured)
        ma, ca = section_map(measured), section_map(obj)
        selected = [HELPER, ".bss.lumpnumcache", ".sbss.lumpnumcacheindex"]
        for name in selected:
            assert equal_section(measured, ma[name], obj, ca[name]), "measured helper/cache differs: " + name
        result["measured_to_final"] = {"measured_sha256": sha(a.measured), "complete_identical_sections": selected,
                                       "all_ordered_relocations_and_explicit_addends_compared": True}
    if a.measured_elf:
        measured_sdk = sdk_bodies(Object(a.measured_elf))
        equal_sdk(measured_sdk, result["complete_sdk_bodies"])
        result["sdk_measured_to_final"] = {"measured_elf_sha256": sha(a.measured_elf),
                                           "complete_body_and_table_equivalence": True,
                                           "only_masked_bits_are_individually_resolved_ctype_address_fields": True,
                                           "measured_sdk": measured_sdk}
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"binding": {k: result["binding"][k] for k in ("symbol_bytes", "section_bytes", "frame_bytes", "cache_bytes")},
                      "isolation": result.get("isolation"), "measured_to_final": result.get("measured_to_final")}, indent=2))


if __name__ == "__main__":
    main()
