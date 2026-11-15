"""Freeze and compare two exact actual-Hardware three-frame image runs.

Only the separately granted serial emulator action launches opt_run.py.
Complete PPM files are compared against both original and PB-only captures.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

from next12_runtime import run_verified, snapshot

ROOT = Path(__file__).resolve().parents[2]
SCENES = (('images-map02', 'pb-images-hw-map02'), ('images-fog-stereo', 'pb-images-hw-fog-stereo'))


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(out, work):
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable image stage required')
    out.mkdir(parents=True); pinned = out / 'pinned-tools'; pinned.mkdir()
    ready = work / 'game-v1/readiness.json'; pressure = json.loads(ready.read_text())
    pins = dict(pressure['complete_full_build_source_IR_engine_ELF_map_generated_inputs_and_runner_inputs'])
    reports = (work / 'gamecheck-map02-v1/game-report.json', work / 'gamecheck-fog-v2/game-report.json')
    for path in reports:
        report = json.loads(path.read_text())
        if report['status'] != 'passed actual combined native350 and strict unchanged-capacity/policy gate': raise SystemExit('Actual native pressure gate not passed')
        pins[str(path)] = sha(path)
    pins[str(ready)] = sha(ready)
    diagnostic = work / 'local-rng-candidate-v1/DIAGNOSTIC-LOCAL-RNG-01b2d3f5.ELF'
    if sha(diagnostic) != pressure['diagnostic_ELF_sha256']: raise SystemExit('Pressure-pinned actual game changed')
    for name in ('next12_lighttable_images.py', 'next12_runtime.py', 'next10_gs_run.py', 'opt_run.py', 'run_pcsx2.py'):
        path = ROOT / 'tools/ps2' / name; pins[str(path)] = sha(path); shutil.copyfile(path, pinned / name)
    profile = snapshot(out, 'prepared'); scenes = []
    for scene, name in SCENES:
        directory = ROOT / 'build/nov15-next12/pb-scratch' / scene
        audit = json.loads((directory / 'report.json').read_text())
        if audit['status'] != 'PASS full native Hardware image oracle': raise SystemExit('Matching PB-only/original image gate not passed')
        runtime = directory / 'runtime' / name; pre = json.loads((runtime / 'run-pre.json').read_text())
        if pre['critical_settings'] != profile['critical_settings'] or pre['emulator_binary_sha256'] != profile['emulator_binary_sha256']:
            raise SystemExit('Matching exact image profile/emulator differs')
        command = list(pre['command']); new_name = name.replace('pb-images-', 'lighttable-images-', 1)
        command[command.index('--name') + 1] = new_name; command[command.index('--elf') + 1] = str(diagnostic)
        command[command.index('--out') + 1] = str(out / 'runs')
        original = Path(json.loads((directory / 'readiness.json').read_text())['baseline']); pb = directory / 'runs' / name
        expected = {p.name: sha(p) for p in sorted(original.glob('*.ppm'))}
        if len(expected) != 3 or expected != {p.name: sha(p) for p in sorted(pb.glob('*.ppm'))}:
            raise SystemExit('Complete original/PB-only PPM captures differ')
        for base in (directory, original):
            for p in base.rglob('*'):
                if p.is_file(): pins[str(p)] = sha(p)
        scenes.append({'scene': scene, 'name': new_name, 'command': command, 'original': str(original), 'PB_only': str(pb),
                       'reference_runtime': str(runtime), 'complete_original_and_PB_only_PPM_sha256': expected})
    if any(sha(Path(p)) != digest for p, digest in pins.items()): raise SystemExit('Frozen native image input changed')
    report = {'pressure_readiness_sha256': sha(ready), 'shipping_ELF_sha256': pressure['shipping_ELF_sha256'],
        'diagnostic_ELF_sha256': sha(diagnostic), 'source_context': pressure['source_context'],
        'all_complete_source_IR_build_ELF_map_tools_reference_and_pressure_inputs': pins,
        'preparation_profile': profile, 'scenes': scenes,
        'scope': 'Two serial actual Hardware image runs, complete 320x200 RGB PPM at level frames20/70/140 in each; fixed local seed only in diagnostic copies'}
    (out / 'readiness.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Frozen two-scene complete-image readiness: ' + str(out / 'readiness.json'))


def run(out, index):
    if os.environ.get('SRB2_PCSX2_SLOTS') != '1': raise SystemExit('Explicit serialized one-emulator configuration required')
    path = out / 'readiness.json'; ready = json.loads(path.read_text()); pins = ready['all_complete_source_IR_build_ELF_map_tools_reference_and_pressure_inputs']
    if any(sha(Path(p)) != value for p, value in pins.items()): raise SystemExit('Frozen image input changed')
    scene = ready['scenes'][index]; runtime = out / 'runtime' / scene['name']
    result = run_verified(scene['command'], runtime, [path, *map(Path, pins)])
    if result.returncode: raise SystemExit('Actual native image wrapper failed')
    pre, post = [json.loads((runtime / n).read_text()) for n in ('run-pre.json', 'run-post.json')]
    reference = json.loads((Path(scene['reference_runtime']) / 'run-pre.json').read_text())
    if pre['critical_settings'] != reference['critical_settings'] or post['rc'] or not all(post[k] for k in ('all_frozen_inputs_unchanged', 'all_non_UI_settings_unchanged', 'emulator_binary_unchanged')):
        raise SystemExit('Actual image settings/input/outcome gate failed')
    directory = out / 'runs' / scene['name']; expected = scene['complete_original_and_PB_only_PPM_sha256']
    observed = {p.name: sha(p) for p in sorted(directory.glob('*.ppm'))}
    if observed != expected: raise SystemExit('Actual complete PPM bytes differ from original/PB-only')
    staged = directory / 'SRB2.ELF'
    if sha(staged) != ready['diagnostic_ELF_sha256']: raise SystemExit('Actually executed image ELF changed')
    report = {'status': 'passed', 'scene': scene['scene'], 'readiness_sha256': sha(path), 'actually_staged_ELF_sha256': sha(staged),
        'complete_images': {name: {'sha256': value, 'bytes': (directory / name).stat().st_size} for name, value in observed.items()},
        'all_complete_PPM_file_bytes_exact_original_and_PB_only': True,
        'full_non_UI_settings_reference_exact_and_unchanged': True,
        'all_frozen_native_inputs_and_emulator_unchanged': True,
        'runtime_sha256': {n: sha(runtime / n) for n in ('run-pre.json', 'run-post.json')}}
    (out / ('image-report-' + str(index) + '.json')).write_text(json.dumps(report, indent=2) + '\n')
    print('Complete images exact: ' + scene['name'])


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov15-next12/lighttable-combined-v2')
    parser.add_argument('--run', type=int, choices=(0, 1)); args = parser.parse_args()
    if args.run is None: prepare(args.out.resolve(), args.work.resolve())
    else: run(args.out.resolve(), args.run)


if __name__ == '__main__': main()
