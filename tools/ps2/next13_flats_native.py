#!/usr/bin/env python3
"""Prepare/bind actual shipping-LTO flat constructor/get/free native callbacks.

Allocator callbacks expose exact requests and lifetimes. They are not the
shipping ZA/spill allocator and do not establish game cache/LOD residency.
Preparation/binding is read-only; --build needs a granted serial EE worker.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

from next13_flats_variant import ROOT, BASE
from next13_flats_hosttest import section
from audio_storage_next7_finalbind import Object
from next8_gs_codecheck import symbols
from next10_sw_bounds_codecheck import frame
from next7_render_fog_codecheck import Elf32
import next10_gs_low as graph


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


CODE = ('HWR_LoadMapTextures', 'HWR_FreeMapTextures', 'HWR_GetLevelFlat',
        'M_CheckParm', 'M_IsNextParm', 'M_GetNextParm', 'FreeMapTexture')
SPECIAL = {'calloc': 'flat_native_calloc', 'free': 'flat_native_free', 'memset': 'memset',
    'strcasecmp': 'strcasecmp', 'atoi': 'atoi', 'HWR_GCacheFlush': 'flat_native_flush',
    'Z_Free': 'flat_native_zone_free', 'Z_ChangeTag': 'flat_native_zone_tag',
    'PS2HWD_TouchTexture': 'flat_native_touch', 'HWR_GCRecTex': 'flat_native_record',
    'I_Error': 'flat_native_error', 'Z_GuardThrow': 'flat_native_guard'}


def run(command, log):
    import subprocess
    r = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    log.write_text(' '.join(map(str, command)) + '\n' + r.stdout + r.stderr)
    if r.returncode:
        raise RuntimeError('Native fixture build failed: ' + str(log) + '\n' + (r.stdout + r.stderr)[-2500:])


def prepare(work, builds, source):
    work.mkdir(parents=True, exist_ok=False)
    graphs = {}
    for tag, build in builds.items():
        obj = Object(build / 'obj/engine_lto.o')
        graph.CODE = tuple(n for n in CODE if '.text.' + n in obj.sections)
        graph.SPECIAL = SPECIAL
        graphs[tag], _ = graph.bundle(work, build / 'obj/engine_lto.o', tag)
        if any(r['target'] == 'low_unexpected' for node in graphs[tag].values() for r in node['ordered_relocations']):
            raise RuntimeError('A native flat callback boundary is unspecified')
    fields = {'count': 'gl_numtextures', 'walls': 'gl_textures', 'flats': 'gl_flats',
        'loaded': 'gl_maptexturesloaded', 'argc': 'myargc', 'argv': 'myargv',
        'translation': 'texturetranslation', 'textures': 'textures', 'driver': 'hwdriver',
        'batching': 'currently_batching', 'selected': 'current_texture'}
    declarations, values, resets = [], [], []
    for tag, nodes in graphs.items():
        by_name = {graph.canonical(sec.split('.', 2)[-1]): node for sec, node in nodes.items() if node['kind'] == 'data'}
        # Most globals are .sbss.NAME; long input names retain .lto_priv.N.
        for node in nodes.values():
            if node['kind'] == 'data':
                declarations.append('extern unsigned char ' + node['label'] + '[];')
                if node['signature']['flags'] & 1:
                    resets.append('{' + node['label'] + ',' + str(node['bytes']) + '}')
        chosen = []
        for field, wanted in fields.items():
            matches = [node for sec, node in nodes.items() if node['kind'] == 'data' and graph.canonical(sec.split('.', 2)[-1]) == wanted]
            if len(matches) != 1:
                raise RuntimeError('One complete native state object required: ' + tag + '/' + wanted)
            chosen.append(matches[0]['label'])
        values.append('{' + ','.join(chosen) + '}')
    state = '\n'.join(declarations) + '\nstatic struct { unsigned char *' + ',*'.join(fields) + '; } native_state[2]={' + ','.join(values) + '};\n'
    state += 'static struct { unsigned char *p; unsigned bytes; } native_globals[]={' + ','.join(resets) + '};\n'
    (work / 'native-state.h').write_text(state)
    shutil.copyfile(ROOT / 'tools/ps2/next13_flats_native.c', work / 'fixture.c')
    pinned = work / 'pinned-tools'
    pinned.mkdir()
    for name in ('next13_flats_native.py', 'next13_flats_native.c', 'next10_gs_low.py', 'audio_storage_next7_finalbind.py'):
        shutil.copyfile(ROOT / 'tools/ps2' / name, pinned / name)
    (work / 'inputs.json').write_text(json.dumps({'builds': {t: str(p) for t, p in builds.items()}, 'source': str(source),
        'source_sha256': {p: sha(source / p) for p in ('src/hardware/hw_data.h', 'src/hardware/hw_drv.h', 'src/p_setup.h', 'src/r_defs.h')},
        'callback_boundaries': SPECIAL, 'shipping_ZA_spill_cache_LOD_residency_not_measured_by_fixture': True,
        'generated_sha256': {p.name: sha(p) for p in work.iterdir() if p.is_file()},
        'actual_input_frames': {t: {n: frame(Elf32(p / 'obj/engine_lto.o'), n) for n in CODE if '.text.' + n in Object(p / 'obj/engine_lto.o').sections} for t, p in builds.items()}}, indent=2) + '\n')
    print('Prepared actual-LTO flat callback fixture ' + str(work))


def build(work, builds, source):
    import build as B
    flags = [f for f in B.CFLAGS if f not in ('-MMD', '-MP', '-flto')]
    flags += ['-fno-lto', '-Werror', '-DHWRENDER', '-I' + str(work), '-I' + str(source / 'src')]
    objects, report = [], {}
    for tag, old in builds.items():
        src = work / (tag + '-graph.S')
        obj = src.with_suffix('.o')
        run([B.CC, *flags, *B.INCS, '-c', src, '-o', obj], work / (tag + '-graph-compile.log'))
        data = json.loads((work / (tag + '-graph.json')).read_text())
        graph.restore_section_symbol_types(obj, data['nodes'], data['original_GP0'])
        report[tag] = graph.verify_object(old / 'obj/engine_lto.o', obj, data['nodes'])
        objects.append(obj)
    fixture = work / 'fixture.o'
    run([B.CC, *flags, *B.INCS, '-c', work / 'fixture.c', '-o', fixture], work / 'fixture-compile.log')
    objects.append(fixture)
    elf = work / 'FLATS_NEXT13.ELF'
    run([B.CC, *B.LDFLAGS, *objects, '-o', elf, '-Wl,-Map=' + str(work / 'FLATS_NEXT13.map'), '-ldebug', '-lpatches', '-lm'], work / 'link.log')
    (work / 'build-report.json').write_text(json.dumps({'ELF_sha256': sha(elf), 'flags': flags,
        'actual_graph_object_checks': report, 'fixture_sha256': sha(work / 'fixture.c'),
        'only_complete_actual_game_LTO_helpers_execute': True,
        'explicit_callback_allocator_is_not_shipping_ZA_spill': True}, indent=2) + '\n')
    print('Built actual-LTO flat callback fixture ' + str(elf))


def bind(work, builds):
    fixture = Object(work / 'FLATS_NEXT13.ELF')
    fs = symbols(fixture)
    reports = {}
    if sha(work / 'FLATS_NEXT13.ELF') != json.loads((work / 'build-report.json').read_text())['ELF_sha256']:
        raise RuntimeError('Actual fixture output changed')
    for tag, build in builds.items():
        data = json.loads((work / (tag + '-graph.json')).read_text())
        if sha(build / 'obj/engine_lto.o') != data['engine_sha256']:
            raise RuntimeError('Actual shipping-LTO graph changed')
        obj = Object(build / 'obj/engine_lto.o')
        entries = {}
        for sec, node in data['nodes'].items():
            label = node['label']
            if fs[label]['bytes'] != node['body_bytes'] or fs[label]['address'] % max(4, obj.sections[sec][8]):
                raise RuntimeError('Whole actual fixture graph size/alignment changed')
            if obj.sections[sec][1] == 8:
                entries[sec] = {'whole_BSS_capacity': node['bytes'], 'whole_input_shape_preserved': True}
                continue
            expected = graph.relocate(obj.content(obj.sections[sec]), node['ordered_relocations'],
                lambda r: fs[r['target']]['address'] + r['addend'], fs['_gp']['address'], data['original_GP0'])
            if expected != graph.linked_span(fixture, fs, label, node['bytes']):
                raise RuntimeError('Whole actual native graph resolved byte/padding differs: ' + sec)
            entries[sec] = {'whole_input_bytes': node['bytes'], 'whole_padding_included': True,
                'ordered_RELA_records': node['ordered_relocations'], 'resolved_fixture_sha256': hashlib.sha256(expected).hexdigest()}
        reports[tag] = {'engine_sha256': data['engine_sha256'], 'ELF_sha256': sha(build / 'SRB2.ELF'), 'nodes': entries}
    (work / 'native-binding.json').write_text(json.dumps({'fixture_ELF_sha256': sha(work / 'FLATS_NEXT13.ELF'), 'variants': reports,
        'shipping_game_bodies_data_literal_and_frames_bound_separately': str(ROOT / 'build/nov15-next13/flats-binding-v3/report.json')}, indent=2) + '\n')
    print('Bound all actual native flat graph bytes, ordered RELAs, padding and BSS extents.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base', type=Path, default=ROOT / 'build/nov15-next12/final/hardware')
    p.add_argument('--candidate', type=Path, default=ROOT / 'build/nov15-next13/flats-context-v1')
    p.add_argument('--source', type=Path, default=BASE)
    p.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next13/flats-native-v1')
    p.add_argument('--prepare', action='store_true')
    p.add_argument('--build', action='store_true')
    p.add_argument('--bind', action='store_true')
    a = p.parse_args()
    builds = {'base': a.base.resolve(), 'cur': a.candidate.resolve()}
    if a.prepare:
        prepare(a.out.resolve(), builds, a.source.resolve())
    if a.build:
        build(a.out.resolve(), builds, a.source.resolve())
    if a.bind:
        bind(a.out.resolve(), builds)


if __name__ == '__main__':
    main()
