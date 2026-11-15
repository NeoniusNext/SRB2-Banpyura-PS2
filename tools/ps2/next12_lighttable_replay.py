"""Replay the exact light-table candidate into a fresh isolated 602-file tree.

This does not edit production source or invoke a compiler/emulator. The patch,
every input and every output file are checked against their complete hashes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replay(baseline, out):
    out = out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        raise SystemExit('A fresh isolated output under build/ is required')
    manifest_path = ROOT / 'tools/ps2/next12_lighttable_candidate.json'
    manifest = json.loads(manifest_path.read_text())
    patch = ROOT / 'tools/ps2/next12_lighttable_candidate.patch'
    baseline_manifest = ROOT / 'tools/ps2/next12_baseline.json'
    if sha(patch) != manifest['patch_sha256'] or sha(baseline_manifest) != manifest['baseline_manifest_sha256']:
        raise SystemExit('Pinned patch or complete baseline manifest changed')
    expected = json.loads(baseline_manifest.read_text())['source_sha256']
    actual = {str(p.relative_to(baseline)): sha(p) for p in sorted((baseline / 'src').rglob('*')) if p.is_file()}
    if actual != expected or len(actual) != 602:
        raise SystemExit('The complete602 baseline source differs')
    out.mkdir(parents=True)
    shutil.copytree(baseline / 'src', out / 'src')
    command = ['git', 'apply', '--directory=' + str(out.relative_to(ROOT)), str(patch)]
    subprocess.run([*command[:2], '--check', *command[2:]], cwd=ROOT, check=True)
    subprocess.run(command, cwd=ROOT, check=True)
    observed = {str(p.relative_to(out)): sha(p) for p in sorted((out / 'src').rglob('*')) if p.is_file()}
    required = dict(expected)
    for name, entry in manifest['changed'].items():
        if expected[name] != entry['baseline_sha256']:
            raise SystemExit('Candidate belongs to another complete baseline')
        required[name] = entry['candidate_sha256']
    if observed != required:
        raise SystemExit('Replayed complete candidate source differs')
    pinned = out / 'pinned-tools'
    pinned.mkdir()
    for path in (Path(__file__).resolve(), manifest_path, patch, baseline_manifest):
        shutil.copyfile(path, pinned / path.name)
    report = {'status': manifest['status'], 'source_revision': manifest['source_revision'],
              'source_files': len(observed), 'changed_files': sorted(manifest['changed']),
              'all_candidate_source_sha256': observed,
              'pinned_tools_sha256': {p.name: sha(p) for p in pinned.iterdir()},
              'production_src_edited': False, 'compiler_or_emulator_invoked': False}
    (out / 'replay-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next12/baseline-src')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    report = replay(args.baseline.resolve(), args.out)
    print('Replayed exact light-table candidate:602 files, ' + str(len(report['changed_files'])) + ' changed')


if __name__ == '__main__':
    main()
