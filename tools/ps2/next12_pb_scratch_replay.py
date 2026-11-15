#!/usr/bin/env python3
"""Prepare or verify the isolated pass12 PB scratch candidate, without compiling.

The default destination is a checked 602-file snapshot, never the live source.
Only the full-width planner/walk lifetime overlay differs from commit 85bd9716b.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
REVISION = '85bd9716b1ed114d46f95831c64a87623038eeaa'
SOURCE = 'src/hardware/hw_pbatch.inc'
BASE_SHA = '69e58e933c355d3752457c00b6e0dde98df139ce1517d39ea18378afb7d81e4b'
CANDIDATE_SHA = '08e3ce0c810ba69f957ab8aaa0082e28bcdb1e3f7a3939f8d0da1e8133e2e350'
PATCH_SHA = 'e8776c3e424fadaf9ed15479b2c99ab443ebf4d8a07aa109358ba40c0ab7a8be'
VARIANT = 'pb-plan-walk-shared-scalar-scratch'
DEFAULT_BASE = ROOT / 'build/nov15-next12/pb-baseline-src'
DEFAULT_CANDIDATE = ROOT / 'build/nov15-next12/pb-scratch-src'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_manifest(path):
    return {p.relative_to(path).as_posix(): sha(p)
            for p in sorted((path / 'src').rglob('*')) if p.is_file()}


def baseline_files():
    archive = subprocess.check_output(['git', 'archive', '--format=tar', REVISION, 'src'], cwd=ROOT)
    files = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:') as tar:
        for member in tar:
            if member.isdir():
                continue
            if not member.isfile() or not member.name.startswith('src/') or '..' in Path(member.name).parts:
                raise RuntimeError('Unexpected baseline archive member')
            files[member.name] = tar.extractfile(member).read()
    if len(files) != 602 or hashlib.sha256(files[SOURCE]).hexdigest() != BASE_SHA:
        raise RuntimeError('Published baseline tree differs from its pinned 602 source files')
    return files


def validate(path, candidate=True):
    path = Path(path).resolve()
    manifest_path = path / 'manifest.json'
    if not manifest_path.is_file():
        raise RuntimeError('A checked isolated snapshot is required; run next12_pb_scratch_replay.py first')
    manifest = json.loads(manifest_path.read_text())
    expected = {name: hashlib.sha256(data).hexdigest() for name, data in baseline_files().items()}
    if candidate:
        expected[SOURCE] = CANDIDATE_SHA
    if manifest.get('revision') != REVISION or manifest.get('source_sha256') != expected:
        raise RuntimeError('Snapshot manifest does not match the pinned baseline-only overlay')
    if source_manifest(path) != expected:
        raise RuntimeError('Snapshot source changed after it was frozen')
    if candidate and manifest.get('variant') != VARIANT:
        raise RuntimeError('An unchanged baseline is not the scratch candidate')
    return manifest


def prepare(baseline=DEFAULT_BASE, candidate=DEFAULT_CANDIDATE):
    baseline, candidate = Path(baseline).resolve(), Path(candidate).resolve()
    if baseline == candidate:
        raise RuntimeError('Baseline and candidate destinations must differ')
    for path in (baseline, candidate):
        if not path.is_relative_to(ROOT / 'build') or path == ROOT / 'build':
            raise RuntimeError('Snapshot must be an isolated child of repository build/')
    patch = ROOT / 'tools/ps2/next12_pb_scratch_candidate.patch'
    if sha(patch) != PATCH_SHA:
        raise RuntimeError('Candidate patch checksum differs')
    files = baseline_files()
    expected_base = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    for path, is_candidate in ((baseline, False), (candidate, True)):
        if path.exists() and any(path.iterdir()):
            validate(path, is_candidate)
            continue
        path.mkdir(parents=True, exist_ok=True)
        for name, data in files.items():
            target = path / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        expected = dict(expected_base)
        manifest = {'revision': REVISION, 'source_sha256': expected}
        if is_candidate:
            command = ['git', 'apply', '--unidiff-zero', '--directory=' + path.relative_to(ROOT).as_posix()]
            subprocess.run([*command, '--check', str(patch)], cwd=ROOT, check=True)
            subprocess.run([*command, str(patch)], cwd=ROOT, check=True)
            expected[SOURCE] = CANDIDATE_SHA
            manifest['variant'] = VARIANT
        (path / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        validate(path, is_candidate)
    return {'revision': REVISION, 'baseline': str(baseline), 'candidate': str(candidate),
            'source_files': len(files), 'only_source_change': SOURCE,
            'baseline_sha256': BASE_SHA, 'candidate_sha256': CANDIDATE_SHA,
            'patch_sha256': PATCH_SHA}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=DEFAULT_BASE)
    parser.add_argument('--out', type=Path, default=DEFAULT_CANDIDATE)
    args = parser.parse_args()
    print(json.dumps(prepare(args.baseline, args.out), indent=2))


if __name__ == '__main__':
    main()
