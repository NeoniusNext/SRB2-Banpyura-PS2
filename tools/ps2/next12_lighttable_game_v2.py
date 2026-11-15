"""Freeze, then run the final shared-store + PB full Hardware candidate serially.

Preparation is read-only with respect to original builds. Execution requires
the separately granted global emulator slot and uses opt_run.py only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

from next12_runtime import run_verified, snapshot
from next7_render_fog_codecheck import Elf32
from next10_sw_bounds_codecheck import output_bytes
from next8_sw_codecheck import linked_symbols

ROOT = Path(__file__).resolve().parents[2]
CASES = ('base-hw-map02-spawn-350', 'base-hw-fog-stereo-spawn-350')
PB_CASES = (('gate-pressure-map02-01b2d3f5-retry1', 'pb-hw-map02-spawn-350'), ('gate-pressure-map01-01b2d3f5', 'pb-hw-fog-stereo-spawn-350'))


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare(out, work, baseline):
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        raise SystemExit('A fresh immutable game stage under build/ is required')
    out.mkdir(parents=True); pinned = out / 'pinned-tools'; pinned.mkdir()
    inputs = set()
    for path in (work / 'candidate-source', work / 'candidate-hardware', work / 'local-rng-candidate-v1'):
        inputs.update(p for p in path.rglob('*') if p.is_file())
    proof_reports = (work / 'binding-v1/binding-report.json', work / 'allocator-v2/allocator-report.json',
                     ROOT / 'build/nov15-next12/lighttable-v2c/host-v1/host-report.json',
                     ROOT / 'build/nov15-next12/lighttable-v2c/host-v1/source-hashes.json',
                     ROOT / 'build/nov15-next12/lighttable-v2c/policy-v1/policy-report.json',
                     ROOT / 'build/nov15-next12/lighttable-v2/ownership-v1/ownership-report.json')
    if not all(p.is_file() for p in proof_reports): raise SystemExit('Complete frozen host/policy/ownership/LTO/allocator proofs required')
    inputs.update(proof_reports)
    for name in ('next12_lighttable_game_v2.py', 'next12_runtime.py', 'next12_local_rng.py',
                 'next10_gs_run.py', 'opt_run.py', 'run_pcsx2.py'):
        path = ROOT / 'tools/ps2' / name; inputs.add(path); shutil.copyfile(path, pinned / name)
    patch_path = work / 'local-rng-candidate-v1/patch-report.json'
    patches = json.loads(patch_path.read_text()); shipping_path = work / 'candidate-hardware/SRB2.ELF'
    shipping = Elf32(shipping_path); symbols = linked_symbols(shipping)
    if patches['original_inputs']['elf']['sha256'] != sha(shipping_path):
        raise SystemExit('Diagnostic report pins another full game')
    selected = ('HWR_GetLightTableID', 'lt_store.lto_priv.0', 'PS2HWD_UpdateIndexedLightTable',
                'clut_get.lto_priv.0', 'clut_build_into', 'clut_alloc', '__wrap_memcpy', '__wrap_memset')
    diagnostic = Path(patches['variants']['01b2d3f5']['ELF']); executed = Elf32(diagnostic)
    unchanged = {}
    for name in selected:
        s = symbols[name]; before = output_bytes(shipping, s['value'], s['size'])
        if output_bytes(executed, s['value'], s['size']) != before:
            raise SystemExit('Actual production helper/consumer bytes changed in diagnostic copy')
        unchanged[name] = {'address': s['value'], 'bytes': s['size'], 'sha256': hashlib.sha256(before).hexdigest()}
    commands = []
    for index, name in enumerate(CASES):
        runtime = baseline / 'runtime' / name
        pre = json.loads((runtime / 'run-pre.json').read_text()); post = json.loads((runtime / 'run-post.json').read_text())
        if post['rc'] or not all(post[k] for k in ('all_frozen_inputs_unchanged', 'all_non_UI_settings_unchanged', 'emulator_binary_unchanged')):
            raise SystemExit('Reference baseline runtime gate failed')
        inputs.update(Path(p) for p in pre['input_sha256'])
        inputs.update(p for p in runtime.rglob('*') if p.is_file())
        result_dir = baseline / 'runs' / name
        inputs.update(p for p in result_dir.rglob('*') if p.is_file())
        command = list(pre['command']); new_name = name.replace('base-', 'lighttable-combined-', 1)
        command[command.index('--name') + 1] = new_name
        command[command.index('--elf') + 1] = str(diagnostic)
        command[command.index('--out') + 1] = str(out / 'runs')
        pb_stage, pb_name = PB_CASES[index]
        pb_root = ROOT / 'build/nov15-next12/pb-scratch' / pb_stage
        pb_runtime = pb_root / 'runtime' / pb_name
        pb_result = pb_root / 'runs' / pb_name
        if not pb_runtime.is_dir() or not pb_result.is_dir(): raise SystemExit('Frozen matching PB-only native control missing')
        inputs.update(p for p in pb_runtime.rglob('*') if p.is_file())
        inputs.update(p for p in pb_result.rglob('*') if p.is_file())
        commands.append({'name': new_name, 'PB_only_control': str(pb_result), 'PB_only_runtime': str(pb_runtime), 'baseline_name': name, 'baseline': str(result_dir),
                         'baseline_runtime': str(runtime), 'command': command})
    inputs.update(p for p in (ROOT / 'build/pak').rglob('*') if p.is_file())
    profile = snapshot(out, 'prepared')
    report = {'source_revision': '85bd9716b1ed114d46f95831c64a87623038eeaa',
              'source_context': '602 baseline files plus only five final v2c light-table API/cache files and exact tested PB08e3 overlay; cull/P4 excluded',
              'shipping_ELF_sha256': sha(shipping_path), 'diagnostic_ELF_sha256': sha(diagnostic),
              'diagnostic_patch_report_sha256': sha(patch_path), 'diagnostic_only_fixed_local_seed': '0x01b2d3f5',
              'frozen_proof_reports': {str(p): sha(p) for p in proof_reports},
              'actual_selected_graph_symbol_bytes_unchanged': unchanged,
              'complete_full_build_source_IR_engine_ELF_map_generated_inputs_and_runner_inputs': {str(p): sha(p) for p in sorted(inputs)},
              'preparation_profile': profile, 'cases': commands,
              'scope': 'Actual full-default-Hardware native process/caches/arena/GIF/polygon/screenshot gate; no standalone helper or allocator timing claim'}
    (out / 'readiness.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Frozen full-game readiness: ' + str(out / 'readiness.json'))


def run(out, index):
    if os.environ.get('SRB2_PCSX2_SLOTS') != '1':
        raise SystemExit('Explicit serialized one-emulator configuration required')
    path = out / 'readiness.json'; readiness = json.loads(path.read_text())
    pins = readiness['complete_full_build_source_IR_engine_ELF_map_generated_inputs_and_runner_inputs']
    if any(sha(Path(p)) != value for p, value in pins.items()):
        raise SystemExit('Frozen full-game input changed before execution')
    case = readiness['cases'][index]
    result = run_verified(case['command'], out / 'runtime' / case['name'], [path, *map(Path, pins)])
    if result.returncode:
        raise SystemExit('Actual full-game wrapper failed: ' + str(result.returncode))
    print('Completed full game: ' + case['name'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov15-next12/lighttable-combined-v2')
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next12/hardware-profile-seed1')
    parser.add_argument('--run', type=int, choices=(0, 1))
    args = parser.parse_args()
    if args.run is None: prepare(args.out.resolve(), args.work.resolve(), args.baseline.resolve())
    else: run(args.out.resolve(), args.run)


if __name__ == '__main__':
    main()
