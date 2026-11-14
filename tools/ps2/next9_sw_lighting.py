"""Investigate exact slope-light clamp scheduling without editing production.

Complete helpers come from the frozen source. Variants only change the integer
clamp after its existing accumulation/shift. Functions and driver are separate
non-LTO TUs; the baseline helper must match actual default engine LTO code.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
sys.path.insert(0, str(TOOLS))
from next7_render_fog_hosttest import function, run

VARIANTS = {
    'reference': None,
    'ternary': '''\tfor (i = left; i <= right; i++) {
\t\tINT32 light = (start += step) >> FRACBITS;
\t\tif (light < 0)
\t\t\tlight = 0;
\t\telse
\t\t\tlight = light < MAXLIGHTSCALE ? light : MAXLIGHTSCALE-1;
\t\ttiltlighting[i] = light;
\t}''',
    'unsigned': '''\tfor (i = left; i <= right; i++) {
\t\tINT32 light = (start += step) >> FRACBITS;
\t\tif ((UINT32)light >= MAXLIGHTSCALE)
\t\t\tlight = light < 0 ? 0 : MAXLIGHTSCALE-1;
\t\ttiltlighting[i] = light;
\t}''',
    'uppermove': '''\tfor (i = left; i <= right; i++) {
\t\tINT32 light = (start += step) >> FRACBITS;
\t\tif (light < 0)
\t\t\ttiltlighting[i] = 0;
\t\telse
\t\t{
#if defined(PS2_OPT_DRAW) && defined(__mips__)
\t\t\tINT32 above;
\t\t\tconst INT32 high = MAXLIGHTSCALE-1;
\t\t\t__asm__("slt %0,%2,%1\\n\\tmovn %1,%2,%0" : "=&r"(above), "+r"(light) : "r"(high));
#else
\t\t\tif (light >= MAXLIGHTSCALE)
\t\t\t\tlight = MAXLIGHTSCALE-1;
#endif
\t\t\ttiltlighting[i] = light;
\t\t}
\t}''',
    'countbranch': '''\t(void)i;
\tif (left <= right)
\t{
\t\tINT32 remaining = right-left+1;
\t\tINT32 *dest = &tiltlighting[left];
\t\tdo {
\t\t\t*dest = (start += step) >> FRACBITS;
\t\t\tif (*dest < 0)
\t\t\t\t*dest = 0;
\t\t\telse if (*dest >= MAXLIGHTSCALE)
\t\t\t\t*dest = MAXLIGHTSCALE-1;
\t\t\tdest++;
\t\t} while (--remaining);
\t}''',
    'countmmi': '''\t(void)i;
\tif (left <= right)
\t{
\t\tINT32 remaining = right-left+1;
\t\tINT32 *dest = &tiltlighting[left];
\t\tdo {
\t\t\tINT32 light = (start += step) >> FRACBITS;
#if defined(PS2_OPT_DRAW) && defined(__mips__)
\t\t\tconst INT32 high = MAXLIGHTSCALE-1;
\t\t\t__asm__("pmaxw %0,%0,$0\\n\\tpminw %0,%0,%1" : "+r"(light) : "r"(high));
#else
\t\t\tif (light < 0) light = 0;
\t\t\telse if (light >= MAXLIGHTSCALE) light = MAXLIGHTSCALE-1;
#endif
\t\t\t*dest++ = light;
\t\t} while (--remaining);
\t}''',
    'countupper': '''\t(void)i;
\tif (left <= right)
\t{
\t\tINT32 remaining = right-left+1;
\t\tINT32 *dest = &tiltlighting[left];
\t\tdo {
\t\t\tINT32 light = (start += step) >> FRACBITS;
\t\t\tif (light < 0)
\t\t\t\t*dest = 0;
\t\t\telse
\t\t\t{
#if defined(PS2_OPT_DRAW) && defined(__mips__)
\t\t\t\tINT32 above;
\t\t\t\tconst INT32 high = MAXLIGHTSCALE-1;
\t\t\t\t__asm__("slt %0,%2,%1\\n\\tmovn %1,%2,%0" : "=&r"(above), "+r"(light) : "r"(high));
#else
\t\t\t\tif (light >= MAXLIGHTSCALE) light = MAXLIGHTSCALE-1;
#endif
\t\t\t\t*dest = light;
\t\t\t}
\t\t\tdest++;
\t\t} while (--remaining);
\t}''',
    'countbarrier': '''\t(void)i;
\tif (left <= right)
\t{
\t\tINT32 remaining = right-left+1;
\t\tINT32 *dest = &tiltlighting[left];
\t\tdo {
\t\t\tINT32 light = (start += step) >> FRACBITS;
\t\t\tif (light < 0)
\t\t\t\t*dest = 0;
\t\t\telse
\t\t\t{
\t\t\t\t__asm__ volatile("" : "+r"(light));
\t\t\t\t*dest = light < MAXLIGHTSCALE ? light : MAXLIGHTSCALE-1;
\t\t\t}
\t\t\tdest++;
\t\t} while (--remaining);
\t}''',
    'production': None,
}


def prepare(out, base, selected):
    out.mkdir(parents=True, exist_ok=True)
    source = (base / 'r_draw.c').read_text()
    original = function(source, 'R_CalcTiltedLighting')
    slope = function(source, 'R_CalcSlopeLight')
    first = original.index('\tfor (i = left;')
    last = original.rindex('\n}')
    manifest = {'source': str(base / 'r_draw.c'), 'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
                'selected': selected, 'variants': {}}
    chunks = ['#include "next9_sw_lighting.h"\nINT32 tiltlighting[MAXVIDWIDTH];\n',
              '#ifdef PS2_OPT_SLOPE\n#define PLANELIGHTFLOAT ds_lightscale\n#else\n',
              '#define PLANELIGHTFLOAT (BASEVIDWIDTH * BASEVIDWIDTH / vid.width / zeroheight / 21.0f * FIXED_TO_FLOAT(fovtan))\n#endif\n']
    for variant, loop in VARIANTS.items():
        integer = original if loop is None else original[:first] + '#ifdef PS2_OPT_DRAW\n' + loop + '\n#else\n' + original[first:last] + '\n#endif\n' + original[last:]
        if variant == 'production':
            production = (ROOT / 'src/r_draw.c').read_text()
            integer = function(production, 'R_CalcTiltedLighting')
            if function(production, 'R_CalcSlopeLight') != slope:
                raise SystemExit('Production floating helper source changed')
            manifest['production_source_sha256'] = hashlib.sha256(production.encode()).hexdigest()
        private = integer.replace('R_CalcTiltedLighting', variant + '_private')
        integer = integer.replace('static void R_CalcTiltedLighting', '__attribute__((noinline, noipa, externally_visible))\nvoid ' + variant + '_integer')
        floating = slope.replace('R_CalcTiltedLighting', variant + '_private')
        floating = floating.replace('static void R_CalcSlopeLight', '__attribute__((noinline, noipa, externally_visible))\nvoid ' + variant + '_R_CalcSlopeLight')
        text = private + '\n' + integer + '\n' + floating
        (out / (variant + '.inc')).write_text(text)
        if variant in selected:
            chunks.append('#include "' + variant + '.inc"\n')
        manifest['variants'][variant] = hashlib.sha256(text.encode()).hexdigest()
    (out / 'functions.c').write_text(''.join(chunks))
    production = (out / 'production.inc').read_text()
    negatives = {
        'shift': production.replace('>> FRACBITS', '>> (FRACBITS-1)'),
        'upper': production.replace('MAXLIGHTSCALE-1', 'MAXLIGHTSCALE-2'),
        'count': production.replace('remaining = right-left+1', 'remaining = right-left+2').replace('i <= right', 'i < right'),
    }
    for name, text in negatives.items():
        if text == production:
            raise SystemExit('Missing actual production mutation: ' + name)
        (out / ('production-negative-' + name + '.inc')).write_text(text)
        (out / ('functions-negative-' + name + '.c')).write_text(''.join(chunks).replace('"production.inc"', '"production-negative-' + name + '.inc"'))
    stub = (TOOLS / 'next7_render_fog_stub.h').read_text()
    stub = stub[:stub.index('#define NEXT7_DECLARE')]
    for variant in VARIANTS:
        stub += 'void ' + variant + '_integer(fixed_t start, fixed_t end);\n'
        stub += 'void ' + variant + '_R_CalcSlopeLight(void);\n'
    stub += '#endif\n'
    (out / 'next9_sw_lighting.h').write_text(stub)
    driver = (TOOLS / 'next9_sw_lighting.c').read_text()
    driver = driver.replace('NEXT9_LIGHT_VARIANTS', ','.join('VARIANT(' + name + ')' for name in selected))
    (out / 'driver.c').write_text(driver)
    for name in ('functions.c', 'driver.c', 'next9_sw_lighting.h'):
        manifest[name] = hashlib.sha256((out / name).read_bytes()).hexdigest()
    (out / 'source-hashes.json').write_text(json.dumps(manifest, indent=2) + '\n')


def host(out, sanitize, negative_controls):
    results = {}
    for policy, extra in [('float', []), ('double', ['-DPS2_NOOPT_SLOPE']),
                          ('noopt-draw', ['-DPS2_NOOPT_DRAW']), ('noopt', ['-DPS2_NOOPT'])]:
        binary = out / policy
        flags = ['gcc', '-O2', '-std=gnu17', '-fwrapv', '-ffp-contract=off', '-DPS2_PROFILE', '-Wall', '-Wextra', '-Werror',
                 *extra, '-I' + str(out), '-I' + str(ROOT / 'src')]
        run([*flags, out / 'driver.c', out / 'functions.c', '-o', binary], out / (policy + '-build.log'))
        result = run([binary], out / (policy + '-test.log'))
        results[policy] = result.stdout.strip()
        print(policy + ': ' + result.stdout.strip(), flush=True)
        if negative_controls:
            for name in ('shift', 'upper', 'count'):
                exe = out / (policy + '-negative-' + name)
                run([*flags, out / 'driver.c', out / ('functions-negative-' + name + '.c'), '-o', exe], out / (policy + '-negative-' + name + '-build.log'))
                result = run([exe], out / (policy + '-negative-' + name + '.log'), False)
                if result.returncode != 1:
                    raise SystemExit('Actual production negative was not detected: ' + policy + ':' + name)
            print(policy + ': all three actual helper mutations detected', flush=True)
    if sanitize:
        binary = out / 'asan'
        flags = ['gcc', '-O1', '-g', '-std=gnu17', '-fwrapv', '-ffp-contract=off', '-DPS2_PROFILE',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                 '-I' + str(out), '-I' + str(ROOT / 'src')]
        run([*flags, out / 'driver.c', out / 'functions.c', '-o', binary], out / 'asan-build.log')
        result = run([binary], out / 'asan-test.log')
        if 'runtime error:' in result.stderr:
            raise SystemExit('Sanitizer reported undefined behavior')
        results['asan'] = result.stdout.strip()
        print('asan: ' + result.stdout.strip(), flush=True)
    (out / 'host-report.json').write_text(json.dumps(results, indent=2) + '\n')


def ee(out):
    import build as B
    flags = [f for f in B.CFLAGS if not f.startswith('-MMD') and f not in ('-MP', '-flto')]
    objects = []
    for name in ('functions', 'driver'):
        obj = out / (name + '.o')
        run([B.CC, *flags, '-Werror', '-DNEXT9_LIGHT_EE', '-I' + str(out), *B.INCS,
             '-c', out / (name + '.c'), '-o', obj], out / ('ee-' + name + '-build.log'))
        objects.append(obj)
    binary = out / 'LIGHT.ELF'
    run([B.CC, *B.LDFLAGS, *objects, '-o', binary, '-ldebug', '-lpatches', '-lm'], out / 'ee-link.log')
    print('Built ' + str(binary), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--base-src', required=True, type=Path)
    p.add_argument('--ee-only', action='store_true')
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--prepare-only', action='store_true')
    p.add_argument('--negative-controls', action='store_true')
    p.add_argument('--variants', default='reference,production')
    a = p.parse_args(); out = a.out.resolve()
    selected = a.variants.split(',')
    if not selected or selected[0] != 'reference' or len(selected) != len(set(selected)) or any(name not in VARIANTS for name in selected):
        raise SystemExit('Variants must be unique supported names, beginning with reference')
    prepare(out, a.base_src.resolve(), selected)
    if a.prepare_only:
        return
    if a.ee_only:
        ee(out)
    else:
        if a.negative_controls and 'production' not in selected:
            raise SystemExit('Source negatives require selected production variant')
        host(out, a.sanitize, a.negative_controls)


if __name__ == '__main__':
    main()
