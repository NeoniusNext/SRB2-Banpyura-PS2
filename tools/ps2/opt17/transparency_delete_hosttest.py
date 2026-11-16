#!/usr/bin/env python3
"""Prove DeleteTexture view/fence behavior and reused-key source invalidation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent; ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE)); from transparency_hosttest import prepare

DELETE_SUFFIX = """\tTexInfo->downloaded = 0;
}
"""
DELETE_NEW_SUFFIX = """\tTexInfo->downloaded = 0;
\tif (H.up && TexInfo->format == GL_TEXFMT_P_8 && (TexInfo->regen_kind == 1 || TexInfo->regen_kind == 2) && !TexInfo->ps2_twin)
\t{
\t\tint i;

\t\t// The engine deletes the child variant before the canonical object.
\t\t// A new object under this number must not inherit its cached source.
\t\tTexInfo->ps2_h255 = 0;
\t\tfor (i = 0; i < DC_MAX; i++)
\t\t\tif (dc[i].key == dc_key(TexInfo))
\t\t\t\tdc_drop(&dc[i]);
\t}
}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument("--source-root", type=Path, default=ROOT); ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(); source = a.source_root.resolve(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    driver = (source / "src/ps2/hw/ps2_hwd.c").read_text(); m = re.search(r"static void hw_DeleteTexture\(GLMipmap_t \*TexInfo\)\n\{.*?^\}", driver, re.M | re.S)
    if not m: raise RuntimeError("Actual DeleteTexture anchor changed")
    observed = m.group() + "\n"
    if observed.count(DELETE_NEW_SUFFIX) == 1: original = observed.replace(DELETE_NEW_SUFFIX, DELETE_SUFFIX)
    elif observed.count(DELETE_SUFFIX) == 1: original = observed
    else: raise RuntimeError("Reviewed DeleteTexture invalidation block missing or ambiguous")
    corrected = original.replace(DELETE_SUFFIX, DELETE_NEW_SUFFIX)
    report = {"scope": __doc__, "actual_delete_sha256": hashlib.sha256(original.encode()).hexdigest(), "fixture_sha256": hashlib.sha256((HERE / "transparency_delete_hosttest.c").read_bytes()).hexdigest(), "variants": {}}
    for name, fixed in (("baseline", False), ("fixed", True)):
        work = out / name; inputs = prepare(work, source, True)
        base = (HERE / "transparency_hosttest.c").read_text().replace("void HWR_PS2_FreeData(void *p) { free(p); }", "static void free_source_checked(void *p);\nvoid HWR_PS2_FreeData(void *p) { free_source_checked(p); }")
        (work / "fixture_base.c").write_text(base); (work / "fixture.c").write_bytes((HERE / "transparency_delete_hosttest.c").read_bytes())
        (work / "texture-delete.inc").write_text(corrected if fixed else original)
        results = {}
        for sanitize in (False, True):
            mode = "sanitize" if sanitize else "normal"; binary = work / mode
            flags = ["-std=gnu17", "-O2", "-DPS2_PROFILE", "-DPS2", "-fwrapv", "-ffp-contract=off", "-I" + str(work)]
            if sanitize: flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
            c = subprocess.run(["gcc", *flags, str(work / "fixture.c"), "-o", str(binary), "-lm"], capture_output=True, text=True)
            (work / (mode + "-build.log")).write_text(c.stdout + c.stderr)
            if c.returncode: raise RuntimeError("Delete fixture compile failed: " + c.stderr)
            r = subprocess.run([str(binary)], capture_output=True, text=True); log = r.stdout + r.stderr; (work / (mode + ".log")).write_text(log)
            if r.returncode or "T17D COMPLETE" not in log or "runtime error:" in log or "ERROR: AddressSanitizer" in log: raise RuntimeError("Delete fixture execution failed: " + log)
            rows = [dict(v.split("=", 1) for v in line.split()[1:]) for line in log.splitlines() if line.startswith("T17D pending_before=")]
            if len(rows) != 1: raise RuntimeError("Missing delete observation")
            results[mode] = rows[0]
        if results["normal"] != results["sanitize"]: raise RuntimeError("Sanitized delete differs")
        row = results["normal"]
        if row["cache_before"] != "2" or row["cache_after"] != ("0" if fixed else "2") or row["wrong_indices"] != ("0" if fixed else "1024"):
            raise RuntimeError("Delete cache observation did not expose/repair stale sources")
        report["variants"][name] = {"source_inputs": inputs, "results": results}
    (out / "candidate-delete.inc").write_text(corrected)
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n"); print(json.dumps(report, indent=2))


if __name__ == "__main__": main()
