#!/usr/bin/env python3
"""Reuse the exact pass11 Hardware image oracle for the isolated PB candidate.

Readiness is the default. --run uses only the coordinator's granted emulator,
one scene at a time through the official verified wrapper. No compilation.
"""
import argparse
import json
from pathlib import Path
import sys

from next12_pb_scratch_run import HELPERS, PROOFS, analyze, sha
import next12_pb_scratch_replay as replay
from next11_smoke_check import runtime_evidence
from next12_runtime import run_verified

ROOT = replay.ROOT
TOOLS = ROOT / 'tools/ps2'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene', choices=('map02', 'fog-stereo'), required=True)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov15-next12/pb-scratch')
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--out', type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--run', action='store_true')
    mode.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    replay.validate(replay.DEFAULT_CANDIDATE)
    work = args.work.resolve()
    baseline = (args.baseline or ROOT / ('build/nov15-next11/release-seed1/runs/new-hw-' + args.scene)).resolve()
    out = (args.out or work / ('images-' + args.scene)).resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build':
        parser.error('--out must be an isolated child of build/')
    out.mkdir(parents=True, exist_ok=True)
    patch_path = work / 'local-rng/candidate/patch-report.json'
    patch = json.loads(patch_path.read_text())
    selected = patch['variants']['01b2d3f5']
    elf = Path(selected['ELF'])
    if sha(elf) != selected['ELF_sha256'] or not selected['all_other_ELF_bytes_identical']:
        raise RuntimeError('Candidate fixed-M-RNG diagnostic differs')
    base_patch = json.loads((work / 'local-rng/baseline/patch-report.json').read_text())
    if sha(baseline / 'SRB2.ELF') != base_patch['variants']['01b2d3f5']['ELF_sha256']:
        raise RuntimeError('Historical image oracle is not the exact canonical Hardware diagnostic')
    runtime_dir = baseline.parent.parent / 'runtime' / baseline.name
    before = json.loads((runtime_dir / 'run-pre.json').read_text())
    baseline_command = before['command']
    base_runtime = runtime_evidence(runtime_dir, baseline_command)
    expected_args = (baseline / 'ps2args').read_text().splitlines()
    name = 'pb-images-hw-' + args.scene
    command = list(baseline_command)
    command[command.index('--name') + 1] = name
    command[command.index('--elf') + 1] = str(elf)
    command[command.index('--out') + 1] = str(out / 'runs')
    if expected_args[expected_args.index('-renderer') + 1] != 'Hardware':
        raise RuntimeError('Hardware image baseline required')
    assets = {p.name: sha(p) for p in sorted(baseline.glob('*.PAK'))}
    assets['FINEACON.DAT'] = sha(baseline / 'FINEACON.DAT')
    wanted = {'vidshot-320x200-l%d.ppm' % n for n in (20, 70, 140)}
    captures = {p.name: sha(p) for p in sorted(baseline.glob('vidshot-*.ppm'))}
    if set(captures) != wanted:
        raise RuntimeError('Complete three-image oracle required')
    files = {TOOLS / name: sha(TOOLS / name) for name in HELPERS}
    files[Path(__file__).resolve()] = sha(Path(__file__).resolve())
    files[patch_path] = sha(patch_path)
    files[elf] = sha(elf)
    for proof_name in PROOFS:
        files[work / proof_name] = sha(work / proof_name)
    for path in [work / 'hardware/SRB2.ELF', work / 'hardware/SRB2.map',
                 work / 'hardware/obj/engine_lto.o', work / 'hardware/build-report.json',
                 replay.DEFAULT_CANDIDATE / 'manifest.json']:
        files[path] = sha(path)
    for path in [*sorted(baseline.glob('*.PAK')), baseline / 'FINEACON.DAT',
                 baseline / 'SRB2.ELF', baseline / 'boot.txt', baseline / 'summary.json',
                 baseline / 'ps2args', baseline / 'reference.cfg',
                 runtime_dir / 'run-pre.json', runtime_dir / 'run-post.json',
                 *sorted(baseline.glob('vidshot-*.ppm'))]:
        files[path] = sha(path)
    # Every actual source asset/addon and official wrapper must still match
    # the prior run. Keep its full historical tool/input record as well.
    for original, digest in before['input_sha256'].items():
        path = Path(original)
        if sha(path) != digest:
            raise RuntimeError('Historical oracle frozen input changed: ' + original)
        files[path] = digest
    plan = {'scene': args.scene, 'baseline': str(baseline), 'command': command,
            'baseline_command': baseline_command, 'expected_args': expected_args,
            'candidate_ELF_sha256': sha(elf), 'candidate_source_sha256': replay.CANDIDATE_SHA,
            'complete_baseline_PPMs': captures, 'assets': assets,
            'input_sha256': {str(p): digest for p, digest in files.items()},
            'status': 'READY; historical full PPM oracle checked; emulator not invoked by preparation'}
    readiness = out / 'readiness.json'
    if readiness.exists() and json.loads(readiness.read_text()) != plan:
        raise RuntimeError('Frozen image readiness changed; preserve it and use a fresh --out')
    if not readiness.exists():
        readiness.write_text(json.dumps(plan, indent=2) + '\n')
    if not (args.run or args.analyze_only):
        print(json.dumps({'status': plan['status'], 'readiness': str(readiness)}, indent=2))
        return
    for path, digest in files.items():
        if sha(path) != digest:
            raise RuntimeError('Frozen image input changed')
    run = out / 'runs' / name
    if args.run:
        if run.exists() or (out / 'report.json').exists():
            raise RuntimeError('Image run exists; choose a fresh --out or --analyze-only')
        result = run_verified(command, out / 'runtime' / name, list(files))
        if result.returncode:
            raise RuntimeError('Official image wrapper failed')
    base_entry = {'ELF_sha256': sha(baseline / 'SRB2.ELF'), 'assets': assets}
    new_entry = {'ELF_sha256': sha(elf), 'assets': assets}
    base = analyze(baseline, baseline.name, baseline_command, expected_args, base_entry, 140, 'images')
    new = analyze(run, name, command, expected_args, new_entry, 140, 'images')
    same_images = {filename: {'all_bytes_equal': (baseline / filename).read_bytes() == (run / filename).read_bytes(),
                    'bytes': (baseline / filename).stat().st_size, 'baseline_sha256': captures[filename],
                    'candidate_sha256': sha(run / filename)} for filename in sorted(wanted)}
    errors = ['baseline: ' + err for err in base['errors']] + ['candidate: ' + err for err in new['errors']]
    lhash_equal = bool(base['lhash']) and base['lhash'] == new['lhash']
    settings_equal = base_runtime['critical_settings'] == new['runtime']['critical_settings']
    emulator_equal = base_runtime['emulator_binary_sha256'] == new['runtime']['emulator_binary_sha256']
    if not all(row['all_bytes_equal'] for row in same_images.values()):
        errors.append('complete image bytes differ')
    if not lhash_equal or not settings_equal or not emulator_equal:
        errors.append('loader hashes, full non-UI settings or emulator bytes differ')
    report = {'scene': args.scene, 'readiness_sha256': sha(readiness),
              'candidate_source_sha256': replay.CANDIDATE_SHA, 'baseline': base, 'candidate': new,
              'complete_images': same_images, 'lhash_equal': lhash_equal,
              'full_non_UI_settings_equal': settings_equal, 'emulator_binary_equal': emulator_equal,
              'errors': errors, 'status': 'PASS full native Hardware image oracle' if not errors else 'FAIL'}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'report': str(out / 'report.json'), 'status': report['status'], 'errors': errors}, indent=2))
    if errors:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
