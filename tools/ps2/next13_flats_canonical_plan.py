#!/usr/bin/env python3
"""Freeze fog or default-observer legacy image plans from an admitted MAP02 plan.

Preparation only: no compiler, emulator, source edit, or ELF rewrite. Execution
uses next13_flats_combined_native.py through the existing official runner.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil

from next12_runtime import snapshot
from next13_flats_pressure import ROOT, sha, verify_files


def prepare(args):
    base = json.loads(args.map02_readiness.read_text())
    verify_files(base)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    plan = copy.deepcopy(base)
    plan['status'] = 'FROZEN before execution; no emulator/compiler invoked'
    plan['phase'] = 'pressure' if args.phase == 'fog' else 'images'
    plan['map'] = 'MAP01' if args.phase == 'fog' else 'MAP02'
    plan['cases'] = []
    files = plan['immutable_input_sha256']
    for path in (args.map02_readiness.resolve(), args.resource_policy.resolve()):
        files[str(path)] = sha(path)
    pinned = out / 'pinned-tools'
    pinned.mkdir()
    for name in ('next13_flats_canonical_plan.py', 'next13_flats_combined_native.py',
                 'next11_integration_check.py', 'next11_smoke_check.py'):
        source = ROOT / 'tools/ps2' / name
        shutil.copyfile(source, pinned / name)
        for path in (source, pinned / name):
            files[str(path)] = sha(path)
    plan['map02_readiness_authority'] = {'path': str(args.map02_readiness.resolve()), 'sha256': sha(args.map02_readiness)}
    seed = plan['seed']
    if args.phase == 'fog':
        prior = ROOT / 'build/nov15-next12/final-hardware-pressure/runtime/final-hw-fog-stereo-01b2d3f5-spawn-350/run-pre.json'
        old_command = json.loads(prior.read_text())['command']
        files[str(prior)] = sha(prior)
        plan['prior_full_fog_command_authority'] = {'path': str(prior), 'sha256': sha(prior)}
        for flag, value in zip(old_command, old_command[1:]):
            if flag == '--home-file':
                source = Path(value.rsplit('=', 1)[0])
                files[str(source)] = sha(source)
        for role in ('baseline', 'candidate'):
            name = role + '-hw-fog-stereo-' + seed + '-spawn-350'
            command = list(old_command)
            for flag, value in (('--name', name), ('--elf', plan['admissions'][role]['diagnostic_ELF']), ('--out', str(out / 'runs'))):
                command[command.index(flag) + 1] = value
            at = command.index('--')
            command[at:at] = ['--cfg', 'con_hudlines "0"']
            command.append('-zspill')
            engine = command[command.index('--') + 1:]
            plan['cases'].append(case(role + '-default', role, 'default', name, command, engine, 'MAP01', out))
        plan['expected_flat_count_prediction_until_native_requests'] = 4743
        plan['fog_adaptation'] = 'Prior exact full350 NPOT/fog/stereo profile; only role/name/ELF/output and common HUD0/zspill differ'
    else:
        plan['config_addition'] = None
        plan['expected_staged_config'] = 'fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\nrollingdemos "Off"\n'
        plan['capture_names'] = ['vidshot-320x200-l%d.ppm' % n for n in (20, 70, 140)]
        plan['whole_PPM_policy'] = 'Require every raw file byte equal within each legacy mode, including headers; no crop or normalization'
        plan['no_hwstats_zspill_or_console_HUD_override'] = True
        for mode, bits in (('legacy', '65536'), ('legacy-scalar', '65600')):
            for role in ('baseline', 'candidate'):
                source = next(c for c in base['cases'] if c['id'] == role + '-default')
                name = role + '-hw-map02-' + mode + '-images-' + seed
                engine = ['-singletics', '-keepspawn', '-renderer', 'Hardware', '-hwfb', '-hwgo', bits, '-vidshot', 'l20,l70,l140']
                command = [*source['command'][:3], '--name', name, '--elf', plan['admissions'][role]['diagnostic_ELF'],
                    '--pak', source['command'][source['command'].index('--pak') + 1], '--out', str(out / 'runs'),
                    '--map', 'MAP02', '--timeout', '420', '--until', 'VIDSHOT COMPLETE 3', '--', *engine]
                plan['cases'].append(case(role + '-' + mode, role, mode, name, command, engine, 'MAP02', out))
    plan['settings'] = snapshot(out / 'prepared-profile', 'before')
    target = out / 'readiness.json'
    target.write_text(json.dumps(plan, indent=2) + '\n')
    print(json.dumps({'readiness': str(target), 'sha256': sha(target), 'cases': [c['id'] for c in plan['cases']]}))


def case(identifier, role, mode, name, command, engine, mapname, out):
    return {'id': identifier, 'role': role, 'mode': mode, 'name': name, 'command': command,
        'run': str(out / 'runs' / name), 'runtime': str(out / 'runtime' / name),
        'expected_ps2args': ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', mapname, *engine]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=('fog', 'legacy-images'), required=True)
    parser.add_argument('--map02-readiness', type=Path, required=True)
    parser.add_argument('--resource-policy', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if not args.out.resolve().is_relative_to(ROOT / 'build/nov15-next13'):
        parser.error('An isolated pass13 evidence directory is required')
    prepare(args)


if __name__ == '__main__':
    main()
