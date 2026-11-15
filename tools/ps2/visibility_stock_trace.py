#!/usr/bin/env python3
"""Trace a transient stock full-size texture using unchanged shipping code.

Uses admitted fixed-local-RNG diagnostic ELF copies, not a new instrumentation
build. The existing -hwtrace observer is identical in both roles. HWT records
expose actual stored PSM/dimensions/address, not nblk/TBW; report that limitation
and separately bind the complete native fixture's directly queried footprints.
No global VRAM or FPS improvement is inferred from this transient observation.
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
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def footprint_sections(text):
    start = '// ---- footprint of a texture buffer in 256-byte blocks'
    end = '// ---- record table and LRU ----'
    stride = '\t\ttbw = psm == PSM_T8 ?'
    stride_end = '\n\t\tif (split)\n\t\t\tnblk +='
    if any(text.count(marker) != 1 for marker in (start, end, stride, stride_end)):
        raise RuntimeError('Ambiguous native footprint/stride contract')
    return {'tables_and_complete_footprint_function': text[text.index(start):text.index(end)],
            'complete_stride_PSM_and_nblk_selection': text[text.index(stride):text.index(stride_end)]}


def bind_footprint_contract(candidate, fixture_source):
    relative = 'src/ps2/hw/ps2_hw_tex.inc'
    before, after = fixture_source / relative, candidate / relative
    sections_before, sections_after = footprint_sections(before.read_text()), footprint_sections(after.read_text())
    if sections_before != sections_after:
        raise RuntimeError('Native-tested footprint tables/function or stride/nblk contract changed')
    private = 'src/ps2/hw/ps2_hw_priv.inc'
    if sha(candidate / private) != sha(fixture_source / private):
        raise RuntimeError('Native-tested private format constants or record/query layout changed')
    return {'candidate_complete_texture_file_sha256': sha(after),
            'fixture_complete_texture_file_sha256': sha(before),
            'whole_texture_upload_file_identical': sha(after) == sha(before),
            'exact_unchanged_sections_sha256': {k: hashlib.sha256(v.encode()).hexdigest() for k, v in sections_after.items()},
            'exact_private_format_constants_record_and_query_layout_sha256': sha(candidate / private),
            'scope': 'Exact allocation/layout contract only. Packing/classification code outside these sections may differ and requires separate correctness evidence; no whole-upload equivalence is claimed.'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline-build', 'candidate-build', 'baseline-elf', 'candidate-elf', 'baseline-source', 'candidate-source', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--native-fixture', type=Path, default=ROOT / 'build/nov16-hwopt/p4-clip-native-fixed-v1')
    p.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    p.add_argument('--seed', type=lambda x: int(x, 0), default=0x01b2d3f5)
    p.add_argument('--map', default='MAP02')
    p.add_argument('--frames', type=int, default=90)
    p.add_argument('--trace-frame', type=int, default=70)
    p.add_argument('--texture', default='F:GFS_CLD3')
    p.add_argument('--analyze-only', action='store_true')
    a = p.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build' or a.frames < 1 or a.trace_frame < 1:
        p.error('Use a fresh child of build/ and positive frame counts')
    builds = {role: getattr(a, role + '_build').resolve() for role in ('baseline', 'candidate')}
    elfs = {role: getattr(a, role + '_elf').resolve() for role in builds}
    sources = {role: getattr(a, role + '_source').resolve() for role in builds}
    admissions = {role: admit_elf(builds[role], elf, a.seed) for role, elf in elfs.items()}
    assets = sorted(a.pak.resolve().glob('*.PAK')) + [a.pak.resolve() / 'FINEACON.DAT']
    if not {'SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK', 'FINEACON.DAT'}.issubset({x.name for x in assets if x.is_file()}):
        p.error('Complete stock assets required')
    fixture = a.native_fixture.resolve()
    fixture_report = json.loads((fixture / 'native-summary.json').read_text())
    footprint_binding = bind_footprint_contract(sources['candidate'], fixture / 'source')
    native_1024 = [line for mode in fixture_report['modes'].values() for line in mode['allocation_observations']
                   if '1024x1024 native P8=4096 P4=2048 saving=524288 bytes' in line]
    if len(native_1024) != 2:
        raise RuntimeError('Both directly measured CT32/CT16 native 1024-square footprints required')
    engine = ['-singletics', '-keepspawn', '-loadprof', '-loadhash', '-zquit', str(a.frames), '-astat',
              '-renderer', 'Hardware', '-ps2prof', '-hwstats', '35', '-hwtrace', str(a.trace_frame), '-hwdbg', '33554432']
    inputs = [Path(__file__), *elfs.values(), *assets, fixture / 'native-summary.json', fixture / 'build-report.json',
              fixture / 'source/src/ps2/hw/ps2_hw_tex.inc', fixture / 'source/src/ps2/hw/ps2_hw_priv.inc']
    inputs += [build / name for build in builds.values() for name in ('SRB2.ELF', 'build-report.json')]
    inputs += [path for source in sources.values() for path in sorted((source / 'src').rglob('*')) if path.is_file()]
    manifest = {str(path): sha(path) for path in inputs}
    ready = {'input_sha256': manifest, 'admissions': admissions, 'same_engine_args': engine, 'map': a.map,
             'frames': a.frames, 'trace_frame': a.trace_frame, 'texture': a.texture, 'seed': a.seed}
    out.mkdir(parents=True, exist_ok=True)
    readiness = out / 'readiness.json'
    if a.analyze_only:
        if json.loads(readiness.read_text()) != ready:
            raise RuntimeError('Frozen trace inputs changed')
    else:
        if readiness.exists():
            p.error('Choose a fresh --out; existing evidence is immutable')
        readiness.write_text(json.dumps(ready, indent=2) + '\n')
    report = {'scope': __doc__, 'physical_PS2_tested': False, 'FPS_claimed': False,
              'global_pool_reduction_claimed': False, 'runs': {}, 'errors': [],
              'native_footprint_source_binding': footprint_binding,
              'native_fixture_directly_measured_1024_square_footprints': native_1024,
              'stock_footprint_interpretation': 'HWT exposes actual stored format/dimensions/address only. For an observed full 1024x1024 image, fixed frozen upload stride and the same native-tested footprint code determine P8=4096 and P4=2048 blocks. Those nblk numbers were directly queried in the separate native fixture, not printed from this stock run.'}
    for role, elf in elfs.items():
        run, runtime = out / 'runs' / role, out / 'runtime' / role
        if not a.analyze_only:
            if run.exists():
                raise RuntimeError('Trace run exists: ' + str(run))
            command = [sys.executable, '-B', str(TOOLS / 'opt_run.py'), '--name', role, '--elf', str(elf),
                       '--ram', '32', '--pak', str(a.pak.resolve()), '--out', str(out / 'runs'), '--map', a.map,
                       '--timeout', '180', '--cfg', 'con_hudlines "0"', '--', *engine]
            print('START stock trace ' + role, flush=True)
            result = run_verified(command, runtime, [*inputs, readiness])
            if result.returncode:
                raise RuntimeError('Official trace runner failed: ' + role)
        boot = (run / 'boot.txt').read_text(errors='replace')
        summary = json.loads((run / 'summary.json').read_text())
        pre, post = [json.loads((runtime / ('run-' + tag + '.json')).read_text()) for tag in ('pre', 'post')]
        errors = []
        expected = ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', a.map, *engine]
        if (run / 'ps2args').read_text().splitlines() != expected or sha(run / 'SRB2.ELF') != sha(elf):
            errors.append('Staged ELF/arguments differ')
        if summary['rc'] or not summary['done'] or summary['ram'] != 32 or summary['errors']:
            errors.append('Official 32-MiB run incomplete')
        if re.search(r'I_Error|HEAP CHECK FAILED|Out of memory|WARNING:|^HWSKIP |HWD texture not resident', boot, re.M):
            errors.append('Runtime warning/error or missing content')
        if 'HWE startup renderer=Hardware chosen=2 hardware=1' not in boot or 'HARDWARE -> SOFTWARE' in boot:
            errors.append('Hardware renderer absent or abandoned')
        if any(map(int, re.findall(r'(?:underruns|hmis|herr)=(\d+)', boot))):
            errors.append('Audio underrun/hash error')
        for key in ('all_frozen_inputs_unchanged', 'all_non_UI_settings_unchanged', 'emulator_binary_unchanged'):
            if not post.get(key):
                errors.append('Runtime invariant false: ' + key)
        if any(sha(run / asset.name) != sha(asset) for asset in assets):
            errors.append('Staged asset mismatch')
        observations = []
        for line in boot.splitlines():
            match = re.match(r'^HWT fans .*?tex=' + re.escape(a.texture) + r' rec=(\d+) blk=(\d+) (\d+)x(\d+) psm=(\d+) clut=(\d+)$', line)
            if match:
                record, block, w, h, psm, clut = map(int, match.groups())
                observations.append({'raw': line, 'record': record, 'block256': block, 'width': w, 'height': h, 'psm': psm, 'clut': clut})
        expected_psm = 19 if role == 'baseline' else 20
        if not observations or not any(x['psm'] == expected_psm and x['width'] == x['height'] == 1024 for x in observations):
            errors.append('Expected transient stock 1024-square stored format not observed')
        if not summary['zstat'] or int(summary['zstat'][0]['frames']) != a.frames or int(summary['zstat'][0]['ram']) != 33554432:
            errors.append('Native frame count or physical memory mismatch')
        report['runs'][role] = {'observations': observations, 'errors': errors, 'ZSTAT': summary['zstat'],
                                'boot_sha256': sha(run / 'boot.txt'), 'ELF_sha256': sha(elf),
                                'observer_cfg_sha256': sha(run / '.srb2/reference.cfg'),
                                'critical_settings': pre['critical_settings'], 'emulator_binary_sha256': pre['emulator_binary_sha256']}
        report['errors'] += [role + ': ' + error for error in errors]
    before, after = report['runs'].values()
    for key in ('observer_cfg_sha256', 'critical_settings', 'emulator_binary_sha256'):
        if before[key] != after[key]:
            report['errors'].append('Matched runtime condition differs: ' + key)
    if manifest != {str(path): sha(path) for path in inputs}:
        report['errors'].append('Trace inputs changed during execution')
    report['transient_stock_format_witness_complete'] = not report['errors']
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'transient_stock_format_witness_complete': not report['errors'],
                      'observations': {role: data['observations'] for role, data in report['runs'].items()},
                      'errors': report['errors']}, indent=2))
    return 1 if report['errors'] else 0


if __name__ == '__main__':
    sys.exit(main())
