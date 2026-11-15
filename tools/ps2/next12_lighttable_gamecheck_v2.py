"""Bind the executed final combined Hardware game and report real RAM deltas.

The original and PB-only native controls remain distinct. No allocated-capacity
normalization or CPU/FPS inference is made.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import frame
from next12_lighttable_gamecheck import pressure, diagnostic_graph, tags
from next12_lighttable_allocator import SDK
from next12_lighttable_codecheck import copy_binder
from next12_local_rng import verify_patch, stub_words

ROOT = Path(__file__).resolve().parents[2]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def deltas(a, b):
    return {k: int(b[k]) - int(v) for k, v in a.items()}


def tag_deltas(a, b):
    result = {}
    for n in sorted(a.keys() | b.keys()):
        x, y = a.get(n, {}), b.get(n, {})
        result[n] = {'name': x.get('name', y.get('name')),
                     **{field: y.get(field, 0) - x.get(field, 0) for field in ('blocks', 'allocated', 'payload')}}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov15-next12/lighttable-combined-v2')
    parser.add_argument('--case', type=int, choices=(0, 1), required=True); parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(); work = args.work.resolve(); game = work / 'game-v1'; out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable actual-game report required')
    out.mkdir(parents=True); pinned = out / 'pinned-tools'; pinned.mkdir()
    for name in ('next12_lighttable_gamecheck_v2.py', 'next12_lighttable_gamecheck.py', 'next12_lighttable_codecheck.py',
                 'next12_lighttable_allocator.py', 'next12_local_rng.py', 'next11_sw_checkplane_codecheck.py',
                 'next11_sw_linker_relas.py', 'next10_sw_bounds_codecheck.py'):
        shutil.copyfile(ROOT / 'tools/ps2' / name, pinned / name)
    readiness = json.loads((game / 'readiness.json').read_text()); case = readiness['cases'][args.case]
    for name, value in readiness['complete_full_build_source_IR_engine_ELF_map_generated_inputs_and_runner_inputs'].items():
        if sha(Path(name)) != value: raise SystemExit('Frozen full-game input changed')
    candidate = game / 'runs' / case['name']; runtime = game / 'runtime' / case['name']
    original, pb = Path(case['baseline']), Path(case['PB_only_control'])
    pre, post = [json.loads((runtime / n).read_text()) for n in ('run-pre.json', 'run-post.json')]
    for control in (Path(case['baseline_runtime']), Path(case['PB_only_runtime'])):
        baseline = json.loads((control / 'run-pre.json').read_text())
        if pre['critical_settings'] != baseline['critical_settings'] or pre['emulator_binary_sha256'] != baseline['emulator_binary_sha256']:
            raise SystemExit('Actual baseline critical profile or emulator differs')
    if post['rc'] or not all(post[k] for k in ('all_frozen_inputs_unchanged', 'all_non_UI_settings_unchanged', 'emulator_binary_unchanged')):
        raise SystemExit('Actual native settings/input/outcome gate failed')
    diagnostic = Path(case['command'][case['command'].index('--elf') + 1])
    if sha(candidate / 'SRB2.ELF') != sha(diagnostic) or sha(diagnostic) != readiness['diagnostic_ELF_sha256']:
        raise SystemExit('Actually staged/executed ELF differs')
    binding_path = work / 'binding-v1/binding-report.json'; binding = json.loads(binding_path.read_text())
    names = list(binding['versions']['candidate']['actual_linked_function_bindings'])
    actual, executed_graph = diagnostic_graph(work / 'candidate-hardware', work / 'link-rela-candidate-v1', diagnostic, names)
    executed = copy_binder(actual, elf=Elf32(diagnostic))
    allocator_path = work / 'allocator-v2/allocator-report.json'; allocator = json.loads(allocator_path.read_text())
    allocator_graph = {name: executed.text(name) for name in allocator['actual_complete_engine_function_bindings']}
    sdk_graph = {}
    for function, record in allocator['actual_complete_SDK_member_bindings'].items():
        member = work / 'allocator-v2' / record['member']
        if sha(member) != record['member_sha256']: raise SystemExit('Frozen SDK member changed')
        sdk_graph[function] = SDK(Elf32(member), executed, record['archive'], record['member']).section_text(function)
    patch = json.loads((work / 'local-rng-candidate-v1/patch-report.json').read_text())['variants']['01b2d3f5']
    payload = struct.pack('<10I', *stub_words(patch['seed_uint32'], patch['actual_M_RandomSeed_address']))
    verify_patch((work / 'candidate-hardware/SRB2.ELF').read_bytes(), diagnostic.read_bytes(), patch['patched_file_offset'], patch['owned_symbol_bytes'], payload)
    component = pressure(pb, candidate, allowed_nonincreasing=('failed',))
    z = {role: json.loads((p / 'summary.json').read_text())['zstat'][0] for role, p in (('original', original), ('PB_only', pb), ('combined', candidate))}
    whole = deltas(z['original'], z['combined']); incremental = deltas(z['PB_only'], z['combined'])
    if whole['arena'] or incremental['arena']:
        raise SystemExit('Actual original/PB-only arena capacity changed')
    if any(whole[k] or incremental[k] for k in ('libcfree', 'libcpeak', 'stackused', 'spillpeak', 'spillnow', 'evicted', 'evictedbytes', 'flushes')) or whole['failed'] > 0 or incremental['failed'] > 0:
        raise SystemExit('Actual capacity/allocator/stack/spill/policy behavior changed')
    if whole['free'] <= 0 or incremental['free'] <= 0:
        raise SystemExit('Actual process/arena saving not observed')
    texts = {role: (p / 'boot.txt').read_text(errors='replace') for role, p in (('original', original), ('PB_only', pb), ('combined', candidate))}
    for prefix in ('LHASH ', 'HWPH ', 'HWLT ', 'HW mem '):
        streams = {role: [line for line in text.splitlines() if line.startswith(prefix)] for role, text in texts.items()}
        if not streams['original'] or len({tuple(value) for value in streams.values()}) != 1:
            raise SystemExit('Actual complete ordered loader/polygon/light-table/residency stream changed: ' + prefix)
    all_tags = {role: tags(text) for role, text in texts.items()}
    detailed = {'original_to_combined': tag_deltas(all_tags['original'], all_tags['combined']),
                'PB_only_to_combined': tag_deltas(all_tags['PB_only'], all_tags['combined'])}
    observed_tag24 = all_tags['PB_only'][24]['allocated']
    residual = -incremental['used'] - observed_tag24
    original_elf = Elf32(ROOT / 'build/nov15-next12/baseline/hardware/SRB2.ELF')
    before, after = linked_symbols(original_elf), linked_symbols(actual.elf)
    static = {name: after[name]['value'] - before[name]['value'] for name in ('_end', '_gp')}
    if any(static.values()): raise SystemExit('Actual linked original end/GP capacity changed')
    report = {'status': 'passed actual combined native350 and strict unchanged-capacity/policy gate',
        'scope': 'Actual final full-default-Hardware native game; original baseline and separately tested PB-only control remain distinct',
        'readiness_sha256': sha(game / 'readiness.json'), 'binding_report_sha256': sha(binding_path), 'allocator_report_sha256': sha(allocator_path),
        'reference_paths': {'original': str(original), 'PB_only': str(pb), 'combined': str(candidate)},
        'full_ZSTAT': z, 'whole_combined_minus_original': whole, 'lighttable_incremental_combined_minus_PB_only': incremental,
        'observed_tag24_allocated_bytes_removed': observed_tag24, 'other_tag_allocation_residual_bytes': residual,
        'residual_interpretation': 'Measured allocated-byte residual only. No exact alignment/header cause is claimed without an allocation trace.',
        'complete_per_tag_block_allocated_payload_deltas': detailed, 'all_raw_tag_records': all_tags,
        'component_exactness_and_resource_gate': component,
        'actual_linked_static_capacity_deltas': static,
        'actual_store_frame_bytes': frame(actual.obj, 'lt_store.lto_priv.0')['stack_frame_bytes'],
        'actual_executed_full_LTO_graph': executed_graph, 'actual_executed_spill_wrapper_graph': allocator_graph,
        'actual_executed_SDK_malloc_graph': sdk_graph,
        'all_complete_selected_text_padding_data_literals_frames_and_wrappers_bound_to_executed_ELF': True,
        'all_source_and_native_inputs_unchanged_and_full_non_UI_profile_exact': True,
        'all_input_result_sha256': {str(p / n): sha(p / n) for p in (original, pb, candidate) for n in ('boot.txt', 'summary.json', 'ps2args', 'SRB2.ELF')},
        'runtime_sha256': {n: sha(runtime / n) for n in ('run-pre.json', 'run-post.json')},
        'CPU_GPU_or_FPS_gain_claimed': False, 'elapsed_ms_is_observation_only': True,
        'pinned_tool_sha256': {p.name: sha(p) for p in pinned.iterdir()}}
    (out / 'game-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'whole_combined_minus_original': whole, 'lighttable_incremental_combined_minus_PB_only': incremental,
                      'tag24_removed': observed_tag24, 'other_tag_allocated_residual': residual}, indent=2))


if __name__ == '__main__': main()
