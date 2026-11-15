#!/usr/bin/env python3
"""Read-only complete pack graph, ordered RELA, final ELF and stack audit.

No compiler or emulator. Every live owned code/nonstring data section is bound
in full to its linked bytes, with every relocation field individually checked.
Merged strings are checked at their resolved NUL-inclusive accessed ranges.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from audio_storage_next7_finalbind import Object
from next9_io_codecheck import canonical, frame, symbols
from next9_io_variant import BASE_SHA, CANDIDATE_SHA

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def source_functions(source):
    text = source.read_text()
    result = {}
    for match in re.finditer(r"^([^\n;{}]+)\([^\n;{}]*\)\n\{", text, re.M):
        name = match[1].split()[-1].lstrip("*")
        end = text.index("\n}\n", match.end()) + 3
        assert name not in result
        result[name] = text[match.start():end]
    return result


def selected(obj, source):
    names = set(source_functions(source)) | {"PeekCheckedMagic"}
    roots = {name for name, section in obj.sections.items() if section[2] & 4
             and canonical(name).removeprefix(".text.").split(".")[0] in names}
    # LTO can inline a stream access into a direct consumer outside w_pack.c.
    roots.update(name for name, rows in obj.relocations.items()
                 if any(canonical(row["target"]["section"]) == ".bss.pkst" for row in rows))
    included, pending = set(roots), list(roots)
    while pending:
        for row in obj.relocations.get(pending.pop(), []):
            target = row["target"]["section"]
            if target in obj.sections and obj.sections[target][2] & 2 and not obj.sections[target][2] & 4 and target not in included:
                included.add(target)
                pending.append(target)
    return roots, included


def allocated_sections(obj):
    result = {}
    for name, section in obj.sections.items():
        if section[2] & 2:
            assert name not in result
            result[name] = name
    return result


def ordered_relocations(obj, names, normalize=True):
    result, seen = {}, {"name": {}, "section": {}}
    for name in sorted(names):
        records = []
        for row in obj.relocations.get(name, []):
            target = dict(row["target"])
            for field in seen:
                original = target[field]
                normal = canonical(original) if normalize else original
                assert normal not in seen[field] or seen[field][normal] == original, "colliding canonical target: " + original
                seen[field][normal] = original
                target[field] = normal
            assert row["explicit_addend"] is not None, "implicit addend is not audited"
            records.append({"offset": row["offset"], "type": row["type"],
                            "explicit_addend": row["explicit_addend"], "target": target})
        result[canonical(name) if normalize else name] = records
    return result


def graph(obj, source):
    roots, included = selected(obj, source)
    rows = ordered_relocations(obj, included)
    result = {canonical(name): {"signature": obj.signature(name), "ordered_relocations": rows[canonical(name)]}
              for name in sorted(included)}
    assert len(result) == len(included)
    return roots, included, result


def map_sections(path, owner):
    text = path.read_text().split("Linker script and memory map\n", 1)
    assert len(text) == 2
    result, pending = {}, None
    for line in text[1].splitlines():
        one = re.match(r"^ (\.[^\s]+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(.+?)\s*$", line)
        start = re.match(r"^ (\.[^\s]+)\s*$", line)
        more = re.match(r"^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(.+?)\s*$", line)
        if one:
            name, address, size, file = one.groups()
            pending = None
        elif start:
            pending = start[1]
            continue
        elif more and pending:
            name, (address, size, file) = pending, more.groups()
            pending = None
        else:
            continue
        if file != owner:
            continue
        assert name not in result, "duplicate live input section: " + name
        result[name] = {"address": int(address, 16), "bytes": int(size, 16)}
    assert result
    return result


def elf_bytes(elf, address, size):
    choices = [section for section in elf.headers if section[2] & 2 and section[1] != 8
               and section[3] <= address and address + size <= section[3] + section[5]]
    assert len(choices) == 1, (hex(address), size, len(choices))
    section = choices[0]
    start = address - section[3]
    return elf.content(section)[start:start + size]


def linked_graph(obj, elf, binary, included):
    report_path = binary.parent / "build-report.json"
    build = json.loads(report_path.read_text())
    assert sha(binary) == build["elf_sha256"]
    owners = [word for word in build["link_command"] if word.endswith("/engine_lto.o")]
    assert len(owners) == 1
    layout = map_sections(binary.parent / "SRB2.map", owners[0])
    wrapped = {word.removeprefix("-Wl,--wrap=") for word in build["link_command"] if word.startswith("-Wl,--wrap=")}
    osym, esym = symbols(obj), symbols(elf)
    input_gp = struct.unpack_from("<I", obj.content(obj.sections[".reginfo"]), 20)[0]
    result, strings, dead = {}, {}, []
    for name in sorted(included):
        section = obj.sections[name]
        if name not in layout:
            dead.append(name)
            continue
        merged_string = section[2] & 0x30 == 0x30
        if merged_string:
            # GNU ld folds these input sections. Every addressed range is
            # instead checked through its individually resolved relocations.
            continue
        place = layout[name]
        assert place["bytes"] == section[5], (name, place, section[5])
        anchors = []
        index = obj.names.index(name)
        for symbol, row in osym.items():
            if row["section"] != index or row["info"] & 15 not in (1, 2) or symbol not in esym:
                continue
            assert esym[symbol]["address"] == place["address"] + row["address"], symbol
            assert esym[symbol]["bytes"] == row["bytes"], symbol
            anchors.append(symbol)
        if section[1] == 8:
            result[name] = {**place, "nobits": True, "checked_named_anchors": anchors}
            continue
        original = obj.content(section)
        linked = elf_bytes(elf, place["address"], section[5]) if section[5] else b""
        a, b = bytearray(original), bytearray(linked)
        patches = []
        string_groups = {}
        for row in obj.relocations.get(name, []):
            target = row["target"]
            target_section = target["section"]
            if target_section in obj.sections and obj.sections[target_section][2] & 0x30 == 0x30:
                key = (target_section, target["value"], row["explicit_addend"])
                string_groups.setdefault(key, []).append(row)
        resolved_strings = {}
        for key, rows in string_groups.items():
            target_section, value, addend = key
            assert addend is not None
            start = value + addend
            if any(row["type"] == 7 for row in rows):
                assert all(row["type"] == 7 for row in rows)
                if rows[0]["target"]["info"] >> 4 == 0:
                    start += input_gp
            raw = obj.content(obj.sections[target_section])
            assert 0 <= start < len(raw)
            literal = raw[start:raw.index(0, start) + 1]
            high = {struct.unpack_from("<I", linked, row["offset"])[0] & 65535 for row in rows if row["type"] == 5}
            low = {struct.unpack_from("<h", linked, row["offset"])[0] for row in rows if row["type"] == 6}
            addresses = {struct.unpack_from("<I", linked, row["offset"])[0] for row in rows if row["type"] == 2}
            gp_addresses = {(struct.unpack_from("<h", linked, row["offset"])[0] + esym["_gp"]["address"]) & 0xFFFFFFFF
                            for row in rows if row["type"] == 7}
            if high or low:
                assert len(high) == len(low) == 1, (name, key, high, low)
                addresses.add(((next(iter(high)) << 16) + next(iter(low))) & 0xFFFFFFFF)
            addresses.update(gp_addresses)
            assert len(addresses) == 1, (name, key, addresses)
            absolute = next(iter(addresses))
            assert elf_bytes(elf, absolute, len(literal)) == literal, (name, target_section, start)
            resolved_strings[key] = absolute
            strings.setdefault(target_section, {})[str(start)] = {"bytes": len(literal),
                "sha256": digest(literal), "linked_address": absolute,
                "extent": "NUL-inclusive compiler string literal; no binary span is assumed"}
        for row in obj.relocations.get(name, []):
            offset, kind, addend = row["offset"], row["type"], row["explicit_addend"]
            assert addend is not None and offset + 4 <= len(original) and kind in (2, 4, 5, 6, 7), (name, row)
            target = row["target"]
            key = (target["section"], target["value"], addend)
            if key in resolved_strings:
                absolute = resolved_strings[key]
                resolution = "merged literal range"
            elif target["section"] in layout:
                absolute = layout[target["section"]]["address"] + target["value"] + addend
                if kind == 7 and target["info"] >> 4 == 0:
                    absolute += input_gp
                resolution = "exact input section and symbol offset"
            else:
                symbol = target["name"]
                assert not target["section"], (name, row)
                if symbol in wrapped:
                    symbol = "__wrap_" + symbol
                assert symbol in esym, (name, symbol)
                absolute = esym[symbol]["address"] + addend
                resolution = "exact linked external or wrapped symbol: " + symbol
            word = struct.unpack_from("<I", linked, offset)[0]
            if kind == 2:
                assert word == absolute & 0xFFFFFFFF, (name, row, hex(word), hex(absolute))
                mask = 0
            elif kind == 4:
                assert word & 0x03FFFFFF == (absolute >> 2) & 0x03FFFFFF, (name, row)
                assert (place["address"] + offset + 4) >> 28 == absolute >> 28
                mask = 0xFC000000
            else:
                value = (absolute + 0x8000) >> 16 if kind == 5 else absolute
                if kind == 7:
                    # GNU MIPS GPREL16 retains the input GP adjustment for
                    # local symbols; global symbols have no such adjustment.
                    # It was applied to the local target above, exactly once.
                    value -= esym["_gp"]["address"]
                assert word & 65535 == value & 65535, (name, row, hex(word), hex(value))
                mask = 0xFFFF0000
            for data in (a, b):
                unpatched = struct.unpack_from("<I", data, offset)[0]
                struct.pack_into("<I", data, offset, unpatched & mask)
            patches.append({"offset": offset, "type": kind, "target": target,
                            "explicit_addend": addend, "absolute_address": absolute, "resolution": resolution})
        assert a == b, "unrelocated linked section bits differ: " + name
        if section[2] & 4:
            assert frame(original) == frame(linked)
        result[name] = {**place, "nobits": False, "linked_sha256": digest(linked),
                        "nonrelocated_bits_exact": True, "checked_named_anchors": anchors,
                        "ordered_resolved_relocations": patches}
    # Every discarded selected function is explicitly outside final ELF code;
    # its complete section remains part of the input-object ownership graph.
    return {"sections": result, "merged_literal_ranges": strings, "discarded_sections": dead,
            "every_live_owned_nonstring_section_bound_in_full": True,
            "every_live_relocation_field_checked": True, "input_reginfo_gp": input_gp,
            "GPREL16_rule": "S + explicit A + (input GP for local symbols; zero for globals) - linked GP",
            "build_report_sha256": sha(report_path),
            "map_sha256": sha(binary.parent / "SRB2.map")}


def inspect(engine, binary, source):
    obj, elf = Object(engine), Object(binary)
    roots, included, owned = graph(obj, source)
    esym = symbols(elf)
    pkst = [name for name in included if canonical(name) == ".bss.pkst"]
    assert len(pkst) == 1
    linked_pkst = [row for name, row in esym.items() if canonical(name) == "pkst"]
    assert len(linked_pkst) == 1 and linked_pkst[0]["bytes"] == obj.sections[pkst[0]][5]
    frames = {canonical(name): frame(obj.content(obj.sections[name])) for name in sorted(roots)}
    probe = obj.content(obj.sections[".text.PeekMagic"])
    transfers = [row for row in obj.relocations[".text.PeekMagic"] if row["type"] == 4]
    late = [row for row in transfers if canonical(row["target"]["section"]) == ".text.PeekCheckedMagic"]
    report = {"engine": str(engine.resolve()), "engine_sha256": sha(engine),
              "elf": str(binary.resolve()), "elf_sha256": sha(binary), "source_sha256": sha(source),
              "owned_code_sections": len(roots), "owned_data_sections": len(included - roots),
              "complete_owned_input_graph": owned, "pkst_bss_bytes": obj.sections[pkst[0]][5],
              "linked_pkst_bytes": linked_pkst[0]["bytes"], "frames": frames,
              "selected_text_bytes": sum(obj.sections[name][5] for name in roots),
              "ordinary_probe_targets": [canonical(row["target"]["name"]) for row in transfers if row not in late],
              "linked_binding": linked_graph(obj, elf, binary, included)}
    if late:
        assert len(late) == 1
        offset = late[0]["offset"]
        jump, delay = struct.unpack_from("<II", probe, offset)
        caller, helper = frames[".text.PeekMagic"], frames[".text.PeekCheckedMagic"]
        assert jump >> 26 == 2 and delay == 0x27BD0000 | caller
        report["late_edge"] = {"tail_jump": True, "caller_frame_released_in_delay_slot": caller,
            "private_probe_bytes": 128, "helper_frame_bytes": helper,
            "local_call_depth_increase_bytes": helper - caller,
            "whole_thread_stack_peak_claimed": False}
    return obj, report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline-engine", type=Path, required=True)
    ap.add_argument("--baseline-elf", type=Path, required=True)
    ap.add_argument("--current-engine", type=Path)
    ap.add_argument("--current-elf", type=Path)
    ap.add_argument("--baseline-source", type=Path, default=ROOT / "build/nov15-next11/baseline-src/src/w_pack.c")
    ap.add_argument("--candidate-source", type=Path, default=ROOT / "build/nov15-next11/io/candidate-src/src/w_pack.c")
    ap.add_argument("--isolated", action="store_true", help="require every other complete allocated section and RELA unchanged")
    ap.add_argument("--optout", action="store_true")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    assert sha(a.baseline_source) == BASE_SHA and sha(a.candidate_source) == CANDIDATE_SHA
    before_functions, after_functions = source_functions(a.baseline_source), source_functions(a.candidate_source)
    assert set(after_functions) - set(before_functions) == {"PeekCheckedMagic"}
    changed_functions = [name for name in before_functions if before_functions[name] != after_functions[name]]
    assert changed_functions == ["PeekMagic"]
    base, baseline = inspect(a.baseline_engine, a.baseline_elf, a.baseline_source)
    result = {"source_audit": {"baseline_sha256": BASE_SHA, "candidate_sha256": CANDIDATE_SHA,
                "changed_existing_functions": changed_functions, "unchanged_ReadHeader_bytes": len(before_functions["ReadHeader"]),
                "added_function": "PeekCheckedMagic", "stream_slots": 6, "path_bytes_per_slot": 192,
                "complete_header_extension_and_reserved_fields_retained": True},
              "baseline": baseline, "checker_sha256": sha(Path(__file__))}
    if a.current_engine:
        assert a.current_elf
        current, candidate = inspect(a.current_engine, a.current_elf, a.candidate_source)
        result["current"] = candidate
        assert baseline["pkst_bss_bytes"] == 2952
        assert candidate["pkst_bss_bytes"] == (2952 if a.optout else 2184)
        assert baseline["ordinary_probe_targets"] == candidate["ordinary_probe_targets"]
        differences = {name: [size, candidate["frames"].get(name)] for name, size in baseline["frames"].items()
                       if size != candidate["frames"].get(name)}
        assert not differences, differences
        result["existing_frame_changes"] = differences
        result["selected_text_delta"] = candidate["selected_text_bytes"] - baseline["selected_text_bytes"]
        if a.isolated:
            old_map, new_map = allocated_sections(base), allocated_sections(current)
            old_rela = ordered_relocations(base, old_map.values(), normalize=False)
            new_rela = ordered_relocations(current, new_map.values(), normalize=False)
            changed = [name for name in sorted(set(old_map) | set(new_map))
                       if name not in old_map or name not in new_map
                       or base.signature(old_map[name]) != current.signature(new_map[name]) or old_rela[name] != new_rela[name]]
            allowed = set(baseline["complete_owned_input_graph"]) | set(candidate["complete_owned_input_graph"])
            unowned = {name for name in changed if canonical(name) not in allowed}
            assert not unowned, "unowned whole-engine difference: " + repr(unowned)
            if a.optout:
                assert not changed, changed
            result["whole_engine_isolation"] = {"baseline_allocated_sections": len(old_map),
                "current_allocated_sections": len(new_map), "changed_sections": changed,
                "unchanged_complete_allocated_sections": len(set(old_map) & set(new_map)) - len([n for n in changed if n in old_map and n in new_map]),
                "every_ordered_exact_symbol_RELA_and_explicit_addend_compared": True,
                "whole_engine_uses_exact_unnormalized_section_and_symbol_names": True,
                "owned_graph_canonical_name_collisions_rejected": True}
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"baseline_pkst_bytes": baseline["pkst_bss_bytes"],
        "current_pkst_bytes": result.get("current", {}).get("pkst_bss_bytes"),
        "baseline_bound_sections": len(baseline["linked_binding"]["sections"]),
        "current_bound_sections": len(result.get("current", {}).get("linked_binding", {}).get("sections", {})),
        "selected_text_delta": result.get("selected_text_delta"),
        "late_edge": result.get("current", {}).get("late_edge"),
        "isolation": result.get("whole_engine_isolation")}, indent=2))


if __name__ == "__main__":
    main()
