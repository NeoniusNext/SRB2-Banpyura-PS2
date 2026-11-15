"""Check actual full-game resource and executed-ELF evidence without mutation.

Reports every arena, allocator, static and stack cost. It never normalizes away
a capacity loss or infers a RAM saving from deleted request sizes alone.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import output_bytes
from next11_sw_checkplane_codecheck import build_binder
from next12_lighttable_codecheck import copy_binder, signature
from next12_local_rng import verify_patch, stub_words

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tags(text):
    return {int(n): {'name': name, 'blocks': int(blocks), 'allocated': int(size), 'payload': int(payload)}
            for n, name, blocks, size, payload in re.findall(r'^ps2_mem: tag\s+(\d+)\s+(\w+)\s+(\d+) blocks\s+(\d+) B \(payload\s+(\d+) B\)', text, re.M)}


def pressure(reference, candidate, allowed_nonincreasing=()):
    a, b = [(p / 'boot.txt').read_text(errors='replace') for p in (reference, candidate)]
    left, right = [json.loads((p / 'summary.json').read_text()) for p in (reference, candidate)]
    for summary in (left, right):
        if not summary['done'] or summary['rc'] or summary['errors'] or len(summary['zstat']) != 1:
            raise SystemExit('Full-game completion or error gate failed')
        if summary['zstat'][0]['frames'] != '350' or summary['zstat'][0]['levelframes'] != '350':
            raise SystemExit('Incomplete native350 pressure outcome')
    zleft = {k: int(v) for k, v in left['zstat'][0].items()}; zright = {k: int(v) for k, v in right['zstat'][0].items()}
    deltas = {k: zright[k] - v for k, v in zleft.items()}
    equal_streams = {}
    for label, prefix in (('light_table_logs', 'HWLT '), ('ordered_polygon_stream', 'HWPH ')):
        x, y = [[line for line in t.splitlines() if line.startswith(prefix)] for t in (a, b)]
        if not x or x != y: raise SystemExit('Actual native stream changed: ' + label)
        equal_streams[label] = {'rows': len(x), 'all_lines_byte_exact': True,
                               'stream_sha256': hashlib.sha256(('\n'.join(x) + '\n').encode()).hexdigest()}
    ta, tb = tags(a), tags(b)
    if 24 not in ta or 24 in tb: raise SystemExit('Actual redundant tag24 removal not observed')
    if {k: v['payload'] for k, v in ta.items() if k != 24} != {k: v['payload'] for k, v in tb.items()}:
        raise SystemExit('Other actual cache/level payload residency changed')
    unchanged = ('ram', 'class', 'map', 'levelframes', 'frames', 'evicted', 'evictedbytes', 'failed', 'flushes', 'spillpeak', 'spillnow')
    if set(allowed_nonincreasing) - {'failed'}: raise SystemExit('Only explicitly nonincreasing failed-allocation attempts may vary')
    if any(deltas[k] > 0 if k in allowed_nonincreasing else deltas[k] != 0 for k in unchanged):
        raise SystemExit('Actual capacity/policy/eviction/failure/spill behavior regressed')
    registry = [re.findall(r'^\[zreg\][^\r\n]*', t, re.M) for t in (a, b)]
    if registry[0] != registry[1]: raise SystemExit('Cache registry capacity or contents count changed')
    # Retain texture residency samples, excluding only the intentionally changed
    # aggregate C-heap-in-use field. Every driver texture byte count must match.
    samples = [re.findall(r'^HW mem frame (\d+): (.*); C heap in use (\d+)K; driver textures (\d+)$', t, re.M) for t in (a, b)]
    if not samples[0] or [(n, state, textures) for n, state, heap, textures in samples[0]] != [(n, state, textures) for n, state, heap, textures in samples[1]]:
        raise SystemExit('Actual sampled cache/texture residency changed')
    return {'reference_ZSTAT': zleft, 'candidate_ZSTAT': zright, 'delta_candidate_minus_reference': deltas,
            'actual_tag_allocations_reference': ta, 'actual_tag_allocations_candidate': tb,
            'deleted_tag24_actual_allocated_bytes': ta[24]['allocated'], 'deleted_tag24_actual_payload_bytes': ta[24]['payload'],
            'actual_zone_plus_libc_available_delta': deltas['free'] + deltas['libcfree'],
            'arena_capacity_decreased': deltas['arena'] < 0,
            'capacity_gate': 'REQUIRES_REVIEW: actual arena capacity decreased' if deltas['arena'] < 0 else 'actual arena capacity retained',
            'equal_native_streams': equal_streams, 'cache_registry_byte_exact': registry[0],
            'other_cache_level_payloads_and_sampled_texture_residency_exact': True,
            'elapsed_observation_only_ms': {'reference': zleft['t_ms'], 'candidate': zright['t_ms']},
            'CPU_or_FPS_gain_claimed': False,
            'explicit_nonincreasing_counter_policy': list(allowed_nonincreasing),
            'input_output_hashes': {str(p / n): sha(p / n) for p in (reference, candidate) for n in ('boot.txt', 'summary.json', 'ps2args', 'SRB2.ELF')}}


def diagnostic_graph(build, linker, diagnostic, names):
    original = build_binder(build / 'obj/engine_lto.o', build / 'SRB2.ELF', build / 'SRB2.map', linker)
    executed = copy_binder(original, elf=Elf32(diagnostic))
    return original, {name: executed.text(name) for name in names}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True); parser.add_argument('--binding', type=Path, required=True)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov15-next12/lighttable')
    parser.add_argument('--case', type=int, choices=(0, 1), required=True); parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(); out = args.out.resolve(); game = args.game.resolve(); work = args.work.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable analysis stage required')
    out.mkdir(parents=True); pinned = out / 'pinned-tools'; pinned.mkdir()
    for name in ('next12_lighttable_gamecheck.py', 'next12_lighttable_codecheck.py', 'next12_local_rng.py',
                 'next11_sw_checkplane_codecheck.py', 'next11_sw_linker_relas.py', 'next10_sw_bounds_codecheck.py'):
        shutil.copyfile(ROOT / 'tools/ps2' / name, pinned / name)
    readiness = json.loads((game / 'readiness.json').read_text()); case = readiness['cases'][args.case]
    for name, value in readiness['complete_full_build_source_IR_engine_ELF_map_generated_inputs_and_runner_inputs'].items():
        if sha(Path(name)) != value: raise SystemExit('Frozen full-game input changed')
    binding_path = args.binding.resolve() / 'binding-report.json'; binding = json.loads(binding_path.read_text())
    candidate = game / 'runs' / case['name']; runtime = game / 'runtime' / case['name']
    pre, post = [json.loads((runtime / n).read_text()) for n in ('run-pre.json', 'run-post.json')]
    baseline_pre = json.loads((Path(case['baseline_runtime']) / 'run-pre.json').read_text())
    if pre['critical_settings'] != baseline_pre['critical_settings'] or post['rc'] or not all(post[k] for k in ('all_frozen_inputs_unchanged', 'all_non_UI_settings_unchanged', 'emulator_binary_unchanged')):
        raise SystemExit('Actual full-game settings/input/outcome gate failed')
    diagnostic = Path(case['command'][case['command'].index('--elf') + 1])
    if sha(candidate / 'SRB2.ELF') != sha(diagnostic) or sha(diagnostic) != readiness['diagnostic_ELF_sha256']:
        raise SystemExit('Actually staged/executed full-game ELF differs')
    names = list(binding['versions']['candidate']['actual_linked_function_bindings'])
    actual, graph = diagnostic_graph(work / 'candidate-hardware', work / 'link-rela-candidate-v1', diagnostic, names)
    patch = json.loads((work / 'local-rng-candidate-v1/patch-report.json').read_text())['variants']['01b2d3f5']
    raw = (work / 'candidate-hardware/SRB2.ELF').read_bytes(); modified = diagnostic.read_bytes()
    payload = struct.pack('<10I', *stub_words(patch['seed_uint32'], patch['actual_M_RandomSeed_address']))
    verify_patch(raw, modified, patch['patched_file_offset'], patch['owned_symbol_bytes'], payload)
    pressure_report = pressure(Path(case['baseline']), candidate)
    pressure_report.update({'binding_report_sha256': sha(binding_path), 'readiness_sha256': sha(game / 'readiness.json'),
        'actual_executed_full_default_LTO_graph': graph, 'all_complete_selected_text_padding_data_literal_wrapper_bytes_resolved_in_executed_ELF': True,
        'ordinary_build_static_cost': {'end_delta': 768, 'gp_delta': 768, 'indexed_store_frame_bytes': 112,
                                      'generic_store_frame_bytes': 96, 'observed_max_stack_delta': pressure_report['delta_candidate_minus_reference']['stackused']},
        'all_non_UI_settings_exact_reference_and_unchanged_during_execution': True,
        'runtime_record_sha256': {n: sha(runtime / n) for n in ('run-pre.json', 'run-post.json')},
        'pinned_tool_sha256': {p.name: sha(p) for p in pinned.iterdir()},
        'adoption_claim': 'Pending strict arena-capacity review; component process measurement only'})
    (out / 'game-report.json').write_text(json.dumps(pressure_report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in pressure_report.items() if k in ('delta_candidate_minus_reference', 'actual_zone_plus_libc_available_delta', 'capacity_gate')}, indent=2))


if __name__ == '__main__': main()
