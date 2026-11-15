#!/usr/bin/env python3
"""Reconstruct a tested cache-walk candidate without editing production.

Applies a published zero-context patch to the exact frozen preceding source.
Both inputs and the complete output are checked; output is limited to build/.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
BASE_SHA = "c313f8a589318eb9853325494f329a7539605d15931b5fc7ebef1107130aab24"
VARIANTS = {
    "span1": ("d430108da6544e1f668a932a7e3a0e8063f6253c810d2e1064a3c6d33b701915",
              "ddbf39ce9d7e2f6df4b897813fa225e402879bfbf7e7cdd9004a10d642769fe1"),
    "fastslot": ("6d34f21f8de41065d5f102372ca88cd5d792931b970cfe6e112588299f33f093",
                 "8145bde9fc4f27f8e1bee9d11a6cd6b72044b8fc6d11154eb0bdc9d36b4c9744"),
    "wide": ("9d6c3291ac958c8174c89f3d1c704e818339cab83ba72f74c40c1b9acd5e8063",
             "069a619e61c2d725962c31b1a3e60a4a9f7d4dde18fef574196968b65d97d1cf"),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def prepare(out, baseline, variant):
    source = baseline.read_bytes()
    assert sha(source) == BASE_SHA, "wrong preceding cache source"
    candidate_hash, patch_hash = VARIANTS[variant]
    patch_data = (ROOT / ("tools/ps2/next10_io_" + variant + ".patch")).read_bytes()
    assert sha(patch_data) == patch_hash, "candidate patch changed"
    original, patch = source.decode().splitlines(keepends=True), patch_data.decode().splitlines(keepends=True)
    assert patch[:2] == ["--- a/src/w_wad.c\n", "+++ b/src/w_wad.c\n"]
    result, cursor, index = [], 0, 2
    while index < len(patch):
        match = re.fullmatch(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n", patch[index])
        assert match, "invalid checked hunk"
        old_start, old_count, new_start, new_count = (int(match[1]), int(match[2] or 1), int(match[3]), int(match[4] or 1))
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
    assert sha(data) == candidate_hash
    out = out.resolve()
    assert out.is_relative_to(ROOT / "build")
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        assert out.read_bytes() == data, "refusing to replace a different artifact"
    else:
        out.write_bytes(data)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--variant", choices=VARIANTS, required=True)
    ap.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next10/baseline-src/src/w_wad.c")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    out = prepare(a.out, a.baseline, a.variant)
    print(json.dumps({"source": str(out), "sha256": sha(out.read_bytes()), "production_edited": False}))


if __name__ == "__main__":
    main()
