#!/usr/bin/env python3
"""Compare actual frozen/current resource-cache functions and every cache byte.

Reuses the preceding complete lookup/name-index proof and adds exhaustive
cursor/slot/collision/duplicate precedence and protected eight-byte queries.
No EE compiler or emulator is used by this host-only checker.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import zipfile

import next2_lookup_hosttest as old

ROOT, TOOLS = old.ROOT, old.TOOLS


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(out, baseline, mutation=None, candidate=ROOT / "src/w_wad.c"):
    assert candidate.name == "w_wad.c" and candidate.parent.name == "src"
    original_root = old.ROOT
    try:
        old.ROOT = candidate.parent.parent
        sources = old.prepare(out, baseline)
    finally:
        old.ROOT = original_root
    for kind, source in zip(("reference", "candidate"), sources):
        text = source.read_text()
        text += f"""
void {kind}_cache_set(const next2_cache_state *s) {{
    unsigned i; lumpnumcacheindex=(UINT16)s->cursor;
    for(i=0;i<64;i++) {{ W_CACHE_HASH(i)=s->hash[i]; W_CACHE_NUM(i)=s->number[i]; memcpy(W_CACHE_NAME(i),s->name[i],32); }}
}}
"""
        if kind == "candidate" and mutation:
            mutations = {
                "order": ("const UINT32 *split = base + lumpnumcacheindex + 1;",
                          "const UINT32 *split = base + lumpnumcacheindex;"),
                "last": ("stop = split;", "stop = split + 1;"),
                "payload": ("return W_CACHE_NUM(i);", "return W_CACHE_NUM(i) ^ 1;"),
            }
            if "const UINT32 *split, *p;" in text:
                slot_type = "UINT32" if "UINT32 i = lumpnumcacheindex;" in text else "UINT16"
                mutations = {
                    "order": (slot_type + " i = lumpnumcacheindex;", slot_type + " i = (lumpnumcacheindex + 63) & 63;"),
                    "last": ("p != split + 1", "p != split + 2"),
                    "payload": ("return W_CACHE_NUM(i);", "return W_CACHE_NUM(i) ^ 1;"),
                }
            before, after = mutations[mutation]
            assert text.count(before) >= 1
            text = text.replace(before, after)
        source.write_text(text)
    return sources


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next10/baseline-src/src/w_wad.c")
    ap.add_argument("--candidate", type=Path, default=ROOT / "src/w_wad.c")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next10/io/host")
    ap.add_argument("--assets", type=Path, default=Path("/workspace/.onboarding/srb2-assets"))
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--optout", action="store_true")
    ap.add_argument("--negative-controls", action="store_true")
    a = ap.parse_args()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    sources = prepare(a.out, a.baseline, candidate=a.candidate.resolve())
    flags = ["-O2", "-std=gnu17", "-fwrapv", "-DNDEBUG", "-DNOHW", "-DPS2_PROFILE",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-stringop-truncation",
             "-I" + str(TOOLS), "-I" + str(ROOT / "src"), "-I" + str(ROOT / "build/host-gen")]
    if a.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    if a.optout:
        flags += ["-DPS2_NOOPT_LUMPCACHEWALK"]
    fixture = TOOLS / "next10_io_hosttest.c"
    exe = a.out / "lookup"
    command = [os.environ.get("CC", "cc"), *flags, *sources, fixture, "-o", exe]
    old.run(command, a.out / "build.log")
    names = []
    archives = sorted(a.assets.glob("*.pk3"))
    for archive in archives:
        with zipfile.ZipFile(archive) as z:
            for name in z.namelist():
                basename = name.rsplit("/", 1)[-1]
                longname = basename.rsplit(".", 1)[0] if "." in basename else basename
                names.append(longname.encode("utf-8"))
    assert names and len(names) <= 65535
    path = a.out / "asset-names.bin"
    path.write_bytes(struct.pack("<I", len(names)) + b"".join(struct.pack("<I", len(n)) + n for n in names))
    output = old.run([exe, path], a.out / "test.log")
    print(output, end="")
    controls = {}
    if a.negative_controls:
        assert not a.optout
        for mutation in ("order", "last", "payload"):
            work = a.out / mutation
            paths = prepare(work, a.baseline, mutation, a.candidate.resolve())
            neg = work / "lookup"
            old.run([os.environ.get("CC", "cc"), *flags, *paths, fixture, "-o", neg], work / "build.log")
            p = subprocess.run([str(neg)], cwd=ROOT, capture_output=True, text=True)
            (work / "test.log").write_text(p.stdout + p.stderr)
            assert p.returncode == 1 and "LOOKUP FAIL" in p.stderr, (mutation, p.returncode, p.stderr)
            controls[mutation] = p.stderr.strip()
            print("Detected", mutation, "mutation")
    report = {"baseline": str(a.baseline), "baseline_sha256": sha(a.baseline),
              "candidate": str(a.candidate.resolve()), "candidate_sha256": sha(a.candidate), "flags": flags,
              "compile_command": list(map(str, command)), "output": output,
              "asset_names": len(names), "assets": {str(p): sha(p) for p in archives},
              "negative_controls": controls, "sources": {str(p): sha(p) for p in sources},
              "fixture_sha256": sha(fixture), "checker_sha256": sha(Path(__file__)),
              "reused_fixture_sha256": sha(TOOLS / "next2_lookup_hosttest.c"),
              "reused_checker_sha256": sha(TOOLS / "next2_lookup_hosttest.py")}
    (a.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
