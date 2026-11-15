#!/usr/bin/env python3
"""Prepare frozen fog/image plans or execute one coordinator-granted case.

Uses only the official runtime/opt_run/run_pcsx2 chain. Pressure quality and
resource acceptance belong to next13_combined_pressure_check.py, unchanged.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil

from next12_runtime import run_verified, snapshot
from next13_flats_pressure import ROOT, sha, verify_files
from next11_integration_check import inspect


WORK = ROOT / 'build/nov15-next13/flats-combined-native-v1'
DEFAULT_CONFIG = 'fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\nrollingdemos "Off"\n'


def prepare(kind):
    base = json.loads((WORK / 'map02/readiness.json').read_text())
    out = WORK / (kind + '-v2')
    out.mkdir(parents=True, exist_ok=False)
    plan = copy.deepcopy(base)
    plan['status'] = 'FROZEN before execution; no emulator/compiler invoked'
    plan['phase'] = 'pressure' if kind == 'fog' else 'images'
    plan['map'] = 'MAP01' if kind == 'fog' else 'MAP02'
    plan['cases'] = []
    pinned = out / 'pinned-tools'
    pinned.mkdir()
    # Keep the immutable original map02 preparation and runner pins, then add
    # this wrapper and inspectors without modifying that existing plan.
    for name in ('next13_flats_combined_native.py', 'next11_integration_check.py', 'next11_smoke_check.py'):
        source = ROOT / 'tools/ps2' / name
        shutil.copyfile(source, pinned / name)
        for p in (source, pinned / name):
            plan['immutable_input_sha256'][str(p)] = sha(p)
    plan['immutable_input_sha256'][str(WORK / 'map02/readiness.json')] = sha(WORK / 'map02/readiness.json')
    plan['immutable_input_sha256'][str(WORK / 'resource-policy.json')] = sha(WORK / 'resource-policy.json')
    if kind == 'fog':
        prior = ROOT / 'build/nov15-next12/final-hardware-pressure/runtime/final-hw-fog-stereo-01b2d3f5-spawn-350/run-pre.json'
        old_command = json.loads(prior.read_text())['command']
        plan['prior_fog_command_authority'] = {'path': str(prior), 'sha256': sha(prior)}
        plan['immutable_input_sha256'][str(prior)] = sha(prior)
        for flag, arg in zip(old_command, old_command[1:]):
            if flag == '--home-file':
                source = Path(arg.rsplit('=', 1)[0])
                plan['immutable_input_sha256'][str(source)] = sha(source)
        for role in ('baseline', 'candidate'):
            row = next(c for c in base['cases'] if c['id'] == role + '-default')
            command = list(old_command)
            name = role + '-hw-fog-stereo-01b2d3f5-spawn-350'
            for flag, value in (('--name', name), ('--elf', plan['admissions'][role]['diagnostic_ELF']), ('--out', str(out / 'runs'))):
                command[command.index(flag) + 1] = value
            at = command.index('--')
            command[at:at] = ['--cfg', 'con_hudlines "0"']
            command.append('-zspill')
            args = command[command.index('--') + 1:]
            plan['cases'].append({'id': role + '-default', 'role': role, 'mode': 'default', 'name': name,
                'command': command, 'run': str(out / 'runs' / name), 'runtime': str(out / 'runtime' / name),
                'expected_ps2args': ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', 'MAP01', *args]})
        plan['expected_flat_count_prediction_until_native_requests'] = 4743
        plan['fog_adaptation'] = 'Prior full350 NPOT/fog/stereo profile; change only role/name/ELF/output and add matched HUD0/zspill'
    else:
        plan['config_addition'] = None
        plan['expected_staged_config'] = DEFAULT_CONFIG
        plan['capture_names'] = ['vidshot-320x200-l%d.ppm' % n for n in (20, 70, 140)]
        plan['whole_PPM_policy'] = 'Require every raw file byte equal for each matching legacy mode; no crop or normalization'
        for mode, bits in (('legacy', '65536'), ('legacy-scalar', '65600')):
            for role in ('baseline', 'candidate'):
                row = next(c for c in base['cases'] if c['id'] == role + '-default')
                name = role + '-hw-map02-' + mode + '-images-01b2d3f5'
                args = ['-singletics', '-keepspawn', '-renderer', 'Hardware', '-hwfb', '-hwgo', bits, '-vidshot', 'l20,l70,l140']
                command = [*row['command'][:3], '--name', name, '--elf', plan['admissions'][role]['diagnostic_ELF'],
                    '--pak', str(ROOT / 'build/pak'), '--out', str(out / 'runs'), '--map', 'MAP02',
                    '--timeout', '420', '--until', 'VIDSHOT COMPLETE 3', '--', *args]
                plan['cases'].append({'id': role + '-' + mode, 'role': role, 'mode': mode, 'name': name,
                    'command': command, 'run': str(out / 'runs' / name), 'runtime': str(out / 'runtime' / name),
                    'expected_ps2args': ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', 'MAP02', *args]})
        plan['no_hwstats_zspill_or_console_HUD_override'] = True
    plan['settings'] = snapshot(out / 'prepared-profile', 'before')
    (out / 'readiness.json').write_text(json.dumps(plan, indent=2) + '\n')
    print(json.dumps({'readiness': str(out / 'readiness.json'), 'sha256': sha(out / 'readiness.json'), 'cases': [c['id'] for c in plan['cases']]}))


def execute(readiness, case_id, policy_manifest):
    plan = json.loads(readiness.read_text())
    verify_files(plan)
    case = next(c for c in plan['cases'] if c['id'] == case_id)
    inputs = [Path(p) for p in plan['immutable_input_sha256']] + [readiness]
    if plan['phase'] == 'pressure':
        if not policy_manifest:
            raise RuntimeError('Frozen pre-execution pressure policy required')
        manifest = json.loads(policy_manifest.read_text())
        if manifest['readiness_sha256'] != sha(readiness):
            raise RuntimeError('Frozen pressure readiness differs')
        inputs += [policy_manifest, Path(manifest['resource_policy']), *map(Path, manifest['helper_sha256'])]
        for path, digest in manifest['helper_sha256'].items():
            if sha(path) != digest:
                raise RuntimeError('Frozen pressure helper changed')
    run, runtime = Path(case['run']), Path(case['runtime'])
    if run.exists() or runtime.exists():
        raise RuntimeError('Fresh official case required; old outputs remain immutable')
    result = run_verified(case['command'], runtime, inputs)
    if result.returncode:
        raise RuntimeError('Official runtime failed')
    item = inspect(run, case['name'])
    if (run / 'ps2args').read_text().splitlines() != case['expected_ps2args']:
        item['errors'].append('Exact staged engine arguments differ')
    if sha(run / 'SRB2.ELF') != plan['admissions'][case['role']]['diagnostic_ELF_sha256']:
        item['errors'].append('Exact staged diagnostic ELF differs')
    if plan['phase'] == 'images':
        if set(item['captures']) != set(plan['capture_names']):
            item['errors'].append('Three complete native PPM captures absent')
        if (run / '.srb2/reference.cfg').read_text() != plan['expected_staged_config']:
            item['errors'].append('Normal staged observer configuration differs')
    (readiness.parent / (case_id + '-inspection.json')).write_text(json.dumps(item, indent=2) + '\n')
    print(json.dumps({'case': case_id, 'errors': item['errors'], 'captures': item['captures']}))
    if item['errors']:
        raise RuntimeError('Native case rejected; inspect retained raw results')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', choices=('fog', 'legacy-images'))
    parser.add_argument('--readiness', type=Path)
    parser.add_argument('--case')
    parser.add_argument('--policy-manifest', type=Path)
    args = parser.parse_args()
    if args.prepare:
        prepare(args.prepare)
    else:
        if not args.readiness or not args.case:
            parser.error('Select --prepare or a frozen --readiness and --case')
        execute(args.readiness.resolve(), args.case, args.policy_manifest.resolve() if args.policy_manifest else None)


if __name__ == '__main__':
    main()
