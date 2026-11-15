"""Check the imported pass13 ROOT GS driver against published pass12 results.

The historical VU0 failure remains a failure: the underlying suite returns
nonzero. This orchestrator accepts only its exact unchanged signature and
checks every CT32 readback against the preceding published full-image hash.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from next12_runtime import run_verified

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
KNOWN_VU0_FAILURE = "H0 FAIL vu0_transform 49152 vertices, 24625 outcode differences, max dX 0 dY 0 dZ 0, max 1/w relative error 0"
EXPECTED_CHECKS = {"ct32": 56, "ct16": 51}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def suite_inputs(out, baseline_validation):
    report_path = out / "hwt/build-report.json"
    report = json.loads(report_path.read_text())
    elf = out / "HW_TEST.ELF"
    if (report["elf_sha256"] != sha(elf) or report["compile_diagnostics"]
            or report["link_diagnostics_excluding_sdk_abi_warning"]):
        raise RuntimeError("GS fixture build hash or diagnostics differ before execution")
    inputs = {elf, report_path, Path(__file__), TOOLS / "run_hw_test.py",
              TOOLS / "gs_list_eetest.py", baseline_validation}
    for name, digest in report["input_sha256"].items():
        path = Path(name)
        path = path if path.is_absolute() else ROOT / path
        if sha(path) != digest:
            raise RuntimeError("GS build input changed before execution: " + str(path))
        inputs.add(path)
    # The compiler's dependency files also identify transitive production/SDK
    # headers beyond the builder's explicit GS-source inventory.
    for path in (out / "hwt").glob("*.d"):
        inputs.add(path)
        primary = path.read_text().replace("\\\n", " ").splitlines()[0]
        if ": " not in primary:
            raise RuntimeError("Malformed GS compile dependency file: " + str(path))
        for name in shlex.split(primary.split(": ", 1)[1]):
            dependency = Path(name)
            inputs.add(dependency if dependency.is_absolute() else ROOT / dependency)
    return inputs


def run_suite(command, runtime_out, inputs, selected_out):
    # run_verified records hashes and configurations; retain actual input bytes
    # too, since run_hw_test executes HW_TEST.ELF in place rather than staging it.
    if (runtime_out / "run-pre.json").exists():
        raise RuntimeError("GS runtime evidence exists; choose a fresh output directory")
    files = {path.resolve() for path in inputs}
    files.update((TOOLS / name).resolve() for name in (
        "next12_runtime.py", "next10_gs_run.py", "opt_run.py", "run_pcsx2.py"))
    files.add(Path("/workspace/.onboarding/emulator-env.sh"))
    files.update(path.resolve() for path in tuple(files))
    archived = {}
    for path in sorted(files):
        relative = (path.relative_to(ROOT) if path.is_relative_to(ROOT)
                    else Path("external") / str(path).lstrip("/"))
        dest = runtime_out / "pinned-inputs" / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
        if sha(path) != sha(dest):
            raise RuntimeError("GS runtime input changed while archiving: " + str(path))
        archived[str(path)] = {"archive": str(dest.relative_to(runtime_out)), "sha256": sha(dest)}
    (runtime_out / "pinned-inputs.json").write_text(json.dumps(archived, indent=2) + "\n")
    prior_out = os.environ.get("SRB2_PS2_OUT")
    os.environ["SRB2_PS2_OUT"] = str(selected_out)
    try:
        process = run_verified(command, runtime_out, files)
    finally:
        if prior_out is None:
            os.environ.pop("SRB2_PS2_OUT", None)
        else:
            os.environ["SRB2_PS2_OUT"] = prior_out
    before = json.loads((runtime_out / "run-pre.json").read_text())["input_sha256"]
    if before != {name: item["sha256"] for name, item in archived.items()}:
        raise RuntimeError("Archived GS input bytes differ from the actual run inputs")
    return process


def check_runtime(runtime_out, command, rc, orchestration):
    pre = json.loads((runtime_out / "run-pre.json").read_text())
    post = json.loads((runtime_out / "run-post.json").read_text())
    archived = json.loads((runtime_out / "pinned-inputs.json").read_text())
    expected = list(map(str, command))
    recorded = pre["command"]
    if (recorded != post["command"] or recorded[1:] != expected[1:]
            or Path(recorded[0]).resolve() != Path(expected[0]).resolve()
            or post["rc"] != rc or rc != 1):
        raise RuntimeError("GS runtime command or historical failure return differs")
    if (pre["input_sha256"] != post["input_sha256"]
            or pre["input_sha256"] != {name: item["sha256"] for name, item in archived.items()}
            or pre["critical_settings"] != post["critical_settings"]
            or pre["emulator_binary_sha256"] != post["emulator_binary_sha256"]
            or not all(post[key] for key in (
                "all_frozen_inputs_unchanged", "all_non_UI_settings_unchanged", "emulator_binary_unchanged"))):
        raise RuntimeError("GS runtime input/configuration/binary invariance failed")
    if (sha(runtime_out / "orchestration.log") != post["orchestration_sha256"]
            or sha(orchestration) != post["orchestration_sha256"]):
        raise RuntimeError("GS orchestration output differs from its runtime record")
    for item in archived.values():
        if sha(runtime_out / item["archive"]) != item["sha256"]:
            raise RuntimeError("Archived GS runtime input bytes changed")
    return {"rc": rc, "historical_failure_return_preserved": True,
            "input_files": len(archived), "all_input_bytes_archived": True,
            "all_runtime_invariance_checks_pass": True,
            "pre_sha256": sha(runtime_out / "run-pre.json"),
            "post_sha256": sha(runtime_out / "run-post.json")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov15-next13/gs-integration")
    parser.add_argument("--baseline-validation", type=Path,
                        default=ROOT / "prebuilt/lossless-next12-20261115/validation.json")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--analyze-only", action="store_true")
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / "build") or out == ROOT / "build":
        parser.error("--out must be a child of the repository build directory")
    out.mkdir(parents=True, exist_ok=True)
    baseline = json.loads(args.baseline_validation.read_text())
    reference_gs = baseline.get("standard_GS", baseline.get("standard_gs"))
    reference_suite = reference_gs.get("suite", reference_gs)
    if any(reference_suite[tag]["rc"] != 1 for tag in ("ct32", "ct16")):
        raise RuntimeError("Baseline must retain both historical GS failure returns of one")
    for tag, count in EXPECTED_CHECKS.items():
        expected = {"rc": 1, "complete": [f"H0 COMPLETE checks={count} failures=1"],
                    "fail": [KNOWN_VU0_FAILURE]}
        if reference_suite[tag] != expected:
            raise RuntimeError("Published pass12 GS signature differs: " + tag)
    reference_images = baseline.get("gs_readback_sha256", reference_gs.get("readback_sha256"))
    environment = dict(os.environ, SRB2_PS2_OUT=str(out))
    if not args.no_build and not args.analyze_only:
        subprocess.run([sys.executable, "-B", str(TOOLS / "gs_list_eetest.py")],
                       cwd=ROOT, env=environment, check=True)
    inputs = suite_inputs(out, args.baseline_validation.resolve())
    report = {"suite": {}, "runtime": {}, "errors": [], "all_standard_checks_pass": False,
              "known_VU0_failure": KNOWN_VU0_FAILURE, "observed_pass_counts": {},
              "scope": "Standard default GS regression against pass12; the historical VU0 failure remains. No physical GS/FPS claim."}
    for tag, test_args in (("ct32", ["quick", "dump"]), ("ct16", ["fb16", "quick"])):
        command = [sys.executable, "-B", str(TOOLS / "run_hw_test.py"),
                   "--no-build", "--tag", "next13-" + tag,
                   "--timeout", "180", "--", *test_args]
        if not args.analyze_only:
            process = run_suite(command, out / "runtime" / tag, inputs, out)
            (out / (tag + "-orchestration.log")).write_text(process.stdout + process.stderr)
            (out / (tag + "-orchestration.json")).write_text(json.dumps({"rc": process.returncode}) + "\n")
        wrapper = json.loads((out / ("hwt-next13-" + tag + ".json")).read_text())
        rc = json.loads((out / (tag + "-orchestration.json")).read_text())["rc"]
        report["runtime"][tag] = check_runtime(out / "runtime" / tag, command, rc,
                                               out / (tag + "-orchestration.log"))
        observed = {"rc": rc, "complete": wrapper["complete"], "fail": wrapper["fail"]}
        if rc != 1:
            report["errors"].append(tag + ": historical underlying suite return must remain one")
        if observed != reference_suite[tag] or wrapper["wrapper_exit"]:
            report["errors"].append(tag + ": standard suite differs from the exact preceding result")
        report["observed_pass_counts"][tag] = len(wrapper["pass"])
        if len(wrapper["pass"]) != EXPECTED_CHECKS[tag] - 1:
            report["errors"].append(tag + ": complete historical passing-check count differs")
        if wrapper["elf_sha256"] != hashlib.sha256((out / "HW_TEST.ELF").read_bytes()).hexdigest():
            report["errors"].append(tag + ": tested driver ELF differs")
        if ("--elf" not in wrapper["command"] or
                Path(wrapper["command"][wrapper["command"].index("--elf") + 1]).resolve() != out / "HW_TEST.ELF"):
            report["errors"].append(tag + ": underlying runner selected a different driver ELF")
        if wrapper["command"][-2:] != ["--args", " ".join(test_args)]:
            report["errors"].append(tag + ": suite arguments differ")
        report["suite"][tag] = observed
    expected = reference_images
    images = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
              for path in sorted(out.glob("hw-*-32.ppm"))}
    if len(expected) != 17 or images != expected:
        report["errors"].append("complete CT32 readback image hashes differ")
    report["readback_sha256"] = images
    report["readback_captures"] = len(images)
    build_report = json.loads((out / "hwt/build-report.json").read_text())
    report["build"] = build_report
    report["baseline_validation_sha256"] = hashlib.sha256(args.baseline_validation.read_bytes()).hexdigest()
    for source, digest in build_report["input_sha256"].items():
        if hashlib.sha256((ROOT / source).read_bytes()).hexdigest() != digest:
            report["errors"].append("driver build input changed: " + source)
    if build_report["compile_diagnostics"] or build_report["link_diagnostics_excluding_sdk_abi_warning"]:
        report["errors"].append("GS driver build diagnostics")
    if build_report["elf_sha256"] != hashlib.sha256((out / "HW_TEST.ELF").read_bytes()).hexdigest():
        report["errors"].append("GS driver ELF differs from its build report")
    report["historical_failure_unchanged"] = not report["errors"]
    (out / "gs-integration-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ("build", "readback_sha256")}, indent=2))
    return bool(report["errors"])


if __name__ == "__main__":
    raise SystemExit(main())
