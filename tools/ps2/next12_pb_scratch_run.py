#!/usr/bin/env python3
"""Prepare or replay the matched-seed native Hardware PB memory/image gate.

Default is readiness only. --run consumes the coordinator's one emulator grant,
serially through next12_runtime.py -> opt_run.py -> run_pcsx2.py. No compiler.
Never changes production random initialization, source, emulator settings or ELFs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

import next12_pb_scratch_replay as replay
from next11_integration_check import inspect
from next11_smoke_check import runtime_evidence
from next12_runtime import run_verified

ROOT = replay.ROOT
TOOLS = ROOT / 'tools/ps2'
PROOFS = ('host-profile/report.json', 'lifetime-profile/report.json', 'objectcheck/report.json',
          'profile-guard-binding/report.json')
HELPERS = ('next12_pb_scratch_run.py', 'next12_pb_scratch_replay.py',
           'next12_pb_scratch_candidate.patch', 'next12_pb_scratch_context.py',
           'next12_pb_scratch_hosttest.py', 'next12_pb_scratch_hosttest.c',
           'next12_pb_scratch_lifetime.py', 'next12_pb_scratch_objectcheck.py',
           'next11_local_rng.py', 'next12_runtime.py', 'next11_smoke_check.py',
           'next11_integration_check.py', 'next10_gs_run.py', 'opt_run.py', 'run_pcsx2.py')


def sha(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def pairs(text):
    return {key: int(value) for key, value in re.findall(r'(\w+)=([0-9]+)', text)}


def analyze(run, name, command, expected_args, inputs, frames, phase):
    item = inspect(run, name)
    boot = (run / 'boot.txt').read_text(errors='replace')
    if (run / 'ps2args').read_text().splitlines() != expected_args:
        item['errors'].append('staged exact argument list differs')
    if sha(run / 'SRB2.ELF') != inputs['ELF_sha256']:
        item['errors'].append('staged exact diagnostic ELF differs')
    if 'HWE startup renderer=Hardware chosen=2 hardware=1' not in boot or 'HARDWARE -> SOFTWARE' in boot:
        item['errors'].append('actual native Hardware execution absent')
    for name, digest in inputs['assets'].items():
        if sha(run / name) != digest:
            item['errors'].append('staged asset differs: ' + name)
    item['all_checkpoint_rows'] = re.findall(r'^\[zck\].*$', boot, re.M)
    item['zone_rows'] = re.findall(r'^\[zmem\].*$', boot, re.M)
    item['hardware_profile_rows'] = re.findall(r'^(?:HWPROF\d*|HWFH|HW flip|HW state|HW mem).*$', boot, re.M)
    item['canonical_geometry_hash_rows'] = re.findall(r'^HWPH .+$', boot, re.M)
    item['actual_driver_memory_lines'] = [line for line in boot.splitlines() if line.startswith('HW mem')]
    if phase == 'pressure':
        if not item['zstat'] or item['zstat'][-1].get('levelframes') != str(frames):
            item['errors'].append('exact requested Hardware pressure frames not reached')
        if not re.search(r'^HWPROF50 ', boot, re.M):
            item['errors'].append('actual PB profile/capacity output absent')
    else:
        wanted = {'vidshot-320x200-l%d.ppm' % n for n in (20, 70, 140)}
        if set(item['captures']) != wanted:
            item['errors'].append('three complete native Hardware images absent')
        item['capture_sizes'] = {p.name: p.stat().st_size for p in sorted(run.glob('vidshot-*.ppm'))}
    item['runtime'] = runtime_evidence(run.parent.parent / 'runtime' / run.name, command)
    item['boot_sha256'] = sha(run / 'boot.txt')
    item['ps2args_sha256'] = sha(run / 'ps2args')
    return item


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov15-next12/pb-scratch')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    parser.add_argument('--seed', choices=('01b2d3f5', '9e3779b9'), default='01b2d3f5')
    parser.add_argument('--map', default='MAP02')
    parser.add_argument('--baseline-run', type=Path, help='Reuse an already complete exact fixed-seed native Hardware run and its full verified command')
    parser.add_argument('--frames', type=int, default=350)
    parser.add_argument('--phase', choices=('pressure', 'images'), default='pressure')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--run', action='store_true')
    mode.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    work = args.work.resolve()
    out = (args.out or work / ('gate-' + args.phase + '-' + args.map.lower() + '-' + args.seed)).resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build' or args.frames <= 0:
        parser.error('Choose a child of build/ and a positive frame count')
    replay.validate(replay.DEFAULT_CANDIDATE)
    out.mkdir(parents=True, exist_ok=True)
    if args.run and (out / 'report.json').exists():
        raise RuntimeError('Completed runtime gate exists; choose a fresh --out')
    proof_inputs = {work / name: sha(work / name) for name in PROOFS}
    helpers = {TOOLS / name: sha(TOOLS / name) for name in HELPERS}
    assets = {p.name: sha(p) for p in sorted(args.pak.resolve().glob('*.PAK'))}
    assets['FINEACON.DAT'] = sha(args.pak.resolve() / 'FINEACON.DAT')
    if not {'SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK'}.issubset(assets):
        raise RuntimeError('Complete stock assets are required')
    entries = []
    for tag, build in (('baseline', ROOT / 'build/nov15-next12/baseline/hardware'),
                       ('candidate', work / 'hardware')):
        diagnostic = work / 'local-rng' / tag
        patch_path = diagnostic / 'patch-report.json'
        patch = json.loads(patch_path.read_text())
        variant = patch['variants'][args.seed]
        elf = Path(variant['ELF'])
        if not patch['diagnostic_only'] or not variant['all_other_ELF_bytes_identical'] or sha(elf) != variant['ELF_sha256']:
            raise RuntimeError('Exact fixed-M-RNG diagnostic input differs')
        if sha(build / 'SRB2.ELF') != patch['original_inputs']['elf']['sha256']:
            raise RuntimeError('Production ELF changed after diagnostic preparation')
        if not variant['assembled_template'] or not variant['assembled_template']['reused_without_recompilation']:
            raise RuntimeError('Checked existing same-seed scalar template required')
        name = tag + '-hw-' + args.map.lower() + '-' + args.phase + '-' + str(args.frames)
        game_args = ['-singletics', '-keepspawn', '-loadprof', '-loadhash', '-zck', '-zheap', '1',
                     '-zstack', '-astat', '-renderer', 'Hardware', '-ps2prof', '-hwfb', '-hwstats', '35', '-hwpolyhash']
        if args.phase == 'pressure':
            game_args += ['-zquit', str(args.frames)]
        else:
            game_args += ['-vidshot', 'l20,l70,l140']
        command = [sys.executable, '-B', str(TOOLS / 'opt_run.py'), '--name', name, '--elf', str(elf),
                   '--pak', str(args.pak.resolve()), '--out', str(out / 'runs'), '--map', args.map,
                   '--timeout', '420']
        if args.phase == 'images':
            command += ['--until', 'VIDSHOT COMPLETE 3']
        command += ['--', *game_args]
        expected = ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', args.map, *game_args]
        reused = args.baseline_run.resolve() if args.baseline_run else None
        if reused:
            # The complete existing command is authoritative, including ordering,
            # staged addon names, profiling, and all config/timeout arguments.
            before_path = reused.parent.parent / 'runtime' / reused.name / 'run-pre.json'
            after_path = before_path.with_name('run-post.json')
            if not after_path.is_file() or not (reused / 'summary.json').is_file():
                raise RuntimeError('Reused baseline is still running or lacks complete runtime evidence')
            before = json.loads(before_path.read_text())
            command = list(before['command'])
            expected = (reused / 'ps2args').read_text().splitlines()
            if expected[expected.index('-warp') + 1] != args.map or (tag == 'baseline' and sha(reused / 'SRB2.ELF') != patch['variants'][args.seed]['ELF_sha256']):
                raise RuntimeError('Reused baseline map/seed/ELF differs')
            if args.phase == 'pressure' and expected[expected.index('-zquit') + 1] != str(args.frames):
                raise RuntimeError('Reused baseline frame count differs')
            if tag == 'baseline':
                name = reused.name
            else:
                name = 'pb-' + reused.name.removeprefix('base-')
                command[command.index('--name') + 1] = name
                command[command.index('--elf') + 1] = str(elf)
                command[command.index('--out') + 1] = str(out / 'runs')
        files = {**proof_inputs, **helpers, patch_path: sha(patch_path), elf: sha(elf),
                 build / 'obj/engine_lto.o': sha(build / 'obj/engine_lto.o'),
                 build / 'SRB2.ELF': sha(build / 'SRB2.ELF'), build / 'SRB2.map': sha(build / 'SRB2.map'),
                 build / 'build-report.json': sha(build / 'build-report.json'),
                 replay.DEFAULT_CANDIDATE / 'manifest.json': sha(replay.DEFAULT_CANDIDATE / 'manifest.json')}
        for asset in assets:
            files[args.pak.resolve() / asset] = assets[asset]
        if reused:
            for filename in ('boot.txt', 'summary.json', 'ps2args', 'reference.cfg'):
                path = reused / filename
                files[path] = sha(path)
            for path in (before_path, after_path):
                files[path] = sha(path)
            for original, digest in before['input_sha256'].items():
                path = Path(original)
                if sha(path) != digest:
                    raise RuntimeError('Reused baseline frozen input changed: ' + original)
                files[path] = digest
            for path in sorted(reused.glob('vidshot-*.ppm')):
                files[path] = sha(path)
        entries.append({'variant': tag, 'name': name, 'command': command, 'expected_args': expected,
                        'inputs': {str(p): digest for p, digest in files.items()},
                        'ELF_sha256': sha(elf), 'assets': assets,
                        'reused_baseline': str(reused) if reused and tag == 'baseline' else None})
    plan = {'phase': args.phase, 'seed': args.seed, 'map': args.map, 'frames': args.frames,
            'candidate_source_sha256': replay.CANDIDATE_SHA, 'entries': entries,
            'status': 'READY; no emulator run by readiness preparation'}
    readiness_path = out / 'readiness.json'
    if readiness_path.exists() and json.loads(readiness_path.read_text()) != plan:
        raise RuntimeError('Frozen runtime readiness inputs changed; preserve it and choose a fresh --out')
    if not readiness_path.exists():
        readiness_path.write_text(json.dumps(plan, indent=2) + '\n')
    if not (args.run or args.analyze_only):
        print(json.dumps({'status': plan['status'], 'readiness': str(readiness_path)}, indent=2))
        return
    report = {'phase': args.phase, 'seed': args.seed, 'map': args.map, 'frames': args.frames,
              'readiness_sha256': sha(readiness_path), 'runs': {}, 'errors': []}
    for entry in entries:
        for name, digest in entry['inputs'].items():
            if sha(Path(name)) != digest:
                raise RuntimeError('Frozen runtime input changed: ' + name)
        run = Path(entry['reused_baseline']) if entry['reused_baseline'] else out / 'runs' / entry['name']
        if args.run and not entry['reused_baseline']:
            if run.exists():
                raise RuntimeError('Runtime output exists; use --analyze-only or a fresh --out')
            print('START', entry['name'], flush=True)
            result = run_verified(entry['command'], out / 'runtime' / entry['name'],
                                  [Path(name) for name in entry['inputs']])
            if result.returncode:
                raise RuntimeError('Official runtime wrapper failed: ' + entry['name'])
        item = analyze(run, entry['name'], entry['command'], entry['expected_args'], entry,
                       args.frames, args.phase)
        report['runs'][entry['variant']] = item
        report['errors'] += [entry['variant'] + ': ' + err for err in item['errors']]
    base, current = report['runs']['baseline'], report['runs']['candidate']
    report['lhash_equal'] = bool(base['lhash']) and base['lhash'] == current['lhash']
    if not report['lhash_equal']:
        report['errors'].append('full loader hashes differ')
    report['canonical_geometry_hashes_equal'] = bool(base['canonical_geometry_hash_rows']) and base['canonical_geometry_hash_rows'] == current['canonical_geometry_hash_rows']
    if not report['canonical_geometry_hashes_equal']:
        report['errors'].append('complete canonical geometry hash rows differ')
    if args.phase == 'images':
        captures = {}
        for filename in base['captures']:
            a, b = [(Path(entry['reused_baseline']) if entry['reused_baseline'] else out / 'runs' / entry['name']) / filename for entry in entries]
            equal = a.read_bytes() == b.read_bytes()
            captures[filename] = {'all_bytes_equal': equal, 'baseline_sha256': sha(a),
                                  'candidate_sha256': sha(b), 'complete_bytes': a.stat().st_size}
        report['complete_images'] = captures
        if not captures or not all(row['all_bytes_equal'] for row in captures.values()):
            report['errors'].append('complete image bytes differ')
    else:
        report['final_resources'] = {tag: item['zstat'][-1] for tag, item in report['runs'].items() if item['zstat']}
        if len(report['final_resources']) == 2:
            old, new = report['final_resources']['baseline'], report['final_resources']['candidate']
            report['literal_numeric_resource_deltas'] = {key: int(new[key]) - int(value)
                for key, value in old.items() if str(value).isdigit() and str(new.get(key, '')).isdigit()}
            keys = ('used', 'peak', 'gpeak', 'evicted', 'evictedbytes', 'failed', 'libcpeak', 'stackused', 'spillpeak')
            report['nonincreasing_resource_checks'] = {key: int(new[key]) <= int(old[key])
                for key in keys if key in old and key in new}
        report['resource_policy'] = 'Review all literal checkpoint/zone/HW profile rows. No timing, placement, cache or failure differences are normalized away; requested payload saving alone is not an adoption result.'
    report['status'] = 'COMPLETE; adoption requires explicit review of physical resources and timing'
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'report': str(out / 'report.json'), 'errors': report['errors'],
                      'lhash_equal': report['lhash_equal']}, indent=2))
    if report['errors']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
