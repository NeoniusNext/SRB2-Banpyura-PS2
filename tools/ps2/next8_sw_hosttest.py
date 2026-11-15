"""Extract and exercise complete actual affine NPOT blended drawers.

Framebuffer pointers never alias renderer global variable storage. Source,
translation, colormap, blend and background contents may alias framebuffer bytes.
The target fixture uses exact real global declarations and viddef_t/screens ABI;
complete LTO instruction/RELA identity is a separate mandatory timing gate.
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
from next3_render_tilted_hosttest import run
NAMES = ['R_DrawTranslucentSpan_NPO2_8', 'R_DrawTranslucentSplat_NPO2_8',
         'R_DrawTranslucentFloorSprite_NPO2_8', 'R_DrawWaterSpan_NPO2_8']


def function(source, name):
    start = re.search(r'(?:static )?void ' + name + r'\s*\(', source).start()
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', lambda m: ' ' * len(m[0]), source, flags=re.S)
    begin = masked.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return source[start:end] + '\n'


def prepare(work, base):
    work.mkdir(parents=True, exist_ok=True)
    hashes = {}
    (work / 'driver.c').write_bytes((TOOLS / 'next8_sw_hosttest.c').read_bytes())
    (work / 'next8_sw_stub.h').write_bytes((TOOLS / 'next8_sw_stub.h').read_bytes())
    for variant, directory in [('reference', base), ('candidate', ROOT / 'src')]:
        source = (directory / 'r_draw8_npo2.c').read_text()
        prefix = ''
        if '#define AFFINE_NPO2_TRANSMAP' in source:
            a = source.index('#ifdef PS2_OPT_DRAW\n#define AFFINE_NPO2_TRANSMAP')
            prefix = source[a:source.index('void R_DrawTranslucentSplat_NPO2_8', a)]
        body = prefix + '\n'.join('__attribute__((noinline, noipa, externally_visible))\n' + function(source, name) for name in NAMES)
        for name in NAMES:
            body = re.sub(r'\b' + name + r'\b', variant + '_' + name, body)
        body += '\n#undef AFFINE_NPO2_TRANSMAP\n'
        (work / (variant + '.inc')).write_text(body)
        hashes[variant] = {'file': str(directory / 'r_draw8_npo2.c'), 'sha256': hashlib.sha256(source.encode()).hexdigest(),
                           'extracted_sha256': hashlib.sha256(body.encode()).hexdigest()}
    functions = '#include "next8_sw_stub.h"\n#include "reference.inc"\n#include "candidate.inc"\n'
    (work / 'functions.c').write_text(functions)
    candidate = (work / 'candidate.inc').read_text()
    # Alter a real source sample, keeping all real wrapping and drawing code.
    lookup = re.sub(r'\bsource\[([^\[\]]+)\]', r'(1 ^ source[\1])', candidate)
    if lookup == candidate:
        raise SystemExit('Missing actual source-lookup mutation')
    stale = candidate
    for name in NAMES:
        old = function(stale, 'candidate_' + name)
        new = old.replace('const UINT8 *const affine_transmap = ds_transmap;', '').replace('const UINT8 *const transmap = ds_transmap;', '')
        new = new.replace('AFFINE_NPO2_TRANSMAP', 'negative_transmap').replace('ds_transmap +', 'negative_transmap +').replace('*(transmap +', '*(negative_transmap +')
        at = new.index('{') + 1
        new = new[:at] + '\n\tUINT8 negative_transmap[65536];\n\tmemcpy(negative_transmap, ds_transmap, sizeof negative_transmap);\n' + new[at:]
        stale = stale.replace(old, new)
    for name, body in [('lookup', lookup), ('stale', stale)]:
        (work / ('candidate-negative-' + name + '.inc')).write_text(body)
        (work / ('functions-negative-' + name + '.c')).write_text(functions.replace('"candidate.inc"', '"candidate-negative-' + name + '.inc"'))
    hashes['driver_sha256'] = hashlib.sha256((TOOLS / 'next8_sw_hosttest.c').read_bytes()).hexdigest()
    hashes['stub_sha256'] = hashlib.sha256((TOOLS / 'next8_sw_stub.h').read_bytes()).hexdigest()
    (work / 'source-hashes.json').write_text(json.dumps(hashes, indent=2) + '\n')


def host(work, args):
    results = {}
    policies = [('profile', []), ('noopt-draw', ['-DPS2_NOOPT_DRAW']), ('noopt', ['-DPS2_NOOPT'])]
    for policy, extra in policies:
        binary = work / policy
        flags = [args.cc, '-O2', '-std=gnu17', '-fwrapv', '-DPS2_PROFILE', '-Wall', '-Wextra', '-Werror', *extra,
                 '-I' + str(work), '-I' + str(TOOLS), '-I' + str(ROOT / 'src')]
        run([*flags, work / 'driver.c', work / 'functions.c', '-o', binary], work / (policy + '-build.log'))
        output = run([binary], work / (policy + '-test.log')).stdout
        results[policy] = output.strip()
        print(policy + ': ' + output.strip(), flush=True)
        if args.negative_controls:
            result = run([binary, 'broken-pixel'], work / (policy + '-negative-pixel.log'), False)
            if result.returncode != 1:
                raise SystemExit(policy + ' missed pixel negative')
            for negative in ('lookup', 'stale'):
                exe = work / (policy + '-negative-' + negative)
                run([*flags, work / 'driver.c', work / ('functions-negative-' + negative + '.c'), '-o', exe],
                    work / (policy + '-negative-' + negative + '-build.log'))
                result = run([exe, 'aliases' if negative == 'stale' else 'negative'], work / (policy + '-negative-' + negative + '.log'), False)
                if result.returncode != 1:
                    raise SystemExit(policy + ' missed actual ' + negative + ' negative')
            print(policy + ': all three executed negatives detected', flush=True)
    if args.sanitize:
        binary = work / 'asan'
        flags = [args.cc, '-O1', '-g', '-std=gnu17', '-fwrapv', '-DPS2_PROFILE', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-I' + str(work), '-I' + str(TOOLS), '-I' + str(ROOT / 'src')]
        run([*flags, work / 'driver.c', work / 'functions.c', '-o', binary], work / 'asan-build.log')
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
    for name, source in [('functions', work / 'functions.c'), ('driver', work / 'driver.c')]:
        obj = work / (name + '.o')
        run([B.CC, *flags, '-Werror', '-DNEXT8_SW_EE', '-I' + str(TOOLS), '-I' + str(work), *B.INCS, '-c', source, '-o', obj],
            work / ('ee-' + name + '-build.log'))
        objects.append(obj)
    binary = work / 'AFFINE.ELF'
    run([B.CC, *B.LDFLAGS, *objects, '-o', binary, '-ldebug', '-lpatches', '-lm'], work / 'ee-link.log')
    print('Built ' + str(binary), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--base-src', required=True, type=Path)
    p.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
    p.add_argument('--negative-controls', action='store_true')
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--ee-only', action='store_true')
    p.add_argument('--prepare-only', action='store_true')
    a = p.parse_args(); work = a.out.resolve()
    prepare(work, a.base_src.resolve())
    if a.prepare_only:
        return
    if a.ee_only:
        ee(work)
    else:
        host(work, a)


if __name__ == '__main__':
    main()
