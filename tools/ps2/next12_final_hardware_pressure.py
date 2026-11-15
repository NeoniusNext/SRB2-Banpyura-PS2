#!/usr/bin/env python3
"""Prepare or execute the final combined Hardware pressure controls.

Default preparation uses no compiler/emulator. --run-case or --run requires the
coordinator's sole emulator grant and invokes only next12_runtime -> opt_run ->
run_pcsx2, serially. All commands clone the exact completed primary baselines.
The separate next12_final_hardware_pressure_check.py is read-only reporting.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

from next11_smoke_check import runtime_evidence
from next12_runtime import run_verified, snapshot
from next12_pb_scratch_finalbind import ROOT, WORK, REVISION, FINAL_ELF, FINAL_DIAGNOSTICS, exact_source, exact_build
import next12_pb_scratch_codecheck as codecheck
import next12_pb_scratch_replay as replay

DEFAULT = WORK / 'final-hardware-pressure'
BASE_ELF = 'fc7a2919a519cc5b23be4ded8e295df41fda68c90350326cfa1113b63e46eff5'
BASE_DIAGNOSTICS = {'01b2d3f5': '7542c365e7ea0383d5055bf78e45316ad9bd8f58e6950e608a3b5e1c7a6ec5ad',
                    '9e3779b9': 'a5be0d1d5dd736cd0a0006cdeaff41303b289051f418a763cb70d952cc0d9995'}
SCENES = {'map02': 'base-hw-map02-spawn-350', 'fog-stereo': 'base-hw-fog-stereo-spawn-350'}
PB_PROOF = WORK / 'final/pb-binding/final-codecheck/report.json'
PB_SHA = '2c1f88927d647fdb6bcd0eb6c100d798ed9b0c0bf808f172877928855cba43bf'
TOOLS = ('next12_final_hardware_pressure.py', 'next12_final_hardware_pressure_check.py',
    'next12_runtime.py', 'next11_smoke_check.py', 'next11_integration_check.py', 'next10_gs_run.py',
    'opt_run.py', 'run_pcsx2.py', 'next12_pb_scratch_finalbind.py', 'next12_pb_scratch_codecheck.py',
    'next12_pb_scratch_replay.py', 'next11_local_rng.py', 'next7_render_fog_codecheck.py',
    'next8_sw_codecheck.py', 'next10_sw_bounds_codecheck.py')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_files(files):
    for name, digest in files.items():
        if sha(Path(name)) != digest:
            raise SystemExit('Frozen pressure input changed: ' + name)


def prepare(out):
    if out.exists():
        raise SystemExit('Preparation requires a fresh directory; completed readiness is immutable')
    if sha(PB_PROOF) != PB_SHA:
        raise SystemExit('Exact final PB linked proof differs')
    final_source = WORK / 'final/source'
    source = exact_source(final_source)
    builds = {'baseline': WORK / 'baseline/hardware', 'final': WORK / 'final/hardware'}
    sources = {'baseline': WORK / 'baseline-src', 'final': final_source}
    admissions = {role: exact_build(build, sources[role], BASE_ELF if role == 'baseline' else FINAL_ELF)
                  for role, build in builds.items()}
    baseline_files = replay.baseline_files()
    actual_baseline = replay.source_manifest(sources['baseline'])
    if actual_baseline != {name: hashlib.sha256(raw).hexdigest() for name, raw in baseline_files.items()}:
        raise SystemExit('Exact complete602 original Hardware baseline source differs')
    files = {str(PB_PROOF): PB_SHA}
    pb = json.loads(PB_PROOF.read_text())
    for entry in pb['proof_tools'].values():
        files[entry['original']] = files[entry['snapshot']] = entry['sha256']
    for entry in pb['retained_linker_replays'].values():
        path = Path(entry['report_path'])
        files[str(path)] = entry['report_sha256']
        files.update(entry['all_original_link_inputs_rechecked'])
        for name in ('LINKER-RELAS.ELF', 'LINKER-RELAS.map', 'link.log'):
            files[str(path.parent / name)] = sha(path.parent / name)
    component = WORK / 'pb-scratch/resolved-binding/final-codecheck-v2/report.json'
    files[str(component)] = pb['frozen_component_resolved_report_sha256']
    for role, admission in admissions.items():
        files.update({row['path']: row['sha256'] for row in admission['artifacts'].values()})
        files.update(admission['complete_actual_IR_objects'])
        files.update({row['checked_path']: row['sha256'] for row in admission['all_actual_build_inputs'].values()})
        files.update({str(sources[role] / name): sha(sources[role] / name) for name in
                      (actual_baseline if role == 'baseline' else source)})
    diagnostics = {}
    for role in ('baseline', 'final'):
        folder = WORK / ('local-rng/' + role + '-hardware')
        patch_path = folder / 'patch-report.json'
        patch = json.loads(patch_path.read_text())
        expected = BASE_DIAGNOSTICS if role == 'baseline' else FINAL_DIAGNOSTICS
        if patch['original_inputs']['elf']['sha256'] != admissions[role]['artifacts']['SRB2.ELF']['sha256']:
            raise SystemExit('Exact diagnostic original ELF differs')
        diagnostics[role] = {}
        for seed, row in patch['variants'].items():
            path = Path(row['ELF'])
            if seed not in expected or sha(path) != expected[seed] or row['ELF_sha256'] != expected[seed]:
                raise SystemExit('Exact pressure seed/diagnostic ELF differs')
            payload = __import__('struct').pack('<10I', *codecheck.stub_words(row['seed_uint32'], row['actual_M_RandomSeed_address']))
            codecheck.verify_patch((builds[role] / 'SRB2.ELF').read_bytes(), path.read_bytes(),
                row['patched_file_offset'], row['owned_symbol_bytes'], payload)
            diagnostics[role][seed] = {'path': str(path), 'sha256': sha(path)}
            files[str(path)] = sha(path)
        if set(diagnostics[role]) != set(expected):
            raise SystemExit('Both predefined fixed-local-RNG copies are required')
        files[str(patch_path)] = sha(patch_path)
    out.mkdir(parents=True)
    pinned = out / 'pinned-tools'
    pinned.mkdir()
    for name in TOOLS:
        original = ROOT / 'tools/ps2' / name
        copied = pinned / name
        shutil.copyfile(original, copied)
        files[str(original)] = files[str(copied)] = sha(original)
    files['/workspace/.onboarding/emulator-env.sh'] = sha(Path('/workspace/.onboarding/emulator-env.sh'))
    cases = []
    baseline_root = WORK / 'hardware-profile-seed1'
    for seed in BASE_DIAGNOSTICS:
        for scene, base_name in SCENES.items():
            reused = baseline_root / 'runs' / base_name
            runtime = baseline_root / 'runtime' / base_name
            before = json.loads((runtime / 'run-pre.json').read_text())
            command = before['command']
            evidence = runtime_evidence(runtime, command)
            if evidence['wrapper_rc'] or sha(reused / 'SRB2.ELF') != BASE_DIAGNOSTICS['01b2d3f5']:
                raise SystemExit('Exact completed primary baseline seed/command/outcome differs')
            expected_args = (reused / 'ps2args').read_text().splitlines()
            if expected_args[expected_args.index('-zquit') + 1] != '350':
                raise SystemExit('Actual primary350 argument order differs')
            files.update(before['input_sha256'])
            for name in ('boot.txt', 'summary.json', 'SRB2.ELF', 'ps2args', 'reference.cfg'):
                files[str(reused / name)] = sha(reused / name)
            for path in runtime.rglob('*'):
                if path.is_file():
                    files[str(path)] = sha(path)
            pak = Path(command[command.index('--pak') + 1])
            assets = {p.name: sha(p) for p in sorted(pak.glob('*.PAK'))}
            assets['FINEACON.DAT'] = sha(pak / 'FINEACON.DAT')
            home = {}
            for i, argument in enumerate(command):
                if argument == '--home-file':
                    source_path, target = command[i + 1].rsplit('=', 1)
                    home[target] = {'source': source_path, 'sha256': sha(Path(source_path))}
            for name, digest in assets.items():
                files[str(pak / name)] = digest
            for item in home.values():
                files[item['source']] = item['sha256']
            for role in ('baseline', 'final'):
                use_old = seed == '01b2d3f5' and role == 'baseline'
                name = base_name if use_old else role + '-hw-' + scene + '-' + seed + '-spawn-350'
                cloned = list(command)
                if not use_old:
                    for flag, value in (('--name', name), ('--elf', diagnostics[role][seed]['path']), ('--out', str(out / 'runs'))):
                        cloned[cloned.index(flag) + 1] = value
                allowed = {command.index(flag) + 1 for flag in ('--name', '--elf', '--out')}
                if len(cloned) != len(command) or any(a != b and i not in allowed for i, (a, b) in enumerate(zip(command, cloned))):
                    raise SystemExit('A cloned exact baseline command changed an unauthorized field')
                cases.append({'id': role + '-' + seed + '-' + scene, 'role': role, 'seed': seed, 'scene': scene,
                    'name': name, 'command': cloned, 'expected_args': expected_args,
                    'run': str(reused if use_old else out / 'runs' / name),
                    'runtime': str(runtime if use_old else out / 'runtime' / name), 'reused_completed_primary_baseline': use_old,
                    'diagnostic': diagnostics[role][seed], 'assets': assets, 'home_files': home,
                    'baseline_template_command': command, 'baseline_template_runtime': str(runtime),
                    'baseline_non_UI_settings': before['critical_settings'], 'baseline_emulator_binary': before['emulator_binary_sha256']})
    settings = snapshot(out / 'readiness-runtime', 'prepared')
    for case in cases:
        if settings['critical_settings'] != case['baseline_non_UI_settings'] or settings['emulator_binary_sha256'] != case['baseline_emulator_binary']:
            raise SystemExit('Prepared full non-UI profile/emulator differs from the completed primary baselines')
    files.update({str(p): sha(p) for p in (out / 'readiness-runtime').rglob('*') if p.is_file()})
    files.update(settings['emulator_binary_sha256'])
    verify_files(files)
    readiness = {'status': 'READY; no emulator/compiler executed by preparation', 'source_revision': REVISION,
        'shipping_ELF_sha256': FINAL_ELF, 'source602': source, 'build_admissions': admissions,
        'PB_linked_proof': {'path': str(PB_PROOF), 'sha256': PB_SHA}, 'diagnostics': diagnostics,
        'cases': cases, 'complete_immutable_input_sha256': files, 'prepared_emulator_settings': settings,
        'new_runs': 6, 'reused_completed_runs': 2, 'frames_each': 350,
        'late_report_admission': 'Final LT linked/allocator proofs are admitted independently when reporting; no mutable development helper is a runtime input',
        'resource_policy': 'Arena/end/GP/libc/stack/spill/cache/flush policies exact; only failed attempts may decrease. Full geometry/residency/capacity streams literal. RAM used/peak gains must be measured. All timing/profile differences retained; no FPS claim.'}
    (out / 'readiness.json').write_text(json.dumps(readiness, indent=2) + '\n')
    return readiness


def execute(out, plan, case):
    if case['reused_completed_primary_baseline']:
        raise SystemExit('The completed primary baseline must be reused, never overwritten')
    verify_files(plan['complete_immutable_input_sha256'])
    run, runtime = Path(case['run']), Path(case['runtime'])
    if run.exists() or runtime.exists():
        raise SystemExit('Run/runtime evidence already exists; preserve it and analyze independently')
    print('START ' + case['id'], flush=True)
    process = run_verified(case['command'], runtime, [Path(name) for name in plan['complete_immutable_input_sha256']] + [out / 'readiness.json'])
    if process.returncode:
        raise SystemExit('Official final pressure runner failed: ' + case['id'])
    from next12_final_hardware_pressure_check import inspect_case, compare_pair
    item = inspect_case(case)
    if item['errors']:
        raise SystemExit('Final pressure outcome failed: ' + str(item['errors']))
    if case['role'] == 'final':
        baseline = next(value for value in plan['cases'] if value['role'] == 'baseline' and value['seed'] == case['seed'] and value['scene'] == case['scene'])
        if (Path(baseline['run']) / 'summary.json').is_file():
            result = compare_pair(inspect_case(baseline), item)
            if result['errors']:
                raise SystemExit('Final pressure pair gate failed: ' + str(result['errors']))
    print('COMPLETE ' + case['id'], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=DEFAULT)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--prepare', action='store_true')
    mode.add_argument('--run-case', help='Execute exactly one pending case ID with an explicit emulator grant')
    mode.add_argument('--run', action='store_true', help='Execute the six pending cases serially with an explicit sole-emulator grant')
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build':
        raise SystemExit('An isolated child of build/ is required')
    if not (args.run or args.run_case):
        plan = prepare(out)
        print(json.dumps({'readiness': str(out / 'readiness.json'), 'new_case_ids': [c['id'] for c in plan['cases'] if not c['reused_completed_primary_baseline']]}, indent=2))
        return
    plan = json.loads((out / 'readiness.json').read_text())
    cases = [c for c in plan['cases'] if not c['reused_completed_primary_baseline']] if args.run else [c for c in plan['cases'] if c['id'] == args.run_case]
    if not cases:
        raise SystemExit('Unknown exact pressure case ID')
    for case in cases:
        execute(out, plan, case)


if __name__ == '__main__':
    main()
