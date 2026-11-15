#!/usr/bin/env python3
"""Bind the tested scalar drawnode graph into the actual combined Hardware ELF.

Read-only: no compiler, linker, emulator, source or existing evidence writes.
The complete actual all-up graph is reconstructed, not borrowed from a component.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

from audio_storage_next7_finalbind import Object
from next12_pb_scratch_objectcheck import pdr
from next13_ram_codecheck import controls
import next12_pb_scratch_codecheck as shared
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import output_bytes
from next11_local_rng import verify_patch, stub_words

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "build/nov15-next13"
SCALAR = WORK / "ram-scalar-context-v1"
ROOTS = ("CompareDrawNodes", "CompareDrawNodePlanes", "HWR_CreateDrawNodes.lto_priv.0")
SOURCE_CHANGES = {"src/hardware/hw_cache.c", "src/hardware/hw_main.c", "src/hardware/hw_pbatch.inc", "src/ps2/hw/ps2_hw_tex.inc"}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=WORK / "combined-classic-v1")
    parser.add_argument("--relas", type=Path)
    parser.add_argument("--rng-folder", type=Path, default=WORK / "local-rng/combined-classic-v2")
    parser.add_argument("--sdk-report", type=Path, default=WORK / "review/ram-sdk-combined-classic-v1/report.json")
    parser.add_argument("--out", type=Path, default=WORK / "ram-combined-classic-binding-v1")
    args = parser.parse_args()
    build, out = args.build.resolve(), args.out.resolve()
    relas = args.relas.resolve() if args.relas else build / "linker-relas"
    context = json.loads((build / "context-report.json").read_text())
    component_path = WORK / "ram-scalar-binding-v1/report.json"
    component = json.loads(component_path.read_text())
    if set(context["project_source_differences"]) != SOURCE_CHANGES:
        raise RuntimeError("The selected complete four-source all-up scope differs")
    source_pins = {str(p.relative_to(build / "snapshot")): sha(p) for p in (build / "snapshot/src").rglob("*") if p.is_file()}
    if len(source_pins) != 602 or source_pins != context["complete_source_overlay_sha256"]:
        raise RuntimeError("Actual all602 compile-source snapshot differs")
    if sha(build / "snapshot/src/hardware/hw_main.c") != "25ab400933e147ae202648629b088999f7f2b3d84a70a839aeed471cffcdd3a6":
        raise RuntimeError("Actual scalar DN source is not the tested v2")
    if len(context["complete_actual_IR_objects"]) != 205 or set(context["changed_actual_IR_objects"]) != {
        "src__hardware__hw_main.o", "src__hardware__hw_cache.o", "src__hardware__hw_batching.o", "src__ps2__hw__ps2_hwd.o"}:
        raise RuntimeError("Complete205 default Hardware graph scope differs")
    for path, digest in context["complete_actual_IR_objects"].items():
        if sha(path) != digest:
            raise RuntimeError("Actual complete all-up IR changed: " + path)
    if not context["all_other201_complete_IR_inputs_identical"] or not context["whole_LTO_option_section_bytes_identical_all_four_units"]:
        raise RuntimeError("Actual canonical IR/options retention admission fails")
    main_IR = {"component_sha256": sha(SCALAR / "obj/src__hardware__hw_main.o"),
        "all_up_sha256": sha(build / "obj/src__hardware__hw_main.o"),
        "scope": "Both complete hashes retained. Source/dependency/options and actual complete DN body/PDR/RELA comparisons establish this admission; no whole-IR byte identity is inferred."}
    main_IR["complete_IR_bytes_equal"] = main_IR["component_sha256"] == main_IR["all_up_sha256"]
    if sha(build / "obj/engine_lto.o") != context["engine_sha256"] or sha(build / "SRB2.ELF") != context["ELF_sha256"]:
        raise RuntimeError("Actual all-up engine/ELF pin differs")
    out.mkdir(parents=True, exist_ok=False)
    tools = out / "pinned-tools"
    tools.mkdir()
    for path in (Path(__file__), ROOT / "tools/ps2/next13_ram_codecheck.py", ROOT / "tools/ps2/next12_pb_scratch_codecheck.py",
                 ROOT / "tools/ps2/next11_sw_checkplane_codecheck.py", ROOT / "tools/ps2/next10_sw_bounds_codecheck.py"):
        shutil.copyfile(path, tools / path.name)
    old, new = Object(SCALAR / "obj/engine_lto.o"), Object(build / "obj/engine_lto.o")
    old_frames, new_frames = pdr(old), pdr(new)
    roots = {}
    for name in ROOTS:
        section = ".text." + name
        if old.signature(section) != new.signature(section) or old_frames["records"][section] != new_frames["records"][section]:
            raise RuntimeError("Complete scalar root instructions/padding/PDR differ: " + name)
        a, b = old.relocations[section], new.relocations[section]
        if len(a) != len(b):
            raise RuntimeError("Full DN ordered RELA count differs")
        differences = []
        for i, (x, y) in enumerate(zip(a, b)):
            if x == y:
                continue
            # Retain both complete records. This classifies two exact target
            # size descriptors, never masks a relocation/instruction/address.
            before, after = dict(x), dict(y)
            before["target"], after["target"] = dict(x["target"]), dict(y["target"])
            before["target"].pop("size")
            after["target"].pop("size")
            if before != after or y["target"]["name"] != "HWR_GetLevelFlat" or y["offset"] not in (1332, 1380):
                raise RuntimeError("An unexpected DN root RELA field differs")
            if y["target"]["size"] != new.sections[".text.HWR_GetLevelFlat"][5]:
                raise RuntimeError("Callee target size is not its complete actual body")
            differences.append({"ordered_index": i, "component": x, "all_up": y})
        roots[name] = {"complete_input_signature": new.signature(section), "complete_PDR": new_frames["records"][section],
            "component_complete_ordered_RELAs": a, "all_up_complete_ordered_RELAs": b,
            "only_explicit_actual_callee_size_descriptors_differ": differences}
    value = shared.binder(build, relas)
    checked, dispositions = {}, []
    selected = set(component["selected_sections"])
    selected |= {n for n in new.sections if n.startswith((".text.ZA_NoteEvict", ".text.Z_FlushCache", ".text.Z_EvictLRU",
        ".text.Z_FreeTagRange", ".text.PS2Mem_Checkpoint", ".text.P_SetupLevel", ".text.P_LoadLevel"))}
    for section in sorted(selected):
        if section not in value.obj.index:
            dispositions.append({"section": section, "input_present": False, "reason": "Absent actual spelling; no body fabricated"})
            continue
        if section not in value.maps or not value.maps[section][0]:
            dispositions.append({"section": section, "input_present": True, "complete_signature": new.signature(section),
                "complete_ordered_RELAs": new.relocations.get(section, []), "complete_PDR": new_frames["records"].get(section),
                "reason": "Actual linker-discarded input body; no runtime byte claim"})
            continue
        checked[section[6:]] = value.text(section[6:])
    if not set(ROOTS) <= checked.keys() or "HWR_GetLevelFlat" not in checked:
        raise RuntimeError("Actual complete DN roots or updated flat callee missing")
    sdk = json.loads(args.sdk_report.read_text())
    if sdk["variants"]["candidate"]["actual_ELF"]["sha256"] != sha(build / "SRB2.ELF"):
        raise RuntimeError("SDK closure refers to a different actual all-up ELF")
    diagnostics = {}
    patch_path = args.rng_folder / "patch-report.json"
    if patch_path.is_file():
        patch = json.loads(patch_path.read_text())
        if patch["original_inputs"]["elf"]["sha256"] != sha(build / "SRB2.ELF"):
            raise RuntimeError("Actual diagnostic patch has a different original ELF")
        for seed, row in patch["variants"].items():
            path = Path(row["ELF"])
            verify_patch(value.elf.data, path.read_bytes(), row["patched_file_offset"], row["owned_symbol_bytes"],
                struct.pack("<10I", *stub_words(row["seed_uint32"], row["actual_M_RandomSeed_address"])))
            parsed = Elf32(path)
            if linked_symbols(parsed) != value.symbols:
                raise RuntimeError("Actual diagnostic symbols/addresses differ")
            spans = {}
            for name, body in checked.items():
                raw = output_bytes(parsed, value.maps[".text." + name][0], body["complete_text_bytes"])
                if hashlib.sha256(raw).hexdigest() != body["resolved_text_sha256"]:
                    raise RuntimeError("Bound all-up graph differs in actually staged RNG copy")
                spans[name] = {"complete_bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
            diagnostics[seed] = {"path": str(path), "sha256": sha(path), "every_other_ELF_byte_identical": True,
                "complete_bound_graph_spans": spans}
    report = {"status": "pass actual all-up scalar DN/allocator/render graph binding", "ELF_sha256": sha(build / "SRB2.ELF"),
        "engine_sha256": sha(build / "obj/engine_lto.o"), "actual602_source_pins": source_pins,
        "context_report_sha256": sha(build / "context-report.json"), "complete_actual205_IR_pins": context["complete_actual_IR_objects"],
        "complete_main_IR_admission": main_IR, "complete_three_root_comparison": roots,
        "complete_actual_all_up_bound_text": checked, "input_dispositions": dispositions,
        "complete_referenced_data": value.data, "complete_exact_literals": list(value.literals.values()),
        "exact_literal_RELA_uses": value.literal_uses, "complete_actual_ABI_boundaries": shared.boundary_records(value, checked),
        "SDK_closure_report": {"path": str(args.sdk_report), "sha256": sha(args.sdk_report)},
        "actual_diagnostic_copies": diagnostics, "binary_negative_controls": controls(out / "binary-negatives", value),
        "no_compiler_linker_emulator_invoked": True, "scope": "Actual combined context, independently resolved bodies/data/RELAs/literals/frames/capacities and SDK routes; no component runtime or memory result is substituted."}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "actual_ELF_sha256": report["ELF_sha256"], "bound_bodies": len(checked),
        "binary_controls": len(report["binary_negative_controls"]), "diagnostics": {k: v["sha256"] for k, v in diagnostics.items()},
        "report_sha256": sha(out / "report.json")}, indent=2))


if __name__ == "__main__":
    main()
