#!/usr/bin/env python3
"""Fresh matched pressure pair testing the diagnostic-console observer effect.

This changes only the staged con_hudlines diagnostic setting in both versions.
It does not modify source, ELF code, shipping defaults, world settings or old
rejected runs. --run-case requires the coordinator's sole emulator grant.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

from next13_ram_scalar_native import compare, sha, verify, ROOT, WORK
from next12_runtime import run_verified, snapshot
from next11_local_rng import verify_patch, stub_words
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import output_bytes
import opt_run

BASE = ROOT / "build/nov15-next12/final/hardware"
SOURCE = ROOT / "build/nov15-next12/final/source"
ACTIVE = WORK / "ram-scalar-context-v1"
OLD = ROOT / "build/nov15-next12/final-hardware-pressure"
OLD_NAME = "final-hw-map02-01b2d3f5-spawn-350"
BINDING = WORK / "ram-scalar-binding-v1/report.json"
CONSOLE = WORK / "review/console-observer-v1/report.json"
CONSOLE_SHA = "bab8bcf013495c4a8a00a05071b2813f02eee27f2713ef12b0638aeb4c2f1977"
SETTING = 'con_hudlines "0"'
ROLES = {
    "baseline": {"build": BASE, "source": SOURCE,
        "normal": "efc963c7876a5f2a1c395baf5ba9982b68c6db1e56811d7a5e8a690ba840ab20",
        "rng": ROOT / "build/nov15-next12/local-rng/final-hardware",
        "diagnostic": "6766d019d20ef841ee12604011c466231c4cb9ba213e0467ed92f313bbdfb122"},
    "candidate": {"build": ACTIVE, "source": ACTIVE / "snapshot",
        "normal": "f91ad5b0e87fb011229daa9b9670e9928536223cbf15562090b3015a3f2c7f8d",
        "rng": WORK / "ram-scalar-local-rng-v1",
        "diagnostic": "11773e08e5eebc12a69ac21d77a1855c9cafff4c934496fba3bbdf4ae7afe988"},
}


def prepare(out):
    out.mkdir(parents=True, exist_ok=False)
    if sha(CONSOLE) != CONSOLE_SHA:
        raise RuntimeError("Exact source/font/console evidence differs")
    binding = json.loads(BINDING.read_text())
    files = {str(p): sha(p) for p in (Path(__file__), BINDING, CONSOLE,
        WORK / "review/ram-sdk-scalar-v1/report.json",
        WORK / "ram-scalar-native-v2/map02-report.json",
        ROOT / "tools/ps2/next13_ram_scalar_native.py", ROOT / "tools/ps2/next12_runtime.py",
        ROOT / "tools/ps2/next12_final_hardware_pressure_check.py", ROOT / "tools/ps2/next11_smoke_check.py",
        ROOT / "tools/ps2/next11_integration_check.py", ROOT / "tools/ps2/next10_gs_run.py",
        ROOT / "tools/ps2/next11_local_rng.py", ROOT / "tools/ps2/opt_run.py", ROOT / "tools/ps2/run_pcsx2.py",
        Path('/workspace/.onboarding/environment.sh'), Path('/workspace/.onboarding/emulator-env.sh'))}
    oldrun, oldruntime = OLD / "runs" / OLD_NAME, OLD / "runtime" / OLD_NAME
    before = json.loads((oldruntime / "run-pre.json").read_text())
    original = before["command"]
    if "--cfg" in original or original[0] != "/workspace/.onboarding/assets-venv/bin/python3":
        raise RuntimeError("Unexpected baseline wrapper configuration/Python")
    if sha(oldrun / "SRB2.ELF") != ROLES["baseline"]["diagnostic"]:
        raise RuntimeError("Exact accepted-final original baseline differs")
    for p in (oldrun / "ps2args", oldrun / "boot.txt", oldrun / "summary.json", oldruntime / "run-pre.json", oldruntime / "run-post.json"):
        files[str(p)] = sha(p)
    pak = Path(original[original.index("--pak") + 1])
    assets = {p.name: sha(p) for p in pak.glob("*.PAK")}
    assets["FINEACON.DAT"] = sha(pak / "FINEACON.DAT")
    files.update({str(pak / name): digest for name, digest in assets.items()})
    if "--home-file" in original:
        raise RuntimeError("The selected MAP02 template has unexpected home overrides")
    common = {"scene": "map02-observer-control", "seed": "01b2d3f5",
        "expected_args": (oldrun / "ps2args").read_text().splitlines(), "assets": assets,
        "home_files": {}, "baseline_non_UI_settings": before["critical_settings"],
        "baseline_emulator_binary": before["emulator_binary_sha256"]}
    cases, graphs, layouts, configs = {}, {}, {}, {}
    for role, admission in ROLES.items():
        build, source, rng = admission["build"], admission["source"], admission["rng"]
        normal = build / "SRB2.ELF"
        patch = json.loads((rng / "patch-report.json").read_text())
        patch_row = patch["variants"]["01b2d3f5"]
        diagnostic = Path(patch_row["ELF"])
        if sha(normal) != admission["normal"] or sha(diagnostic) != admission["diagnostic"]:
            raise RuntimeError("Exact actual " + role + " ELF differs")
        if patch["original_inputs"]["elf"]["sha256"] != admission["normal"]:
            raise RuntimeError("Diagnostic patch has a different original")
        verify_patch(normal.read_bytes(), diagnostic.read_bytes(), patch_row["patched_file_offset"],
            patch_row["owned_symbol_bytes"], struct.pack("<10I", *stub_words(patch_row["seed_uint32"], patch_row["actual_M_RandomSeed_address"])))
        executable = Elf32(diagnostic)
        symbols = linked_symbols(executable)
        graph = {}
        for name, body in binding["variants"][role]["complete_bound_text"].items():
            address = symbols[name]["value"]
            raw = output_bytes(executable, address, body["complete_text_bytes"])
            if hashlib.sha256(raw).hexdigest() != body["resolved_text_sha256"]:
                raise RuntimeError("Exact bound actual diagnostic body differs " + role + ":" + name)
            graph[name] = {"address": address, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        graphs[role] = graph
        layouts[role] = {name: symbols[name]["value"] for name in ("_end", "_gp")}
        report = json.loads((build / "build-report.json").read_text())
        for name in report["sources"]:
            p = build / "obj" / name.replace("/", "__").replace(".c", ".o")
            files[str(p)] = sha(p)
        for name in ("ps2_hw_vu1_vsm.o", "ps2_hw_vu1_spr_vsm.o"):
            files[str(build / "obj" / name)] = sha(build / "obj" / name)
        for p in (build / "SRB2.ELF", build / "SRB2.map", build / "build-report.json", build / "obj/engine_lto.o", rng / "patch-report.json", diagnostic):
            files[str(p)] = sha(p)
        for p in (source / "src").rglob("*"):
            if p.is_file():
                files[str(p)] = sha(p)
        name = "observer-" + role + "-hw-map02-01b2d3f5-spawn-350"
        command = list(original)
        for flag, value in (("--name", name), ("--elf", str(diagnostic)), ("--out", str(out / "runs"))):
            command[command.index(flag) + 1] = value
        separator = command.index("--")
        command[separator:separator] = ["--cfg", SETTING]
        restored = command[:separator] + command[separator + 2:]
        allowed = {original.index(flag) + 1 for flag in ("--name", "--elf", "--out")}
        if len(restored) != len(original) or any(a != b and i not in allowed for i, (a, b) in enumerate(zip(original, restored))):
            raise RuntimeError("Observer command changes an unauthorized field")
        preview = out / "prepared-input-stage" / role
        opt_run.stage(preview, diagnostic, pak, None, SETTING + "\n")
        config = preview / ".srb2/reference.cfg"
        expected = b'fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\nrollingdemos "Off"\n' + SETTING.encode() + b"\n"
        if config.read_bytes() != expected or expected.count(SETTING.encode()) != 1:
            raise RuntimeError("The complete prepared observer config differs")
        configs[role] = {"path": str(config), "bytes": len(expected), "sha256": sha(config), "complete_text": expected.decode()}
        files[str(config)] = sha(config)
        cases[role] = dict(common, id=role, name=name, command=command, run=str(out / "runs" / name),
            runtime=str(out / "runtime" / name), diagnostic={"path": str(diagnostic), "sha256": admission["diagnostic"]})
    if layouts["baseline"] != layouts["candidate"] or configs["baseline"]["sha256"] != configs["candidate"]["sha256"]:
        raise RuntimeError("Matched static footprint or complete observer configs differ")
    settings = snapshot(out / "readiness-runtime", "prepared")
    if settings["critical_settings"] != before["critical_settings"] or settings["emulator_binary_sha256"] != before["emulator_binary_sha256"]:
        raise RuntimeError("Full non-UI profile or actual emulator differs from accepted baseline")
    files.update({str(p): sha(p) for p in (out / "readiness-runtime").rglob("*") if p.is_file()})
    tools = out / "pinned-tools"
    tools.mkdir()
    for path in (Path(__file__), ROOT / "tools/ps2/next13_ram_scalar_native.py", ROOT / "tools/ps2/next12_runtime.py",
                 ROOT / "tools/ps2/opt_run.py", ROOT / "tools/ps2/run_pcsx2.py"):
        shutil.copyfile(path, tools / path.name)
        files[str(tools / path.name)] = sha(tools / path.name)
    verify(files)
    plan = {"status": "READY; no emulator/compiler executed", "scope": "Fresh matched diagnostic observer-control pair. Old strict rejection remains unchanged; shipping console default5 and every source/ELF body remain unchanged.",
        "controlled_setting": SETTING, "complete_prepared_configs": configs, "matched_linker_layout": layouts,
        "console_font_evidence": {"path": str(CONSOLE), "sha256": CONSOLE_SHA}, "complete_bound_diagnostic_graphs": graphs,
        "cases": cases, "complete_immutable_input_sha256": files, "prepared_emulator_settings": settings,
        "policy": "Two fresh350-frame32MiB native MAP02 Hardware executions, exact same world arguments/inputs/local seed and common con_hudlines0 configuration. Arena/end/GP/stack/spill/eviction/flush exact; only intended allocator totals may improve. Every world/loader/lighttable/HWWS/texture/residency/dimension/format/LOD/budget/upload/PB stream remains literal. Only HW mem C-heap-use numeric accounting may decrease as preapproved. No outcome criteria change after execution; complete raw rows retained. Default-shipping/no-hwstats image gates remain separate, and matching this pair supports rather than uniquely proves the observer hypothesis."}
    (out / "readiness.json").write_text(json.dumps(plan, indent=2) + "\n")
    print(json.dumps({"out": str(out), "readiness_sha256": sha(out / "readiness.json"),
        "prepared_configs": configs, "cases": {k: v["command"] for k, v in cases.items()}}, indent=2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=WORK / "ram-observer-control-v1")
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--run-case", choices=("baseline", "candidate"))
    args = parser.parse_args()
    out = args.out.resolve()
    if args.prepare:
        prepare(out)
        return
    plan = json.loads((out / "readiness.json").read_text())
    verify(plan["complete_immutable_input_sha256"])
    case = plan["cases"][args.run_case]
    if Path(case["run"]).exists() or Path(case["runtime"]).exists():
        raise RuntimeError("Preserve actual run; use a fresh immutable destination")
    if args.run_case == "candidate" and not (Path(plan["cases"]["baseline"]["run"]) / "summary.json").is_file():
        raise RuntimeError("Run the fresh matched baseline first")
    process = run_verified(case["command"], case["runtime"],
        [Path(p) for p in plan["complete_immutable_input_sha256"]] + [out / "readiness.json"])
    if process.returncode:
        raise RuntimeError("Official observer-control wrapper failed")
    from next12_final_hardware_pressure_check import inspect_case
    item = inspect_case(case)
    boot = (Path(case["run"]) / "boot.txt").read_text()
    marker = "Number of console HUD lines is now 0"
    if marker not in boot or boot.index(marker) > boot.index("HW mem frame "):
        item["errors"].append("Actual diagnostic console setting was not acknowledged before HW mem observations")
    (out / (args.run_case + "-inspection.json")).write_text(json.dumps(item, indent=2) + "\n")
    if item["errors"]:
        raise RuntimeError("Fresh observer execution rejected: " + str(item["errors"]))
    if args.run_case == "candidate":
        result = compare(plan["cases"])
        result["readiness_sha256"] = sha(out / "readiness.json")
        result["console_font_evidence_sha256"] = CONSOLE_SHA
        result["old_strict_rejection_retained"] = True
        result["scope"] = plan["scope"]
        (out / "pair-report.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({"status": result["status"], "errors": result["errors"], "resource_deltas": result["resource_deltas"]}, indent=2))
    else:
        print("Completed fresh observer-control baseline; candidate remains unexecuted")


if __name__ == "__main__":
    main()
