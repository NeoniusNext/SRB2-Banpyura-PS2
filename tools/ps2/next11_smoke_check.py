#!/usr/bin/env python3
"""Two normal-RNG shipping smokes through the official verified wrappers only.

Replays the preceding unmodified Software MAP01 fog/stereo and native Hardware
MAP02 scenes, comparing all three complete PPMs and loader hashes. These are
visual/startup/audio checks, not controlled timing or cache-pressure evidence.

Run only with the coordinator's emulator grant. --plan-only performs read-only
input/RNG preflight; --analyze-only inspects existing runs without emulation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import bind
from next11_integration_check import inspect
from next11_runtime import run_verified

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
CAPTURES = {"vidshot-320x200-l%d.ppm" % frame for frame in (20, 70, 140)}
STAGE_CONFIG = b'fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\nrollingdemos "Off"\n'
SCENARIOS = (("shipping-sw-fog-stereo", "MAP01", False), ("shipping-hw-map02", "MAP02", True))
PROOF_TOOLS = ("next11_smoke_check.py", "next11_runtime.py", "next11_integration_check.py",
               "next10_gs_run.py", "opt_run.py", "run_pcsx2.py", "next7_render_fog_codecheck.py",
               "next8_sw_codecheck.py", "next10_sw_bounds_codecheck.py", "next10_sw_bounds_report.py")


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def normal_rng(elf_path, hardware):
    """Bind the unpatched OS initializer to the actual release engine/map."""
    build_dir = elf_path.parent
    report_path = build_dir / "build-report.json"
    report = json.loads(report_path.read_text())
    if report["hardware"] != hardware or report.get("diagnostic_build") is not False:
        raise RuntimeError("Expected a non-diagnostic build of the selected renderer: " + str(report_path))
    if sha(elf_path) != report["elf_sha256"]:
        raise RuntimeError("Shipping ELF differs from its complete build report: " + str(elf_path))
    engine_path, map_path, config = build_dir / "obj/engine_lto.o", build_dir / "SRB2.map", build_dir / "gen/config.h"
    engine, elf = Elf32(engine_path), Elf32(elf_path)
    initializer = bind(engine, elf, map_path, "M_RandomSeedFromOS")
    seeder = bind(engine, elf, map_path, "M_RandomSeed")
    entropy = bind(engine, elf, map_path, "I_GetRandomBytes")
    if linked_symbols(elf)["M_RandomSeedFromOS"]["size"] != 212 or initializer["exact_frame_record"]["stack_frame_bytes"] != 32:
        raise RuntimeError("Unexpected complete production OS initializer ABI")
    targets = [row["exact_symbol"] for row in initializer["ordered_RELA_records"]]
    if not any(name in ("I_GetRandomBytes", ".text.I_GetRandomBytes") for name in targets):
        raise RuntimeError("Production OS initializer does not call the actual entropy provider")
    entropy_targets = {row["exact_symbol"] for row in entropy["ordered_RELA_records"]}
    if not {"GetTimerSystemTime", "cpu_ticks", "time"}.issubset(entropy_targets):
        raise RuntimeError("Actual entropy provider lacks its complete production clock inputs")
    revision = re.search(r'^#define SRB2_COMP_REVISION\s+"([^"]+)"', config.read_text(), re.M)
    if not revision:
        raise RuntimeError("Frozen release configuration lacks source revision")
    return {"ELF": str(elf_path), "ELF_sha256": report["elf_sha256"], "hardware": hardware,
            "diagnostic_build": False, "source_revision": revision[1], "normal_OS_rng_initializer_bound": True,
            "complete_initializer_binding": initializer, "complete_actual_seeder_binding": seeder,
            "complete_entropy_provider_binding": entropy,
            "inputs": {str(path): sha(path) for path in (elf_path, engine_path, map_path, report_path, config)}}


def runtime_evidence(path, expected_command):
    """Check the archived full INIs as well as the verifier's before/after pins."""
    before_path, after_path = path / "run-pre.json", path / "run-post.json"
    before, after = json.loads(before_path.read_text()), json.loads(after_path.read_text())
    if before["command"][1:] != list(map(str, expected_command))[1:] or before["command"] != after["command"]:
        raise RuntimeError("Recorded official command differs")
    for field in ("all_frozen_inputs_unchanged", "all_non_UI_settings_unchanged", "emulator_binary_unchanged"):
        if not after.get(field):
            raise RuntimeError("Runtime verifier rejected " + field)
    for field in ("input_sha256", "critical_settings", "emulator_binary_sha256"):
        if before[field] != after[field]:
            raise RuntimeError("Recorded before/after inputs differ: " + field)
    profile = Path(before["environment"].get("SRB2_PCSX2_ROOT", "/workspace/.onboarding/pcsx2")).resolve()
    archived = {}
    for label, record in (("before", before), ("after", after)):
        for original, expected in record["config_sha256"].items():
            copy = path / ("config-" + label) / Path(original).relative_to(profile)
            if sha(copy) != expected:
                raise RuntimeError("Complete archived emulator INI differs: " + str(copy))
            archived[str(copy)] = expected
    return {"pre_sha256": sha(before_path), "post_sha256": sha(after_path), "wrapper_rc": after["rc"],
            "all_inputs_and_non_UI_settings_unchanged": True, "complete_archived_INI_sha256": archived,
            "critical_settings": before["critical_settings"], "emulator_binary_sha256": before["emulator_binary_sha256"],
            "runtime_input_sha256": before["input_sha256"], "python_executable": before["command"][0]}


def output_config_artifacts(run, expected):
    # The official runner may stop between the complete temporary config write
    # and M_SaveDefaults' atomic rename. Never rename or fabricate run evidence.
    paths = [path for path in (run / "reference.cfg", run / "reference.cfg.tmp") if path.is_file()]
    if not paths or any(path.read_bytes() != expected for path in paths):
        raise RuntimeError("Complete generated configuration differs or is incomplete: " + str(run))
    return {"observed_output_files": {path.name: sha(path) for path in paths},
            "atomic_save_rename_pending": not (run / "reference.cfg").is_file(),
            "complete_generated_config_literal_equal": True}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next11/release-unmodified")
    ap.add_argument("--software", type=Path, default=ROOT / "build/nov15-next11/release/software/SRB2.ELF")
    ap.add_argument("--hardware", type=Path, default=ROOT / "build/nov15-next11/release/hardware/SRB2.ELF")
    ap.add_argument("--pak", type=Path, default=ROOT / "build/pak")
    ap.add_argument("--historical", type=Path, default=ROOT / "build/nov15-next10/final-unmodified")
    ap.add_argument("--npot-addon", type=Path, default=ROOT / "build/nov14-next5/npot-addons/NX5_FLOOR.pk3")
    ap.add_argument("--fog-addon", type=Path, default=ROOT / "build/nov14-next7/fog-addons/NX7_FOG.pk3")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--analyze-only", action="store_true")
    mode.add_argument("--resume", action="store_true", help="inspect complete existing immutable runs and execute only missing scenes")
    mode.add_argument("--plan-only", action="store_true", help="read-only release/RNG/historical preflight; no emulator")
    a = ap.parse_args()
    a.out, a.historical, a.pak = a.out.resolve(), a.historical.resolve(), a.pak.resolve()
    a.software, a.hardware = a.software.resolve(), a.hardware.resolve()
    if not a.out.is_relative_to(ROOT / "build") or a.out == ROOT / "build":
        ap.error("--out must be a child of repository build/")
    historical_report_path = a.historical / "report.json"
    historical = json.loads(historical_report_path.read_text())
    asset_names = sorted(path.name for path in a.pak.glob("*.PAK")) + ["FINEACON.DAT"]
    if not {"SRB2.PAK", "ZONES.PAK", "CHARS.PAK", "MUSIC.PAK"}.issubset(asset_names):
        ap.error("--pak lacks a complete stock pack set")
    assets = {name: sha(a.pak / name) for name in asset_names}
    fixtures = ((a.npot_addon.resolve(), "NX5_FLOOR.pk3"),
                (a.fog_addon.resolve(), "NX7_FOG.pk3"), (TOOLS / "next5_audio_integration.lua", "NX5_AUDIO.lua"))
    entries, frozen_inputs = [], {str(TOOLS / name): sha(TOOLS / name) for name in PROOF_TOOLS}
    frozen_inputs[str(historical_report_path)] = sha(historical_report_path)
    for name, mapname, hardware in SCENARIOS:
        elf = a.hardware if hardware else a.software
        release = normal_rng(elf, hardware)
        frozen_inputs.update(release["inputs"])
        old = a.historical / name
        reference = historical[name]
        if reference["errors"] or reference["rc"] or set(reference["captures"]) != CAPTURES:
            raise RuntimeError("Historical smoke is incomplete: " + name)
        game_args = ["-singletics"]
        if not hardware:
            game_args += ["-keepspawn", "-nodigmusic"]
        game_args += ["-loadprof", "-loadhash", "-zck", "-zheap", "35", "-astat"]
        if not hardware:
            game_args += ["-file", *["host:/.srb2/" + target for _, target in fixtures]]
        game_args += ["-vidshot", "l20,l70,l140"]
        if hardware:
            game_args += ["-renderer", "Hardware"]
        expected_args = ["-config", "reference.cfg", "-nolog", "-noendtxt", "-skipintro", "-warp", mapname, *game_args]
        if (old / "ps2args").read_text().splitlines() != expected_args:
            raise RuntimeError("Historical argument vector differs from the exact intended smoke: " + name)
        if (old / ".srb2/reference.cfg").read_bytes() != STAGE_CONFIG:
            raise RuntimeError("Historical staged reference configuration differs")
        for capture, expected in reference["captures"].items():
            raw = (old / capture).read_bytes()
            if sha(old / capture) != expected or not raw.startswith(b"P6\n320 200\n255\n") or len(raw) != 192015:
                raise RuntimeError("Historical complete frame differs: " + str(old / capture))
            frozen_inputs[str(old / capture)] = expected
        for asset, expected in assets.items():
            if sha(old / asset) != expected:
                raise RuntimeError("Historical/current stock asset differs: " + asset)
        if not hardware:
            for source, target in fixtures:
                if source.read_bytes() != (old / ".srb2" / target).read_bytes():
                    raise RuntimeError("Historical/current scene fixture differs: " + target)
                frozen_inputs[str(source)] = sha(source)
        for path in (old / "ps2args", old / "reference.cfg", old / ".srb2/reference.cfg"):
            frozen_inputs[str(path)] = sha(path)
        command = [sys.executable, "-B", str(TOOLS / "opt_run.py"), "--name", name,
                   "--elf", str(elf), "--ram", "32", "--pak", str(a.pak), "--out", str(a.out / "runs"),
                   "--map", mapname, "--timeout", "180", "--until", "VIDSHOT COMPLETE 3"]
        if not hardware:
            for source, target in fixtures:
                command += ["--home-file", str(source) + "=" + target]
        command += ["--", *game_args]
        entries.append({"name": name, "map": mapname, "hardware": hardware, "release": release,
                        "command": command, "expected_arguments": expected_args})
    frozen_inputs.update({str(a.pak / name): expected for name, expected in assets.items()})
    if a.plan_only:
        print(json.dumps({"mode": "read-only-preflight", "emulator_started": False,
            "out": str(a.out), "historical_report_sha256": sha(historical_report_path),
            "scenarios": [{"name": e["name"], "map": e["map"], "ELF_sha256": e["release"]["ELF_sha256"],
                           "source_revision": e["release"]["source_revision"], "normal_OS_rng_initializer_bound": True,
                           "command": e["command"]} for e in entries],
            "asset_sha256": assets, "performance_comparison_claimed": False}, indent=2))
        return 0
    if not a.analyze_only and not a.resume:
        for entry in entries:
            if (a.out / "runs" / entry["name"]).exists() or (a.out / "runtime" / entry["name"]).exists():
                ap.error("Choose a fresh --out or --analyze-only; a smoke destination already exists")
    a.out.mkdir(parents=True, exist_ok=True)
    report = {"normal_local_rng": True, "performance_comparison_claimed": False,
              "comparison_scope": "Whole PPM/loader/startup/audio checks; OS-seeded local RNG precludes causal timing/cache claims",
              "historical_report": str(historical_report_path), "historical_report_sha256": sha(historical_report_path),
              "asset_sha256": assets, "checker_sha256": sha(Path(__file__)), "runs": {}, "errors": []}
    for entry in entries:
        name, command = entry["name"], entry["command"]
        run, old = a.out / "runs" / name, a.historical / name
        existing = (a.out / "runtime" / name / "run-post.json").is_file() and (run / "summary.json").is_file()
        if not a.analyze_only and not (a.resume and existing):
            print("START", name, flush=True)
            result = run_verified(command, a.out / "runtime" / name, [Path(p) for p in frozen_inputs])
            (a.out / (name + ".orchestration.log")).write_text(result.stdout + result.stderr)
        item = inspect(run, name)
        item["run"] = str(run)
        item["normal_rng_release"] = entry["release"]
        item["runtime"] = runtime_evidence(a.out / "runtime" / name, command)
        boot = (run / "boot.txt").read_text(errors="replace")
        expected_renderer = "HWE startup renderer=Hardware chosen=2 hardware=1" if entry["hardware"] else "HWE startup renderer=Software chosen=1 hardware=0"
        if expected_renderer not in boot or "Switching to the Software renderer" in boot:
            item["errors"].append("Selected native renderer is absent")
        if item["elf_sha256"] != entry["release"]["ELF_sha256"]:
            item["errors"].append("Staged complete ELF differs from unmodified shipping build")
        if set(item["captures"]) != CAPTURES or re.findall(r"^VIDSHOT COMPLETE (\d+)$", boot, re.M) != ["3"]:
            item["errors"].append("Exactly three requested frame captures did not complete")
        if not item["audio_underruns"] or any(item["audio_underruns"]):
            item["errors"].append("Missing or nonzero audio-underrun counters")
        if (run / "ps2args").read_text().splitlines() != entry["expected_arguments"]:
            item["errors"].append("Complete staged argument vector differs")
        if (run / ".srb2/reference.cfg").read_bytes() != STAGE_CONFIG:
            item["errors"].append("Complete staged reference configuration differs")
        item["generated_config"] = output_config_artifacts(run, (old / "reference.cfg").read_bytes())
        item["staged_reference_config_sha256"] = sha(run / ".srb2/reference.cfg")
        item["historical_complete_config_literal_equal"] = item["generated_config"]["complete_generated_config_literal_equal"]
        item["existing_runtime_reused"] = bool(a.resume and existing)
        item["actual_executing_checker_sha256"] = item["runtime"]["runtime_input_sha256"][str(Path(__file__).resolve())]
        item["analysis_checker_sha256"] = sha(Path(__file__))
        for asset, expected in assets.items():
            if sha(run / asset) != expected:
                item["errors"].append("Complete staged stock asset differs: " + asset)
        if not entry["hardware"]:
            for source, target in fixtures:
                if (run / ".srb2" / target).read_bytes() != source.read_bytes():
                    item["errors"].append("Complete staged scene fixture differs: " + target)
        item["historical_frame_hashes_equal"] = item["captures"] == historical[name]["captures"]
        item["historical_loader_hashes_equal"] = bool(item["lhash"]) and item["lhash"] == historical[name]["lhash"]
        item["whole_reference_images_literal_equal"] = set(item["captures"]) == CAPTURES and all(
            (run / capture).read_bytes() == (old / capture).read_bytes() for capture in CAPTURES)
        if not all(item[key] for key in ("historical_frame_hashes_equal", "historical_loader_hashes_equal", "whole_reference_images_literal_equal")):
            item["errors"].append("Complete historical frame/loader comparison differs; no causal timing inference")
        item["evidence_sha256"] = {path.name: sha(path) for path in (run / "summary.json", run / "boot.txt", run / "pcsx2.log", run / "ps2args")}
        report["runs"][name] = item
        report["errors"] += [name + ": " + error for error in item["errors"]]
    if frozen_inputs != {path: sha(Path(path)) for path in frozen_inputs}:
        report["errors"].append("A complete release, fixture, asset, proof-tool or historical input changed")
    report["all_frozen_inputs_unchanged"] = not any("input changed" in error for error in report["errors"])
    (a.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"runs": len(report["runs"]), "normal_local_rng": True,
        "capture_counts": {name: len(item["captures"]) for name, item in report["runs"].items()},
        "errors": report["errors"], "performance_comparison_claimed": False}, indent=2))
    return bool(report["errors"])


if __name__ == "__main__":
    raise SystemExit(main())
