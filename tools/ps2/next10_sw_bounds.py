"""Extract complete original/current R_PlaneBounds and exact exploration variants.

The real r_plane.h supplies visplane_t in a separate functions translation unit.
EE compilation and execution require the coordinator's global worker slot.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
sys.path.insert(0, str(TOOLS))
from next3_render_tilted_hosttest import function, run


def variants(original):
    old = '''\t\tif (plane->top[i] < hi)
\t\thi = plane->top[i];
\t\tif (plane->bottom[i] > low)
\t\tlow = plane->bottom[i];'''
    new = '''#if defined(PS2_OPT_REND) && defined(__mips__)
\t\tconst INT32 top = plane->top[i], bottom = plane->bottom[i];
\t\t__asm__("pminw %0,%0,%2\\n\\tpmaxw %1,%1,%3"
\t\t\t: "+&r"(hi), "+&r"(low) : "r"(top), "r"(bottom) : "$12", "$13");
#else
''' + old + '''
#endif'''
    if old not in original:
        raise SystemExit('Original complete min/max loop not found')
    mmi = original.replace(old, new)
    count = '''void R_PlaneBounds(visplane_t *plane)
{
\tINT32 hi = plane->top[plane->minx];
\tINT32 low = plane->bottom[plane->minx];
#ifdef PS2_OPT_REND
\tINT32 remaining = plane->maxx-plane->minx;
\tif (remaining > 0)
\t{
\t\tconst UINT16 *top = plane->top + plane->minx + 1;
\t\tconst UINT16 *bottom = plane->bottom + plane->minx + 1;
\t\tdo
\t\t{
\t\t\tconst INT32 t = *top++, b = *bottom++;
COUNT_UPDATE
\t\t} while (--remaining);
\t}
#else
\tINT32 i;
\tfor (i = plane->minx + 1; i <= plane->maxx; i++)
\t{
''' + old + '''
\t}
#endif
\tplane->high = hi;
\tplane->low = low;
}
'''
    scalar_update = '\t\t\tif (t < hi) hi = t;\n\t\t\tif (b > low) low = b;'
    mmi_update = '''#if defined(__mips__)
\t\t\t__asm__("pminw %0,%0,%2\\n\\tpmaxw %1,%1,%3"
\t\t\t\t: "+&r"(hi), "+&r"(low) : "r"(t), "r"(b));
#else
''' + scalar_update + '\n#endif'
    return {'reference': original, 'mmi': mmi,
            'count': count.replace('COUNT_UPDATE', scalar_update),
            'countmmi': count.replace('COUNT_UPDATE', mmi_update),
            'production': function((ROOT / 'src/r_plane.c').read_text(), 'R_PlaneBounds')}


def prepare(out, base, selected, cold=False):
    out.mkdir(parents=True, exist_ok=True)
    source = (base / 'r_plane.c').read_text()
    texts = variants(function(source, 'R_PlaneBounds'))
    manifest = {'baseline_source': str(base / 'r_plane.c'),
                'baseline_source_sha256': hashlib.sha256(source.encode()).hexdigest(),
                'production_source_sha256': hashlib.sha256((ROOT / 'src/r_plane.c').read_bytes()).hexdigest(),
                'selected': selected, 'cold': cold, 'variants': {}}
    manifest['current_header_snapshot']={}
    for path in sorted((ROOT/'src').rglob('*.h')):
        relative=path.relative_to(ROOT/'src');target=out/'frozen-src'/relative
        target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
        manifest['current_header_snapshot'][str(relative)]=hashlib.sha256(target.read_bytes()).hexdigest()
    chunks = ['#include "r_plane.h"\n']
    for name, body in texts.items():
        body = '__attribute__((noinline, noipa, externally_visible))\n' + body.replace(
            'void R_PlaneBounds(', 'void ' + name + '_R_PlaneBounds(')
        (out / (name + '.inc')).write_text(body)
        manifest['variants'][name] = hashlib.sha256(body.encode()).hexdigest()
        if name in selected:
            chunks.append('#include "' + name + '.inc"\n')
    (out / 'functions.c').write_text(''.join(chunks))
    production=(out/'production.inc').read_text()
    negatives={
        'extent':production.replace('i <= plane->maxx','i < plane->maxx'),
        'sample':production.replace('plane->top[i]','plane->bottom[i]'),
        'output':production.replace('plane->high = hi;', 'plane->high = hi ^ 1;'),
        'mmi':production.replace('pminw %0,%0,%2', 'pmaxw %0,%0,%2'),
    }
    for name,text in (negatives.items() if 'production' in selected else []):
        if text==production:raise SystemExit('Actual source mutation missing '+name)
        (out/('production-negative-'+name+'.inc')).write_text(text)
        (out/('functions-negative-'+name+'.c')).write_text(''.join(chunks).replace(
            '"production.inc"','"production-negative-'+name+'.inc"'))
    driver = (TOOLS / 'next10_sw_bounds.c').read_text()
    if cold:
        driver = '#define NEXT10_BOUNDS_COLD 1\n' + driver
    driver = driver.replace('NEXT10_BOUNDS_DECLARE', '\n'.join(
        'void ' + name + '_R_PlaneBounds(visplane_t *);' for name in selected))
    driver = driver.replace('NEXT10_BOUNDS_VARIANTS', ','.join('VARIANT(' + name + ')' for name in selected))
    (out / 'driver.c').write_text(driver)
    for name in ('functions.c', 'driver.c'):
        manifest[name] = hashlib.sha256((out / name).read_bytes()).hexdigest()
    (out / 'source-hashes.json').write_text(json.dumps(manifest, indent=2) + '\n')


def host(out, sanitize, negative_controls):
    results = {}
    for policy, extra in [('profile', []), ('noopt-bounds', ['-DPS2_NOOPT_PLANEBOUNDS_MMI']),
                          ('noopt-rend', ['-DPS2_NOOPT_REND']),
                          ('noopt', ['-DPS2_NOOPT'])]:
        flags = ['gcc', '-O2', '-std=gnu17', '-fwrapv', '-DPS2_PROFILE', '-Wall', '-Wextra', '-Werror',
                 *extra, '-I' + str(out/'frozen-src'), '-I' + str(ROOT / 'src')]
        exe = out / policy
        run([*flags, out / 'driver.c', out / 'functions.c', '-o', exe], out / (policy + '-build.log'))
        result = run([exe], out / (policy + '-test.log'))
        results[policy] = result.stdout.strip()
        print(policy + ': ' + result.stdout.strip(), flush=True)
        if negative_controls:
            for negative in ('extent','sample','output'):
                exe=out/(policy+'-negative-'+negative)
                run([*flags,out/'driver.c',out/('functions-negative-'+negative+'.c'),'-o',exe],
                    out/(policy+'-negative-'+negative+'-build.log'))
                result=run([exe],out/(policy+'-negative-'+negative+'.log'),False)
                if result.returncode!=1 or 'BOUNDS FAIL' not in result.stdout:
                    raise SystemExit('Actual source mutation not detected '+policy+':'+negative)
            print(policy+': all three actual source mutations detected',flush=True)
    if sanitize:
        exe = out / 'asan'
        flags = ['gcc', '-O1', '-g', '-std=gnu17', '-fwrapv', '-DPS2_PROFILE', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-I' + str(ROOT / 'src')]
        flags.insert(-1,'-I'+str(out/'frozen-src'))
        run([*flags, out / 'driver.c', out / 'functions.c', '-o', exe], out / 'asan-build.log')
        result = run([exe], out / 'asan-test.log')
        if 'runtime error:' in result.stderr:
            raise SystemExit('Sanitizer reported undefined behavior')
        results['asan'] = result.stdout.strip()
        print('asan: ' + result.stdout.strip(), flush=True)
    (out / 'host-report.json').write_text(json.dumps(results, indent=2) + '\n')


def ee(out, negative=False):
    import build as B
    flags = [f for f in B.CFLAGS if not f.startswith('-MMD') and f not in ('-MP', '-flto')]
    objects = []
    for name in ('functions', 'driver'):
        obj = out / (name + '.o')
        source=out/('functions-negative-mmi.c' if negative and name=='functions' else name+'.c')
        run([B.CC, *flags, '-Werror', '-DNEXT10_BOUNDS_EE', '-I'+str(out/'frozen-src'), *B.INCS,
             '-c', source, '-o', obj], out / ('ee-' + name + '-build.log'))
        objects.append(obj)
    binary = out / 'BOUNDS.ELF'
    run([B.CC, *B.LDFLAGS, *objects, '-o', binary, '-ldebug', '-lpatches', '-lm'], out / 'ee-link.log')
    print('Built ' + str(binary), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--base-src', type=Path, required=True)
    p.add_argument('--variants', default='reference,mmi,count,countmmi')
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--ee-only', action='store_true')
    p.add_argument('--negative-ee-only', action='store_true')
    p.add_argument('--negative-controls', action='store_true')
    p.add_argument('--prepare-only', action='store_true')
    p.add_argument('--cold', action='store_true')
    a = p.parse_args(); out = a.out.resolve(); selected = a.variants.split(',')
    if len(selected)<2 or selected[0] != 'reference' or len(selected) != len(set(selected)) or any(
            n not in ('reference', 'mmi', 'count', 'countmmi', 'production') for n in selected):
        raise SystemExit('Variants must be unique supported names beginning with reference')
    prepare(out, a.base_src.resolve(), selected, a.cold)
    if (a.negative_controls or a.negative_ee_only) and 'production' not in selected:
        raise SystemExit('Actual source controls require selected production')
    if a.prepare_only:
        return
    if a.ee_only or a.negative_ee_only:
        ee(out,a.negative_ee_only)
    else:
        host(out, a.sanitize,a.negative_controls)


if __name__ == '__main__':
    main()
