#!/usr/bin/env python3
"""Execute mip-source lock ownership in real z_zone.c, without an emulator."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent; ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE)); from transparency_hosttest import prepare

LOCK_OLD = """\t\t\tu32 dt0 = cyc();

\t\t\tif (srcblk && data == (const u8 *)srcblk)
\t\t\t\tHWR_PS2_LockData(srcblk);"""
LOCK_NEW = """\t\t\tu32 dt0 = cyc();
\t\t\tconst int src_locked = srcblk && data == (const u8 *)srcblk;

\t\t\tif (src_locked)
\t\t\t\tHWR_PS2_LockData(srcblk);"""
UNLOCK_OLD = """\t\t\tif (srcblk && data == (const u8 *)srcblk)
\t\t\t\tHWR_PS2_UnlockData(srcblk);"""
UNLOCK_NEW = """\t\t\tif (src_locked && (!l0 || !l0->busy) && (!dsrc || !dsrc->busy))
\t\t\t\tHWR_PS2_UnlockData(srcblk);"""


def function(text, signature):
    m = re.search(re.escape(signature) + r"\n\{.*?^\}", text, re.M | re.S)
    if not m: raise RuntimeError("Missing actual callback: " + signature)
    return m.group()


def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument("--source-root", type=Path, default=ROOT); ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(); source = a.source_root.resolve(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    cache = (source / "src/hardware/hw_cache.c").read_text()
    callbacks = {s: function(cache, s) for s in ("void HWR_PS2_LockData(void *data)", "void HWR_PS2_UnlockData(void *data)", "void HWR_PS2_FreeData(void *data)", "void *HWR_PS2_AllocData(size_t bytes, void **newuser)", "void *HWR_PS2_StealData(GLMipmap_t *m, void **newuser)", "void HWR_PS2_ReleaseMipmapData(GLMipmap_t *m)")}
    hwd = (source / "src/ps2/hw/ps2_hwd.c").read_text()
    caller = re.search(r"\tif \(!zc_last\).*?\n\t\tHWR_PS2_ReleaseMipmapData\(TexInfo\);", hwd).group().replace("TexInfo", "m")
    report = {"scope": __doc__, "source_root": str(source), "callbacks": {s: hashlib.sha256(t.encode()).hexdigest() for s, t in callbacks.items()}, "variants": {}}
    for name, fixed, negative in (("baseline", False, False), ("fixed", True, False), ("premature_unlock_control", True, True)):
        work = out / name; inputs = prepare(work, source, True)
        text = (work / "ps2_hw_tex.inc").read_text()
        if text.count(LOCK_NEW) == 1 and text.count(UNLOCK_NEW) == 1:
            text = text.replace(LOCK_NEW, LOCK_OLD).replace(UNLOCK_NEW, UNLOCK_OLD)
        elif text.count(LOCK_OLD) != 1 or text.count(UNLOCK_OLD) != 1:
            raise RuntimeError("Reviewed source lock blocks missing or ambiguous")
        if fixed: text = text.replace(LOCK_OLD, LOCK_NEW).replace(UNLOCK_OLD, UNLOCK_NEW)
        if negative: text = text.replace(UNLOCK_NEW, "\t\t\tif (src_locked)\n\t\t\t\tHWR_PS2_UnlockData(srcblk);")
        (work / "ps2_hw_tex.inc").write_text(text)
        base = (HERE / "transparency_hosttest.c").read_text()
        base = re.sub(r"^void Z_SetRebuildCost\(.*?\n", "", base, flags=re.M)
        signatures = ("void HWR_PS2_LockData(void *p)", "void HWR_PS2_UnlockData(void *p)", "void HWR_PS2_FreeData(void *p)", "void *HWR_PS2_AllocData(size_t n, void **user)", "void *HWR_PS2_StealData(GLMipmap_t *m, void **user)")
        for sig, replacement in zip(signatures, list(callbacks.values())[:5]):
            pattern = re.escape(sig) + r" \{.*?\}"
            base, n = re.subn(pattern, replacement, base)
            if n != 1: raise RuntimeError("Fixture callback boundary changed: " + sig)
        base = base.replace('#include "ps2_hw_pal.inc"', callbacks["void HWR_PS2_ReleaseMipmapData(GLMipmap_t *m)"] + '\n#include "ps2_hw_pal.inc"')
        (work / "fixture_base.c").write_text(base); (work / "fixture.c").write_bytes((HERE / "transparency_lock_hosttest.c").read_bytes())
        zone = (ROOT / "tools/ps2/zone_hosttest.c").read_text().replace('"../../src/z_zone.c"', '"' + str(source / "src/z_zone.c") + '"').replace('"../../src/ps2/ps2_mem.c"', '"' + str(source / "src/ps2/ps2_mem.c") + '"')
        (work / "zone_fixture.c").write_text(zone); (work / "caller-release.inc").write_text(caller + "\n")
        results = {}
        for sanitize in (False, True):
            mode = "sanitize" if sanitize else "normal"; binary = work / mode
            flags = ["-std=gnu17", "-O2", "-fwrapv", "-ffp-contract=off", "-I" + str(work), "-I" + str(source / "src"), "-I" + str(source / "src/ps2")]
            if sanitize: flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
            c = subprocess.run(["gcc", *flags, str(work / "fixture.c"), "-o", str(binary), "-lm"], capture_output=True, text=True)
            (work / (mode + "-build.log")).write_text(c.stdout + c.stderr)
            if c.returncode: raise RuntimeError("Real-zone lock compile failed: " + c.stderr)
            r = subprocess.run([str(binary)], capture_output=True, text=True); log = r.stdout + r.stderr; (work / (mode + ".log")).write_text(log)
            if negative:
                if r.returncode != 1 or "old pending DMA source cannot be unlocked early" not in log: raise RuntimeError("Premature-unlock control did not fail its safety check: " + log)
            else:
                if r.returncode or "T17L COMPLETE" not in log or "runtime error:" in log or "ERROR: AddressSanitizer" in log: raise RuntimeError("Real-zone lock fixture failed: " + log)
            rows = [dict(v.split("=", 1) for v in line.split()[1:]) for line in log.splitlines() if line.startswith("T17L mode=")]
            results[mode] = rows
        if results["normal"] != results["sanitize"]: raise RuntimeError("Real-zone tag behavior differs under sanitizers")
        if not negative:
            if len(results["normal"]) != 4: raise RuntimeError("Missing source-lifetime cases")
            for row in results["normal"]:
                stranded = not fixed and row["mode"] != "2"
                if row["source_alive_after_purge"] != ("1" if stranded else "0"): raise RuntimeError("Wrong real-zone source purge behavior")
        report["variants"][name] = {"source_inputs": inputs, "results": results}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n"); print(json.dumps({k: {"rows": len(v["results"]["normal"]), "stranded": sum(r["source_alive_after_purge"] == "1" for r in v["results"]["normal"])} for k, v in report["variants"].items()}, indent=2))


if __name__ == "__main__": main()
