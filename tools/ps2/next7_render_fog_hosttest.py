"""Differential proof and target fixture for complete actual fog/solid drawers.

Actual reference/current functions and lighting helpers are extracted verbatim.
Functions have external globals in a separate TU without LTO for the fixture;
the actual whole-engine LTO ELF is audited separately.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
sys.path.insert(0, str(TOOLS))
from next3_render_tilted_hosttest import function, run
NAMES = ['R_DrawTiltedFogSpan_8', 'R_DrawTransSolidColorSpan_8',
         'R_DrawTiltedSolidColorSpan_8', 'R_DrawTiltedTransSolidColorSpan_8',
         'R_DrawWaterSolidColorSpan_8', 'R_DrawTiltedWaterSolidColorSpan_8',
         'R_DrawFogColumn_8']
BLENDED = [name for name in NAMES if 'Trans' in name or 'Water' in name]


def prepare(work, base):
    work.mkdir(parents=True, exist_ok=True)
    hashes = {}
    for variant, directory in [('reference', base), ('candidate', ROOT / 'src')]:
        source = (directory / 'r_draw8.c').read_text()
        prefix = ''
        if '#define SOLID_TILT_LIGHT' in source:
            prefix = source[source.index('#ifdef PS2_OPT_DRAW\n#define SOLID_TILT_LIGHT'):
                            source.index('void R_DrawTiltedFogSpan_8(void)')]
        bodies = {name: function(source, name) for name in NAMES}
        body = prefix + '\n'.join('__attribute__((noinline, noipa, externally_visible))\n' + v
                                  for v in bodies.values())
        for name in NAMES:
            body = re.sub(r'\b' + name + r'\b', variant + '_' + name, body)
        body += '\n#undef SOLID_TILT_LIGHT\n#undef SOLID_TRANSMAP\n'
        (work / (variant + '.inc')).write_text(body)
        hashes[variant] = hashlib.sha256(source.encode()).hexdigest()
    lighting_source = (base / 'r_draw.c').read_text()
    a = lighting_source.index('static INT32 tiltlighting')
    b = lighting_source.index('#ifdef PS2_OPT_SLOPE\n// (UINT32)', a)
    lighting = lighting_source[a:b].replace('static INT32 tiltlighting', 'INT32 tiltlighting', 1)
    (work / 'lighting.inc').write_text(lighting)
    functions = ('#include "next7_render_fog_stub.h"\n#include "lighting.inc"\n'
                 '#include "reference.inc"\n#include "candidate.inc"\n')
    (work / 'functions.c').write_text(functions)
    candidate = (work / 'candidate.inc').read_text()
    negative = candidate.replace('*dest = colormap[*dest];', '*dest = (UINT8)(1 ^ colormap[*dest]);')
    # This mutation alters actual lookup results in both enabled/disabled policies.
    negative = negative.replace('*dest = dc_colormap[*dest];', '*dest = (UINT8)(1 ^ dc_colormap[*dest]);')
    if negative == candidate:
        raise SystemExit('Actual lookup negative did not mutate')
    variants = {'lookup': negative}
    cursor = candidate
    for name in NAMES:
        if 'Tilted' not in name:
            continue
        old = function(cursor, 'candidate_' + name)
        end = old.rindex('}')
        cursor = cursor.replace(old, old[:end] + '\tds_x1--;\n' + old[end:])
    variants['cursor'] = cursor
    stale = candidate
    for name in BLENDED:
        old = function(stale, 'candidate_' + name)
        new = old.replace('const UINT8 *const solid_transmap = ds_transmap;', '')
        new = new.replace('SOLID_TRANSMAP', 'negative_transmap').replace('ds_transmap', 'negative_transmap')
        first = new.index('{') + 1
        new = new[:first] + ('\n\tUINT8 negative_transmap[65536];\n'
                            '\tmemcpy(negative_transmap, ds_transmap, sizeof negative_transmap);\n') + new[first:]
        stale = stale.replace(old, new)
    variants['stale'] = stale
    for name, body in variants.items():
        (work / ('candidate-negative-' + name + '.inc')).write_text(body)
        (work / ('functions-negative-' + name + '.c')).write_text(functions.replace(
            '"candidate.inc"', '"candidate-negative-' + name + '.inc"'))
    hashes['lighting'] = hashlib.sha256(lighting.encode()).hexdigest()
    (work / 'source-hashes.json').write_text(json.dumps(hashes, indent=2) + '\n')


def host(work, args):
    results = {}
    for policy, extra in [('float', []), ('double', ['-DPS2_NOOPT_SLOPE']),
                          ('noopt-draw', ['-DPS2_NOOPT_DRAW']), ('noopt', ['-DPS2_NOOPT'])]:
        binary = work / policy
        flags = [args.cc, '-O2', '-std=gnu17', '-fwrapv', '-ffp-contract=off', '-DPS2_PROFILE',
                 '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', *extra,
                 '-I' + str(work), '-I' + str(TOOLS), '-I' + str(ROOT / 'src')]
        run([*flags, TOOLS / 'next7_render_fog_hosttest.c', work / 'functions.c', '-o', binary],
            work / (policy + '-build.log'))
        output = run([binary], work / (policy + '-test.log')).stdout
        results[policy] = output.strip()
        print(policy + ': ' + output.strip(), flush=True)
        if args.negative_controls:
            result = run([binary, 'broken-pixel'], work / (policy + '-negative-pixel.log'), False)
            if result.returncode != 1:
                raise SystemExit(policy + ' pixel corruption not detected')
            for negative in ('lookup', 'cursor', 'stale'):
                exe = work / (policy + '-negative-' + negative)
                run([*flags, TOOLS / 'next7_render_fog_hosttest.c', work / ('functions-negative-' + negative + '.c'),
                     '-o', exe], work / (policy + '-negative-' + negative + '-build.log'))
                result = run([exe, 'aliases' if negative == 'stale' else 'negative'],
                             work / (policy + '-negative-' + negative + '.log'), False)
                if result.returncode != 1:
                    raise SystemExit(policy + ' actual ' + negative + ' mutation not detected')
            print(policy + ': detected all four negative controls', flush=True)
    if args.sanitize:
        binary = work / 'asan'
        flags = [args.cc, '-O1', '-g', '-std=gnu17', '-fwrapv', '-ffp-contract=off', '-DPS2_PROFILE',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                 '-I' + str(work), '-I' + str(TOOLS), '-I' + str(ROOT / 'src')]
        run([*flags, TOOLS / 'next7_render_fog_hosttest.c', work / 'functions.c', '-o', binary],
            work / 'asan-build.log')
        result = run([binary], work / 'asan-test.log')
        if 'runtime error:' in result.stderr:
            raise SystemExit('Sanitizer reported undefined behavior')
        results['asan'] = result.stdout.strip()
        print('asan: ' + result.stdout.strip(), flush=True)
    (work / 'host-report.json').write_text(json.dumps(results, indent=2) + '\n')


def ee(work):
    import build as B
    flags = [f for f in B.CFLAGS if not f.startswith('-MMD') and f not in ('-MP', '-flto')]
    objects = []
    for name, source in [('functions', work / 'functions.c'), ('driver', TOOLS / 'next7_render_fog_hosttest.c')]:
        obj = work / (name + '.o')
        run([B.CC, *flags, '-Werror', '-DNEXT7_FOG_EE', '-I' + str(TOOLS), '-I' + str(work),
             *B.INCS, '-c', source, '-o', obj], work / ('ee-' + name + '-build.log'))
        objects.append(obj)
    binary = work / 'FOG.ELF'
    run([B.CC, *B.LDFLAGS, *objects, '-o', binary, '-ldebug', '-lpatches', '-lm'], work / 'ee-link.log')
    print('Built ' + str(binary), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--base-src', type=Path, required=True)
    p.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
    p.add_argument('--negative-controls', action='store_true')
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--ee-only', action='store_true')
    a = p.parse_args()
    work = a.out.resolve()
    prepare(work, a.base_src.resolve())
    if a.ee_only:
        ee(work)
    else:
        host(work, a)


if __name__ == '__main__':
    main()
