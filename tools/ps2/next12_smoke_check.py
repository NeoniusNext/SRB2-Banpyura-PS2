#!/usr/bin/env python3
"""Pair unmodified shipping games through saved configuration and shutdown.

Only opt_run.py is used. Normal OS-seeded local RNG is retained, so these
are image/startup/audio/save checks and cannot support timing or RAM claims.
The late log marker follows M_SaveConfig and sound/graphics shutdown; it
does not prove the subsequent W_Shutdown/PS2Boot_Exit has completed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

from next10_sw_bounds_codecheck import bind
from next7_render_fog_codecheck import Elf32
from next11_smoke_check import normal_rng, runtime_evidence, STAGE_CONFIG, CAPTURES
from next12_integration_check import inspect
from next12_runtime import run_verified

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
MARKER = 'I_ShutdownSystem(): end of logstream.'
SCENES = (('sw-fog-stereo', 'MAP01', False), ('hw-map02', 'MAP02', True))


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare(out):
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        raise RuntimeError('Choose a fresh immutable stage under build/')
    out.mkdir(parents=True)
    pinned = out / 'pinned-tools'; pinned.mkdir()
    inputs = set()
    for name in ('next12_smoke_check.py', 'next11_smoke_check.py', 'next12_runtime.py',
                 'next12_integration_check.py', 'next10_gs_run.py', 'opt_run.py', 'run_pcsx2.py',
                 'next10_sw_bounds_codecheck.py', 'next10_sw_bounds_report.py',
                 'next7_render_fog_codecheck.py', 'next8_sw_codecheck.py'):
        path = TOOLS / name; inputs.add(path)
        (pinned / name).write_bytes(path.read_bytes())
    pak = ROOT / 'build/pak'
    assets = sorted(pak.glob('*.PAK')) + [pak / 'FINEACON.DAT']
    if not {'SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK'}.issubset({p.name for p in assets}):
        raise RuntimeError('Full stock assets required')
    inputs.update(assets)
    fixtures = ((ROOT / 'build/nov14-next5/npot-addons/NX5_FLOOR.pk3', 'NX5_FLOOR.pk3'),
                (ROOT / 'build/nov14-next7/fog-addons/NX7_FOG.pk3', 'NX7_FOG.pk3'),
                (TOOLS / 'next5_audio_integration.lua', 'NX5_AUDIO.lua'))
    inputs.update(p for p, _ in fixtures)
    entries = []
    for role, base in (('base', ROOT / 'build/nov15-next12/baseline'),
                       ('final', ROOT / 'build/nov15-next12/final')):
        for scene, mapname, hardware in SCENES:
            build = base / ('hardware' if hardware else 'software')
            elf = build / 'SRB2.ELF'; release = normal_rng(elf, hardware)
            engine, linked = Elf32(build / 'obj/engine_lto.o'), Elf32(elf)
            shutdown = {n: bind(engine, linked, build / 'SRB2.map', n)
                        for n in ('I_Quit', 'I_ShutdownSystem')}
            inputs.update(Path(p) for p in release['inputs'])
            inputs.add(ROOT / 'src/ps2/i_system.c')
            name = role + '-normal-' + scene
            args = ['-singletics']
            if not hardware:
                args += ['-keepspawn', '-nodigmusic']
            args += ['-loadprof', '-loadhash', '-zck', '-zheap', '35', '-astat']
            if not hardware:
                args += ['-file', *['host:/.srb2/' + target for _, target in fixtures]]
            args += ['-vidshot', 'l20,l70,l140', '-zquit', '175']
            if hardware:
                args += ['-renderer', 'Hardware']
            command = [sys.executable, '-B', str(TOOLS / 'opt_run.py'), '--name', name,
                       '--elf', str(elf), '--ram', '32', '--pak', str(pak), '--out', str(out / 'runs'),
                       '--map', mapname, '--timeout', '180', '--until', MARKER]
            if not hardware:
                for source, target in fixtures:
                    command += ['--home-file', str(source) + '=' + target]
            command += ['--', *args]
            entries.append({'name': name, 'role': role, 'scene': scene, 'hardware': hardware,
                            'release': release, 'actual_shutdown_bindings': shutdown, 'command': command,
                            'expected_arguments': ['-config', 'reference.cfg', '-nolog', '-noendtxt',
                                                   '-skipintro', '-warp', mapname, *args]})
    report = {'normal_local_rng': True, 'performance_comparison_claimed': False,
              'shutdown_observation_limit': 'Late log marker follows config save and sound/graphics shutdown. W_Shutdown/PS2Boot_Exit after it are not proved.',
              'cases': entries, 'asset_sha256': {str(p): sha(p) for p in assets},
              'fixtures': {str(p): target for p, target in fixtures},
              'input_sha256': {str(p): sha(p) for p in sorted(inputs)}}
    (out / 'readiness.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Prepared four fresh matched normal-RNG runs: ' + str(out), flush=True)


def analyze(out):
    readiness = json.loads((out / 'readiness.json').read_text())
    pins = readiness['input_sha256']
    if any(sha(Path(p)) != value for p, value in pins.items()):
        raise RuntimeError('Frozen normal smoke inputs changed')
    report = {'normal_local_rng': True, 'performance_comparison_claimed': False,
              'readiness_sha256': sha(out / 'readiness.json'), 'runs': {}, 'pairs': [], 'errors': [],
              'shutdown_observation_limit': readiness['shutdown_observation_limit']}
    for entry in readiness['cases']:
        name = entry['name']; run = out / 'runs' / name
        item = inspect(run, name)
        item['runtime'] = runtime_evidence(out / 'runtime' / name, entry['command'])
        boot = (run / 'boot.txt').read_text(errors='replace')
        if item['elf_sha256'] != entry['release']['ELF_sha256']:
            item['errors'].append('Staged unmodified full ELF differs')
        if set(item['captures']) != CAPTURES or re.findall(r'^VIDSHOT COMPLETE (\d+)$', boot, re.M) != ['3']:
            item['errors'].append('Three complete frames are required')
        if not item['audio_underruns'] or any(item['audio_underruns']):
            item['errors'].append('Missing or nonzero audio underruns')
        if MARKER not in boot or not json.loads((run / 'summary.json').read_text())['done']:
            item['errors'].append('Late post-save shutdown marker or ZQUIT DONE absent')
        if (run / '.srb2/reference.cfg').read_bytes() != STAGE_CONFIG:
            item['errors'].append('Complete staged configuration differs')
        saved = run / 'reference.cfg'
        if not saved.is_file() or not saved.stat().st_size or (run / 'reference.cfg.tmp').exists():
            item['errors'].append('Full atomic configuration save did not finish')
        item['saved_configuration_bytes'] = saved.stat().st_size if saved.is_file() else 0
        item['saved_configuration_sha256'] = sha(saved) if saved.is_file() else None
        if (run / 'ps2args').read_text().splitlines() != entry['expected_arguments']:
            item['errors'].append('Complete staged argument vector differs')
        for path, value in readiness['asset_sha256'].items():
            if sha(run / Path(path).name) != value:
                item['errors'].append('Staged stock asset differs: ' + Path(path).name)
        if not entry['hardware']:
            for source, target in readiness['fixtures'].items():
                if (run / '.srb2' / target).read_bytes() != Path(source).read_bytes():
                    item['errors'].append('Staged scene fixture differs: ' + target)
        item['evidence_sha256'] = {p.name: sha(p) for p in (run / 'boot.txt', run / 'summary.json', run / 'ps2args')}
        report['runs'][name] = item
        report['errors'] += [name + ': ' + e for e in item['errors']]
    for scene, _, _ in SCENES:
        names = ['base-normal-' + scene, 'final-normal-' + scene]
        base, final = [out / 'runs' / n for n in names]
        before, after = [report['runs'][n] for n in names]
        pair = {'scene': scene, 'complete_PPMs_literal_equal': all((base / n).read_bytes() == (final / n).read_bytes() for n in CAPTURES),
                'captures': len(CAPTURES), 'LHASH_equal': bool(before['lhash']) and before['lhash'] == after['lhash'],
                'complete_saved_configuration_literal_equal': (base / 'reference.cfg').read_bytes() == (final / 'reference.cfg').read_bytes(),
                'normal_RNG_disallows_causal_timing_or_memory_claim': True}
        if not all(pair[k] for k in ('complete_PPMs_literal_equal', 'LHASH_equal', 'complete_saved_configuration_literal_equal')):
            report['errors'].append('Fresh normal smoke pair differs: ' + scene)
        report['pairs'].append(pair)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'runs': len(report['runs']), 'pairs': report['pairs'], 'errors': report['errors']}, indent=2), flush=True)
    return bool(report['errors'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next12/final-unmodified')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--prepare-only', action='store_true')
    mode.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args(); out = args.out.resolve()
    if args.analyze_only:
        return analyze(out)
    prepare(out)
    if args.prepare_only:
        return 0
    readiness = json.loads((out / 'readiness.json').read_text())
    for entry in readiness['cases']:
        if any(sha(Path(p)) != value for p, value in readiness['input_sha256'].items()):
            raise RuntimeError('Frozen normal smoke input changed before run')
        print('START ' + entry['name'], flush=True)
        result = run_verified(entry['command'], out / 'runtime' / entry['name'],
                              [out / 'readiness.json', *map(Path, readiness['input_sha256'])])
        if result.returncode:
            raise RuntimeError('Normal shipping smoke failed: ' + entry['name'])
    return analyze(out)


if __name__ == '__main__':
    raise SystemExit(main())
