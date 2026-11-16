#!/usr/bin/env python3
"""Prove full-source P8 fallback and CPU leases after GS residency failure.

Compile the complete production texture driver through the maintained host GS
packet boundary. Exercise real allocator/cache failures, inspect every uploaded
source/halo byte independently, and track engine Zone owner cells and locked
tags. Six isolated mutations must reproduce named semantic failures in normal
and ASan/UBSan builds. Native GS raster and whole-game timing are separate gates.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE / "opt17"))
from tile_host_common import compile_run, prepare, replace_once

TOOLS = (
    "tools/ps2/opt18_vram_fallback_hosttest.py",
    "tools/ps2/opt18_vram_fallback_hosttest.c",
    "tools/ps2/opt18_vram_fallback_services.inc",
    "tools/ps2/opt17/tile_host_common.py",
    "tools/ps2/hw_hosttest.py",
    "tools/ps2/hw_hosttest.c",
)


def source_manifest(source):
    return {str(path.relative_to(source)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted((source / "src").rglob("*")) if path.is_file()}


def tools_manifest():
    return {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in TOOLS}


def prepare_fixture(source, work):
    hashes = prepare(source, work)
    base = (work / "base_hosttest.c").read_text().split("int main(int argc, char **argv)")[0]
    base = replace_once(base, '#include "shim.h"', '#include "shim.h"\n#include "services.inc"')
    for old in (
        "void HWR_PS2_LockData(void *data) { (void)data; }",
        "void HWR_PS2_UnlockData(void *data) { (void)data; }",
        "void HWR_PS2_FreeData(void *data) { free(data); }",
        "void *HWR_PS2_StealData(GLMipmap_t *m, void **newuser) { void *p = m->data; m->data = NULL; *newuser = p; return p; }",
        "void *HWR_PS2_AllocData(size_t bytes, void **newuser) { void *p = malloc(bytes); *newuser = p; return p; }",
        "void HWR_PS2_RegenerateMipmap(GLMipmap_t *m) { (void)m; }",
    ):
        base = replace_once(base, old, "")
    base = replace_once(base,
        "static u32 tex_want(const GLMipmap_t *m, int *visible) { (void)m; *visible = 1; return 0; }",
        "static u32 tex_want(const GLMipmap_t *m, int *visible) { (void)m; *visible = 1; return fixture_want; }")
    base = replace_once(base, "static void ring_wait(int limit)\n{\n\t(void)limit;\n}",
        "static void ring_wait(int limit)\n{\n\t(void)limit; H.done_seq=H.q_seq;\n}")
    (work / "base_hosttest.c").write_text(base)
    (work / "services.inc").write_bytes((HERE / "opt18_vram_fallback_services.inc").read_bytes())
    (work / "fixture.c").write_bytes((HERE / "opt18_vram_fallback_hosttest.c").read_bytes())
    return hashes


def mutate(text, variant):
    if variant == "no-fallback":
        return replace_once(text, "ri = tex_full_p8_proxy(m);", "ri = NOREC;"), "planned failure falls back to full size"
    if variant == "no-cleanup":
        return replace_once(text,
            "// No queued reader exists on this failure path, so release its lease.\n\t\t\tif (dlock && dlock->p && !dlock->busy)\n\t\t\t\tHWR_PS2_UnlockData(dlock->p);",
            "// No queued reader exists on this failure path, so release its lease.\n\t\t\tif (0) HWR_PS2_UnlockData(dlock->p);"), "failed fresh-flat mip releases its temporary CPU lock"
    if variant == "wrong-classification":
        return replace_once(text, "has255 = scan_has_ff(full, (u32)m->width * m->height);", "has255 = 0;"), "canonical index255 classification replaces cached mip classification"
    if variant == "unlock-queued":
        return replace_once(text, "tile_source_unlock((void *)full, full_cache, 0);", "HWR_PS2_UnlockData((void *)full);"), "full cache source keeps its older queued lease"
    if variant == "budget-lock":
        return replace_once(text,
            'tex_fail_why = "footprint over budget";\n\t\t\tif (dlock && dlock->p && !dlock->busy)\n\t\t\t\tHWR_PS2_UnlockData(dlock->p);',
            'tex_fail_why = "footprint over budget";\n\t\t\tif (0) HWR_PS2_UnlockData(dlock->p);'), "explicit budget releases fresh-flat lock before resource error callback"
    if variant == "common-lock":
        return replace_once(text,
            "if (dlock && dlock->p && !dlock->busy)\n\t\tHWR_PS2_UnlockData(dlock->p); // a zero-copy upload keeps the lock (busy) until its DMA has read the texels",
            "if (0) HWR_PS2_UnlockData(dlock->p); // negative common exit"), "common failure exit releases fresh-flat lock after copied upload"
    raise ValueError(variant)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    source, out = args.source_root.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs, tool_inputs = source_manifest(source), tools_manifest()
    results = {}
    for variant in ("positive", "no-fallback", "no-cleanup", "wrong-classification",
                    "unlock-queued", "budget-lock", "common-lock"):
        for sanitize in (False, True):
            work = out / (variant + ("-sanitize" if sanitize else "-normal"))
            hashes = prepare_fixture(source, work)
            path = work / "ps2_hw_tex.inc"
            if variant != "positive":
                text, expected = mutate(path.read_text(), variant)
                path.write_text(text)
            ran = compile_run(source, work, work / "fixture.c", sanitize)
            output = ran.stdout + ran.stderr
            if variant == "positive":
                good = ran.returncode == 0 and "VRAM FALLBACK COMPLETE cases=30 proxies=18 missing_regen=6 explicit_policies=4 record_failures=2 exact_source_bytes=1843200" in output
            else:
                good = ran.returncode == 1 and expected in output
            good = good and "runtime error:" not in output and "ERROR: AddressSanitizer" not in output
            item = {"variant": variant, "sanitize": sanitize, "returncode": ran.returncode,
                    "matched_expectation": good, "output": output, "source_sha256": hashes}
            (work / "result.json").write_text(json.dumps(item, indent=2) + "\n")
            results[work.name] = item
            if not good:
                raise RuntimeError("Unexpected proof result: " + str(work))
    if source_manifest(source) != inputs or tools_manifest() != tool_inputs:
        raise RuntimeError("Production sources or proof inputs changed during execution")
    report = {"schema": "ps2-opt18-vram-fallback-host-v1", "source_root": str(source),
              "source_inputs_sha256": inputs, "tool_inputs_sha256": tool_inputs,
              "scope": __doc__.strip(), "results": results, "emulator_used": False}
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({name: {key: value[key] for key in ("returncode", "matched_expectation", "output")}
                      for name, value in results.items()}, indent=2))


if __name__ == "__main__":
    main()
