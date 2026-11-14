#!/usr/bin/env python3
"""Reconstruct the rejected ninth-pass header overlay without editing production.

Applies the published checked patch to the exact preceding pack reader. The
output is restricted to build/, and both source hashes are verified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
BASE_SHA = "94532c49e6a1886677099849ce96ecd5e804eb2facd6a05c1732660443ab2664"
CANDIDATE_SHA = "ec0065b0198695006f0019f237dcb8b0ea5ea2c9b351893af607c0f866ca8e06"
PATCH_SHA = "e392a1203f2b4ccf5ab49c71ded4eb550b553a63312c2262b927de8a32e89353"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def prepare(out, baseline=None):
    baseline = Path(baseline) if baseline is not None else ROOT / "src/w_pack.c"
    source = baseline.read_bytes()
    assert sha(source) == BASE_SHA, "wrong preceding pack-reader source: " + str(baseline)
    patch_path = ROOT / "tools/ps2/next9_io_rejected.patch"
    patch_data = patch_path.read_bytes()
    assert sha(patch_data) == PATCH_SHA, "rejected-candidate patch changed"
    original, patch = source.decode().splitlines(keepends=True), patch_data.decode().splitlines(keepends=True)
    assert patch[:2] == ["--- a/src/w_pack.c\n", "+++ b/src/w_pack.c\n"]
    result, cursor, index = [], 0, 2
    while index < len(patch):
        match = re.fullmatch(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n", patch[index])
        assert match, "invalid checked patch hunk"
        old_start, old_count, new_start, new_count = (int(match[1]), int(match[2] or 1), int(match[3]), int(match[4] or 1))
        # A zero-length old hunk inserts after old_start, as unified diff specifies.
        old_offset = old_start if old_count == 0 else old_start - 1
        assert old_offset >= cursor
        result.extend(original[cursor:old_offset])
        cursor = old_offset
        assert len(result) == new_start - 1
        consumed = produced = 0
        index += 1
        while index < len(patch) and not patch[index].startswith("@@"):
            line = patch[index]
            assert line[:1] in (" ", "+", "-")
            if line[0] in (" ", "-"):
                assert original[cursor] == line[1:], "checked patch context mismatch"
                cursor += 1
                consumed += 1
            if line[0] in (" ", "+"):
                result.append(line[1:])
                produced += 1
            index += 1
        assert (consumed, produced) == (old_count, new_count)
    result.extend(original[cursor:])
    data = "".join(result).encode()
    assert sha(data) == CANDIDATE_SHA
    out = Path(out).resolve()
    assert out.is_relative_to(ROOT / "build"), "rejected variant output must be under build/"
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        assert out.read_bytes() == data, "refusing to replace a different build artifact"
    else:
        out.write_bytes(data)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=ROOT / "src/w_pack.c")
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov15-next9/io/rejected-replay/w_pack.c")
    args = parser.parse_args()
    out = prepare(args.out, args.baseline)
    print(json.dumps({"source": str(out), "sha256": sha(out.read_bytes()), "production_edited": False}))


if __name__ == "__main__":
    main()
