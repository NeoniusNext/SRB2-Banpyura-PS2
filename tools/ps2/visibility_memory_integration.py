#!/usr/bin/env python3
"""Compare full Hardware frames through the official locked 32-MiB runner.

Shipping ELF files retain their OS-seeded local RNG. These scene comparisons
are correctness checks, not a whole-game performance benchmark. Frozen source
builds and focused culling/record tests provide separate evidence.
"""
import argparse
import filecmp
import hashlib
import json
from pathlib import Path
import re
import sys

from next12_runtime import run_verified

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
SHUTDOWN = 'I_ShutdownSystem(): end of logstream.'
SCENES = {
    'map02': ('MAP02', [], False),
    'map01': ('MAP01', [], False),
    'fog-npot': ('MAP01', ['-keepspawn', '-nodigmusic'], True),
    'legacy': ('MAP02', ['-hwgo', '65536'], False),
    'legacy-sprites': ('MAP02', ['-hwgo', '65600'], False),
}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def classify_skips(lines):
    """Classify diagnostic text without excusing any skipped-draw signal."""
    return {'selected_while_collecting': [x for x in lines if 'selected while collecting' in x],
            'purged_source': [x for x in lines if 'no data (purged)' in x],
            'not_resident': [x for x in lines if x.startswith('HWD texture not resident ')],
            'other': [x for x in lines if 'selected while collecting' not in x
                      and 'no data (purged)' not in x and not x.startswith('HWD texture not resident ')]}


def inspect(run, elf, expected_args, assets, fixtures):
    summary = json.loads((run / 'summary.json').read_text())
    boot = (run / 'boot.txt').read_text(errors='replace')
    captures = {p.name: sha(p) for p in sorted(run.glob('vidshot-*.ppm'))}
    errors = []
    if summary['rc'] or summary['ram'] != 32 or summary['errors']:
        errors.append('Wrapper, memory configuration or engine failure')
    if sha(run / 'SRB2.ELF') != sha(elf):
        errors.append('Staged ELF changed')
    if (run / 'ps2args').read_text().splitlines() != expected_args:
        errors.append('Engine argument order changed')
    if 'HWE startup renderer=Hardware chosen=2 hardware=1' not in boot or re.search(
            r'Switching to the Software renderer|HARDWARE -> SOFTWARE', boot):
        errors.append('Real Hardware renderer absent or abandoned')
    if SHUTDOWN not in boot:
        errors.append('Configuration save / sound and graphics shutdown marker absent')
    if re.search(r'I_Error|HEAP CHECK FAILED|Out of memory|^ERROR:', boot, re.M):
        errors.append('Fatal or heap error')
    warnings = [line for line in boot.splitlines() if 'WARNING:' in line]
    if warnings:
        errors.append('Unexpected warning')
    skipped = [line for line in boot.splitlines()
               if line.startswith(('HWSKIP ', 'HWD texture not resident '))]
    if skipped:
        errors.append('Hardware texture draws missing or skipped between captures')
    if any(map(int, re.findall(r'(?:underruns|hmis|herr)=(\d+)', boot))):
        errors.append('Audio underrun or hash error')
    if len(captures) != 3 or 'VIDSHOT COMPLETE 3' not in boot:
        errors.append('Three complete frame captures absent')
    tags = set()
    for name in captures:
        raw = (run / name).read_bytes()
        header = re.match(rb'P6\n([1-9][0-9]*) ([1-9][0-9]*)\n255\n', raw)
        tag = re.fullmatch(r'vidshot-(\d+)x(\d+)-(l\d+)\.ppm', name)
        if not header or not tag:
            errors.append('Invalid full-frame PPM header or name: ' + name)
            continue
        width, height = map(int, header.groups())
        if (width, height) != (int(tag[1]), int(tag[2])) or len(raw) != header.end() + width * height * 3:
            errors.append('Incomplete PPM pixel payload: ' + name)
        tags.add(tag[3])
    if tags != {'l20', 'l70', 'l140'}:
        errors.append('Requested capture frames absent')
    config = run / 'reference.cfg'
    if not config.is_file() or '// SRB2 configuration file.' not in config.read_text(errors='replace'):
        errors.append('Saved engine configuration absent')
    for source in assets:
        if sha(run / source.name) != sha(source):
            errors.append('Stock asset changed: ' + source.name)
    for source, target in fixtures:
        if sha(run / '.srb2' / target) != sha(source):
            errors.append('Scene fixture changed: ' + target)
    if fixtures and not all(marker in boot for marker in (
            'NEXT5 NPOT SPRITES READY', 'NEXT7 FOG SOLID READY', 'NEXT5 STEREO SFX event=4')):
        errors.append('Requested scripted scene did not execute')
    lhash = re.findall(r'^LHASH .+$', boot, re.M)
    if not lhash:
        errors.append('Loader hash evidence absent')
    return {'rc': summary['rc'], 'elf_sha256': sha(elf), 'captures': captures,
            'loader_hashes': lhash, 'saved_config_sha256': sha(config) if config.is_file() else None,
            'boot_sha256': sha(run / 'boot.txt'), 'warnings': warnings,
            'skipped_draw_lines': skipped, 'skipped_draw_classification': classify_skips(skipped),
            'skipped_draw_scope': 'Diagnostic lines only, not complete draw counters. No classification is exempted from the strict gate.',
            'errors': errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    parser.add_argument('--scenes', default=','.join(SCENES))
    parser.add_argument('--full-textures', action='store_true',
                        help='Explicit full-size mode in BOTH roles: -hwdbg 33554432 (default frame planning remains unchanged)')
    parser.add_argument('--stop-on-purged-texture', action='store_true',
                        help='Stop safely between scene pairs if candidate reports purged or nonresident texture draws; retain all strict errors')
    parser.add_argument('--diagnostic-seed', type=lambda value: int(value, 0),
                        help='Verify already patched fixed-local-RNG copies against both original build directories')
    parser.add_argument('--baseline-build', type=Path)
    parser.add_argument('--candidate-build', type=Path)
    parser.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    args.out = args.out.resolve()
    if not args.out.is_relative_to(ROOT / 'build') or args.out == ROOT / 'build':
        parser.error('--out must be a child of repository build/')
    scenes = args.scenes.split(',')
    if not scenes or len(set(scenes)) != len(scenes) or set(scenes) - SCENES.keys():
        parser.error('--scenes must name distinct supported scenarios')
    assets = sorted(args.pak.resolve().glob('*.PAK')) + [args.pak.resolve() / 'FINEACON.DAT']
    if not {'SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK', 'FINEACON.DAT'}.issubset(
            {p.name for p in assets if p.is_file()}):
        parser.error('Full stock PAK assets and FINEACON.DAT required')
    fixtures = [(ROOT / 'build/visibility-memory-20261115/fixtures/npot/NX5_FLOOR.pk3', 'NX5_FLOOR.pk3'),
                (ROOT / 'build/visibility-memory-20261115/fixtures/fog/NX7_FOG.pk3', 'NX7_FOG.pk3'),
                (TOOLS / 'next5_audio_integration.lua', 'NX5_AUDIO.lua')]
    if 'fog-npot' in scenes and any(not p.is_file() for p, _ in fixtures):
        parser.error('Generate fixtures first with next5_render_npot_fixture.py and next7_render_fog_fixture.py')
    elfs = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
    inputs = [*elfs.values(), *assets, Path(__file__), *[p for p, _ in fixtures if p.is_file()]]
    admissions = {}
    if args.diagnostic_seed is not None:
        from next13_flats_pressure import admit_elf
        if not (args.baseline_build and args.candidate_build):
            parser.error('--diagnostic-seed requires both original build directories')
        for role in elfs:
            build = getattr(args, role + '_build').resolve()
            admissions[role] = admit_elf(build, elfs[role], args.diagnostic_seed)
            inputs += [build / 'SRB2.ELF', build / 'build-report.json']
    manifest = {str(p): sha(p) for p in inputs}
    ready = {'input_sha256': manifest, 'scenes': scenes}
    if args.full_textures:
        ready['explicit_engine_args_both_roles'] = ['-hwdbg', '33554432']
    if args.stop_on_purged_texture:
        ready['stop_on_purged_texture'] = True
    if admissions:
        ready['diagnostic_admissions'] = admissions
    args.out.mkdir(parents=True, exist_ok=True)
    readiness = args.out / 'readiness.json'
    if args.analyze_only:
        frozen = json.loads(readiness.read_text())
        if frozen != ready:
            raise RuntimeError('Frozen scene list or input bytes changed')
    else:
        if readiness.exists():
            parser.error('Choose a fresh --out; existing stages are immutable')
        readiness.write_text(json.dumps(ready, indent=2) + '\n')
    report = {'runs': {}, 'pairs': [], 'errors': [], 'normal_OS_RNG': not bool(admissions),
              'diagnostic_seed': args.diagnostic_seed,
              'FPS_claimed': False, 'physical_PS2_tested': False,
              'shutdown_limit': 'Marker follows save and sound/graphics shutdown; later W_Shutdown/PS2Boot_Exit unproved.'}
    if args.full_textures:
        report['explicit_engine_args_both_roles'] = ['-hwdbg', '33554432']
    for scene in scenes:
        mapname, extra, scripted = SCENES[scene]
        for role, elf in elfs.items():
            name = role + '-' + scene
            run = args.out / 'runs' / name
            selected = fixtures if scripted else []
            engine_args = ['-singletics', '-loadprof', '-loadhash', '-astat', *extra]
            if args.full_textures:
                engine_args += ['-hwdbg', '33554432']
            if scripted:
                engine_args += ['-file', *['host:/.srb2/' + target for _, target in selected]]
            engine_args += ['-vidshot', 'l20,l70,l140', '-renderer', 'Hardware']
            command = [sys.executable, '-B', str(TOOLS / 'opt_run.py'), '--name', name,
                       '--elf', str(elf), '--ram', '32', '--pak', str(args.pak.resolve()),
                       '--out', str(args.out / 'runs'), '--map', mapname,
                       '--timeout', '240', '--until', SHUTDOWN]
            for source, target in selected:
                command += ['--home-file', str(source) + '=' + target]
            command += ['--', *engine_args]
            if not args.analyze_only:
                if run.exists():
                    raise RuntimeError('Run exists: ' + str(run))
                print('START ' + name, flush=True)
                result = run_verified(command, args.out / 'runtime' / name, inputs)
                if result.returncode:
                    raise RuntimeError(name + ': wrapper exit ' + str(result.returncode))
            expected = ['-config', 'reference.cfg', '-nolog', '-noendtxt', '-skipintro', '-warp', mapname, *engine_args]
            item = inspect(run, elf, expected, assets, selected)
            report['runs'][name] = item
            report['errors'] += [name + ': ' + error for error in item['errors']]
        before, after = (report['runs'][role + '-' + scene] for role in elfs)
        equal = before['captures'] == after['captures'] and len(before['captures']) == 3
        if equal:
            equal = all(filecmp.cmp(args.out / 'runs' / ('baseline-' + scene) / name,
                                    args.out / 'runs' / ('candidate-' + scene) / name, shallow=False)
                        for name in before['captures'])
        equal_loader = before['loader_hashes'] == after['loader_hashes']
        equal_config = before['saved_config_sha256'] == after['saved_config_sha256']
        report['pairs'].append({'scene': scene, 'complete_frames': 3, 'equal_frames': equal,
                                'equal_loader_hashes': equal_loader, 'equal_saved_config': equal_config})
        if not all((equal, equal_loader, equal_config)):
            report['errors'].append(scene + ': full frame / loader / saved configuration mismatch')
        print(scene + ': ' + ('PASS' if all((equal, equal_loader, equal_config)) else 'FAIL'), flush=True)
        classification = after['skipped_draw_classification']
        if args.stop_on_purged_texture and (classification['purged_source'] or classification['not_resident']):
            report['stopped_on_missing_content_scene'] = scene
            report['errors'].append(scene + ': stopped before further scenes because texture content was purged or nonresident')
            break
    report['requested_scenes'] = scenes
    report['complete_scene_suite'] = len(report['pairs']) == len(scenes)
    if manifest != {str(p): sha(p) for p in inputs}:
        report['errors'].append('Inputs changed during comparison')
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'frame_pairs': sum(p['complete_frames'] for p in report['pairs']), 'errors': report['errors']}))
    return 1 if report['errors'] else 0


if __name__ == '__main__':
    sys.exit(main())
