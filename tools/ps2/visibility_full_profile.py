#!/usr/bin/env python3
"""Record matched explicit-full-size Hardware profiles without relaxing old gates.

Both roles use -hwdbg 33554432, 32 MiB and the same admitted local-RNG seed.
This diagnostic retains all zone failures/evictions and native GS counters.
It validates completion, matching input/settings/observers and no missing or
skipped driver content. It does not turn resource-stream changes into a strict
quality pass, or compare this opt-in mode's pressure with default quality.
Image admission remains the separate strict frame/explicit-bound protocol.
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
FLAGS = ['-singletics', '-keepspawn', '-loadprof', '-loadhash', '-zck', '-zheap', '35', '-zstack']


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def engine_args(frames):
    return [*FLAGS, '-zquit', str(frames), '-astat', '-renderer', 'Hardware', '-ps2prof',
            '-hwfb', '-hwstats', '35', '-hwpolyhash', '-zspill', '-hwdbg', '33554432']


def inspect(run, runtime, elf, assets, mapname, frames):
    boot = (run / 'boot.txt').read_text(errors='replace')
    summary = json.loads((run / 'summary.json').read_text())
    pre, post = [json.loads((runtime / ('run-' + tag + '.json')).read_text()) for tag in ('pre', 'post')]
    errors = []
    if summary['rc'] or not summary['done'] or summary['ram'] != 32 or summary['errors'] or len(summary['zstat']) != 1:
        errors.append('Incomplete or failed official runner')
    for key in ('all_frozen_inputs_unchanged', 'all_non_UI_settings_unchanged', 'emulator_binary_unchanged'):
        if not post.get(key):
            errors.append('Runtime invariant false: ' + key)
    zstat = {key: int(value) for key, value in summary['zstat'][0].items()} if summary['zstat'] else {}
    for key, value in {'ram': 33554432, 'class': 32, 'frames': frames, 'levelframes': frames}.items():
        if zstat.get(key) != value:
            errors.append('Physical memory/frame mismatch: ' + key)
    if sha(run / 'SRB2.ELF') != sha(elf):
        errors.append('Staged diagnostic ELF changed')
    expected = ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', mapname, *engine_args(frames)]
    if (run / 'ps2args').read_text().splitlines() != expected:
        errors.append('Engine arguments differ from fixed full-size protocol')
    if 'HWE startup renderer=Hardware chosen=2 hardware=1' not in boot or re.search(
            r'Switching to the Software renderer|HARDWARE -> SOFTWARE', boot):
        errors.append('Hardware renderer absent or abandoned')
    if re.search(r'I_Error|HEAP CHECK FAILED|Out of memory|^ERROR:|WARNING:', boot, re.M):
        errors.append('Engine fatal error or warning')
    if re.search(r'^HWSKIP |^HWD texture not resident ', boot, re.M):
        errors.append('Driver reported missing/skipped content outside profile windows')
    if any(map(int, re.findall(r'(?:underruns|hmis|herr)=(\d+)', boot))):
        errors.append('Audio underrun/hash error')
    if 'con_hudlines "0"\n' not in (run / '.srb2/reference.cfg').read_text():
        errors.append('Matched observer configuration absent')
    if not (run / 'reference.cfg').is_file():
        errors.append('Saved engine configuration absent')
    for asset in assets:
        if sha(run / asset.name) != sha(asset):
            errors.append('Staged stock asset differs: ' + asset.name)
    windows = []
    for line in boot.splitlines():
        if not line.startswith('HWPROF win='):
            continue
        counters = {k: int(v) for k, v in re.findall(r'\b([a-zA-Z][a-zA-Z0-9_]*)=(\d+)(?![\d/])', line)}
        pool = re.search(r'\bpool=(\d+)/(\d+)', line)
        if not pool:
            errors.append('Native allocated pool counter absent')
            continue
        counters['pool_used_blocks'], counters['pool_capacity_blocks'] = map(int, pool.groups())
        counters['pool_used_bytes'] = counters['pool_used_blocks'] * 256
        counters['pool_capacity_bytes'] = counters['pool_capacity_blocks'] * 256
        for key in ('missing', 'skipped', 'cap', 'capchg', 'restamp', 'decim'):
            if counters.get(key) != 0:
                errors.append('Missing/skipped content or changed resolution/cap: ' + key)
        windows.append({'raw': line, 'counters': counters})
    if not windows:
        errors.append('Native GS profile windows absent')
    working_set = []
    pattern = r'^HWWS (\S+) (\d+)x(\d+) psm (\d+) blocks (\d+) clut (\d+) scr (\d+) wrap ([01]{2}) fl (0x[0-9a-f]+)$'
    for match in re.finditer(pattern, boot, re.M):
        name, w, h, psm, blocks, clut, screen, wrap, flags = match.groups()
        working_set.append({'name': name, 'width': int(w), 'height': int(h), 'psm': int(psm),
                            'blocks': int(blocks), 'bytes': int(blocks) * 256, 'clut': int(clut),
                            'screen': int(screen), 'wrap': wrap, 'flags': flags})
    streams = {prefix: [line for line in boot.splitlines() if line.startswith(prefix)]
               for prefix in ('LHASH ', 'HWPH ', 'HWLT ', 'HWWS ')}
    if not streams['LHASH '] or not streams['HWPH ']:
        errors.append('Loader or geometry observation absent')
    anchor = re.findall(r'^\[zck\]\s+level-free-after\s+.*?evicted=(\d+)', boot, re.M)
    gameplay_evictions = zstat.get('evicted', 0) - int(anchor[0]) if len(anchor) == 1 else None
    return {'run': str(run), 'runtime': str(runtime), 'errors': errors, 'ZSTAT': zstat,
            'gameplay_zone_evictions_after_startup_flush': gameplay_evictions,
            'GS_windows': windows, 'working_set_record_observations': working_set,
            'working_set_scope': 'Top48 record observations at the existing checkpoint. Shared plain/keyed records can refer to one image; their bytes must not be summed as unique occupancy. Later evicted images can be absent.',
            'ordered_streams': streams,
            'input_SHA256': {'boot.txt': sha(run / 'boot.txt'), 'summary.json': sha(run / 'summary.json'),
                              'SRB2.ELF': sha(run / 'SRB2.ELF'), 'ps2args': sha(run / 'ps2args')},
            'observer_cfg_sha256': sha(run / '.srb2/reference.cfg'),
            'saved_cfg_sha256': sha(run / 'reference.cfg') if (run / 'reference.cfg').is_file() else None,
            'critical_settings': pre['critical_settings'],
            'emulator_binary_sha256': pre['emulator_binary_sha256']}


def compare(before, after):
    def identity(row):
        return tuple(row[k] for k in ('name', 'width', 'height', 'clut', 'screen', 'wrap', 'flags'))
    a = {identity(row): row for row in before['working_set_record_observations'] if row['name'].startswith(('T:', 'F:'))}
    b = {identity(row): row for row in after['working_set_record_observations'] if row['name'].startswith(('T:', 'F:'))}
    matched = [{'identity': list(key), 'baseline': a[key], 'candidate': b[key],
                'observed_record_bytes_delta': b[key]['bytes'] - a[key]['bytes']} for key in sorted(a.keys() & b.keys())]
    stream_results = {}
    for prefix, rows in before['ordered_streams'].items():
        other = after['ordered_streams'][prefix]
        stream_results[prefix] = {'rows': [len(rows), len(other)], 'equal': bool(rows) and rows == other,
                                  'sha256_each': [hashlib.sha256('\n'.join(x).encode()).hexdigest() for x in (rows, other)]}
    return {'numeric_ZSTAT_deltas': {key: after['ZSTAT'][key] - value for key, value in before['ZSTAT'].items() if key in after['ZSTAT']},
            'ordered_stream_comparison_not_a_quality_gate': stream_results,
            'matched_record_footprints': matched,
            'baseline_only_checkpoint_records': [a[key] for key in sorted(a.keys() - b.keys())],
            'candidate_only_checkpoint_records': [b[key] for key in sorted(b.keys() - a.keys())],
            'pool_interpretation': 'H.used_blocks/H.pool_blocks are actual native allocated occupancy at each observed window. More surviving full images can increase total occupancy despite smaller individual footprints; no summed-record or global VRAM-saving claim is inferred.'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline-build', 'candidate-build', 'baseline-elf', 'candidate-elf', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--baseline-run', type=Path, help='Reuse an existing exact-protocol baseline run')
    p.add_argument('--baseline-runtime', type=Path)
    p.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    p.add_argument('--seed', type=lambda x: int(x, 0), default=0x01b2d3f5)
    p.add_argument('--map', default='MAP02')
    p.add_argument('--frames', type=int, default=350)
    p.add_argument('--analyze-only', action='store_true')
    a = p.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build' or a.frames < 1:
        p.error('Use a child of build/ and positive frames')
    if bool(a.baseline_run) != bool(a.baseline_runtime):
        p.error('Existing baseline requires both --baseline-run and --baseline-runtime')
    elfs = {role: getattr(a, role + '_elf').resolve() for role in ('baseline', 'candidate')}
    builds = {role: getattr(a, role + '_build').resolve() for role in elfs}
    admissions = {role: admit_elf(builds[role], elf, a.seed) for role, elf in elfs.items()}
    assets = sorted(a.pak.resolve().glob('*.PAK')) + [a.pak.resolve() / 'FINEACON.DAT']
    if not {'SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK', 'FINEACON.DAT'}.issubset({x.name for x in assets if x.is_file()}):
        p.error('Complete stock assets required')
    runs = {role: out / 'runs' / role for role in elfs}
    runtimes = {role: out / 'runtime' / role for role in elfs}
    if a.baseline_run:
        runs['baseline'], runtimes['baseline'] = a.baseline_run.resolve(), a.baseline_runtime.resolve()
    inputs = [Path(__file__), *elfs.values(), *assets]
    inputs += [build / name for build in builds.values() for name in ('SRB2.ELF', 'build-report.json')]
    if a.baseline_run:
        inputs += [runs['baseline'] / name for name in ('boot.txt', 'summary.json', 'ps2args')]
        inputs += [runtimes['baseline'] / ('run-' + name + '.json') for name in ('pre', 'post')]
    manifest = {str(path): sha(path) for path in inputs}
    ready = {'admissions': admissions, 'input_sha256': manifest, 'map': a.map, 'frames': a.frames,
             'seed': a.seed, 'engine_args_both_roles': engine_args(a.frames),
             'runs': {k: str(v) for k, v in runs.items()}, 'runtimes': {k: str(v) for k, v in runtimes.items()}}
    out.mkdir(parents=True, exist_ok=True)
    readiness = out / 'readiness.json'
    if a.analyze_only:
        if json.loads(readiness.read_text()) != ready:
            raise RuntimeError('Frozen raw-profile inputs changed')
    else:
        if readiness.exists():
            p.error('Choose a fresh --out; existing stages are immutable')
        readiness.write_text(json.dumps(ready, indent=2) + '\n')
    report = {'scope': 'Raw same-full-size-mode diagnostic; separate strict image/resource gates remain unchanged. Zone failures and evictions are retained, not waived into a strict pass.',
              'physical_PS2_tested': False, 'FPS_claimed': False, 'default_mode_regression_claimed': False,
              'input_sha256': manifest, 'runs': {}, 'errors': []}
    for role, elf in elfs.items():
        if not a.analyze_only and not (role == 'baseline' and a.baseline_run):
            if runs[role].exists():
                raise RuntimeError('Raw-profile run already exists: ' + str(runs[role]))
            command = [sys.executable, '-B', str(TOOLS / 'opt_run.py'), '--name', role, '--elf', str(elf),
                       '--ram', '32', '--pak', str(a.pak.resolve()), '--out', str(out / 'runs'), '--map', a.map,
                       '--timeout', '300', '--cfg', 'con_hudlines "0"', '--', *engine_args(a.frames)]
            result = run_verified(command, runtimes[role], [*inputs, readiness])
            if result.returncode:
                raise RuntimeError('Official runner failed: ' + role)
        report['runs'][role] = inspect(runs[role], runtimes[role], elf, assets, a.map, a.frames)
        report['errors'] += [role + ': ' + x for x in report['runs'][role]['errors']]
    before, after = report['runs'].values()
    for key in ('critical_settings', 'emulator_binary_sha256', 'observer_cfg_sha256', 'saved_cfg_sha256'):
        if before[key] != after[key]:
            report['errors'].append('Matched runtime settings differ: ' + key)
    report['comparison'] = compare(before, after)
    if manifest != {str(path): sha(path) for path in inputs}:
        report['errors'].append('Inputs changed during raw profiling')
    report['profile_completed_without_missing_content'] = not report['errors']
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'profile_completed_without_missing_content': not report['errors'],
                      'numeric_ZSTAT_deltas': report['comparison']['numeric_ZSTAT_deltas'], 'errors': report['errors']}, indent=2))
    return 1 if report['errors'] else 0


if __name__ == '__main__':
    sys.exit(main())
