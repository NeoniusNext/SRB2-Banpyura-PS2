#!/usr/bin/env python3
"""Recreate a rejected pass11 memory experiment in an isolated build tree.

Never edits the live source/index, compiles code or runs an emulator. Existing
destinations are verified without modification; mismatches are refused.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
PATCH_SHA256 = '03498a9458f3bbf58c960064a8dba50451d4401dd8572256167045a9821a644b'
BASE = {
    'src/r_defs.h': '6c88912e24bb3f85cc8d0166f5734ad5e01e42dfe0bad3ae266e91cba831f62c',
    'src/p_slopes.c': '3ad3ec8f379a2f9b1395aa6eccd49832e561a70dd09678d3318cf8d82c6f0e4d',
}
CANDIDATE = {
    'src/r_defs.h': 'd58cff0b673ea3cb131737bd80c18a0a17fbfe080d62f812e663d6115de277a3',
    'src/p_slopes.c': 'ae6d25600a4e9a8d04b9aa11fd2e349a889db875dbec5522b6f88ea52cd43e78',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_manifest(path):
    return {str(p.relative_to(path)): sha(p) for p in sorted((path / 'src').rglob('*')) if p.is_file()}


def validate_candidate(path):
    """Require a complete checked baseline-only rejected source overlay."""
    path = Path(path).resolve()
    hint = 'Run tools/ps2/next11_memory_replay.py --stage header-only or --stage byte-pool first.'
    if not (path / 'manifest.json').is_file():
        raise RuntimeError('A checked isolated rejected candidate is required. ' + hint)
    actual = source_manifest(path)
    if actual != json.loads((path / 'manifest.json').read_text()):
        raise RuntimeError('Rejected source snapshot differs from its full manifest')
    preceding = json.loads((TOOLS / 'next11_baseline.json').read_text())['source_sha256']
    expected = {name: digest for name, digest in preceding.items() if name.startswith('src/')}
    if actual.get('src/r_defs.h') != CANDIDATE['src/r_defs.h']:
        raise RuntimeError('The requested tree is not a rejected slope candidate; unchanged ROOT is not measured. ' + hint)
    slope = actual.get('src/p_slopes.c')
    if slope not in (BASE['src/p_slopes.c'], CANDIDATE['src/p_slopes.c']):
        raise RuntimeError('Unknown slope-pool source revision')
    stage = 'header-only' if slope == BASE['src/p_slopes.c'] else 'byte-pool'
    expected['src/r_defs.h'] = CANDIDATE['src/r_defs.h']
    expected['src/p_slopes.c'] = slope
    if actual != expected:
        raise RuntimeError('Rejected source contains changes outside its checked baseline-only overlay')
    return stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=('header-only', 'byte-pool'), default='byte-pool')
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next11/baseline-src')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    baseline = args.baseline.resolve()
    out = (args.out or ROOT / ('build/nov15-next11/memory/' +
           ('byte-pool/candidate-src' if args.stage == 'byte-pool' else 'header-only-replay-src'))).resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build':
        parser.error('--out must be an isolated child of the repository build directory')
    patch = TOOLS / 'next11_memory_candidate.patch'
    if sha(patch) != PATCH_SHA256:
        raise RuntimeError('Rejected candidate patch checksum differs')
    subprocess.run([sys.executable, '-B', TOOLS / 'prepare_next11_baseline.py', '--out', baseline],
                   cwd=ROOT, check=True)
    if any(sha(baseline / path) != digest for path, digest in BASE.items()):
        raise RuntimeError('Preceding production memory source pins differ')
    preceding = source_manifest(baseline)
    expected = dict(preceding)
    expected['src/r_defs.h'] = CANDIDATE['src/r_defs.h']
    if args.stage == 'byte-pool':
        expected['src/p_slopes.c'] = CANDIDATE['src/p_slopes.c']
    blocks = re.split(r'(?=^diff --git )', patch.read_text(), flags=re.M)
    selected = ''.join(block for block in blocks if block and
                       (args.stage == 'byte-pool' or block.startswith('diff --git a/src/r_defs.h ')))
    existed = out.exists() and any(out.iterdir())
    if existed:
        if source_manifest(out) != expected:
            raise RuntimeError('Existing replay destination differs; choose a fresh --out')
        if (out / 'manifest.json').exists() and json.loads((out / 'manifest.json').read_text()) != expected:
            raise RuntimeError('Existing replay source manifest differs')
    else:
        out.mkdir(parents=True, exist_ok=True)
        shutil.copytree(baseline / 'src', out / 'src')
        command = ['git', 'apply', '--unidiff-zero', '--directory=' + out.relative_to(ROOT).as_posix()]
        subprocess.run([*command, '--check', '-'], input=selected, cwd=ROOT, text=True, check=True)
        subprocess.run([*command, '-'], input=selected, cwd=ROOT, text=True, check=True)
        if source_manifest(out) != expected:
            raise RuntimeError('Rejected candidate replay differs from its complete source pins')
        (out / 'manifest.json').write_text(json.dumps(expected, indent=2) + '\n')
    if validate_candidate(out) != args.stage:
        raise RuntimeError('Rejected source stage differs')
    differences = [name for name in expected if expected[name] != preceding[name]]
    result = {'stage': args.stage, 'status': 'REJECTED experiment; isolated replay only',
              'out': str(out), 'verified_existing_without_modification': existed,
              'complete_source_files': len(expected), 'only_source_differences': differences,
              'production_memory_source_sha256': {name: expected[name] for name in BASE},
              'patch_sha256': PATCH_SHA256, 'selected_patch_sha256': hashlib.sha256(selected.encode()).hexdigest(),
              'manifest_sha256': sha(out / 'manifest.json')}
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
