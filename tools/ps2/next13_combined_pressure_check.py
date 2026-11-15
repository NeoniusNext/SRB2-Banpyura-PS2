#!/usr/bin/env python3
"""Freeze/replay the combined flat/DN native pressure gates without execution.

Consumes existing official-run readiness/runtimes. No compiler or emulator is
invoked. The resource policy must be frozen before the candidate game starts.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil

from next12_final_hardware_pressure_check import inspect_case
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def quality(a, b):
    errors, streams = [], {}
    for prefix, left in a["ordered_streams"].items():
        right = b["ordered_streams"].get(prefix, [])
        if prefix == "HW mem ":
            rows, equal = [], bool(left) and len(left) == len(right)
            for old, new in zip(left, right):
                pattern = r"^(.*; C heap in use )(\d+)(K; driver textures \d+)$"
                x, y = re.fullmatch(pattern, old), re.fullmatch(pattern, new)
                same = bool(x and y and x[1] == y[1] and x[3] == y[3])
                permitted = same and int(y[2]) <= int(x[2])
                equal = equal and permitted
                rows.append({"baseline": old, "candidate": new, "all_nonaccounting_fields_literal_equal": same,
                    "C_heap_K_delta": int(y[2]) - int(x[2]) if same else None, "pass": permitted})
            streams[prefix] = {"pass": equal, "rows": rows, "literal_equal": left == right}
        else:
            equal = bool(left) and left == right
            streams[prefix] = {"pass": equal, "rows_each": [len(left), len(right)],
                "baseline_sha256": hashlib.sha256(("\n".join(left) + "\n").encode()).hexdigest(),
                "candidate_sha256": hashlib.sha256(("\n".join(right) + "\n").encode()).hexdigest()}
        if not equal:
            errors.append("full ordered stream differs: " + prefix)
    if len(a["ordered_streams"]["HWPH "]) != 407 or len(b["ordered_streams"]["HWPH "]) != 407:
        errors.append("complete407 canonical Hardware polygon rows absent")
    tails = [[line.partition(" | polys=")[2] for line in item["all_Hardware_rows"].get("HWPROF", [])
              if line.startswith("HWPROF win=")] for item in (a, b)]
    if not tails[0] or tails[0] != tails[1]:
        errors.append("full driver geometry/upload/VRAM/LOD/budget/capacity tails differ")
    pattern = r"polygons=(\d+) in (\d+) batches \((\d+) buckets\).*batches drawn=(\d+).*planned polygons=(\d+).*pool (\d+) quadwords, cut for room=(\d+)"
    pb = [[re.search(pattern, line).groups() for line in item["all_Hardware_rows"].get("HWPROF50", [])
           if line.startswith("HWPROF50 collection:")] for item in (a, b)]
    if not pb[0] or pb[0] != pb[1]:
        errors.append("full PB logical/capacity/output counters differ")
    return {"errors": errors, "ordered_streams": streams, "complete_driver_tails": tails, "complete_PB_counters": pb}


def self_test():
    path = ROOT / "build/nov15-next13/ram-observer-control-v1/pair-report.json"
    old = json.loads(path.read_text())
    a, b = old["baseline"], old["candidate"]
    if quality(a, b)["errors"]:
        raise RuntimeError("Existing actual observer quality oracle does not pass")
    rows = []
    for prefix in ("LHASH ", "HWPH ", "HWLT ", "HWWS ", "[zreg]"):
        bad = copy.deepcopy(b)
        bad["ordered_streams"][prefix][0] += " MUTATED"
        if not quality(a, bad)["errors"]:
            raise RuntimeError("Quality stream mutation escaped " + prefix)
        rows.append(prefix)
    for field in ("driver textures ", "C heap in use "):
        bad = copy.deepcopy(b)
        line = bad["ordered_streams"]["HW mem "][0]
        match = re.search(re.escape(field) + r"(\d+)", line)
        bad["ordered_streams"]["HW mem "][0] = line[:match.start(1)] + str(int(match[1]) + 100000) + line[match.end(1):]
        if not quality(a, bad)["errors"]:
            raise RuntimeError("HW mem mutation escaped " + field)
        rows.append(field)
    for prefix in ("HWPROF", "HWPROF50"):
        bad = copy.deepcopy(b)
        lines = bad["all_Hardware_rows"][prefix]
        index = next(i for i, line in enumerate(lines) if line.startswith("HWPROF win=" if prefix == "HWPROF" else "HWPROF50 collection:"))
        if prefix == "HWPROF":
            lines[index] = lines[index].replace(" | polys=", " | polys=9", 1)
        else:
            lines[index] = re.sub(r"planned polygons=(\d+)", lambda m: "planned polygons=" + str(int(m[1]) + 1), lines[index], count=1)
        if not quality(a, bad)["errors"]:
            raise RuntimeError("Driver/PB counter mutation escaped")
        rows.append(prefix)
    return {"actual_positive_oracle": str(path), "oracle_sha256": sha(path), "executed_negative_controls": rows}


def freeze(args):
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    plan = json.loads(args.readiness.read_text())
    policy = json.loads(args.resource_policy.read_text())
    if policy["Map02_expected_catalogue_N"] != 4740 or policy["Map02_required_actual_flat_payload_saving"] != 37920:
        raise RuntimeError("The predeclared master flat witness differs")
    cases = {case["id"]: case for case in plan["cases"]}
    for name in (args.baseline_id, args.candidate_id):
        if name not in cases or cases[name]["mode"] != "default":
            raise RuntimeError("Select the two default combined pressure cases")
    if cases[args.baseline_id]["role"] != "baseline" or cases[args.candidate_id]["role"] != "candidate":
        raise RuntimeError("Selected native roles differ")
    if plan["config_addition"] != 'con_hudlines "0"':
        raise RuntimeError("Matched predeclared observer control absent")
    if args.expected_flat_count not in (policy["Map02_expected_catalogue_N"], policy["fog_expected_catalogue_N"]):
        raise RuntimeError("Expected N must be one of the independently predeclared scene counts")
    tools = (Path(__file__), ROOT / "tools/ps2/next12_final_hardware_pressure_check.py",
             ROOT / "tools/ps2/next11_smoke_check.py", ROOT / "tools/ps2/next11_integration_check.py",
             ROOT / "tools/ps2/next10_gs_run.py")
    pinned = out / "pinned-tools"
    pinned.mkdir()
    for path in tools:
        shutil.copyfile(path, pinned / path.name)
    manifest = {"status": "FROZEN before candidate execution; no emulator/compiler invoked",
        "readiness": str(args.readiness.resolve()), "readiness_sha256": sha(args.readiness),
        "resource_policy": str(args.resource_policy.resolve()), "resource_policy_sha256": sha(args.resource_policy),
        "baseline_id": args.baseline_id, "candidate_id": args.candidate_id, "expected_flat_count": args.expected_flat_count,
        "required_flat_payload_reduction": args.expected_flat_count * 8,
        "checker_sha256": sha(Path(__file__)), "helper_sha256": {str(p): sha(p) for p in tools},
        "self_test": self_test(), "rules": {
            "exact_ZSTAT_fields": policy["exact_ZSTAT_fields"], "arena_end_gp_stack_evictions_flushes": "exact",
            "libcfree": "nondecreasing", "libcpeak_failed_spillpeak": "nonincreasing",
            "spillnow": "nonincreasing; report literal residual after the source-bound flat saving, without inventing another block origin",
            "used_peak_gpeak": "each strictly decreases; full allocated/header/alignment residual retained",
            "zone_free_largest": "nondecreasing", "tag_blocks": "all exact",
            "tag_payload": "STATIC decreases by8*N; every other payload exact",
            "all_quality_cache_LOD_upload_streams": "literal as master policy; C-heap accounting may decrease only",
            "DN_memory": "measure libc fields; no assumed additive4624 or8192 peak benefit",
            "no_peak_attribution_without_trace": True}}
    (out / "policy-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"policy_manifest": str(out / "policy-manifest.json"), "sha256": sha(out / "policy-manifest.json"),
        "required_flat_payload_reduction": manifest["required_flat_payload_reduction"], "negative_controls": len(manifest["self_test"]["executed_negative_controls"])}, indent=2))


def normalize(case, plan):
    case = dict(case)
    case["expected_args"] = case["expected_ps2args"]
    admission = plan["admissions"][case["role"]]
    case["diagnostic"] = {"path": admission["diagnostic_ELF"], "sha256": admission["diagnostic_ELF_sha256"]}
    case["baseline_non_UI_settings"] = plan["settings"]["critical_settings"]
    case["baseline_emulator_binary"] = plan["settings"]["emulator_binary_sha256"]
    immutable = plan["immutable_input_sha256"]
    command = case["command"]
    pak = Path(command[command.index("--pak") + 1])
    case["assets"] = {p.name: immutable[str(p.resolve())] for p in [*pak.glob("*.PAK"), pak / "FINEACON.DAT"]}
    home = {}
    for i, flag in enumerate(command):
        if flag == "--home-file":
            source, target = command[i + 1].rsplit("=", 1)
            home[target] = {"source": source, "sha256": immutable[str(Path(source).resolve())]}
    case["home_files"] = home
    return case


def actual(case, plan, expected_n):
    item = inspect_case(normalize(case, plan))
    boot = (Path(case["run"]) / "boot.txt").read_text()
    marker = "Number of console HUD lines is now 0"
    if marker not in boot or boot.index(marker) > boot.index("HW mem frame "):
        item["errors"].append("Observer control unacknowledged before the first hardware observation")
    if case["home_files"] if "home_files" in case else "--home-file" in case["command"]:
        if not all(s in boot for s in ("NEXT5 NPOT SPRITES READY", "NEXT7 FOG SOLID READY", "NEXT5 STEREO SFX event=4")):
            item["errors"].append("Complete fog/NPOT/stereo scene markers absent")
    rows = [{"line": i + 1, "raw": line, "bytes": int(m[1]), "now": int(m[2]), "ra": m[3]}
        for i, line in enumerate(boot.splitlines()) if (m := re.fullmatch(r"\[spill\] (\d+) bytes \(now (\d+)\) ra=([0-9a-fA-F]{8})", line))]
    stride = 84 if case["role"] == "baseline" else 76
    pairs = []
    for x, y in zip(rows, rows[1:]):
        if x["bytes"] == expected_n * 84 and y["bytes"] == expected_n * stride and y["line"] == x["line"] + 1:
            if y["now"] - x["now"] != y["bytes"]:
                item["errors"].append("Actual flat ZA cumulative payload increment differs")
            pairs.append({"wall": x, "flat": y, "actual_N_from_full_wall_request": x["bytes"] // 84})
    if not pairs:
        item["errors"].append("Source-bound adjacent full-N wall/flat ZA request witness absent")
    item["all_raw_spill_rows"] = rows
    item["actual_constructor_pairs"] = pairs
    return item


def analyze(args):
    path = args.policy_manifest.resolve()
    manifest = json.loads(path.read_text())
    if sha(Path(__file__)) != manifest["checker_sha256"]:
        raise RuntimeError("Frozen checker changed")
    for name, digest in manifest["helper_sha256"].items():
        if sha(name) != digest:
            raise RuntimeError("Frozen helper changed")
    readiness, resource = Path(manifest["readiness"]), Path(manifest["resource_policy"])
    if sha(readiness) != manifest["readiness_sha256"] or sha(resource) != manifest["resource_policy_sha256"]:
        raise RuntimeError("Predeclared readiness/resource policy changed")
    plan = json.loads(readiness.read_text())
    for name, digest in plan["immutable_input_sha256"].items():
        if sha(name) != digest:
            raise RuntimeError("Frozen actual pressure input changed: " + name)
    cases = {case["id"]: case for case in plan["cases"]}
    a = actual(cases[manifest["baseline_id"]], plan, manifest["expected_flat_count"])
    b = actual(cases[manifest["candidate_id"]], plan, manifest["expected_flat_count"])
    result = quality(a, b)
    errors = a["errors"] + b["errors"] + result["errors"]
    old, new = a["ZSTAT"], b["ZSTAT"]
    delta = {k: new[k] - v for k, v in old.items() if k in new}
    if set(old) != set(new):
        errors.append("Complete ZSTAT field set differs")
    for key in manifest["rules"]["exact_ZSTAT_fields"]:
        if delta.get(key) != 0:
            errors.append("Required exact resource field differs: " + key)
    for key in ("libcpeak", "failed", "spillpeak", "spillnow"):
        if delta.get(key, 1) > 0:
            errors.append("Resource field worsened: " + key)
    for key in ("libcfree", "free", "largest"):
        if delta.get(key, -1) < 0:
            errors.append("Available capacity decreased: " + key)
    for key in ("used", "peak", "gpeak"):
        if delta.get(key, 0) >= 0:
            errors.append("Actual allocated-byte reduction absent: " + key)
    layouts = {role: {n: linked_symbols(Elf32(Path(row["build"]) / "SRB2.ELF"))[n]["value"] for n in ("_end", "_gp")}
               for role, row in plan["admissions"].items()}
    if layouts["baseline"] != layouts["candidate"]:
        errors.append("Actual whole-ELF _end/_gp differs")
    saving = manifest["required_flat_payload_reduction"]
    tags = {}
    for n in sorted(set(a["all_tags"]) | set(b["all_tags"])):
        x, y = a["all_tags"].get(n, {}), b["all_tags"].get(n, {})
        d = {k: y.get(k, 0) - x.get(k, 0) for k in ("blocks", "allocated", "payload")}
        tags[str(n)] = d
        if d["blocks"]:
            errors.append("Tag block residency changed: " + str(n))
        if d["payload"] != (-saving if n == 1 else 0):
            errors.append("Unexpected logical tag payload change: " + str(n))
    if 1 not in a["all_tags"] or 1 not in b["all_tags"]:
        errors.append("Actual STATIC tag witness absent")
    if len(a["actual_constructor_pairs"]) != len(b["actual_constructor_pairs"]):
        errors.append("Constructor request-pair count changed")
    report = {"status": "pass" if not errors else "reject", "errors": errors, "policy_manifest_sha256": sha(path),
        "resource_policy_sha256": manifest["resource_policy_sha256"], "all_resource_deltas": delta, "baseline": a, "candidate": b,
        "quality": result, "actual_layouts": layouts, "all_tag_deltas": tags, "required_flat_payload_saving": saving,
        "actual_allocated_byte_reduction": -delta["used"], "allocated_residual_after_flat_payload": -delta["used"] - saving,
        "spillnow_residual_after_flat_payload": delta["spillnow"] + saving,
        "spillpeak_scope": "Nonincreasing measured peak; no exact per-block cause assigned without a complete peak/live-block trace",
        "DN_scope": "Measured libc totals only; component4624/8192 observations are not assumed additive",
        "timing_scope": "All raw timings retained; no FPS claim", "no_compiler_or_emulator_invoked": True}
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("status", "errors", "all_resource_deltas", "required_flat_payload_saving",
        "actual_allocated_byte_reduction", "allocated_residual_after_flat_payload", "spillnow_residual_after_flat_payload")}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeze", action="store_true")
    parser.add_argument("--readiness", type=Path)
    parser.add_argument("--resource-policy", type=Path)
    parser.add_argument("--policy-manifest", type=Path)
    parser.add_argument("--baseline-id", default="baseline-default")
    parser.add_argument("--candidate-id", default="candidate-default")
    parser.add_argument("--expected-flat-count", type=int, default=4740)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.freeze:
        if not args.readiness or not args.resource_policy:
            parser.error("--freeze requires readiness and resource policy")
        freeze(args)
    else:
        if not args.policy_manifest:
            parser.error("Analysis requires the pre-execution frozen policy manifest")
        analyze(args)


if __name__ == "__main__":
    main()
