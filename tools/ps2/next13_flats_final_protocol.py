#!/usr/bin/env python3
"""Prepare final canonical pressure or normal-observer image plans, never run them.

Creates both fresh350 MAP02 and full fog/NPOT/stereo plans for one audited seed.
Both roles add the predeclared existing -zreport1 stdout observer. Older strict
policies, arguments, runners, and rejected evidence remain unchanged.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil
import sys

from next12_runtime import snapshot
from next13_flats_pressure import ROOT, admit_elf, sha, verify_files


def prepare(args):
    seed = int(args.seed, 16)
    builds = {'baseline': args.baseline_build.resolve(), 'candidate': args.candidate_build.resolve()}
    elfs = {'baseline': args.baseline_elf.resolve(), 'candidate': args.candidate_elf.resolve()}
    admissions = {role: admit_elf(builds[role], elfs[role], seed) for role in builds}
    files = {}
    for role, build in builds.items():
        for name in ('SRB2.ELF', 'SRB2.map', 'build-report.json', 'obj/engine_lto.o'):
            files[str(build / name)] = sha(build / name)
        files[str(elfs[role])] = sha(elfs[role])
    for path in [*args.pak.glob('*.PAK'), args.pak / 'FINEACON.DAT', args.resource_policy, *args.input]:
        path = path.resolve()
        files[str(path)] = sha(path)
    for name in ('src/ps2/i_system.c', 'src/ps2/ps2_spill.c', 'src/ps2/ps2_mem.c', 'src/z_zone.c'):
        files[str(ROOT / name)] = sha(ROOT / name)
    prior = ROOT / 'build/nov15-next12/final-hardware-pressure/runtime/final-hw-fog-stereo-01b2d3f5-spawn-350/run-pre.json'
    old_fog = json.loads(prior.read_text())['command']
    files[str(prior)] = sha(prior)
    fog_files = []
    for flag, value in zip(old_fog, old_fog[1:]):
        if flag == '--home-file':
            source = Path(value.rsplit('=', 1)[0])
            fog_files += ['--home-file', value]
            files[str(source)] = sha(source)
    common = ['-singletics', '-keepspawn', '-loadprof', '-loadhash', '-zck', '-zheap', '1',
              '-zstack', '-zquit', '350', '-astat', '-renderer', 'Hardware', '-ps2prof',
              '-hwfb', '-hwstats', '35', '-hwpolyhash']
    if old_fog[old_fog.index('--') + 1:] != common + ['-nodigmusic', '-file',
            'host:/.srb2/NX5_FLOOR.pk3', 'host:/.srb2/NX7_FOG.pk3', 'host:/.srb2/NX5_AUDIO.lua']:
        raise RuntimeError('Prior complete fog profile differs')
    for scene, mapname, expected_n in (('map02', 'MAP02', 4740), ('fog-stereo', 'MAP01', 4743)):
        out = args.out.resolve() / args.seed / scene
        out.mkdir(parents=True, exist_ok=False)
        pins = copy.deepcopy(files)
        pinned = out / 'pinned-tools'
        pinned.mkdir()
        for name in ('next13_flats_final_protocol.py', 'next13_flats_combined_native.py', 'next13_flats_pressure.py',
                     'next12_runtime.py', 'next10_gs_run.py', 'next11_integration_check.py', 'next11_smoke_check.py',
                     'opt_run.py', 'run_pcsx2.py', 'next11_local_rng.py'):
            source = ROOT / 'tools/ps2' / name
            shutil.copyfile(source, pinned / name)
            for path in (source, pinned / name):
                pins[str(path)] = sha(path)
        cases = []
        for role in ('baseline', 'candidate'):
            name = role + '-hw-' + scene + '-' + args.seed + '-spawn-350'
            if scene == 'fog-stereo':
                command = list(old_fog)
                for flag, value in (('--name', name), ('--elf', str(elfs[role])), ('--out', str(out / 'runs'))):
                    command[command.index(flag) + 1] = value
                at = command.index('--')
                command[at:at] = ['--cfg', 'con_hudlines "0"']
                command += ['-zspill', '-zreport', '1']
            else:
                command = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', name,
                    '--elf', str(elfs[role]), '--pak', str(args.pak.resolve()), '--out', str(out / 'runs'),
                    '--map', mapname, '--timeout', '300', '--cfg', 'con_hudlines "0"', '--',
                    *common, '-zspill', '-zreport', '1']
            engine = command[command.index('--') + 1:]
            cases.append({'id': role + '-default', 'role': role, 'mode': 'default', 'name': name,
                'command': command, 'run': str(out / 'runs' / name), 'runtime': str(out / 'runtime' / name),
                'expected_ps2args': ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', mapname, *engine]})
        plan = {'status': 'FROZEN final canonical protocol; no emulator/compiler invoked', 'phase': 'pressure',
            'seed': args.seed, 'map': mapname, 'frames': 350, 'scene': scene, 'admissions': admissions,
            'cases': cases, 'immutable_input_sha256': pins, 'settings': snapshot(out / 'prepared-profile', 'before'),
            'config_addition': 'con_hudlines "0"', 'no_old_default_config_pressure_run_reused': True,
            'new_protocol_engine_observer': ['-zreport', '1'],
            'existing_shipping_observer_without_source_or_ELF_instrumentation': True,
            'observer_semantics': {'level_exit_calls_ZA_ResetPeak': True, 'peak': 'since the existing level-exit reset',
                'gpeak': 'global high-water value', 'PS2Mem_Report_frame_bytes': 3296, 'I_OutputMsg_frame_bytes': 1360,
                'reporting_stack': 'Actual reporting/call-chain PDR admission is pinned separately; no extrapolation to a default-observer stack high-water value',
                'scope': 'Compare only these fresh matched observer-enabled roles; no inference to old/no-zreport peak or default-observer stack'},
            'expected_flat_count_prediction_until_native_requests': expected_n,
            'resource_policy': str(args.resource_policy.resolve()), 'resource_policy_sha256': sha(args.resource_policy),
            'prior_full_fog_command_authority': {'path': str(prior), 'sha256': sha(prior)},
            'new_protocol_not_old_argument_identity': True, 'default_no_hwstats_image_protocol_is_separate': True}
        verify_files(plan)
        readiness = out / 'readiness.json'
        readiness.write_text(json.dumps(plan, indent=2) + '\n')
        print(json.dumps({'readiness': str(readiness), 'sha256': sha(readiness), 'expected_N': expected_n}))


def prepare_images(args):
    base = json.loads(args.images_from_readiness.read_text())
    verify_files(base)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    files = copy.deepcopy(base['immutable_input_sha256'])
    files[str(args.images_from_readiness.resolve())] = sha(args.images_from_readiness)
    pinned = out / 'pinned-tools'
    pinned.mkdir()
    for name in ('next13_flats_final_protocol.py', 'next13_flats_combined_native.py', 'next13_flats_image_check.py',
                 'next11_integration_check.py', 'next11_smoke_check.py', 'next12_runtime.py',
                 'next10_gs_run.py', 'opt_run.py', 'run_pcsx2.py'):
        source = ROOT / 'tools/ps2' / name
        shutil.copyfile(source, pinned / name)
        for path in (source, pinned / name):
            files[str(path)] = sha(path)
    seed = base['seed']
    cases = []
    for mode, bits in (('legacy', '65536'), ('legacy-scalar', '65600')):
        for role in ('baseline', 'candidate'):
            source = next(c for c in base['cases'] if c['id'] == role + '-default')
            name = role + '-hw-map02-' + mode + '-images-' + seed
            engine = ['-singletics', '-keepspawn', '-renderer', 'Hardware', '-hwfb', '-hwgo', bits, '-vidshot', 'l20,l70,l140']
            command = [*source['command'][:3], '--name', name, '--elf', base['admissions'][role]['diagnostic_ELF'],
                '--pak', source['command'][source['command'].index('--pak') + 1], '--out', str(out / 'runs'),
                '--map', 'MAP02', '--timeout', '420', '--until', 'VIDSHOT COMPLETE 3', '--', *engine]
            cases.append({'id': role + '-' + mode, 'role': role, 'mode': mode, 'name': name, 'command': command,
                'run': str(out / 'runs' / name), 'runtime': str(out / 'runtime' / name),
                'expected_ps2args': ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', 'MAP02', *engine]})
    plan = {'status': 'FROZEN final normal-observer legacy image plan; no emulator/compiler invoked',
        'phase': 'images', 'seed': seed, 'map': 'MAP02', 'cases': cases, 'admissions': base['admissions'],
        'immutable_input_sha256': files, 'settings': snapshot(out / 'prepared-profile', 'before'),
        'config_addition': None, 'no_hwstats_zspill_or_console_HUD_override': True,
        'no_zreport_or_other_pressure_observer_arguments': True,
        'expected_staged_config': 'fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\nrollingdemos "Off"\n',
        'capture_names': ['vidshot-320x200-l%d.ppm' % n for n in (20, 70, 140)],
        'whole_PPM_policy': 'Require every raw header and pixel file byte equal within each matching legacy mode; no crop or normalization',
        'frozen_pressure_readiness_authority': {'path': str(args.images_from_readiness.resolve()), 'sha256': sha(args.images_from_readiness)}}
    readiness = out / 'readiness.json'
    readiness.write_text(json.dumps(plan, indent=2) + '\n')
    print(json.dumps({'readiness': str(readiness), 'sha256': sha(readiness), 'cases': [c['id'] for c in cases]}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next13/final-hardware-pressure')
    parser.add_argument('--baseline-build', type=Path, default=ROOT / 'build/nov15-next12/final/hardware')
    parser.add_argument('--candidate-build', type=Path, default=ROOT / 'build/nov15-next13/final/hardware')
    parser.add_argument('--baseline-elf', type=Path)
    parser.add_argument('--candidate-elf', type=Path)
    parser.add_argument('--seed', choices=('01b2d3f5', '9e3779b9'))
    parser.add_argument('--resource-policy', type=Path)
    parser.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    parser.add_argument('--input', type=Path, action='append', default=[])
    parser.add_argument('--images-from-readiness', type=Path)
    args = parser.parse_args()
    if not args.out.resolve().is_relative_to(ROOT / 'build/nov15-next13'):
        parser.error('An isolated pass13 evidence directory is required')
    if args.images_from_readiness:
        prepare_images(args)
    else:
        if not (args.baseline_elf and args.candidate_elf and args.seed and args.resource_policy):
            parser.error('Pressure preparation requires both audited ELFs, seed, and resource policy')
        prepare(args)


if __name__ == '__main__':
    main()
