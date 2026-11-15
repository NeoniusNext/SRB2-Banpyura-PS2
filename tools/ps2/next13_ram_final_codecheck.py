#!/usr/bin/env python3
"""Bind scalar drawnode and direct memory observer graphs into the canonical Hardware ELF.

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
    parser.add_argument("--build", type=Path, default=WORK / "final/hardware")
    parser.add_argument("--relas", type=Path)
    parser.add_argument("--rng-folder", type=Path, default=WORK / "local-rng/final-hardware")
    parser.add_argument("--sdk-report", type=Path, default=WORK / "review/ram-sdk-final-v1/report.json")
    parser.add_argument("--out", type=Path, default=WORK / "final/ram-binding-v1")
    args = parser.parse_args()
    build, out = args.build.resolve(), args.out.resolve()
    relas = args.relas.resolve() if args.relas else build / "linker-relas"
    admission_path = build / "canonical-build-admission.json"
    context = json.loads(admission_path.read_text())
    manifest_path = WORK / "final/source/manifest.json"
    manifest = json.loads(manifest_path.read_text())
    component_path = WORK / "ram-scalar-binding-v1/report.json"
    component = json.loads(component_path.read_text())
    if context["source_commit"] != "5481bbbdc5aa3167c046836f3e51e4dbb31ad807" or manifest["revision"] != context["source_commit"]:
        raise RuntimeError("Canonical committed source identity differs")
    if set(manifest["only_baseline_differences"]) != SOURCE_CHANGES:
        raise RuntimeError("Complete canonical four-source scope differs")
    source_pins = manifest["source_sha256"]
    if len(source_pins) != 602 or source_pins != context["all602_source_sha256"] or sha(manifest_path) != context["source_manifest_sha256"]:
        raise RuntimeError("Canonical complete602 source admission differs")
    for path, digest in source_pins.items():
        if sha(ROOT / path) != digest:
            raise RuntimeError("Canonical actual source changed: " + path)
    if source_pins["src/hardware/hw_main.c"] != "25ab400933e147ae202648629b088999f7f2b3d84a70a839aeed471cffcdd3a6":
        raise RuntimeError("Canonical scalar DN source is not tested v2")
    if len(context["complete_actual205_IR_objects"]) != 205:
        raise RuntimeError("Canonical actual205 graph scope differs")
    for path, digest in context["complete_actual205_IR_objects"].items():
        if sha(path) != digest:
            raise RuntimeError("Canonical complete IR changed: " + path)
    if not context["all203_C_freshly_compiled"] or not context["all203_whole_LTO_option_sections_match_original_default"]:
        raise RuntimeError("Canonical default full options/compilation admission fails")
    options = json.loads((build / "actual-all-unit-options.json").read_text())
    if len(options) != 203 or not all(x["whole_option_bytes_identical"] for x in options.values()):
        raise RuntimeError("Canonical all203 full LTO option bytes differ")
    if sha(build / "actual-all-unit-options.json") != context["actual_all_unit_options_sha256"]:
        raise RuntimeError("Canonical option evidence changed")
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
        ".text.Z_FreeTagRange", ".text.PS2Mem_Checkpoint", ".text.P_SetupLevel", ".text.P_LoadLevel",
        ".text.PS2Mem_Report", ".text.Z_NextFrame", ".text.Z_FreeTags", ".text.Z_ReportCosts",
        ".text.PS2HWFB_Report", ".text.I_OutputMsg", ".text.D_SRB2Loop", ".text.ZA_Stats", ".text.ZA_ResetPeak",
        ".text.PS2Mem_Line", ".text.PS2Mem_Frame", ".text.ZA_Alloc", ".text.ZA_InitMem"))}
    for section, rows in new.relocations.items():
        if section.startswith(".text.") and any(r["target"]["name"] == "PS2Mem_Report" or
                r["target"]["name"].startswith(("za_nevict", "za_evictbytes")) for r in rows):
            selected.add(section)
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
    report = {"status": "pass actual canonical scalar DN/allocator/render/direct memory observer graph binding", "ELF_sha256": sha(build / "SRB2.ELF"),
        "engine_sha256": sha(build / "obj/engine_lto.o"), "actual602_source_pins": source_pins,
        "canonical_admission_sha256": sha(admission_path), "source_manifest_sha256": sha(manifest_path), "complete_actual205_IR_pins": context["complete_actual205_IR_objects"],
        "complete_main_IR_admission": main_IR, "complete_three_root_comparison": roots,
        "complete_actual_all_up_bound_text": checked, "input_dispositions": dispositions,
        "complete_referenced_data": value.data, "complete_exact_literals": list(value.literals.values()),
        "exact_literal_RELA_uses": value.literal_uses, "complete_actual_ABI_boundaries": shared.boundary_records(value, checked),
        "SDK_closure_report": {"path": str(args.sdk_report), "sha256": sha(args.sdk_report)},
        "actual_diagnostic_copies": diagnostics, "binary_negative_controls": controls(out / "binary-negatives", value),
        "no_compiler_linker_emulator_invoked": True, "scope": "Actual canonical committed context, independently resolved bodies/data/RELAs/literals/frames/capacities and SDK routes; no component runtime or memory result is substituted."}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "actual_ELF_sha256": report["ELF_sha256"], "bound_bodies": len(checked),
        "binary_controls": len(report["binary_negative_controls"]), "diagnostics": {k: v["sha256"] for k, v in diagnostics.items()},
        "report_sha256": sha(out / "report.json")}, indent=2))


if __name__ == "__main__":
    main()
