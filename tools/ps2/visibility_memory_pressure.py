#!/usr/bin/env python3
"""Compare matched Hardware memory and ordered resource traces, without FPS claims.

Use diagnostic ELF copies from next11_local_rng.py. The seed patch is verified
against each original shipping ELF; no production RNG or quality setting changes.
Default-configuration frame captures are tested separately.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

from next12_runtime import run_verified
from next13_flats_pressure import admit_elf

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inspect(run, elf, expected_args, inputs, frames):
    summary = json.loads((run / 'summary.json').read_text())
    boot = (run / 'boot.txt').read_text(errors='replace')
    if summary['rc'] or not summary['done'] or summary['errors'] or len(summary['zstat']) != 1:
        raise RuntimeError('Incomplete or failed native run: ' + str(run))
    z = {key: int(value) for key, value in summary['zstat'][0].items()}
    for key, value in {'ram': 33554432, 'class': 32, 'frames': frames, 'levelframes': frames}.items():
        if z.get(key) != value:
            raise RuntimeError('Frame count / physical memory mismatch: ' + key)
    if (run / 'ps2args').read_text().splitlines() != expected_args or sha(run / 'SRB2.ELF') != sha(elf):
        raise RuntimeError('Staged arguments or ELF mismatch')
    if 'HWE startup renderer=Hardware chosen=2 hardware=1' not in boot or re.search(
            r'Switching to the Software renderer|HARDWARE -> SOFTWARE', boot):
        raise RuntimeError('Real Hardware renderer absent or abandoned')
    if any(map(int, re.findall(r'(?:underruns|hmis|herr)=(\d+)', boot))) or 'WARNING:' in boot:
        raise RuntimeError('Audio error or unexpected warning')
    if 'con_hudlines "0"\n' not in (run / '.srb2/reference.cfg').read_text():
        raise RuntimeError('Matched observer configuration absent')
    # The existing level-load flush precedes gameplay. Eviction counts are
    # cumulative, so an unchanged final count also rules out later evictions.
    anchor = re.findall(r'^\[zck\]\s+level-free-after\s+.*?evicted=(\d+)', boot, re.M)
    if len(anchor) != 1 or int(anchor[0]) != z['evicted']:
        raise RuntimeError('Evictions occurred after the startup level flush')
    for path in inputs:
        if path.suffix == '.PAK' or path.name == 'FINEACON.DAT':
            if sha(run / path.name) != sha(path):
                raise RuntimeError('Staged asset mismatch: ' + path.name)
    streams = {prefix: [line for line in boot.splitlines() if line.startswith(prefix)]
               for prefix in ('LHASH ', 'HWPH ', 'HWLT ', 'HWWS ')}
    # The timing prefix includes asynchronous waits; compare the complete
    # geometry/upload/LOD/budget tail independently from elapsed cycles.
    streams['HWPROF resource tail'] = [line.partition(' | polys=')[2] for line in boot.splitlines()
                                       if line.startswith('HWPROF win=')]
    return {'ZSTAT': z, 'streams': streams, 'gameplay_evictions_after_startup_flush': 0,
            'boot_sha256': sha(run / 'boot.txt'),
            'summary_sha256': sha(run / 'summary.json')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline-build', 'candidate-build', 'baseline-elf', 'candidate-elf', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    parser.add_argument('--seed', type=lambda value: int(value, 0), default=0x01b2d3f5)
    parser.add_argument('--frames', type=int, default=350)
    parser.add_argument('--map', default='MAP02')
    parser.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    args.out = args.out.resolve()
    if not args.out.is_relative_to(ROOT / 'build') or args.out == ROOT / 'build' or args.frames <= 0:
        parser.error('Use a child of build/ and a positive frame count')
    elfs = {'baseline': args.baseline_elf.resolve(), 'candidate': args.candidate_elf.resolve()}
    builds = {'baseline': args.baseline_build.resolve(), 'candidate': args.candidate_build.resolve()}
    admissions = {role: admit_elf(builds[role], elf, args.seed) for role, elf in elfs.items()}
    assets = sorted(args.pak.resolve().glob('*.PAK')) + [args.pak.resolve() / 'FINEACON.DAT']
    if not {'SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK', 'FINEACON.DAT'}.issubset(
            {p.name for p in assets if p.is_file()}):
        parser.error('Full stock PAK assets required')
    inputs = [Path(__file__), *elfs.values(), *assets]
    inputs += [build / name for build in builds.values() for name in ('SRB2.ELF', 'build-report.json')]
    manifest = {str(path): sha(path) for path in inputs}
    ready = {'admissions': admissions, 'input_sha256': manifest,
             'map': args.map, 'frames': args.frames, 'seed': args.seed}
    args.out.mkdir(parents=True, exist_ok=True)
    readiness = args.out / 'readiness.json'
    if args.analyze_only:
        if json.loads(readiness.read_text()) != ready:
            raise RuntimeError('Frozen pressure inputs changed')
    else:
        if readiness.exists():
            parser.error('Choose a fresh --out; completed stages are immutable')
        readiness.write_text(json.dumps(ready, indent=2) + '\n')
    report = {'runs': {}, 'errors': [], 'FPS_claimed': False, 'physical_PS2_tested': False,
              'observer': 'con_hudlines 0; existing memory/profiler/log observers in both runs'}
    for role, elf in elfs.items():
        engine_args = ['-singletics', '-keepspawn', '-loadprof', '-loadhash', '-zck', '-zheap', '35', '-zstack',
                       '-zquit', str(args.frames), '-astat', '-renderer', 'Hardware', '-ps2prof',
                       '-hwfb', '-hwstats', '35', '-hwpolyhash', '-zspill']
        command = [sys.executable, '-B', str(TOOLS / 'opt_run.py'), '--name', role,
                   '--elf', str(elf), '--ram', '32', '--pak', str(args.pak.resolve()),
                   '--out', str(args.out / 'runs'), '--map', args.map, '--timeout', '300',
                   '--cfg', 'con_hudlines "0"', '--', *engine_args]
        run = args.out / 'runs' / role
        if not args.analyze_only:
            if run.exists():
                raise RuntimeError('Pressure run exists: ' + str(run))
            print('START pressure ' + role, flush=True)
            result = run_verified(command, args.out / 'runtime' / role, [*inputs, readiness])
            if result.returncode:
                raise RuntimeError('Official runner failed: ' + role)
        expected = ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', args.map, *engine_args]
        report['runs'][role] = inspect(run, elf, expected, assets, args.frames)
    before, after = report['runs'].values()
    report['numeric_deltas'] = {key: after['ZSTAT'][key] - value for key, value in before['ZSTAT'].items()}
    report['stream_equality'] = {}
    for prefix, stream in before['streams'].items():
        other = after['streams'][prefix]
        equal = bool(stream) and stream == other
        report['stream_equality'][prefix] = {'rows': [len(stream), len(other)], 'equal': equal}
        if not equal:
            report['errors'].append('Ordered geometry/material/residency/quality stream mismatch: ' + prefix)
    for key in ('ram', 'class', 'map', 'frames', 'levelframes', 'evicted', 'flushes'):
        if report['numeric_deltas'][key]:
            report['errors'].append('Capacity or residency counter mismatch: ' + key)
    for key in ('failed', 'used', 'peak', 'gpeak', 'libcpeak', 'spillpeak', 'spillnow'):
        if report['numeric_deltas'][key] > 0:
            report['errors'].append('Memory pressure increased: ' + key)
    for key in ('arena', 'free', 'largest', 'libcfree'):
        if report['numeric_deltas'][key] < 0:
            report['errors'].append('Available memory or arena capacity decreased: ' + key)
    if manifest != {str(path): sha(path) for path in inputs}:
        report['errors'].append('Pressure inputs changed during execution')
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'numeric_deltas': report['numeric_deltas'], 'errors': report['errors']}, indent=2))
    return 1 if report['errors'] else 0


if __name__ == '__main__':
    sys.exit(main())
