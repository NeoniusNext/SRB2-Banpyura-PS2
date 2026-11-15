#!/usr/bin/env python3
"""Check all source pins and exact original preprocessed fallback node paths."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from next13_ram_variant import ROOT, BASE, OUT, MAIN, rewrite
from next12_cull_hosttest import between, function


def sha(value):
    return hashlib.sha256(value).hexdigest()


def chunk(text):
    return (between(text, "typedef struct\n{\n\tFOutVector    wallVerts[4];", "static int CompareDrawNodes(")
            + function(text, "CompareDrawNodes") + function(text, "CompareDrawNodePlanes")
            + function(text, "HWR_CreateDrawNodes"))


def tokens(text):
    return re.findall(r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[a-zA-Z_]\w*|\d[\w.]*|\S''', text)


def preprocess(path, defines, work):
    command = ["gcc", "-E", "-P", "-nostdinc", "-x", "c", *["-D" + d for d in defines], str(path)]
    proc = subprocess.run(command, capture_output=True, text=True)
    (work / (path.stem + ".command.json")).write_text(json.dumps(command) + "\n")
    (work / (path.stem + ".stderr")).write_text(proc.stderr)
    if proc.returncode:
        raise RuntimeError(proc.stderr)
    (work / (path.stem + ".preprocessed.c")).write_text(proc.stdout)
    return tokens(proc.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov15-next13/ram-policy-v1")
    parser.add_argument("--candidate", type=Path, default=OUT)
    parser.add_argument("--scalar", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    candidate_source = args.candidate.resolve()
    manifest = json.loads((candidate_source / "source-manifest.json").read_text())
    for label, source, key in (("baseline", BASE, "source_sha256"), ("candidate", candidate_source, "candidate_source_sha256")):
        actual = {str(p.relative_to(source)): sha(p.read_bytes()) for p in (source / "src").rglob("*") if p.is_file()}
        if actual != manifest[key]:
            raise RuntimeError(label + " full 602 source pins differ")
    before = (BASE / MAIN).read_text()
    after = (candidate_source / MAIN).read_text()
    selected_rewrite = rewrite
    if args.scalar:
        from next13_ram_scalar_variant import rewrite as selected_rewrite
    if selected_rewrite(before) != after:
        raise RuntimeError("Candidate differs from exact isolated rewrite")
    changes = [p for p in manifest["source_sha256"] if manifest["source_sha256"][p] != manifest["candidate_source_sha256"][p]]
    if changes != [MAIN]:
        raise RuntimeError("Unexpected source scope")
    original_chunk, candidate_chunk = chunk(before), chunk(after)
    rows = []
    for label, defines in (("ps2-optout", ["PS2", "PS2_PROFILE", "PS2_NOOPT_IMPLICIT_DN"]),
                           ("nonps2-profile", ["PS2_PROFILE"]),
                           ("ps2-nonprofile", ["PS2"]), ("generic", [])):
        work = args.out / label
        work.mkdir()
        reference = work / "reference.c"
        candidate = work / "candidate.c"
        reference.write_text(original_chunk)
        candidate.write_text(candidate_chunk)
        a, b = preprocess(reference, defines, work), preprocess(candidate, defines, work)
        if a != b:
            mismatch = next((i for i, pair in enumerate(zip(a, b)) if pair[0] != pair[1]), min(len(a), len(b)))
            raise RuntimeError(label + " preprocessed token mismatch at " + str(mismatch))
        # Executed negative: one changed comparator subtraction must reject.
        bad = work / "negative-comparator.c"
        bad.write_text(candidate_chunk.replace("diff = v2 - v1;", "diff = v1 - v2;"))
        bad_tokens = preprocess(bad, defines, work)
        if bad_tokens == a:
            raise RuntimeError("Comparator negative escaped")
        rows.append({"policy": label, "tokens": len(a), "tokens_sha256": sha(json.dumps(a).encode()),
                     "negative_comparator_rejected": True})
    # The two real renderer callers guard the preexisting unsigned p-1 loop.
    guard = "if (numplanes || numpolyplanes || numwalls)"
    if before.count(guard) != 2 or after.count(guard) != 2:
        raise RuntimeError("Caller empty guard changed")
    report = {"status": "pass", "source_files_per_tree": 602, "changed": changes,
              "source_manifest_sha256": sha((candidate_source / "source-manifest.json").read_bytes()),
              "candidate_sha256": sha(after.encode()), "fallback_policies": rows,
              "caller_empty_guards": 2, "default_guard": "PS2 && PS2_PROFILE && !PS2_NOOPT_IMPLICIT_DN",
              "target_or_emulator_used": False,
              "tools": {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in
                        (Path(__file__), ROOT / "tools/ps2/next13_ram_variant.py", ROOT / "tools/ps2/next12_cull_hosttest.py")}}
    (args.out / "tools").mkdir()
    for path in (Path(__file__), ROOT / "tools/ps2/next13_ram_variant.py", ROOT / "tools/ps2/next12_cull_hosttest.py"):
        shutil.copyfile(path, args.out / "tools" / path.name)
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
