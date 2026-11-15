#!/usr/bin/env python3
"""Recreate the nonshipping 420-byte drawseg candidate in an isolated build root.

Verify all 602 frozen source hashes, apply the checked zero-context header patch,
and write exactly those 602 sources with only r_defs.h changed. Production source,
the Git index, game assets and historical proof outputs are never modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
BASE_SHA = '6c88912e24bb3f85cc8d0166f5734ad5e01e42dfe0bad3ae266e91cba831f62c'
CANDIDATE_SHA = 'e58fccb5ada224dafacc95398da41f2a6f683ce9f61c084ec6a487935930844e'
PATCH_SHA = '16fda7c4ce24a05f8dd1c459e061aa8e5a04ef9d37fd52b589c2aba7de95afbc'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def apply_checked_patch(source, patch):
    assert sha(source) == BASE_SHA, 'wrong frozen r_defs.h'
    assert sha(patch) == PATCH_SHA, 'candidate patch checksum differs'
    original, lines = source.decode().splitlines(keepends=True), patch.decode().splitlines(keepends=True)
    assert lines[:2] == ['--- a/src/r_defs.h\n', '+++ b/src/r_defs.h\n']
    result, cursor, index = [], 0, 2
    while index < len(lines):
        match = re.fullmatch(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n', lines[index])
        assert match, 'invalid checked patch hunk'
        old_start, old_count, new_start, new_count = (int(match[1]), int(match[2] or 1), int(match[3]), int(match[4] or 1))
        old_offset = old_start if old_count == 0 else old_start - 1
        assert old_offset >= cursor
        result.extend(original[cursor:old_offset])
        cursor = old_offset
        assert len(result) == new_start - 1
        consumed = produced = 0
        index += 1
        while index < len(lines) and not lines[index].startswith('@@'):
            line = lines[index]
            assert line[:1] in (' ', '+', '-')
            if line[0] in (' ', '-'):
                assert original[cursor] == line[1:], 'checked patch context differs'
                cursor += 1
                consumed += 1
            if line[0] in (' ', '+'):
                result.append(line[1:])
                produced += 1
            index += 1
        assert (consumed, produced) == (old_count, new_count)
    result.extend(original[cursor:])
    candidate = ''.join(result).encode()
    assert sha(candidate) == CANDIDATE_SHA, 'reconstructed candidate header differs'
    return candidate


def prepare(baseline, out):
    baseline, out = Path(baseline).resolve(), Path(out).resolve()
    assert out.is_relative_to(ROOT / 'build') and out != ROOT / 'build', 'output must be a build child'
    assert not out.is_relative_to(baseline) and not baseline.is_relative_to(out), 'snapshot must not overlap frozen baseline'
    manifest_path = TOOLS / 'next10_baseline.json'
    manifest = json.loads(manifest_path.read_text())
    expected = manifest['source_sha256']
    assert len(expected) == 602 and expected['src/r_defs.h'] == BASE_SHA
    for relative, digest in expected.items():
        assert relative.startswith('src/') and '..' not in Path(relative).parts
        source = baseline / relative
        assert source.is_file() and sha(source.read_bytes()) == digest, 'frozen source mismatch: ' + relative
    patch_path = TOOLS / 'next10_memory_candidate.patch'
    candidate = apply_checked_patch((baseline / 'src/r_defs.h').read_bytes(), patch_path.read_bytes())
    candidate_hashes = dict(expected)
    candidate_hashes['src/r_defs.h'] = CANDIDATE_SHA
    if out.exists() and any(out.iterdir()):
        actual = {str(p.relative_to(out)) for p in (out / 'src').rglob('*') if p.is_file()}
        assert actual == set(candidate_hashes), 'existing output has a different source inventory'
        for relative, digest in candidate_hashes.items():
            assert sha((out / relative).read_bytes()) == digest, 'existing candidate differs: ' + relative
    else:
        for relative in expected:
            destination = out / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(candidate if relative == 'src/r_defs.h' else (baseline / relative).read_bytes())
    changed = [relative for relative in expected if sha((out / relative).read_bytes()) != expected[relative]]
    assert changed == ['src/r_defs.h']
    assert len([p for p in (out / 'src').rglob('*') if p.is_file()]) == 602
    report = {'baseline': str(baseline), 'candidate_root': str(out), 'frozen_source_files': 602,
              'changed_from_frozen': changed, 'baseline_header_sha256': BASE_SHA,
              'candidate_header_sha256': CANDIDATE_SHA, 'patch_sha256': PATCH_SHA,
              'baseline_manifest_sha256': sha(manifest_path.read_bytes()),
              'candidate_source_sha256': candidate_hashes, 'production_edited': False}
    report_path = out / 'memory-candidate-replay.json'
    data = json.dumps(report, indent=2) + '\n'
    if report_path.exists():
        assert report_path.read_text() == data, 'existing replay manifest differs'
    else:
        report_path.write_text(data)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next10/baseline-src')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next10/memory/candidate-src')
    args = parser.parse_args()
    out = prepare(args.baseline, args.out)
    print(json.dumps({'candidate_root': str(out), 'source_files': 602,
                      'changed_from_frozen': ['src/r_defs.h'], 'header_sha256': CANDIDATE_SHA,
                      'production_edited': False}))


if __name__ == '__main__':
    main()
