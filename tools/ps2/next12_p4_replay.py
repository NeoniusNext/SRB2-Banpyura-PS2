#!/usr/bin/env python3
"""Replay the rejected P4 research prototype into build/, checking all 602 sources.

This never edits production src or invokes a compiler/emulator.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def apply_file(source, patch):
    original = source.decode().splitlines(keepends=True)
    result, cursor, index = [], 0, 0
    while index < len(patch):
        match = re.fullmatch(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n", patch[index])
        if not match:
            raise RuntimeError("Invalid exact patch hunk")
        old_start, old_count, new_start, new_count = map(int, (match[1], match[2] or 1, match[3], match[4] or 1))
        offset = old_start if old_count == 0 else old_start - 1
        if offset < cursor:
            raise RuntimeError("Overlapping patch hunk")
        result.extend(original[cursor:offset])
        cursor = offset
        expected_new = new_start if new_count == 0 else new_start - 1
        if len(result) != expected_new:
            raise RuntimeError("Incorrect new hunk position")
        consumed = produced = 0
        index += 1
        while index < len(patch) and not patch[index].startswith("@@"):
            line = patch[index]
            if line[:1] not in (" ", "+", "-"):
                raise RuntimeError("Invalid patch line")
            if line[0] in (" ", "-"):
                if cursor >= len(original) or original[cursor] != line[1:]:
                    raise RuntimeError("Literal removed/context line differs")
                cursor += 1
                consumed += 1
            if line[0] in (" ", "+"):
                result.append(line[1:])
                produced += 1
            index += 1
        if (consumed, produced) != (old_count, new_count):
            raise RuntimeError("Incorrect exact hunk length")
    result.extend(original[cursor:])
    return "".join(result).encode()


def replay(baseline, config_base, out):
    out = out.resolve()
    if not out.is_relative_to(ROOT / "build"):
        raise RuntimeError("Variant output must stay under build/")
    base = json.loads((ROOT / "tools/ps2/next12_baseline.json").read_text())
    variant = json.loads((ROOT / "tools/ps2/next12_p4_candidate.json").read_text())
    patch_data = (ROOT / "tools/ps2/next12_p4_candidate.patch").read_bytes()
    if sha(patch_data) != variant["patch_sha256"]:
        raise RuntimeError("Pinned research patch changed")
    lines = patch_data.decode().splitlines(keepends=True)
    patches, index = {}, 0
    while index < len(lines):
        if not lines[index].startswith("--- a/src/"):
            raise RuntimeError("Unexpected research patch path")
        path = lines[index][6:].rstrip("\n")
        if lines[index + 1] != "+++ b/" + path + "\n" or path not in variant["changed"]:
            raise RuntimeError("Unexpected candidate file")
        index += 2
        start = index
        while index < len(lines) and not lines[index].startswith("--- a/"):
            index += 1
        patches[path] = lines[start:index]
    if set(patches) != set(variant["changed"]):
        raise RuntimeError("Candidate patch inventory differs")
    outputs, hashes = {}, {}
    for path, expected in sorted(base["source_sha256"].items()):
        data = (baseline / path).read_bytes()
        if sha(data) != expected:
            raise RuntimeError("Frozen source differs: " + path)
        if path in patches:
            if expected != variant["changed"][path]["baseline_sha256"]:
                raise RuntimeError("Candidate belongs to a different baseline")
            data = apply_file(data, patches[path])
            if sha(data) != variant["changed"][path]["candidate_sha256"]:
                raise RuntimeError("Whole candidate file differs: " + path)
        outputs[path] = data
        hashes[path] = sha(data)
    for mode, expected in variant["frozen_configuration_sha256"].items():
        data = (config_base / mode / "gen/config.h").read_bytes()
        if sha(data) != expected:
            raise RuntimeError("Frozen canonical configuration differs: " + mode)
        outputs["build/" + mode + "/gen/config.h"] = data
    for relative, data in outputs.items():
        target = out / relative
        if target.exists() and target.read_bytes() != data:
            raise RuntimeError("Refusing to overwrite a different artifact: " + str(target))
    for relative, data in outputs.items():
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    report = {"status": variant["status"], "baseline": str(baseline), "source_revision": variant["source_revision"], "out": str(out), "source_files": len(hashes), "changed_files": len(patches), "all_candidate_source_sha256": hashes, "patch_sha256": sha(patch_data), "frozen_configuration_sha256": variant["frozen_configuration_sha256"], "production_src_edited": False, "compiler_or_emulator_invoked": False}
    (out / "replay-report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next12/baseline-src")
    ap.add_argument("--config-base", type=Path, default=ROOT / "build/nov15-next12/baseline")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next12/p4-replay")
    args = ap.parse_args()
    report = replay(args.baseline.resolve(), args.config_base.resolve(), args.out)
    print(json.dumps({key: value for key, value in report.items() if key != "all_candidate_source_sha256"}, indent=2))


if __name__ == "__main__":
    main()
