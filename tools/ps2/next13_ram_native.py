#!/usr/bin/env python3
"""Stage/run exact accepted-final Hardware pressure clones for implicit nodes.

Preparation/reporting are read-only. --run-case consumes the coordinator's sole
emulator grant and invokes only next12_runtime -> opt_run -> run_pcsx2.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct

from next12_runtime import run_verified, snapshot
from next12_final_hardware_pressure_check import inspect_case
from next11_local_rng import verify_patch, stub_words
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import output_bytes
from next13_ram_variant import ROOT

WORK = ROOT / "build/nov15-next13"
OLD = ROOT / "build/nov15-next12/final-hardware-pressure"
BUILD = WORK / "ram-context-v1"
RNG = WORK / "ram-local-rng-v1"
SEED = "01b2d3f5"
ELF = "9b8accf19e167f6fbb4e3c55b27bbc9d11042577e4c05999fdc4e2434b5af1fc"
DIAG = "f0aa2118cf8e5fb5fd9297de4d317b6de4eadad45b79a19a213261947b704ed0"


def sha(path):
    with Path(path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def verify(files):
    for name, digest in files.items():
        if sha(name) != digest:
            raise RuntimeError("Frozen native input changed: " + name)


def prepare(out):
    out.mkdir(parents=True, exist_ok=False)
    patch = json.loads((RNG / "patch-report.json").read_text())
    row = patch["variants"][SEED]
    diagnostic = Path(row["ELF"])
    if sha(BUILD / "SRB2.ELF") != ELF or sha(diagnostic) != DIAG:
        raise RuntimeError("Exact actual candidate/diagnostic ELF differs")
    verify_patch((BUILD / "SRB2.ELF").read_bytes(), diagnostic.read_bytes(),
                 row["patched_file_offset"], row["owned_symbol_bytes"],
                 struct.pack("<10I", *stub_words(row["seed_uint32"], row["actual_M_RandomSeed_address"])))
    binding_path = WORK / "ram-binding-v2/report.json"
    binding = json.loads(binding_path.read_text())
    bodies = binding["variants"]["candidate"]["complete_bound_text"]
    exe = Elf32(diagnostic)
    symbols = linked_symbols(exe)
    graph = {}
    for name, body in bodies.items():
        at = symbols[name]["value"]
        data = output_bytes(exe, at, body["complete_text_bytes"])
        if hashlib.sha256(data).hexdigest() != body["resolved_text_sha256"]:
            raise RuntimeError("Bound full actual graph changed in RNG copy: " + name)
        graph[name] = {"address": at, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    files = {str(p): sha(p) for p in (BUILD / "SRB2.ELF", BUILD / "SRB2.map", BUILD / "build-report.json",
            BUILD / "context-report.json", BUILD / "obj/engine_lto.o", RNG / "patch-report.json", diagnostic, binding_path,
            Path(__file__), ROOT / "tools/ps2/next12_runtime.py", ROOT / "tools/ps2/opt_run.py", ROOT / "tools/ps2/run_pcsx2.py",
            Path('/workspace/.onboarding/environment.sh'), Path('/workspace/.onboarding/emulator-env.sh'))}
    context = json.loads((BUILD / "context-report.json").read_text())
    files.update(context["complete_actual_IR_objects"])
    files.update({str(BUILD / "snapshot" / name): digest for name, digest in context["complete_source_overlay_sha256"].items()})
    (out / "tools").mkdir()
    shutil.copyfile(Path(__file__), out / "tools" / Path(__file__).name)
    cases = []
    for scene in ("map02", "fog-stereo"):
        oldname = "final-hw-" + scene + "-" + SEED + "-spawn-350"
        oldrun, oldruntime = OLD / "runs" / oldname, OLD / "runtime" / oldname
        pre = json.loads((oldruntime / "run-pre.json").read_text())
        command = pre["command"]
        if sha(oldrun / "SRB2.ELF") != "6766d019d20ef841ee12604011c466231c4cb9ba213e0467ed92f313bbdfb122":
            raise RuntimeError("The actual accepted-final seed baseline differs")
        for path in [p for p in oldruntime.rglob("*") if p.is_file()] + [oldrun / n for n in ("boot.txt", "summary.json", "ps2args", "SRB2.ELF")]:
            files[str(path)] = sha(path)
        pak = Path(command[command.index("--pak") + 1])
        assets = {p.name: sha(p) for p in pak.glob("*.PAK")}
        assets["FINEACON.DAT"] = sha(pak / "FINEACON.DAT")
        home = {}
        for i, flag in enumerate(command):
            if flag == "--home-file":
                path, target = command[i + 1].rsplit("=", 1)
                home[target] = {"source": path, "sha256": sha(path)}
        for name, digest in assets.items():
            files[str(pak / name)] = digest
        files.update({row["source"]: row["sha256"] for row in home.values()})
        name = "ram-hw-" + scene + "-" + SEED + "-spawn-350"
        clone = list(command)
        for flag, value in (("--name", name), ("--elf", str(diagnostic)), ("--out", str(out / "runs"))):
            clone[clone.index(flag) + 1] = value
        allowed = {command.index(flag) + 1 for flag in ("--name", "--elf", "--out")}
        if len(clone) != len(command) or any(x != y and i not in allowed for i, (x, y) in enumerate(zip(command, clone))):
            raise RuntimeError("An exact baseline command field changed")
        common = {"scene": scene, "seed": SEED, "expected_args": (oldrun / "ps2args").read_text().splitlines(),
                  "assets": assets, "home_files": home, "baseline_non_UI_settings": pre["critical_settings"],
                  "baseline_emulator_binary": pre["emulator_binary_sha256"]}
        baseline = dict(common, id="baseline-" + scene, name=oldname, run=str(oldrun), runtime=str(oldruntime),
                        command=command, diagnostic={"path": str(oldrun / "SRB2.ELF"), "sha256": sha(oldrun / "SRB2.ELF")})
        candidate = dict(common, id=scene, name=name, run=str(out / "runs" / name), runtime=str(out / "runtime" / name),
                         command=clone, diagnostic={"path": str(diagnostic), "sha256": DIAG})
        cases.append({"baseline": baseline, "candidate": candidate})
    settings = snapshot(out / "readiness-runtime", "prepared")
    for pair in cases:
        case = pair["candidate"]
        if settings["critical_settings"] != case["baseline_non_UI_settings"] or settings["emulator_binary_sha256"] != case["baseline_emulator_binary"]:
            raise RuntimeError("Actual complete non-UI emulator settings differ from accepted baseline")
    files.update({str(p): sha(p) for p in (out / "readiness-runtime").rglob("*") if p.is_file()})
    verify(files)
    plan = {"cases": cases, "complete_immutable_input_sha256": files, "diagnostic_full_graph": graph,
            "shipping_ELF_sha256": ELF, "diagnostic_ELF_sha256": DIAG,
            "initializer_only_patch": {"file_offset": row["patched_file_offset"], "bytes": row["owned_symbol_bytes"]},
            "policy": "Exact physical RAM/arena/frame counts and stack high-water; no increase in used/peak/global-peak/libcpeak/spill/evictions/failures/flushes; free/largest/libcfree nondecreasing. Intended removed node storage can save libc or spill/zone bytes; all outcomes and per-tag deltas retained. All ordered geometry/loader/lighttable/resident streams, complete driver geometry/upload/VRAM/LOD/budget tails and PB output/capacity counters exact. Any regression stops further scenes. No reserve/padding/capacity/quality changes."}
    (out / "readiness.json").write_text(json.dumps(plan, indent=2) + "\n")
    return plan


def compare(pair):
    a, b = (inspect_case(pair[role]) for role in ("baseline", "candidate"))
    errors = a["errors"] + b["errors"]
    x, y = a["ZSTAT"], b["ZSTAT"]
    delta = {k: y[k] - v for k, v in x.items() if k in y}
    if set(x) != set(y):
        errors.append("complete resource field set differs")
    for key in ("ram", "class", "map", "levelframes", "frames", "arena", "stackused", "flushes"):
        if delta.get(key) != 0:
            errors.append("required exact field differs: " + key)
    for key in ("used", "peak", "gpeak", "libcpeak", "spillpeak", "spillnow", "evicted", "evictedbytes", "failed"):
        if delta.get(key, 1) > 0:
            errors.append("allocation/cache field regressed: " + key)
    for key in ("free", "largest", "libcfree"):
        if delta.get(key, -1) < 0:
            errors.append("available capacity regressed: " + key)
    if delta.get("free", 0) + delta.get("libcfree", 0) <= 0:
        errors.append("actual net available memory saving absent")
    streams = {}
    for prefix, left in a["ordered_streams"].items():
        right = b["ordered_streams"][prefix]
        streams[prefix] = {"rows_each": [len(left), len(right)], "equal": bool(left) and left == right}
        if not streams[prefix]["equal"]:
            errors.append("ordered complete stream differs: " + prefix)
    if len(a["ordered_streams"]["HWPH "]) != 407 or len(b["ordered_streams"]["HWPH "]) != 407:
        errors.append("complete407 geometry rows absent")
    tails = [[line.partition(" | polys=")[2] for line in item["all_Hardware_rows"].get("HWPROF", []) if line.startswith("HWPROF win=")]
             for item in (a, b)]
    if not tails[0] or tails[0] != tails[1]:
        errors.append("complete driver geometry/upload/VRAM/LOD/budget tails differ")
    pattern = r"polygons=(\d+) in (\d+) batches \((\d+) buckets\).*batches drawn=(\d+).*planned polygons=(\d+).*pool (\d+) quadwords, cut for room=(\d+)"
    pb = [[re.search(pattern, line).groups() for line in item["all_Hardware_rows"].get("HWPROF50", []) if line.startswith("HWPROF50 collection:")]
          for item in (a, b)]
    if not pb[0] or pb[0] != pb[1]:
        errors.append("complete PB output/capacity counters differ")
    tags = {str(n): {k: b["all_tags"].get(n, {}).get(k, 0) - a["all_tags"].get(n, {}).get(k, 0)
                   for k in ("blocks", "allocated", "payload")} for n in set(a["all_tags"]) | set(b["all_tags"])}
    return {"status": "pass" if not errors else "reject", "errors": errors, "resource_deltas": delta,
            "baseline": a, "candidate": b, "ordered_stream_equality": streams, "complete_driver_tails": tails,
            "complete_PB_counters": pb, "tag_deltas": tags}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=WORK / "ram-native-v1")
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--run-case", choices=("map02", "fog-stereo"))
    args = parser.parse_args()
    out = args.out.resolve()
    if args.prepare:
        prepare(out)
        print("Prepared exact accepted-final pressure clones: " + str(out))
        return
    plan = json.loads((out / "readiness.json").read_text())
    verify(plan["complete_immutable_input_sha256"])
    pair = next(row for row in plan["cases"] if row["candidate"]["id"] == args.run_case)
    if args.run_case == "fog-stereo":
        first = json.loads((out / "map02-report.json").read_text())
        if first["errors"]:
            raise RuntimeError("First scene rejected; no second execution authorized by this gate")
    case = pair["candidate"]
    if Path(case["run"]).exists() or Path(case["runtime"]).exists():
        raise RuntimeError("Use a fresh immutable runtime destination")
    proc = run_verified(case["command"], case["runtime"],
                        [Path(p) for p in plan["complete_immutable_input_sha256"]] + [out / "readiness.json"])
    if proc.returncode:
        raise RuntimeError("Official pressure wrapper failed")
    result = compare(pair)
    result["readiness_sha256"] = sha(out / "readiness.json")
    result["tool_sha256"] = sha(Path(__file__))
    (out / (args.run_case + "-report.json")).write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"scene": args.run_case, "status": result["status"], "errors": result["errors"],
                      "resource_deltas": result["resource_deltas"]}, indent=2))


if __name__ == "__main__":
    main()
