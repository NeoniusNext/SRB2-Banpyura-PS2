#!/usr/bin/env python3
"""Execute the cached-mip row-stride diagnostic path with/without its fix."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from transparency_hosttest import prepare

STRIDE_OLD = """\t\tif (dce)
\t\t{
\t\t\tlvdec = 1;
\t\t\tc->dx = c->dy = 0; // the texels are decimated already
\t\t}"""
STRIDE_NEW = """\t\tif (dce)
\t\t{
\t\t\tlvdec = 1;
\t\t\tc->dx = c->dy = 0; // the texels are decimated already
\t\t\tssw = W2;
\t\t\tssh = H2; // packed cached rows have the stored level's stride
\t\t}"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, default=ROOT)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    report = {"source_root": str(a.source_root.resolve()), "scope": __doc__, "variants": {}}
    for name, fixed in (("baseline", False), ("fixed", True)):
        work = out / name
        # Repair only canonical metadata in both variants so alpha failures
        # cannot hide the independent row-stride issue under investigation.
        inputs = prepare(work, a.source_root.resolve(), True)
        path = work / "ps2_hw_tex.inc"; text = path.read_text()
        if text.count(STRIDE_OLD) == 1 and not text.count(STRIDE_NEW): baseline = text
        elif text.count(STRIDE_NEW) == 1 and not text.count(STRIDE_OLD): baseline = text.replace(STRIDE_NEW, STRIDE_OLD)
        else: raise RuntimeError("Reviewed stride block missing or ambiguous")
        path.write_text(baseline.replace(STRIDE_OLD, STRIDE_NEW) if fixed else baseline)
        results = {}
        for sanitize in (False, True):
            kind = "sanitize" if sanitize else "normal"; binary = work / kind
            flags = ["-std=gnu17", "-O2", "-DPS2_PROFILE", "-DPS2", "-DT17_NOZC", "-fwrapv", "-ffp-contract=off", "-I" + str(work)]
            if sanitize: flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
            build = subprocess.run(["gcc", *flags, str(work / "fixture.c"), "-o", str(binary), "-lm"], capture_output=True, text=True)
            (work / (kind + "-build.log")).write_text(build.stdout + build.stderr)
            if build.returncode: raise RuntimeError("Compile failed")
            r = subprocess.run([str(binary)], capture_output=True, text=True)
            log = r.stdout + r.stderr; (work / (kind + ".log")).write_text(log)
            if fixed:
                if r.returncode or "rgba_bad=0 poisoned=0" not in log or "runtime error:" in log: raise RuntimeError("Fixed stride failed: " + str(work))
            elif sanitize:
                if r.returncode == 0 or "ERROR: AddressSanitizer: heap-buffer-overflow" not in log: raise RuntimeError("Baseline sanitizer did not expose mip overread")
            elif r.returncode == 0 or "all upload bytes exactly match reference" not in log:
                raise RuntimeError("Baseline did not expose actual uploaded-pixel corruption")
            results[kind] = {"exit": r.returncode, "log_sha256": hashlib.sha256(log.encode()).hexdigest(), "last_line": log.strip().splitlines()[-1]}
        report["variants"][name] = {"source_inputs": inputs, "results": results}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__": main()
