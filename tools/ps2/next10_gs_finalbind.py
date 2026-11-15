#!/usr/bin/env python3
"""Read-only binding of both measured GS fixtures to a final whole Hardware ELF."""
import argparse
import json
from pathlib import Path
import shutil

from next10_gs_hosttest import ROOT, bind as high_bind, dump, sha
from next10_gs_low import bind as low_bind, bundle
from next9_gs_finalbind import upload_graph


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--current-engine", type=Path, required=True)
    ap.add_argument("--current-elf", type=Path, required=True)
    ap.add_argument("--build-report", type=Path, required=True)
    ap.add_argument("--baseline-engine", type=Path, default=ROOT / "build/nov15-next10/baseline/hardware/engine_lto.o")
    ap.add_argument("--baseline-elf", type=Path, default=ROOT / "build/nov15-next10/baseline/hardware/SRB2.ELF")
    ap.add_argument("--measured-engine", type=Path, default=ROOT / "build/nov15-next10/gs/candidate-hardware/obj/engine_lto.o")
    ap.add_argument("--measurements", type=Path, default=ROOT / "build/nov15-next10/gs")
    args = ap.parse_args()
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    high = out / "high"; high.mkdir(exist_ok=True)
    low = out / "low"; low.mkdir(exist_ok=True)
    build = json.loads(args.build_report.read_text())
    if build["elf_sha256"] != sha(args.current_elf) or build["compile_diagnostics"] or build["link_diagnostics"]:
        raise RuntimeError("Final Hardware build-report/ELF or diagnostics differ")
    freeze = json.loads((args.measurements / "candidate-source-freeze.json").read_text())
    source = {}
    for name in ("ps2_hw_priv.inc", "ps2_hw_tex.inc", "ps2_hw_gs.inc"):
        suffix = "/src/ps2/hw/" + name
        hashes = [value for path, value in build["input_sha256"].items() if path.endswith(suffix)]
        expected = freeze["source_sha256"]["src/ps2/hw/" + name]
        if hashes != [expected] or sha(ROOT / "src/ps2/hw" / name) != expected:
            raise RuntimeError("Final actual GS source differs from measured source: " + name)
        source[name] = expected
    high_bind(high, {"base": args.baseline_engine, "cur": args.current_engine},
              {"base": args.baseline_elf, "cur": args.current_elf}, args.measurements / "ee/GS_NEXT10.ELF", True)
    graphs = {}
    for tag, engine in (("base", args.baseline_engine), ("cur", args.current_engine)):
        bundle(low, engine, tag)
        fresh = json.loads((low / (tag + "-graph.json")).read_text())
        measured = json.loads((args.measurements / "low" / (tag + "-graph.json")).read_text())
        if (fresh["nodes"] != measured["nodes"] or fresh["original_GP0"] != measured["original_GP0"] or
                fresh["preserved_local_GP_aliases"] != measured["preserved_local_GP_aliases"]):
            raise RuntimeError("Complete measured/final code/data/RELA/GP graph differs: " + tag)
        graphs[tag] = {"code_nodes": sum(n["kind"] == "code" for n in fresh["nodes"].values()),
                       "data_nodes": sum(n["kind"] == "data" for n in fresh["nodes"].values()),
                       "ordered_RELAs": sum(len(n["ordered_relocations"]) for n in fresh["nodes"].values()),
                       "every_complete_original_node_and_ordered_RELA_identical_to_timed_graph": True}
    shutil.copy2(args.measurements / "low/GS_LOW10.ELF", low / "GS_LOW10.ELF")
    shutil.copy2(args.measurements / "low/build-report.json", low / "build-report.json")
    low_bind(low, {"base": args.baseline_engine, "cur": args.current_engine},
             {"base": args.baseline_elf, "cur": args.current_elf}, low / "GS_LOW10.ELF", True)
    previous = upload_graph({"baseline": args.baseline_engine, "measured": args.measured_engine, "current": args.current_engine})
    dump(out / "preceding-upload-retention.json", previous)
    report = {"kind": "read-only complete measured-to-final Hardware binding", "current_engine": str(args.current_engine),
              "current_engine_sha256": sha(args.current_engine), "current_elf": str(args.current_elf),
              "current_elf_sha256": sha(args.current_elf), "build_report_sha256": sha(args.build_report),
              "high_fixture_sha256": sha(args.measurements / "ee/GS_NEXT10.ELF"),
              "low_fixture_sha256": sha(args.measurements / "low/GS_LOW10.ELF"),
              "source_sha256": source, "whole_low_graph": graphs,
              "complete_sections_include_trailing_padding": True,
              "actual_memory_wrappers_and_SDK_closure_bound": True,
              "scope": "No compilation, emulation, game/readback or physical-console result is inferred; every later final rebuild needs fresh binding."}
    dump(out / "final-binding-report.json", report)
    print("Final whole Hardware GS high/free/low/upload/wrapper/data/GP binding passed.")


if __name__ == "__main__":
    main()
