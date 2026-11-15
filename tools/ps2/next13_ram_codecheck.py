#!/usr/bin/env python3
"""Read-only binding of all changed drawnode-context bodies and exact callers."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct

from audio_storage_next7_finalbind import Object
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import frame, output_bytes, relocations, symbol_tables
from next12_pb_scratch_objectcheck import allocated, pdr
import next12_pb_scratch_codecheck as shared
from next13_ram_variant import ROOT, BASE, OUT, MAIN


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def controls(out, original):
    out.mkdir()
    primary = "HWR_CreateDrawNodes.lto_priv.0"
    names = [primary, "CompareDrawNodes", "CompareDrawNodePlanes"]
    rows = []

    def offset(address, size=1):
        matches = [s for s in original.elf.sections if s[1] != 8 and s[2] & 2
                   and s[3] <= address and address + size <= s[3] + s[5]]
        if len(matches) != 1:
            raise RuntimeError("One linked negative span required")
        return matches[0][4] + address - matches[0][3]

    def reject(label, raw, object_mutation, callback):
        path = out / (label + (".o" if object_mutation else ".ELF"))
        path.write_bytes(raw)
        current = copy.copy(original)
        current.obj = Elf32(path) if object_mutation else original.obj
        current.elf = original.elf if object_mutation else Elf32(path)
        current.symbols = linked_symbols(current.elf)
        current.data, current.literals, current.literal_uses = {}, {}, []
        # The actual changed file is admitted, so rejection must occur inside
        # instruction/RELA/data/extent/frame reconstruction, not at an old pin.
        admissions = {"engine": hashlib.sha256(current.obj.data).hexdigest(),
                      "ELF": hashlib.sha256(current.elf.data).hexdigest()}
        try:
            callback(current)
        except (SystemExit, RuntimeError) as error:
            reason = str(error)
            if any(x in reason.lower() for x in ("checksum", "hash pin", "build/elf pin", "sha256")):
                raise RuntimeError("Only an outer pin rejected " + label)
            rows.append({"mutation": label, "path": str(path), "sha256": sha(path),
                         "refreshed_exact_admissions": admissions, "inner_binding_rejected": True, "reason": reason})
        else:
            raise RuntimeError("Actual binary mutation escaped " + label)

    for name in names:
        section = original.obj.sections[original.obj.index[".text." + name]]
        raw = bytearray(original.obj.data)
        struct.pack_into("<I", raw, section[4], struct.unpack_from("<I", raw, section[4])[0] ^ 0x10000)
        reject("object-register-" + name, raw, True, lambda value, n=name: value.text(n))
    raw = bytearray(original.elf.data)
    at = offset(original.maps[".text." + primary][0], 4)
    struct.pack_into("<I", raw, at, struct.unpack_from("<I", raw, at)[0] ^ 0x10000)
    reject("linked-register", raw, False, lambda value: value.text(primary))
    call = next(r for r in relocations(original.obj, primary) if r["kind"] == 4)
    raw = bytearray(original.obj.data)
    struct.pack_into("<i", raw, call["record_file_offset"] + 8, call["addend"] + 4)
    reject("call-RELA-addend", raw, True, lambda value: value.text(primary))
    raw = bytearray(original.obj.data)
    table = symbol_tables(original.obj)[call["symbol_table"]]
    replacement = next(i for i, s in enumerate(table) if s["name"] == "Z_Free")
    at = call["record_file_offset"] + 4
    old = struct.unpack_from("<I", raw, at)[0]
    struct.pack_into("<I", raw, at, (replacement << 8) | (old & 255))
    reject("call-RELA-target", raw, True, lambda value: value.text(primary))
    section = original.obj.sections[original.obj.index[".pdr"]]
    sites = [r["offset"] for r in shared.source_relas(original.obj, original.obj.index[".pdr"])
             if r["symbol"]["name"] in (primary, ".text." + primary)]
    if len(sites) != 1:
        raise RuntimeError("One actual primary PDR required")
    raw = bytearray(original.obj.data)
    at = section[4] + sites[0] + 20
    struct.pack_into("<I", raw, at, struct.unpack_from("<I", raw, at)[0] ^ 16)
    reject("frame-size", raw, True, lambda value: value.text(primary))
    raw = bytearray(original.obj.data)
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", raw)
    index = original.obj.index[".bss.dn_source"]
    at = header[6] + index * header[11] + 20
    struct.pack_into("<I", raw, at, struct.unpack_from("<I", raw, at)[0] - 4)
    reject("snapshot-capacity", raw, True, lambda value: value.text(primary))
    primary_relocations = relocations(original.obj, "CompareDrawNodes")
    string = next(r for r in primary_relocations if r["symbol"]["section"]
                  and original.obj.sections[r["symbol"]["section"]][2] & 0x30 == 0x30)
    address = original.maps[".text.CompareDrawNodes"][0] + string["offset"]
    anchors = original.linker_relas[(address, string["kind"])]
    if len(anchors) != 1:
        raise RuntimeError("One exact final comparator literal anchor required")
    raw = bytearray(original.elf.data)
    raw[offset(anchors[0]["resolved_address"])] ^= 1
    reject("exact-comparator-literal", raw, False, lambda value: value.text("CompareDrawNodes"))
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path, default=ROOT / "build/nov15-next13/ram-context-v1")
    parser.add_argument("--control", type=Path, default=ROOT / "build/nov15-next13/ram-control-v1")
    parser.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next12/final/hardware")
    parser.add_argument("--source", type=Path, default=OUT)
    parser.add_argument("--candidate-relas", type=Path, default=ROOT / "build/nov15-next13/ram-linker-relas-v1")
    parser.add_argument("--baseline-relas", type=Path, default=ROOT / "build/nov15-next12/final/pb-binding/final-linker-relas")
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov15-next13/ram-binding-v1")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "tools").mkdir()
    tools = (Path(__file__), ROOT / "tools/ps2/next13_ram_context.py", ROOT / "tools/ps2/next13_ram_variant.py",
             ROOT / "tools/ps2/next12_pb_scratch_codecheck.py", ROOT / "tools/ps2/next12_pb_scratch_objectcheck.py",
             ROOT / "tools/ps2/next11_sw_checkplane_codecheck.py", ROOT / "tools/ps2/next10_sw_bounds_codecheck.py",
             ROOT / "tools/ps2/next7_render_fog_codecheck.py", ROOT / "tools/ps2/audio_storage_next7_finalbind.py")
    for path in tools:
        shutil.copyfile(path, out / "tools" / path.name)
    source_manifest = json.loads((args.source / "source-manifest.json").read_text())
    for label, build in (("candidate", args.candidate), ("control", args.control)):
        context = json.loads((build / "context-report.json").read_text())
        if context["narrow_optout"] != (label == "control") or context["only_recompiled_unit"] != MAIN:
            raise RuntimeError("Actual context source/opt-out scope differs")
        actual = {str(p.relative_to(build / "snapshot")): sha(p) for p in (build / "snapshot/src").rglob("*") if p.is_file()}
        if actual != source_manifest["candidate_source_sha256"]:
            raise RuntimeError("Complete actual602 compile source differs")
        if len(context["complete_actual_IR_objects"]) != 205 or context["changed_actual_IR_objects"] != ["src__hardware__hw_main.o"]:
            raise RuntimeError("Actual205 input graph differs")
        for path, digest in context["complete_actual_IR_objects"].items():
            if sha(Path(path)) != digest:
                raise RuntimeError("An actual input IR changed")
    builds = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve(), "control": args.control.resolve()}
    objects = {label: Object(build / "obj/engine_lto.o") for label, build in builds.items()}
    signatures = {label: allocated(obj) for label, obj in objects.items()}
    frames = {label: pdr(obj) for label, obj in objects.items()}
    restored = {"complete_allocated_sections_ordered_RELAs": signatures["baseline"] == signatures["control"],
                "complete_PDRs_ordered_RELAs": frames["baseline"] == frames["control"],
                "complete_ELF": sha(args.baseline / "SRB2.ELF") == sha(args.control / "SRB2.ELF")}
    if not all(restored.values()):
        raise RuntimeError("Narrow opt-out fails exact original restoration")
    names = set(signatures["baseline"]) | set(signatures["candidate"])
    changed = sorted(n for n in names if signatures["baseline"].get(n) != signatures["candidate"].get(n))
    code = [n for n in changed if n.startswith(".text.")]
    callers = set()
    for obj in (objects["baseline"], objects["candidate"]):
        for owner, relas in obj.relocations.items():
            if owner.startswith(".text.") and any(r["target"]["section"] in code for r in relas):
                callers.add(owner)
    chosen = set(code) | callers
    # Include the actual allocation/reclaim and the sink roots even when their
    # input sections are otherwise unchanged. Each boundary remains explicit.
    prefixes = (".text.__wrap", ".text.Spill_", ".text.HWR_RenderWall", ".text.HWR_GetLevelFlat",
                ".text.HWR_Lighting", ".text.HWR_RenderPlane", ".text.HWR_RenderPolyObjectPlane")
    chosen |= {n for n in names if n.startswith(prefixes)}
    report = {"status": "pending", "scope": "Actual default205-input Hardware LTO contexts; complete changed-body/binding and direct-caller union plus real allocator/render sinks. No emulator or performance claim.",
              "source_manifest_sha256": sha(args.source / "source-manifest.json"), "source_root": str(args.source.resolve()), "optout_exact_restoration": restored,
              "changed_allocated_sections": changed, "changed_code_or_binding_sections": code,
              "direct_callers": sorted(callers), "selected_sections": sorted(chosen),
              "complete_input_PDRs": frames, "variants": {},
              "tools": {str(p.relative_to(ROOT)): sha(p) for p in tools}}
    values = {}
    for label, build, linker in (("baseline", args.baseline, args.baseline_relas),
                                  ("candidate", args.candidate, args.candidate_relas)):
        value = shared.binder(build.resolve(), linker.resolve())
        values[label] = value
        checked, discarded = {}, []
        for section in sorted(chosen):
            if section not in value.obj.index:
                discarded.append({"section": section, "input_body_present": False,
                    "reason": "This actual LTO context has no body at this spelling; the complete union also binds its emitted ordinary/private variants. No body is fabricated."})
                continue
            name = section[6:]
            if section not in value.maps or not value.maps[section][0]:
                discarded.append({"section": section, "complete_input_signature": objects[label].signature(section),
                                  "complete_ordered_RELAs": objects[label].relocations.get(section, []),
                                  "complete_PDR": frames[label]["records"].get(section),
                                  "reason": "Actual linker-discarded input body; no runtime byte claim"})
                continue
            checked[name] = value.text(name)
        report["variants"][label] = {"complete_bound_text": checked, "discarded_input_bodies": discarded,
            "complete_referenced_data": value.data, "complete_literals": list(value.literals.values()),
            "exact_literal_RELA_uses": value.literal_uses, "actual_boundaries": shared.boundary_records(value, checked),
            "ELF_sha256": sha(build / "SRB2.ELF"), "engine_sha256": sha(build / "obj/engine_lto.o"),
            "retained_linker_RELA_report_sha256": sha(linker / "linker-rela-report.json")}
    report["binary_negative_controls"] = controls(out / "binary-negatives", values["candidate"])
    report["raw_input_body_changes"] = [n for n in code if n in objects["baseline"].sections and n in objects["candidate"].sections
                                        and objects["baseline"].signature(n) != objects["candidate"].signature(n)]
    report["input_function_dispositions"] = {label: [n for n in code if n not in obj.sections] for label, obj in objects.items()}
    report["status"] = "pass actual whole-LTO/ELF text/data/RELA/literal/frame/capacity binding"
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "changed_code_or_binding_sections": len(code),
        "selected_sections": len(chosen), "bound_bodies": {k: len(v["complete_bound_text"]) for k, v in report["variants"].items()},
        "binary_controls": len(report["binary_negative_controls"]), "raw_body_changes": report["raw_input_body_changes"]}, indent=2))


if __name__ == "__main__":
    main()
