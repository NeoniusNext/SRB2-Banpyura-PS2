#!/usr/bin/env python3
"""Probe actual UpdateTexture's canonical and source-cache lifetimes separately."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent; ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from transparency_hosttest import prepare

UPDATE_OLD = """static void hw_UpdateTexture(GLMipmap_t *TexInfo)
{
\ttexrec_t *r;

\tif (!H.up || !TexInfo)
\t\treturn;
\tr = rec_of(TexInfo);
\tif (r)
\t{
\t\tov_flush_all();
\t\tdma_fence(); // the engine has changed the texels: no queued reference may still read the old ones
\t\ttex_drop((int)(r - H.rec), 0);
\t}
\tsettex_now(TexInfo);
}

"""

UPDATE_NEW = """static void hw_UpdateTexture(GLMipmap_t *TexInfo)
{
\ttexrec_t *r;

\tif (!H.up || !TexInfo)
\t\treturn;
\tov_flush_all();
\tdma_fence(); // old source references can outlive an evicted VRAM record
\tr = rec_of(TexInfo);
\tif (!r)
\t\tr = twin_rec(TexInfo);
\tif (r)
\t\ttex_drop(img_of((int)(r - H.rec)), 0); // every view shares the changed texels
\tif (TexInfo->format == GL_TEXFMT_P_8 && (TexInfo->regen_kind == 1 || TexInfo->regen_kind == 2))
\t{
\t\tGLMipmap_t *key = TexInfo->ps2_twin ? TexInfo->ps2_twin : TexInfo;
\t\tint i;

\t\tkey->ps2_h255 = TexInfo->ps2_h255 = 0;
\t\tif (key->nextcolormap)
\t\t\tkey->nextcolormap->ps2_h255 = 0;
\t\t// Cached full and derived levels belong to the preceding content.
\t\tfor (i = 0; i < DC_MAX; i++)
\t\t\tif (dc[i].key == dc_key(TexInfo))
\t\t\t\tdc_drop(&dc[i]);
\t}
\tsettex_now(TexInfo);
}

"""


def updated_function(metadata, sources):
    if metadata and sources:
        return UPDATE_NEW
    text = UPDATE_OLD
    if metadata or sources:
        # Both need the changed canonical pair. Existing engine entry points
        # retain their argument and SetTexture behavior in this isolated probe.
        text = text.replace("\ttexrec_t *r;", "\ttexrec_t *r;\n\tGLMipmap_t *key;")
        text = text.replace("\tr = rec_of(TexInfo);", "\tkey = TexInfo->ps2_twin ? TexInfo->ps2_twin : TexInfo;\n\tr = rec_of(TexInfo);")
    if metadata:
        text = text.replace("\tsettex_now(TexInfo);", "\tkey->ps2_h255 = 0;\n\tif (key->nextcolormap)\n\t\tkey->nextcolormap->ps2_h255 = 0;\n\tTexInfo->ps2_h255 = 0;\n\tsettex_now(TexInfo);")
    if sources:
        text = text.replace("\tr = rec_of(TexInfo);", "\tov_flush_all();\n\tdma_fence();\n\tr = rec_of(TexInfo);\n\tif (!r)\n\t\tr = twin_rec(TexInfo);")
        text = text.replace("\t\ttex_drop((int)(r - H.rec), 0);", "\t\ttex_drop(img_of((int)(r - H.rec)), 0);")
        text = text.replace("\tsettex_now(TexInfo);", "\tif (TexInfo->format == GL_TEXFMT_P_8 && (TexInfo->regen_kind == 1 || TexInfo->regen_kind == 2))\n\t\tfor (int i = 0; i < DC_MAX; i++)\n\t\t\tif (dc[i].key == dc_key(TexInfo))\n\t\t\t\tdc_drop(&dc[i]);\n\tsettex_now(TexInfo);")
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument("--source-root", type=Path, default=ROOT); ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(); source = a.source_root.resolve(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    hwd = source / "src/ps2/hw/ps2_hwd.c"; driver = hwd.read_text()
    if driver.count(UPDATE_OLD) != 1: raise RuntimeError("Actual UpdateTexture changed")
    twin = re.search(r"static texrec_t \*twin_rec\(GLMipmap_t \*m\)\n\{.*?^\}", driver, re.M | re.S)
    if not twin: raise RuntimeError("Actual twin lookup missing")
    report = {"scope": __doc__, "actual_update_source_sha256": hashlib.sha256(UPDATE_OLD.encode()).hexdigest(), "driver_sha256": hashlib.sha256(driver.encode()).hexdigest(),
              "fixture_sha256": hashlib.sha256((HERE / "transparency_update_hosttest.c").read_bytes()).hexdigest(), "variants": {}}
    for name, metadata, sources in (("baseline", False, False), ("metadata_only", True, False), ("source_only", False, True), ("both", True, True)):
        work = out / name; prepare(work, source, True)
        base = (HERE / "transparency_hosttest.c").read_text().replace("void HWR_PS2_FreeData(void *p) { free(p); }", "static void free_source_checked(void *p);\nvoid HWR_PS2_FreeData(void *p) { free_source_checked(p); }")
        (work / "fixture_base.c").write_text(base)
        (work / "fixture.c").write_bytes((HERE / "transparency_update_hosttest.c").read_bytes())
        (work / "texture-update.inc").write_text(twin.group() + "\n\n" + updated_function(metadata, sources))
        results = {}
        for sanitize in (False, True):
            kind = "sanitize" if sanitize else "normal"; binary = work / kind
            flags = ["-std=gnu17", "-O2", "-DPS2_PROFILE", "-DPS2", "-fwrapv", "-ffp-contract=off", "-I" + str(work)]
            if sanitize: flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
            c = subprocess.run(["gcc", *flags, str(work / "fixture.c"), "-o", str(binary), "-lm"], capture_output=True, text=True)
            (work / (kind + "-build.log")).write_text(c.stdout + c.stderr)
            if c.returncode: raise RuntimeError("Update fixture compile failed: " + c.stderr)
            r = subprocess.run([str(binary)], capture_output=True, text=True); log = r.stdout + r.stderr; (work / (kind + ".log")).write_text(log)
            if r.returncode or "T17U COMPLETE" not in log or "runtime error:" in log or "ERROR: AddressSanitizer" in log: raise RuntimeError("Update fixture execution failed: " + log)
            rows = [dict(v.split("=", 1) for v in line.split()[1:]) for line in log.splitlines() if line.startswith("T17U zero_copy=")]
            if len(rows) != 6: raise RuntimeError("Missing update cases")
            results[kind] = rows
        if results["normal"] != results["sanitize"]: raise RuntimeError("Sanitized update differs")
        report["variants"][name] = results
    for name, item in report["variants"].items():
        for row in item["normal"]:
            zc = row["zero_copy"] == "1"
            if name == "baseline" and (row["wrong_indices"] != ("1024" if zc else "0") or row["wrong_rgba"] != "1024"): raise RuntimeError("Baseline did not expose both independent lifetimes")
            if name == "metadata_only" and (row["wrong_rgba"] != ("1024" if zc else "0") or row["wrong_indices"] != ("1024" if zc else "0")): raise RuntimeError("Metadata control did not isolate cache lifetime")
            if name == "source_only" and (row["wrong_indices"] != "0" or row["wrong_rgba"] != "1024"): raise RuntimeError("Source control did not isolate metadata lifetime")
            if name == "both" and (row["wrong_indices"] != "0" or row["wrong_rgba"] != "0" or row["other_handle"] != "0"): raise RuntimeError("Combined lifetime correction failed")
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n"); print(json.dumps(report, indent=2))


if __name__ == "__main__": main()
