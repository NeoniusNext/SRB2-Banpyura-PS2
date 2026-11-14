"""Check the real GS driver against the preceding recorded suite and images.

The historical VU0 failure remains a failure: the underlying suite returns
nonzero. This orchestrator accepts only its exact unchanged signature and
checks every CT32 readback against the preceding published full-image hash.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov15-next9/gs-integration")
    parser.add_argument("--baseline-validation", type=Path,
                        default=ROOT / "prebuilt/lossless-next8-20261115/validation.json")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--analyze-only", action="store_true")
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / "build") or out == ROOT / "build":
        parser.error("--out must be a child of the repository build directory")
    out.mkdir(parents=True, exist_ok=True)
    baseline = json.loads(args.baseline_validation.read_text())
    reference_gs = baseline["standard_gs"]
    reference_suite = reference_gs.get("suite", reference_gs)
    reference_images = baseline.get("gs_readback_sha256", reference_gs.get("readback_sha256"))
    environment = dict(os.environ, SRB2_PS2_OUT=str(out))
    if not args.no_build and not args.analyze_only:
        subprocess.run([sys.executable, "-B", str(TOOLS / "gs_list_eetest.py")],
                       cwd=ROOT, env=environment, check=True)
    report = {"suite": {}, "errors": [], "all_standard_checks_pass": False}
    for tag, test_args in (("ct32", ["quick", "dump"]), ("ct16", ["fb16", "quick"])):
        if not args.analyze_only:
            process = subprocess.run([sys.executable, "-B", str(TOOLS / "run_hw_test.py"),
                                      "--no-build", "--tag", "next9-" + tag,
                                      "--timeout", "180", "--", *test_args],
                                     cwd=ROOT, env=environment, text=True, capture_output=True)
            (out / (tag + "-orchestration.log")).write_text(process.stdout + process.stderr)
            (out / (tag + "-orchestration.json")).write_text(json.dumps({"rc": process.returncode}) + "\n")
        wrapper = json.loads((out / ("hwt-next9-" + tag + ".json")).read_text())
        rc = json.loads((out / (tag + "-orchestration.json")).read_text())["rc"]
        observed = {"rc": rc, "complete": wrapper["complete"], "fail": wrapper["fail"]}
        if observed != reference_suite[tag] or wrapper["wrapper_exit"]:
            report["errors"].append(tag + ": standard suite differs from the exact preceding result")
        if wrapper["elf_sha256"] != hashlib.sha256((out / "HW_TEST.ELF").read_bytes()).hexdigest():
            report["errors"].append(tag + ": tested driver ELF differs")
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
