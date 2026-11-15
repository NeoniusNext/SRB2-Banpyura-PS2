#!/usr/bin/env python3
"""Prepare matched shipping ZA/spill runs; execute only with an emulator grant.

Preparation and analysis invoke no compiler/emulator. Pressure has con_hudlines
0 in both fresh staged configs. Default-config images are a separate protocol.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

from next13_flats_variant import ROOT
from next12_runtime import run_verified, snapshot
from next11_local_rng import stub_words, verify_patch
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next13_flats_hosttest import function


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def admit_elf(build, elf, seed):
    original = build / 'SRB2.ELF'
    report = json.loads((build / 'build-report.json').read_text())
    if sha(original) != report['elf_sha256']:
        raise RuntimeError('Actual shipping build/ELF differs')
    parsed = Elf32(original)
    symbols = linked_symbols(parsed)
    init, seeder = symbols['M_RandomSeedFromOS'], symbols['M_RandomSeed']
    section = parsed.sections[init['section']]
    start = section[4] + init['value'] - section[3]
    payload = __import__('struct').pack('<10I', *stub_words(seed, seeder['value']))
    verify_patch(original.read_bytes(), elf.read_bytes(), start, init['size'], payload)
    return {'build': str(build), 'shipping_ELF_sha256': sha(original), 'diagnostic_ELF': str(elf),
        'diagnostic_ELF_sha256': sha(elf), 'only_local_RNG_initializer_extent_changed': True,
        'seed': seed, 'initializer_file_offset': start, 'initializer_owned_bytes': init['size']}


def prepare(a):
    a.out.mkdir(parents=True, exist_ok=False)
    seed = int(a.seed, 16)
    builds = {'baseline': a.baseline_build.resolve(), 'candidate': a.candidate_build.resolve()}
    elfs = {'baseline': a.baseline_elf.resolve(), 'candidate': a.candidate_elf.resolve()}
    admissions = {tag: admit_elf(builds[tag], elfs[tag], seed) for tag in builds}
    files = {}
    for tag, build in builds.items():
        for name in ('SRB2.ELF', 'SRB2.map', 'build-report.json', 'obj/engine_lto.o'):
            files[str(build / name)] = sha(build / name)
        files[str(elfs[tag])] = sha(elfs[tag])
    for path in [*a.pak.glob('*.PAK'), a.pak / 'FINEACON.DAT']:
        files[str(path.resolve())] = sha(path)
    pinned = a.out / 'pinned-tools'
    pinned.mkdir()
    for name in ('next13_flats_pressure.py', 'next12_runtime.py', 'next10_gs_run.py',
                 'opt_run.py', 'run_pcsx2.py', 'next11_local_rng.py'):
        src = ROOT / 'tools/ps2' / name
        shutil.copyfile(src, pinned / name)
        files[str(src)] = files[str(pinned / name)] = sha(src)
    # Spill diagnostics go directly to stdout/logstream/MemTrace. Their maximum
    # formatted length is far below I_OutputMsg's 1024-byte stack buffer.
    system = (ROOT / 'src/ps2/i_system.c').read_text()
    output = function(system, 'void I_OutputMsg(')
    spill = function((ROOT / 'src/ps2/ps2_spill.c').read_text(), 'static void *Spill_Alloc(')
    if 'CONS_' in output or 'I_OutputMsg("[spill] %lu bytes (now %lu) ra=%08lx\\n"' not in spill:
        raise RuntimeError('Existing spill stdout boundary changed')
    max_spill = '[spill] 4294967295 bytes (now 4294967295) ra=ffffffff\n'
    if len(max_spill) >= 1024:
        raise RuntimeError('Spill log could allocate its output buffer')
    for name in ('src/ps2/i_system.c', 'src/ps2/ps2_spill.c', 'src/ps2/ps2_mem.c'):
        files[str(ROOT / name)] = sha(ROOT / name)
    cases = []
    common = ['-singletics', '-keepspawn', '-loadprof', '-loadhash', '-zck', '-zheap', '1',
              '-zstack', '-zquit', str(a.frames), '-astat', '-renderer', 'Hardware', '-ps2prof',
              '-hwfb', '-hwstats', '35', '-hwpolyhash', '-zspill']
    for mode, extra in [('default', []), ('legacy', ['-hwgo', '65536']), ('legacy-scalar', ['-hwgo', '65600'])]:
        for tag in ('baseline', 'candidate'):
            name = tag + '-flats-' + mode + '-' + a.map.lower() + '-' + a.seed
            args = common + extra
            command = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', name,
                '--elf', str(elfs[tag]), '--pak', str(a.pak.resolve()), '--out', str(a.out / 'runs'),
                '--map', a.map, '--timeout', str(a.timeout), '--cfg', 'con_hudlines "0"', '--', *args]
            cases.append({'id': tag + '-' + mode, 'role': tag, 'mode': mode, 'name': name,
                'command': command, 'run': str(a.out / 'runs' / name), 'runtime': str(a.out / 'runtime' / name),
                'expected_ps2args': ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', a.map, *args]})
    settings = snapshot(a.out / 'prepared-profile', 'before')
    plan = {'status': 'PREPARED; compiler/emulator not invoked', 'seed': a.seed, 'map': a.map, 'frames': a.frames,
        'admissions': admissions, 'cases': cases, 'immutable_input_sha256': files, 'settings': settings,
        'config_addition': 'con_hudlines "0"', 'no_old_default_config_pressure_run_reused': True,
        'default_no_hwstats_image_protocol_is_separate': True,
        'existing_stdout_spill_boundary': {'I_OutputMsg_sha256': hashlib.sha256(output.encode()).hexdigest(),
            'Spill_Alloc_sha256': hashlib.sha256(spill.encode()).hexdigest(), 'maximum_line_bytes': len(max_spill),
            'CONS_glyph_path_not_called': True, 'dynamic_output_buffer_not_needed': True}}
    (a.out / 'readiness.json').write_text(json.dumps(plan, indent=2) + '\n')
    print(json.dumps({'readiness': str(a.out / 'readiness.json'), 'case_ids': [c['id'] for c in cases]}))


def verify_files(plan):
    for path, digest in plan['immutable_input_sha256'].items():
        if sha(path) != digest:
            raise RuntimeError('Frozen pressure input changed: ' + path)


def inspect(case, plan):
    run = Path(case['run'])
    boot = (run / 'boot.txt').read_text(errors='replace')
    summary = json.loads((run / 'summary.json').read_text())
    if not summary['done'] or summary['errors'] or summary['rc']:
        raise RuntimeError('Incomplete/error native pressure run: ' + case['id'])
    if len(summary['zstat']) != 1 or any(int(summary['zstat'][0].get(k, -1)) != v for k, v in
        [('ram', 33554432), ('class', 32), ('frames', plan['frames']), ('levelframes', plan['frames'])]):
        raise RuntimeError('Complete matched retail32MB frame/capacity result absent')
    if (run / 'ps2args').read_text().splitlines() != case['expected_ps2args']:
        raise RuntimeError('Actual engine arguments differ')
    if 'con_hudlines "0"\n' not in (run / '.srb2/reference.cfg').read_text():
        raise RuntimeError('Matched console-HUD observer control absent')
    if 'HWE startup renderer=Hardware chosen=2 hardware=1' not in boot or re.search(r'HARDWARE -> SOFTWARE|Switching to the Software renderer', boot):
        raise RuntimeError('Actual native Hardware renderer absent/abandoned')
    if sha(run / 'SRB2.ELF') != plan['admissions'][case['role']]['diagnostic_ELF_sha256']:
        raise RuntimeError('Actual staged diagnostic ELF differs')
    for path, digest in plan['immutable_input_sha256'].items():
        original = Path(path)
        if original.suffix == '.PAK' or original.name == 'FINEACON.DAT':
            if sha(run / original.name) != digest:
                raise RuntimeError('Staged native asset differs: ' + original.name)
    spills = [{'line': i + 1, 'raw': line, 'bytes': int(m[1]), 'now': int(m[2]), 'ra': m[3]}
        for i, line in enumerate(boot.splitlines()) if (m := re.fullmatch(r'\[spill\] (\d+) bytes \(now (\d+)\) ra=([0-9a-fA-F]{8})', line))]
    flat_stride = 76 if case['role'] == 'candidate' and case['mode'] == 'default' else 84
    pairs, at = [], 0
    while at + 1 < len(spills):
        wall, flat = spills[at:at + 2]
        if wall['bytes'] == 4740 * 84 and flat['bytes'] == 4740 * flat_stride and flat['line'] == wall['line'] + 1:
            if flat['now'] - wall['now'] != flat['bytes']:
                raise RuntimeError('Actual cumulative spill payload does not equal the flat request')
            pairs.append({'wall': wall, 'flat': flat, 'constructor_N_inferred_from_actual_requests': wall['bytes'] // 84,
                          'actual_flat_request_bytes': flat['bytes']})
            at += 2
        else:
            at += 1
    if not pairs:
        raise RuntimeError('No exact adjacent constructor wall/flat ZA payload pair: ' + case['id'])
    return {'case': case['id'], 'all_raw_spill_rows': spills, 'actual_constructor_pairs': pairs,
        'ZSTAT': summary['zstat'], 'all_raw_tag_rows': [s for s in boot.splitlines() if s.startswith('ps2_mem: tag')],
        'all_raw_zone_checkpoints': [s for s in boot.splitlines() if s.startswith(('[zck]', '[zmem]'))],
        'all_raw_geometry_material_and_resource_rows': [s for s in boot.splitlines() if s.startswith(('LHASH ', 'HWPH ', 'HWLT ', 'HWWS ', 'HW mem ', '[zreg]'))],
        'all_raw_Hardware_rows': [s for s in boot.splitlines() if s.startswith('HW')],
        'shipping_ZA_payload_observed_not_plain_libc_requested_bytes': True,
        'request_pair_attribution': 'Exact bound constructor has two consecutive full-N calloc requests; their unique actual ZA spill rows are adjacent and cumulative payload increments by the second request.',
        'boot_sha256': sha(run / 'boot.txt'), 'summary_sha256': sha(run / 'summary.json')}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--baseline-build', type=Path, default=ROOT / 'build/nov15-next12/final/hardware')
    p.add_argument('--candidate-build', type=Path, default=ROOT / 'build/nov15-next13/flats-context-v1')
    p.add_argument('--baseline-elf', type=Path)
    p.add_argument('--candidate-elf', type=Path)
    p.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    p.add_argument('--seed', default='01b2d3f5')
    p.add_argument('--map', default='MAP02')
    p.add_argument('--frames', type=int, default=350)
    p.add_argument('--timeout', type=int, default=300)
    p.add_argument('--run-case', help='Consumes the coordinator-granted sole emulator; execute exactly one fresh case')
    p.add_argument('--analyze', action='store_true')
    a = p.parse_args()
    a.out = a.out.resolve()
    if not a.out.is_relative_to(ROOT / 'build/nov15-next13'):
        raise RuntimeError('Isolated pass13 evidence destination required')
    if not (a.run_case or a.analyze):
        if not (a.baseline_elf and a.candidate_elf):
            p.error('Preparation requires both already audited fixed-local-RNG ELF copies')
        prepare(a)
        return
    plan = json.loads((a.out / 'readiness.json').read_text())
    verify_files(plan)
    if a.run_case:
        case = next(c for c in plan['cases'] if c['id'] == a.run_case)
        if Path(case['run']).exists() or Path(case['runtime']).exists():
            raise RuntimeError('Fresh matched pressure runs required; completed evidence is immutable')
        process = run_verified(case['command'], Path(case['runtime']), [Path(p) for p in plan['immutable_input_sha256']] + [a.out / 'readiness.json'])
        if process.returncode:
            raise RuntimeError('Official native pressure runner failed')
        item = inspect(case, plan)
        (a.out / (case['id'] + '-inspection.json')).write_text(json.dumps(item, indent=2) + '\n')
        print(json.dumps({'case': case['id'], 'actual_constructor_pairs': item['actual_constructor_pairs']}))
    if a.analyze:
        items = {c['id']: inspect(c, plan) for c in plan['cases'] if (Path(c['run']) / 'summary.json').exists()}
        comparisons = {}
        for mode in ('default', 'legacy', 'legacy-scalar'):
            keys = ['baseline-' + mode, 'candidate-' + mode]
            if all(k in items for k in keys):
                old, new = [items[k] for k in keys]
                if len(old['actual_constructor_pairs']) != len(new['actual_constructor_pairs']):
                    raise RuntimeError('Matched actual constructor event counts differ')
                savings = [x['actual_flat_request_bytes'] - y['actual_flat_request_bytes'] for x, y in zip(old['actual_constructor_pairs'], new['actual_constructor_pairs'])]
                expected = 37920 if mode == 'default' else 0
                if any(n != expected for n in savings):
                    raise RuntimeError('Actual ZA flat payload delta does not equal 8*N or legacy zero')
                comparisons[mode] = {'actual_each_constructor_flat_payload_saved_bytes': savings,
                    'all_other_resource_and_cache_LOD_rows_preserved_for_root_integration_gate': True}
        (a.out / 'analysis.json').write_text(json.dumps({'cases': items, 'actual_ZA_payload_comparisons': comparisons,
            'no_cache_LOD_pixel_or_whole_RAM_acceptance_claim_from_payload_alone': True}, indent=2) + '\n')
        print(json.dumps({'analyzed_cases': len(items), 'payload_comparisons': comparisons}))


if __name__ == '__main__':
    main()
