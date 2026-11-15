#!/usr/bin/env python3
"""Read-only actual full-LTO pack storage/frame and optional opt-out audit.

This does not time code or claim CPU improvement. It checks complete owned
sections/ordered relocations and referenced shared-string ranges, plus linked storage,
function frames and every non-relocated instruction bit of the new helper.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from audio_storage_next7_finalbind import Object

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def canonical(name):
    return re.sub(r"\.lto_priv\.\d+", "", name)


def symbols(obj):
    result = {}
    for table in obj.headers:
        if table[1] != 2:
            continue
        names = obj.content(obj.headers[table[6]])
        for offset in range(table[4], table[4] + table[5], table[9]):
            item = struct.unpack_from("<IIIBBH", obj.data, offset)
            name = obj.string(names, item[0])
            if name:
                result[name] = {"address": item[1], "bytes": item[2], "section": item[5], "info": item[3]}
    return result


def frame(data):
    sizes = []
    for word, in struct.iter_unpack("<I", data[:len(data) & ~3]):
        if word & 0xFFFF0000 in (0x27BD0000, 0x67BD0000):
            immediate = struct.unpack("<h", struct.pack("<H", word & 65535))[0]
            if immediate < 0:
                sizes.append(-immediate)
    return max(sizes, default=0)


def function_names():
    text = (ROOT / "src/w_pack.c").read_text()
    return {prefix.split()[-1].lstrip("*") for prefix in
            re.findall(r"^([^\n;{}]+)\([^\n;{}]*\)\n\{", text, re.M)} | {"PeekCheckedMagic"}


def selected(obj):
    names = function_names()
    roots = {name for name, header in obj.sections.items() if header[2] & 4
             and canonical(name).removeprefix(".text.").split(".")[0] in names}
    roots.update(name for name, records in obj.relocations.items()
                 if any(canonical(row["target"]["section"]) == ".bss.pkst" for row in records))
    included = set(roots)
    pending = list(roots)
    while pending:
        for relocation in obj.relocations.get(pending.pop(), []):
            name = relocation["target"]["section"]
            if name in obj.sections and obj.sections[name][2] & 2 and not obj.sections[name][2] & 4 and name not in included:
                included.add(name)
                pending.append(name)
    return roots, included


def graph(obj):
    roots, included = selected(obj)
    result = {}
    target_names, target_sections = {}, {}
    for name in included:
        assert canonical(name) not in result, "colliding normalized section names: " + name
        relocations = []
        for row in obj.relocations.get(name, []):
            target = dict(row["target"])
            for field, seen in (("name", target_names), ("section", target_sections)):
                original = target[field]
                if original:
                    normal = canonical(original)
                    assert normal not in seen or seen[normal] == original, "colliding normalized relocation " + field
                    seen[normal] = original
            target["name"] = canonical(target["name"])
            target["section"] = canonical(target["section"])
            relocations.append({"offset": row["offset"], "type": row["type"],
                                "explicit_addend": row["explicit_addend"], "target": target})
        signature = obj.signature(name)
        if name == ".rodata.str1.8":
            # This is the whole engine's merged string pool, including revision
            # and build metadata from other objects. Compare every literal
            # range that this closure addresses, rather than unrelated strings.
            content = obj.content(obj.sections[name])
            references = [(included_name, row) for included_name in included for row in obj.relocations.get(included_name, [])
                          if row["target"]["section"] == name]
            assert len(references) == 4
            assert {(canonical(referring), row["offset"], row["type"]) for referring, row in references} == {
                (".data.policies", offset, 2) for offset in (0, 24, 48, 72)}
            starts = {row["target"]["value"] + row["explicit_addend"] for _, row in references}
            ranges = []
            for start in sorted(starts):
                assert 0 <= start < len(content)
                end = content.index(0, start) + 1
                data = content[start:end]
                ranges.append({"offset": start, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
            assert ranges
            assert {content[start:content.index(0, start) + 1] for start in starts} == {b"dvd\0", b"usb\0", b"sd\0", b"hdd\0"}
            source = (ROOT / "src/w_pack.c").read_text()
            assert len(re.findall(r"policies\[[^]]+\]\.name", source)) == 1
            assert "strcasecmp(name, policies[i].name)" in source
            signature.pop("sha256")
            signature.pop("bytes")
            signature["referenced_literal_ranges"] = ranges
            signature["extent_audit"] = "Four media-policy name pointers, consumed only by strcasecmp; no binary or embedded-NUL span"
        result[canonical(name)] = {"signature": signature, "relocations": relocations}
    return result


def inspect(engine, binary):
    obj, elf = Object(engine), Object(binary)
    osyms, esyms = symbols(obj), symbols(elf)
    build_report = json.loads((binary.parent / "build-report.json").read_text())
    wrapped = {word.removeprefix("-Wl,--wrap=") for word in build_report["link_command"] if word.startswith("-Wl,--wrap=")}
    roots, included = selected(obj)
    pkst_names = [name for name in obj.sections if canonical(name) == ".bss.pkst"]
    assert len(pkst_names) == 1
    pkst = obj.sections[pkst_names[0]][5]
    linked_pkst = [row for name, row in esyms.items() if canonical(name) == "pkst"]
    assert len(linked_pkst) == 1 and linked_pkst[0]["bytes"] == pkst
    report = {"engine": str(engine), "engine_sha256": sha(engine), "elf": str(binary), "elf_sha256": sha(binary),
              "pkst_bss_bytes": pkst, "linked_pkst_bytes": linked_pkst[0]["bytes"],
              "function_sections": len(roots), "data_sections": len(included - roots), "frames": {},
              "text_section_bytes": sum(obj.sections[name][5] for name in roots)}
    for name in sorted(roots):
        report["frames"][canonical(name)] = frame(obj.content(obj.sections[name]))
    probe = obj.content(obj.sections[".text.PeekMagic"])
    probe_transfers = [row for row in obj.relocations[".text.PeekMagic"] if row["type"] == 4]
    is_late = lambda row: canonical(row["target"]["section"]) == ".text.PeekCheckedMagic"
    report["ordinary_probe_targets"] = [canonical(row["target"]["name"]) for row in probe_transfers if not is_late(row)]
    if ".text.PeekCheckedMagic" in obj.sections:
        transfers = [row for row in probe_transfers if is_late(row)]
        assert len(transfers) == 1
        offset = transfers[0]["offset"]
        jump, delay = struct.unpack_from("<II", probe, offset)
        assert jump >> 26 == 2, "late probe must be a tail jump"
        assert delay == 0x27BD0000 | frame(probe), "late probe must release the caller frame"
        report["late_probe_tail_transfer"] = {"tail_jump": True, "caller_frame_released_in_delay_slot": frame(probe)}
    if ".text.PeekCheckedMagic" in obj.sections:
        name = "PeekCheckedMagic"
        assert name in osyms and name in esyms
        body_size = osyms[name]["bytes"]
        assert esyms[name]["bytes"] == body_size
        header = elf.headers[esyms[name]["section"]]
        start = esyms[name]["address"] - header[3]
        actual = elf.content(header)[start:start + body_size]
        original = obj.content(obj.sections[".text." + name])[:body_size]
        masked_old, masked_new = bytearray(original), bytearray(actual)
        for relocation in obj.relocations.get(".text." + name, []):
            offset, kind = relocation["offset"], relocation["type"]
            assert offset + 4 <= body_size
            assert kind in (4, 5, 6, 7), "unexpected new-helper relocation"
            mask = 0xFC000000 if kind == 4 else 0xFFFF0000
            for data in (masked_old, masked_new):
                word = struct.unpack_from("<I", data, offset)[0]
                struct.pack_into("<I", data, offset, word & mask)
            if kind == 4:
                target = relocation["target"]["name"]
                if target in wrapped and not relocation["target"]["section"]:
                    target = "__wrap_" + target
                assert target in esyms
                word = struct.unpack_from("<I", actual, offset)[0]
                absolute = esyms[target]["address"] + relocation["explicit_addend"]
                assert word & 0x03FFFFFF == (absolute >> 2) & 0x03FFFFFF
        assert masked_old == masked_new, "linked helper changes non-relocation instruction bits"
        assert frame(actual) == frame(original)
        report["linked_late_probe"] = {"symbol_bytes": body_size, "stack_frame_bytes": frame(actual),
                                       "linked_code_sha256": hashlib.sha256(actual).hexdigest(),
                                       "all_non_relocated_instruction_bits_match": True,
                                       "all_call_relocations_resolve_to_original_targets": True}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-engine", type=Path, required=True)
    parser.add_argument("--current-engine", type=Path, required=True)
    parser.add_argument("--baseline-elf", type=Path, required=True)
    parser.add_argument("--current-elf", type=Path, required=True)
    parser.add_argument("--optout-engine", type=Path)
    parser.add_argument("--expect-restored", action="store_true", help="require unchanged preceding pack code/storage after candidate rejection")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = {"baseline": inspect(args.baseline_engine, args.baseline_elf),
              "current": inspect(args.current_engine, args.current_elf)}
    if args.expect_restored:
        assert report["baseline"]["pkst_bss_bytes"] == report["current"]["pkst_bss_bytes"] == 2952
        assert "linked_late_probe" not in report["current"]
        before, restored = graph(Object(args.baseline_engine)), graph(Object(args.current_engine))
        assert before == restored, "restored production pack graph differs"
        report["restored_production"] = {"complete_code_data_sections": len(before) - 1,
                                         "shared_policy_string_ranges": 4, "ordered_relocations_exact": True,
                                         "whole_engine_equality_claimed": False}
    else:
        assert report["baseline"]["pkst_bss_bytes"] - report["current"]["pkst_bss_bytes"] == 768
    assert report["baseline"]["ordinary_probe_targets"] == report["current"]["ordinary_probe_targets"]
    report["existing_frame_changes"] = {name: [size, report["current"]["frames"].get(name)]
                                        for name, size in report["baseline"]["frames"].items()
                                        if size != report["current"]["frames"].get(name)}
    if args.optout_engine:
        before, optout = graph(Object(args.baseline_engine)), graph(Object(args.optout_engine))
        assert before == optout, "complete full-LTO opt-out graph differs"
        shared = before.get(".rodata.str1.8", {}).get("signature", {}).get("referenced_literal_ranges", [])
        report["optout"] = {"engine": str(args.optout_engine), "sha256": sha(args.optout_engine),
                            "complete_code_data_sections": len(before) - bool(shared), "ordered_relocations_exact": True,
                            "complete_owned_section_contents_exact": True,
                            "shared_engine_string_pool_referenced_ranges_exact": len(shared),
                            "unrelated_shared_string_pool_bytes_compared": False,
                            "whole_engine_equality_claimed": False}
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
