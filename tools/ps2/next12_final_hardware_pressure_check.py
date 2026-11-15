#!/usr/bin/env python3
"""Independently inspect the final two-seed native Hardware pressure matrix.

Read-only full logs, exact runtime/configuration archives, actual final graphs
and complete native resource streams. No compiler/emulator or FPS claim.
"""
import argparse
import copy
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import output_bytes
from next10_gs_run import critical_settings
from next11_integration_check import inspect
from next11_smoke_check import runtime_evidence
from next12_final_hardware_pressure import ROOT, WORK, DEFAULT, PB_SHA, FINAL_ELF, FINAL_DIAGNOSTICS, sha, verify_files
from next12_pb_scratch_finalbind import REVISION
from next11_local_rng import stub_words, verify_patch


def stream(boot, prefix):
    return [line for line in boot.splitlines() if line.startswith(prefix)]


def tags(boot):
    return {int(n): {'name': name, 'blocks': int(blocks), 'allocated': int(size), 'payload': int(payload)}
        for n, name, blocks, size, payload in re.findall(
            r'^ps2_mem: tag\s+(\d+)\s+(\w+)\s+(\d+) blocks\s+(\d+) B \(payload\s+(\d+) B\)', boot, re.M)}


def inspect_case(case):
    run, runtime = Path(case['run']), Path(case['runtime'])
    item = inspect(run, case['name'])
    errors = item['errors']
    summary = json.loads((run / 'summary.json').read_text())
    boot = (run / 'boot.txt').read_text(errors='replace')
    if not summary['done'] or summary['errors'] or len(summary['zstat']) != 1:
        errors.append('complete clean native350 summary absent')
    z = {key: int(value) for key, value in summary['zstat'][0].items()}
    if z['frames'] != 350 or z['levelframes'] != 350 or z['ram'] != 33554432 or z['class'] != 32:
        errors.append('exact350 frame or physical EE capacity differs')
    if '-renderer' not in case['expected_args'] or 'HWE startup renderer=Hardware chosen=2 hardware=1' not in boot or re.search(r'HARDWARE -> SOFTWARE|Switching to the Software renderer', boot):
        errors.append('actual native Hardware rendering absent or abandoned')
    if sha(run / 'SRB2.ELF') != case['diagnostic']['sha256'] or sha(Path(case['diagnostic']['path'])) != case['diagnostic']['sha256']:
        errors.append('actual staged/executed exact seed/ELF differs')
    if (run / 'ps2args').read_text().splitlines() != case['expected_args']:
        errors.append('complete exact staged engine argument order differs')
    for name, expected in case['assets'].items():
        if sha(run / name) != expected:
            errors.append('complete staged asset differs: ' + name)
    for name, record in case['home_files'].items():
        if sha(run / '.srb2' / name) != record['sha256']:
            errors.append('complete staged scene/addon differs: ' + name)
    evidence = runtime_evidence(runtime, case['command'])
    if evidence['wrapper_rc'] or evidence['critical_settings'] != case['baseline_non_UI_settings'] or evidence['emulator_binary_sha256'] != case['baseline_emulator_binary']:
        errors.append('complete baseline non-UI emulator/runtime identity differs')
    pre = json.loads((runtime / 'run-pre.json').read_text())
    profile = Path(pre['environment'].get('SRB2_PCSX2_ROOT', '/workspace/.onboarding/pcsx2')).resolve()
    for label in ('before', 'after'):
        record = json.loads((runtime / ('run-pre.json' if label == 'before' else 'run-post.json')).read_text())
        for original, expected in record['critical_settings'].items():
            archived = runtime / ('config-' + label) / Path(original).relative_to(profile)
            if critical_settings(archived) != expected:
                errors.append('complete archived INI non-UI fields differ: ' + str(archived))
    rows = defaultdict(list)
    for line in boot.splitlines():
        if line.startswith('HW'):
            rows[line.split()[0]].append(line)
    outputs = {str(p): sha(p) for p in run.rglob('*') if p.is_file() and
               p.name not in case['assets'] and p.suffix not in ('.PAK', '.DAT')}
    outputs.update({str(p): sha(p) for p in runtime.rglob('*') if p.is_file()})
    return {'case': case, 'errors': errors, 'ZSTAT': z, 'runtime': evidence,
        'ordered_streams': {prefix: stream(boot, prefix) for prefix in ('LHASH ', 'HWPH ', 'HWLT ', 'HW mem ', 'HWWS ', '[zreg]')},
        'all_Hardware_rows': dict(rows), 'all_raw_checkpoints': stream(boot, '[zck]'),
        'all_raw_zone_rows': stream(boot, '[zmem]'), 'all_tags': tags(boot),
        'actual_result_hashes': outputs,
        'all_raw_final_resource_rows': stream(boot, 'ZSTAT '),
        'all_raw_warning_rows': [line for line in boot.splitlines() if 'WARNING:' in line]}


def compare_pair(a, b):
    errors = list(a['errors']) + list(b['errors'])
    old, new = a['ZSTAT'], b['ZSTAT']
    if set(old) != set(new):
        errors.append('complete ZSTAT field set differs')
    delta = {key: new[key] - value for key, value in old.items() if key in new}
    exact = ('ram', 'class', 'map', 'levelframes', 'frames', 'arena', 'libcfree', 'libcpeak',
             'stackused', 'spillpeak', 'spillnow', 'evicted', 'evictedbytes', 'flushes')
    for key in exact:
        if delta.get(key) != 0:
            errors.append('required exact physical/cache/allocator field differs: ' + key)
    if delta.get('failed', 1) > 0:
        errors.append('failed allocation attempts increased')
    for key in ('used', 'peak', 'gpeak'):
        if delta.get(key, 0) >= 0:
            errors.append('actual final zone saving absent: ' + key)
    for key in ('free', 'largest'):
        if delta.get(key, -1) < 0:
            errors.append('actual available zone capacity decreased: ' + key)
    equality = {}
    for prefix, left in a['ordered_streams'].items():
        right = b['ordered_streams'][prefix]
        equality[prefix] = {'rows_each': [len(left), len(right)], 'complete_ordered_lines_equal': bool(left) and left == right,
            'baseline_sha256': hashlib.sha256(('\n'.join(left) + '\n').encode()).hexdigest(),
            'final_sha256': hashlib.sha256(('\n'.join(right) + '\n').encode()).hexdigest()}
        if not left or left != right:
            errors.append('complete ordered geometry/loader/format/residency stream differs: ' + prefix)
    if len(a['ordered_streams']['HWPH ']) != 407 or len(b['ordered_streams']['HWPH ']) != 407:
        errors.append('complete407 canonical Hardware polygon rows absent')
    main = [[line.partition(' | polys=')[2] for line in item['all_Hardware_rows'].get('HWPROF', []) if line.startswith('HWPROF win=')]
            for item in (a, b)]
    if not main[0] or main[0] != main[1]:
        errors.append('full driver geometry/upload/VRAM/LOD/budget/capacity tails differ')
    pattern = r'polygons=(\d+) in (\d+) batches \((\d+) buckets\).*batches drawn=(\d+).*planned polygons=(\d+).*pool (\d+) quadwords, cut for room=(\d+)'
    pb = [[list(map(int, re.search(pattern, line).groups())) for line in item['all_Hardware_rows'].get('HWPROF50', []) if line.startswith('HWPROF50 collection:')]
          for item in (a, b)]
    if not pb[0] or pb[0] != pb[1]:
        errors.append('complete PB capacity/flush/output counters differ')
    ta, tb = a['all_tags'], b['all_tags']
    if 24 not in ta or 24 in tb:
        errors.append('actual removed redundant light-table tag24 not observed')
    if {n: (v['blocks'], v['payload']) for n, v in ta.items() if n not in (24, 25)} != {n: (v['blocks'], v['payload']) for n, v in tb.items() if n not in (24, 25)}:
        errors.append('other actual cache/map/object payload residency or block count changed')
    requested_PB_saving = 4 * ((pb[0][-1][5] + 7) // 8) if pb[0] else None
    if 25 not in ta or 25 not in tb or ta[25]['blocks'] - tb[25]['blocks'] != 1 or ta[25]['payload'] - tb[25]['payload'] != requested_PB_saving:
        errors.append('actual PB tag25 payload/root saving disagrees with unchanged capacity')
    checkpoint_deltas = []
    if len(a['all_raw_checkpoints']) != len(b['all_raw_checkpoints']):
        errors.append('complete checkpoint row count differs')
    for left, right in zip(a['all_raw_checkpoints'], b['all_raw_checkpoints']):
        names = [line.split()[1] for line in (left, right)]
        x, y = [{k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)} for line in (left, right)]
        if names[0] != names[1] or set(x) != set(y):
            errors.append('complete ordered checkpoint name/field set differs')
        d = {k: y[k] - v for k, v in x.items() if k in y}
        if any(d.get(k, 0) for k in ('level', 'cache', 'patch', 'sprite', 'evicted')):
            errors.append('actual checkpoint cache/map/sprite payload or eviction differs: ' + names[0])
        if any(d.get(k, 0) > 0 for k in ('used', 'peak', 'static')) or any(d.get(k, 0) < 0 for k in ('free', 'largest')):
            errors.append('actual checkpoint allocation/free/fragmentation regressed: ' + names[0])
        checkpoint_deltas.append({'checkpoint': names[0], 'baseline': left, 'final': right, 'all_literal_numeric_deltas': d})
    differences = {name: {'baseline': a['all_Hardware_rows'].get(name, []), 'final': b['all_Hardware_rows'].get(name, [])}
        for name in sorted(set(a['all_Hardware_rows']) | set(b['all_Hardware_rows']))
        if a['all_Hardware_rows'].get(name, []) != b['all_Hardware_rows'].get(name, [])}
    tag_deltas = {str(n): {'name': ta.get(n, tb.get(n))['name'], **{
        field: tb.get(n, {}).get(field, 0) - ta.get(n, {}).get(field, 0) for field in ('blocks', 'allocated', 'payload')}} for n in sorted(set(ta) | set(tb))}
    residual = -delta.get('used', 0) - ta.get(24, {}).get('allocated', 0) - (requested_PB_saving or 0)
    return {'errors': errors, 'complete_stream_equality': equality, 'all_final_numeric_resource_deltas': delta,
        'baseline_final_ZSTAT': {'baseline': old, 'final': new}, 'all_raw_checkpoint_deltas': checkpoint_deltas,
        'all_tag_deltas': tag_deltas, 'actual_requested_PB_payload_removed': requested_PB_saving,
        'actual_tag24_allocated_removed': ta.get(24, {}).get('allocated'), 'other_allocated_byte_residual': residual,
        'residual_scope': 'Literal whole-zone residual; no particular allocator alignment/header/address event is inferred',
        'full_driver_geometry_VRAM_LOD_budget_capacity_tails': {'baseline': main[0], 'final': main[1]},
        'full_PB_capacity_output_counter_rows': {'baseline': pb[0], 'final': pb[1]},
        'all_literal_different_Hardware_row_groups': differences,
        'failed_attempt_policy': 'Only this allocation counter may decrease; an increase rejects',
        'timing_lookup_policy': 'All raw profile/cadence/max/spike/lookup rows and cycles/elapsed retained; no normalization or FPS/stability claim'}


def graph_admission(plan, lt_path, allocator_path):
    pb_path = Path(plan['PB_linked_proof']['path'])
    if sha(pb_path) != PB_SHA:
        raise SystemExit('Exact final PB linked proof admission differs')
    pb = json.loads(pb_path.read_text())
    lt, allocator = [json.loads(p.read_text()) for p in (lt_path, allocator_path)]
    expected = plan['build_admissions']['final']['artifacts']
    for report in (lt, allocator):
        elf_key = 'shipping_ELF_sha256' if 'shipping_ELF_sha256' in report else 'ELF_sha256'
        if report[elf_key] != FINAL_ELF or report['engine_sha256'] != expected['obj/engine_lto.o']['sha256'] or report['map_sha256'] != expected['SRB2.map']['sha256']:
            raise SystemExit('Actual final LT/allocator build/ELF graph admission differs')
    if lt['source_and_actual_full_build_admission']['source_revision'] != REVISION or lt['source_and_actual_full_build_admission']['all602_source_sha256'] != {n: v['sha256'] for n, v in plan['source602'].items()}:
        raise SystemExit('Actual LT graph source602 differs from executed final source')
    if not lt['all_complete_final_and_executed_component_code_RELA_targets_addends_frames_and_referenced_data_equal'] or len(lt['executed_final_binary_mutation_controls']) < 22 or not all(x['rejected'] for x in lt['executed_final_binary_mutation_controls']):
        raise SystemExit('Actual final LT complete binding/mutation gate incomplete')
    if len(pb['binary_negative_controls']) != 19 or not all(x['inner_resolved_binding_rejected'] for x in pb['binary_negative_controls']):
        raise SystemExit('Actual final PB complete internal mutation gate incomplete')
    build = Path(plan['build_admissions']['final']['build'])
    normal = Elf32(build / 'SRB2.ELF')
    symbols = linked_symbols(normal)
    patch = json.loads((WORK / 'local-rng/final-hardware/patch-report.json').read_text())
    selected = {**pb['variants']['final']['complete_bound_text'], **lt['actual_final_function_bindings']}
    witnesses = {}
    for seed, row in patch['variants'].items():
        path = Path(row['ELF'])
        if sha(path) != FINAL_DIAGNOSTICS[seed] or lt['both_actual_final_diagnostic_ELF_bindings'][seed]['ELF_sha256'] != sha(path):
            raise SystemExit('Actually staged final diagnostic does not match final PB/LT graphs')
        verify_patch(normal.data, path.read_bytes(), row['patched_file_offset'], row['owned_symbol_bytes'],
            struct.pack('<10I', *stub_words(row['seed_uint32'], row['actual_M_RandomSeed_address'])))
        diagnostic = Elf32(path)
        for name, body in selected.items():
            if not body['all_fully_resolved_text_and_padding_bytes_equal'] or hashlib.sha256(output_bytes(diagnostic, symbols[name]['value'], body['complete_text_bytes'])).hexdigest() != body['resolved_text_sha256']:
                raise SystemExit('Actual bound final body changed in executed diagnostic: ' + name)
        witnesses[seed] = {'diagnostic_sha256': sha(path), 'every_file_byte_outside_initializer_identical': True,
            'all_selected_PB_LT_complete_body_spans_verified': sorted(selected),
            'LT_full_diagnostic_code_data_literal_frames_and_SDK_report': lt['both_actual_final_diagnostic_ELF_bindings'][seed]['scope']}
    original = linked_symbols(Elf32(WORK / 'baseline/hardware/SRB2.ELF'))
    static = {name: {'baseline': original[name]['value'], 'final': symbols[name]['value'],
                    'delta': symbols[name]['value'] - original[name]['value']} for name in ('_end', '_gp')}
    if any(row['delta'] for row in static.values()):
        raise SystemExit('Actual final static end/GP capacity differs')
    return {'PB_report': {'path': str(pb_path), 'sha256': sha(pb_path)},
        'LT_report': {'path': str(lt_path), 'sha256': sha(lt_path)},
        'allocator_report': {'path': str(allocator_path), 'sha256': sha(allocator_path)},
        'selected_complete_function_union': len(selected), 'both_executed_diagnostic_admissions': witnesses,
        'actual_static_capacity_boundaries': static,
        'binding_scope': 'Exact final shipping full-LTO PB/LT/wrapper/data/literal/frame/SDK proof, inherited by initializer-only diagnostic copies and tied to staged ELF hashes; no component-fixture identity assumption'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=DEFAULT)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--lt-binding', type=Path, default=WORK / 'lighttable-final/finalbind-v1/final-binding-report.json')
    parser.add_argument('--allocator', type=Path, default=WORK / 'lighttable-final/allocator-final-v1/allocator-report.json')
    args = parser.parse_args()
    work, out = args.work.resolve(), args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build':
        raise SystemExit('A fresh immutable report directory under build/ is required')
    readiness = work / 'readiness.json'
    plan = json.loads(readiness.read_text())
    verify_files(plan['complete_immutable_input_sha256'])
    graphs = graph_admission(plan, args.lt_binding.resolve(), args.allocator.resolve())
    out.mkdir(parents=True)
    pinned = out / 'pinned-tools'
    pinned.mkdir()
    for name in ('next12_final_hardware_pressure.py', 'next12_final_hardware_pressure_check.py', 'next11_smoke_check.py',
                 'next11_integration_check.py', 'next10_gs_run.py', 'next12_runtime.py'):
        shutil.copyfile(ROOT / 'tools/ps2' / name, pinned / name)
    cases = {case['id']: inspect_case(case) for case in plan['cases']}
    pairs, errors = {}, []
    for seed in FINAL_DIAGNOSTICS:
        for scene in ('map02', 'fog-stereo'):
            pair = compare_pair(cases['baseline-' + seed + '-' + scene], cases['final-' + seed + '-' + scene])
            pairs[seed + '-' + scene] = pair
            errors += [seed + '/' + scene + ': ' + err for err in pair['errors']]
    report = {'status': 'PASS complete actual final two-seed native Hardware pressure gate' if not errors else 'FAIL final Hardware pressure gate',
        'errors': errors, 'readiness_sha256': sha(readiness), 'final_source_revision': REVISION,
        'shipping_ELF_sha256': FINAL_ELF, 'actual_final_executed_graph_admission': graphs,
        'complete_runs': cases, 'within_seed_scene_comparisons': pairs,
        'new_runs': 6, 'reused_primary_baselines': 2, 'each_run350_native_Hardware': True,
        'CPU_GPU_FPS_or_stability_gain_claimed': False, 'elapsed_and_profile_values_are_observations_only': True,
        'pinned_tool_sha256': {p.name: sha(p) for p in pinned.iterdir()}}
    verify_files(plan['complete_immutable_input_sha256'])
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'report': str(out / 'report.json'), 'errors': errors,
        'zone_used_deltas': {key: pair['all_final_numeric_resource_deltas']['used'] for key, pair in pairs.items()}}, indent=2))
    if errors:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
