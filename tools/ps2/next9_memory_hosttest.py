#!/usr/bin/env python3
"""Actual mipmap metadata, constructors, spill wrappers and zone exactness proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from next8_memory_hosttest import parts as sprite_parts

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
FIELDS = '''format data flags height width downloaded nextcolormap colormap
regen_id regen_kind ps2_twin ps2_h255 ps2_planfr ps2_want ps2_pi ps2_uw ps2_uh
ps2_ihint ps2_ihfr ps2_imfr ps2_vis ps2_full_fr ps2_gcv ps2_spv ps2_nup
ps2_drop ps2_cost'''.split()


def function(source, name):
    match = re.search(r'^[^\n;{}]*\b' + re.escape(name) + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth = 0
    tokens = re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', source[match.start():])
    for token in tokens:
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[match.start():match.start() + token.end()] + '\n'
    raise AssertionError(name)


def section(source, start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


def parts(root):
    definitions = (root / 'src/r_defs.h').read_text()
    data = (root / 'src/hardware/hw_data.h').read_text()
    cache = (root / 'src/hardware/hw_cache.c').read_text()
    patch = (root / 'src/r_patch.c').read_text()
    spill = (root / 'src/ps2/ps2_spill.c').read_text()
    plan = (root / 'src/ps2/hw/ps2_hw_plan.inc').read_text()
    tex = (root / 'src/ps2/hw/ps2_hw_tex.inc').read_text()
    types = section(definitions, '// posts are runs of non masked source pixels', '//\n// OTHER TYPES')
    types += section(definitions, '#ifdef ROTSPRITE\ntypedef struct\n{\n\tINT32 angles;', '#if defined(_MSC_VER)')
    types += data[data.index('typedef enum GLTextureFormat_e'):].rsplit('\n#endif', 1)[0]
    init = ''.join(function(cache, name) for name in ('HWR_InitMapTextures', 'DeleteTextureMipmap', 'FreeMapTexture',
                                                     'HWR_FreeMapTextures', 'HWR_LoadMapTextures'))
    texture = function(cache, 'HWR_GetTexture')
    flat = ''.join(function(cache, name) for name in ('MakeLevelFlatMipmap', 'HWR_GetLevelFlat', 'HWR_PS2_RegenerateMipmap'))
    patching = ''.join(function(patch, name) for name in ('Patch_AllocateHardwarePatch', 'Patch_CreateGL'))
    patching += ''.join(function(cache, name) for name in ('HWR_LoadPatchMipmap', 'HWR_UpdatePatchMipmap',
                                                          'HWR_GetPatch', 'HWR_GetMappedPatch'))
    freeing = ''.join(function(cache, name) for name in ('HWR_FreeTextureData', 'HWR_FreeTextureColormaps', 'HWR_FreeTexture'))
    driver = ''.join(function(plan, name) for name in ('plan_wants', 'plan_key', 'tex_want'))
    driver += function(tex, 'dc_key')
    return {'MEMORY_ZONE': sprite_parts(root)['MEMORY_ZONE'], 'MEMORY_TYPES': types,
            'MEMORY_SPILL': section(spill, 'void *__real__malloc_r', '\n#else\nvoid PS2Spill_Init'),
            'MEMORY_INIT': init, 'MEMORY_TEXTURE': texture, 'MEMORY_FLAT': flat,
            'MEMORY_PATCH': patching, 'MEMORY_FREE': freeing,
            'MEMORY_STEAL': function(cache, 'HWR_PS2_StealData'), 'MEMORY_DRIVER': driver}


def run(command, log, negative=False):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=240)
    log.write_text(result.stdout + result.stderr)
    if not negative:
        assert result.returncode == 0, str(log) + ': ' + result.stdout[-1000:] + result.stderr[-4000:]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next9/baseline-src')
    parser.add_argument('--candidate', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next9/memory/host')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    interfaces = {}
    for label, root in [('baseline', args.baseline.resolve()), ('current', args.candidate.resolve())]:
        source = '#include "doomdef.h"\n#include "hardware/hw_data.h"\n'
        result = subprocess.run(['cc', '-E', '-P', '-DNOHW', '-I' + str(root / 'src'),
                                 '-I' + str(ROOT / 'build/host-gen'), '-xc', '-'],
                                input=source, text=True, capture_output=True)
        (args.out / (label + '-nonps2.i')).write_text(result.stdout)
        (args.out / (label + '-nonps2.log')).write_text(result.stderr)
        assert result.returncode == 0, result.stderr
        interfaces[label] = result.stdout
    assert interfaces['baseline'] == interfaces['current'], 'non-PS2 header interface differs'
    variants = [('baseline', args.baseline.resolve(), []), ('current', args.candidate.resolve(), []),
                ('old-switch', args.candidate.resolve(), ['-DPS2_NOOPT_MIPMAP_LAYOUT'])]
    if args.sanitize:
        variants += [('asan', args.candidate.resolve(), ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'])]
    if args.negative_controls:
        variants += [(name, args.candidate.resolve(), []) for name in
                     ('negative-byte', 'negative-frame', 'negative-twin', 'negative-owner', 'negative-copy')]
    reports, snapshots, sources = {}, {}, {}
    mutations = {
        'negative-byte': ('MEMORY_TYPES', 'UINT8                 ps2_cost;', 'UINT8                 ps2_cost : 7;'),
        'negative-frame': ('MEMORY_DRIVER', 'k->ps2_planfr = H.frame_no + 1;', 'k->ps2_planfr = H.frame_no;'),
        'negative-twin': ('MEMORY_TEXTURE', 'newMipmap->ps2_twin = originalMipmap;', 'newMipmap->ps2_twin = NULL;'),
        'negative-owner': ('MEMORY_STEAL', 'Z_SetUser(p, newuser);', '(void)newuser;'),
        'negative-copy': ('FIXTURE', 'memcpy(copy, values, count * sizeof *copy);', 'memcpy(copy, values, (count ? count - 1 : 0) * sizeof *copy);'),
    }
    for label, root, flags in variants:
        work = args.out / label
        work.mkdir(exist_ok=True)
        sourceparts = parts(root)
        fixture = (TOOLS / 'next9_memory_hosttest.c').read_text()
        if label in mutations:
            key, old, new = mutations[label]
            if key == 'FIXTURE':
                assert old in fixture
                fixture = fixture.replace(old, new)
            else:
                assert old in sourceparts[key], label
                sourceparts[key] = sourceparts[key].replace(old, new)
        defines, hashes = [], {}
        for macro, content in sourceparts.items():
            path = work / (macro.lower() + '.inc')
            path.write_text(content)
            defines.append('-D' + macro + '="' + str(path) + '"')
            hashes[macro] = hashlib.sha256(content.encode()).hexdigest()
        source = work / 'fixture.c'
        source.write_text(fixture)
        binary = work / 'fixture'
        result = run(['cc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                      '-Wno-unused-variable', '-Wno-sign-compare', '-Wno-clobbered', '-DPARANOIA',
                      '-I' + str(TOOLS), '-I' + str(ROOT / 'src'), *flags, *defines, source, '-o', binary],
                     work / 'build.log')
        result = run([binary, work / 'snapshot.bin'], work / 'run.log', negative=label in mutations)
        if label in mutations:
            different = result.returncode != 0 or (work / 'snapshot.bin').read_bytes() != snapshots['baseline']
            assert different, label + ' mutation escaped'
            reports[label] = {'mutation_detected': True, 'exit_code': result.returncode}
            continue
        match = re.search(r'MEMORY9 DONE cases=(\d+) checks=(\d+) sizeof=(\d+) map=(\d+) align=(\d+)', result.stdout)
        assert match, result.stdout[-2000:]
        reports[label] = dict(zip(('cases', 'checks', 'sizeof', 'map', 'align'), map(int, match.groups())))
        reports[label]['rows'] = [dict(re.findall(r'(\w+)=([\da-f]+)', line)) for line in result.stdout.splitlines()
                                  if 'MEMORY9 case=' in line]
        snapshots[label] = (work / 'snapshot.bin').read_bytes()
        sources[label] = hashes
        print(label, {k: v for k, v in reports[label].items() if k != 'rows'}, flush=True)
    assert snapshots['baseline'] == snapshots['current'] == snapshots['old-switch'], 'literal field/driver snapshots differ'
    if args.sanitize:
        assert snapshots['asan'] == snapshots['baseline']
    assert reports['baseline'] == reports['old-switch'], 'old-switch allocator or field rows differ'
    # Pure typed-table cases have the same allocation policy and no moving growth.
    # Real calloc/spill cases may cross the production 24 KiB threshold, so their
    # zone/libc occupancy is reported without claiming a universal peak ordering.
    for before, after in zip(reports['baseline']['rows'], reports['current']['rows']):
        assert before['case'] == after['case'] and before['kind'] == after['kind']
        if before['kind'] == '0':
            for key in ('request', 'used', 'peak'):
                assert int(after[key]) <= int(before[key]), (before, after)
    report = {'results': reports, 'source_sha256': sources,
              'fixture_sha256': hashlib.sha256((TOOLS / 'next9_memory_hosttest.c').read_bytes()).hexdigest(),
              'literal_compared_bytes': len(snapshots['baseline']),
              'snapshot_sha256': hashlib.sha256(snapshots['baseline']).hexdigest(),
              'nonps2_preprocessed_interface_equal': True, 'old_switch_all_rows_equal': True,
              'all_literal_snapshots_equal': True,
              'scope': 'Actual types, map/patch/colormap constructors, teardown, selected driver state helpers, real spill wrappers and zone; GPU/resource conversion and libc service boundaries are controlled; full game and EE32 checks separate.'}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Literal snapshot bytes:', report['literal_compared_bytes'], report['snapshot_sha256'], flush=True)


if __name__ == '__main__':
    main()
