#!/usr/bin/env python3
"""Audit static isolation and the observational unseeded e713 pressure matrix.

Checks the fixed canonical IR copies, one-unit controls, selected run ELFs,
stock asset hashes, completion/quality gates and every final zone/cache/stack
metric. OS-seeded ambient sound choices confound these runtime comparisons;
this tool permits no runtime causal or performance conclusion. It neither
compiles nor starts an emulator.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IO = ROOT / "build/nov15-next10/io"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit_control(name, frozen):
    work = IO / name
    report = json.loads((work / "report.json").read_text())
    code = json.loads((work / "code-report.json").read_text())
    assert report["only_recompiled_unit"] == "src/w_wad.c" and report["optout"]
    assert len(report["canonical_objects"]) == 189
    assert sha(frozen / "obj/engine_lto.o") == report["canonical_engine_sha256"]
    assert sha(frozen / "SRB2.ELF") == report["canonical_elf_sha256"]
    for old_path, expected in report["canonical_objects"].items():
        assert sha(frozen / "obj" / Path(old_path).name) == expected
    unchanged = [path for path, value in report["objects"].items()
                 if report["canonical_objects"].get(path) == value]
    assert len(unchanged) == 188
    replacements = [(path, value) for path, value in report["objects"].items() if path not in unchanged]
    assert len(replacements) == 1 and sha(Path(replacements[0][0])) == replacements[0][1]
    assert sha(work / "engine_lto.o") == report["engine_sha256"]
    assert sha(work / "SRB2.ELF") == report["elf_sha256"]
    assert code["isolation"]["changed_sections"] == [".text.CheckLumpInCache"]
    assert code["isolation"]["unchanged_complete_allocated_sections"] == 10490
    assert code["binding"]["section_bytes"] == 552 and code["binding"]["frame_bytes"] == 64
    assert code["sdk_measured_to_final"]["complete_body_and_table_equivalence"]
    assert "-DPS2_NOOPT_LUMPCACHEWALK" in report["compile_flags"]
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=IO / "cache-causal-matrix.json")
    a = ap.parse_args()
    controls = {
        "old_420": audit_control("topology-optout", IO / "canonical-current420"),
        "old_444": audit_control("both-topology-optout", IO / "canonical-current444"),
    }
    assert "-DPS2_NOOPT_DRAWSEG_XY" not in controls["old_420"]["compile_flags"]
    assert "-DPS2_NOOPT_DRAWSEG_XY" in controls["old_444"]["compile_flags"]
    expected_elves = {"wide_420": controls["old_420"]["canonical_elf_sha256"],
                      "wide_444": controls["old_444"]["canonical_elf_sha256"],
                      **{key: value["elf_sha256"] for key, value in controls.items()}}
    specs = {"wide_420": ROOT / "build/nov15-next10/next10-pressure-350-report.json",
             "wide_444": ROOT / "build/nov15-next10/controls/drawseg-pressure/next10-pressure-350-report.json",
             "old_420": ROOT / "build/nov15-next10/controls/io-pressure/next10-pressure-350-report.json",
             "old_444": ROOT / "build/nov15-next10/controls/both-pressure/next10-pressure-350-report.json"}
    rows, assets, baseline = {}, None, None
    for name, path in specs.items():
        report = json.loads(path.read_text())
        before, current = report["runs"]["base"], report["runs"]["new"]
        assert report["frames"] == 350 and not report["errors"] and report["lhash_equal"]
        assert current["elf_sha256"] == expected_elves[name]
        assert current["rc"] == 0 and not current["errors"] and not current["warnings"]
        assert not any(current["audio_underruns"]) and before["lhash"] == current["lhash"]
        assert before["elf_sha256"] == "9ccdd6c2b5b3a3dfc8de212742b4acd48a021174203872288a71b65d8919e5d8"
        assert set(report["asset_sha256"]) == {"SRB2.PAK", "ZONES.PAK", "CHARS.PAK", "MUSIC.PAK", "FINEACON.DAT"}
        if assets is None:
            assets = report["asset_sha256"]
        assert assets == report["asset_sha256"]
        base = {key: int(value) for key, value in before["zstat"][-1].items()}
        if baseline is None:
            baseline = base
        assert baseline == base
        final = {key: int(value) for key, value in current["zstat"][-1].items()}
        assert final["ram"] == 33554432 and final["map"] == 11
        assert final["frames"] == final["levelframes"] == 350
        assert final["arena"] == baseline["arena"] == 24743936
        rows[name] = {"pressure_report": str(path.relative_to(ROOT)), "pressure_report_sha256": sha(path),
                      "elf_sha256": current["elf_sha256"], "final": final, "loader_hashes_match": True,
                      "errors": [], "warnings": []}
    pairs = {}
    for label in ("420", "444"):
        wide, old = rows["wide_" + label]["final"], rows["old_" + label]["final"]
        differences = {key: [wide[key], old[key]] for key in wide if wide[key] != old[key]}
        assert set(differences) <= {"cycles", "t_ms", "libcfree", "libcpeak"}, differences
        pairs[label] = {"differing_final_fields": differences, "all_zone_cache_and_stack_metrics_equal": True}
    result = {"rows": {"baseline": {"final": baseline}, **rows}, "lookup_control_pairs": pairs,
              "asset_sha256": assets, "canonical_input_audit": "build/nov15-next10/io/current-context-controls.json",
              "frozen_189_inputs_verified_for_each_control": True,
              "observed_lookup_pairs_have_equal_final_zone_cache_metrics": True,
              "rng_controlled": False, "runtime_causal_conclusion_allowed": False,
              "rng_confound": "M_Random is seeded from OS clocks; ambient DSATHUN1/2 choices change PU_SOUND allocations",
              "physical_console_or_weighted_fps_claim": False, "checker_sha256": sha(Path(__file__))}
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"rng_controlled": False, "runtime_causal_conclusion_allowed": False,
                      "observed_lookup_pairs": pairs}, indent=2))


if __name__ == "__main__":
    main()
