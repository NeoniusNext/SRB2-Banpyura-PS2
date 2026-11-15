#!/usr/bin/env python3
"""Read-only final binding of GS state staging and the retained upload graph."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from next8_gs_codecheck import canonical_relocations
from next9_gs_hosttest import Object, bind, dump, sha, stream

ROOT = Path(__file__).resolve().parents[2]
UPLOADS = ("fill_idx", "fill_ap88", "fill_ia88", "fill_ap88_ct32", "fill_ct32")


def upload_graph(paths):
    objects = {tag: Object(path) for tag, path in paths.items()}
    queue = [".text." + name for name in UPLOADS]
    nodes, external = {}, set()
    while queue:
        section = queue.pop()
        if section in nodes:
            continue
        signatures = {tag: obj.signature(section) for tag, obj in objects.items()}
        relas = {tag: canonical_relocations(obj, section) for tag, obj in objects.items()}
        if any(value != signatures["baseline"] for value in signatures.values()):
            raise RuntimeError("Complete upload graph section changed: " + section)
        if any(value != relas["baseline"] for value in relas.values()):
            raise RuntimeError("Ordered upload graph RELAs changed: " + section)
        code = bool(objects["current"].sections[section][2] & 4)
        node = {**signatures["current"], "kind": "code" if code else "data",
                "ordered_relocations": relas["current"],
                "complete_bytes_and_ordered_relas_identical_across_all_three_objects": True}
        if section == ".text.fill_ap88":
            data = objects["current"].content(objects["current"].sections[section])
            words = struct.unpack("<" + "I" * (len(data) // 4), data)
            node["ppacb_sites"] = sum((word & 0xfc0007ff) == 0x700006c8 for word in words)
            if node["ppacb_sites"] != 3:
                raise RuntimeError("A retained AP88 PPACB site changed")
        nodes[section] = node
        for record in objects["current"].relocations.get(section, []):
            target = record["target"]["section"]
            if target in objects["current"].sections and target:
                queue.append(target)
            elif not target:
                external.add(record["target"]["name"])
            else:
                raise RuntimeError("Unexpected special graph target: " + repr(record))
    return {"roots": list(UPLOADS), "objects": {tag: {"path": str(path), "sha256": sha(path)} for tag, path in paths.items()},
            "code_sections": sum(node["kind"] == "code" for node in nodes.values()),
            "data_sections": sum(node["kind"] == "data" for node in nodes.values()),
            "ordered_relocations": sum(len(node["ordered_relocations"]) for node in nodes.values()),
            "nodes": nodes, "external_boundaries": sorted(external),
            "scope": "Complete recursive engine-defined code/data section graph from all five upload roots, including diagnostic logging; SDK/newlib function and data externals remain explicit boundaries. BSS equality means identical size/alignment/flags, with no initialized payload."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--baseline-engine", type=Path, required=True)
    parser.add_argument("--measured-engine", type=Path, required=True)
    parser.add_argument("--current-engine", type=Path, required=True)
    parser.add_argument("--baseline-elf", type=Path, required=True)
    parser.add_argument("--current-elf", type=Path, required=True)
    parser.add_argument("--fixture-elf", type=Path, required=True)
    parser.add_argument("--build-report", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    bind(args.out, args.baseline_engine, args.current_engine, args.baseline_elf,
         args.current_elf, args.fixture_elf, negatives=True)
    graph = upload_graph({"baseline": args.baseline_engine,
                          "measured": args.measured_engine,
                          "current": args.current_engine})
    dump(args.out / "unchanged-upload-graph.json", graph)
    measured, current = stream(Object(args.measured_engine)), stream(Object(args.current_engine))
    old_relas = [{key: value for key, value in item.items() if key != "actual_target"} for item in measured["relocations"]]
    new_relas = [{key: value for key, value in item.items() if key != "actual_target"} for item in current["relocations"]]
    if measured["bytes"] != current["bytes"] or measured["body_bytes"] != current["body_bytes"] or old_relas != new_relas:
        raise RuntimeError("Combined context changed measured state_flush instructions/RELAs")
    build = json.loads(args.build_report.read_text())
    if build["elf_sha256"] != sha(args.current_elf):
        raise RuntimeError("Current build report ELF hash differs")
    production = {}
    for name in ("ps2_hw_draw.inc", "ps2_hw_priv.inc", "ps2_hw_tex.inc"):
        values = [value for path, value in build["input_sha256"].items() if path.endswith("/src/ps2/hw/" + name)]
        if len(values) != 1 or values[0] != sha(ROOT / "src/ps2/hw" / name):
            raise RuntimeError("Current production GS source differs from combined build: " + name)
        production[name] = values[0]
    report = {
        "current_engine": str(args.current_engine), "current_engine_sha256": sha(args.current_engine),
        "current_elf": str(args.current_elf), "current_elf_sha256": sha(args.current_elf),
        "current_build_report_sha256": sha(args.build_report), "fixture_elf_sha256": sha(args.fixture_elf),
        "production_source_sha256": production,
        "state_flush_complete_section_bytes": len(current["bytes"]),
        "state_flush_body_bytes": current["body_bytes"], "state_flush_ordered_relocations": len(new_relas),
        "combined_context_changed_any_state_flush_instruction_or_canonical_relocation": False,
        "every_resolved_state_flush_byte_matches_actual_combined_elf_and_measured_fixture": True,
        "upload_graph_code_sections": graph["code_sections"], "upload_graph_data_sections": graph["data_sections"],
        "upload_graph_ordered_relocations": graph["ordered_relocations"],
        "scope": "Read-only code/RELA/resolved-ELF/data/source binding to this exact combined default Hardware build. Reuses the deterministic 64-row measured fixture without rerunning target code. Commit metadata or any later rebuild requires a new ELF/object/source hash binding; no new timing, readback or whole-game result is inferred.",
        "checker_sha256": sha(Path(__file__)),
    }
    dump(args.out / "combined-binding-report.json", report)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
