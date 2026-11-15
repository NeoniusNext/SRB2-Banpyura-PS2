"""Frozen/current resource lookups and cache state, using actual engine types.

The baseline tree is the preceding completed iteration, rather than git HEAD.
Extracted production functions are compiled in separate translation units.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def section(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)] + '\n'


def prepare(out, baseline, mutation=None):
    out.mkdir(parents=True, exist_ok=True)
    sources = []
    for kind, path in [('reference', baseline), ('candidate', ROOT / 'src/w_wad.c')]:
        text = path.read_text()
        body = section(text, '#define LUMPNUMCACHESIZE 64', '//===========================================================================')
        body += section(text, 'static void W_InvalidateLumpnumCache(void)', '#if !defined(PS2_PROFILE)')
        body += section(text, 'static void W_BuildNameIndex(', '\n#endif\n\nUINT16 W_CheckNumForNamePwad')
        body += section(text, 'UINT16 W_CheckNumForLongNamePwad(', '\nUINT16\nW_CheckNumForMarkerStartPwad')
        body += section(text, 'static lumpnum_t CheckLumpInCache(', '\nlumpnum_t W_CheckNumForName(')
        body += section(text, 'static UINT16 W_CheckNumForPatchNamePwad(', '\n//\n// W_CheckNumForPatchNameInternal')
        body = body.replace('W_CheckNumForLongNamePwad', kind + '_long_actual')
        if kind == 'candidate' and mutation == 'prefix':
            old = '!strcmp(lump_p->longname, uname)'
            assert old in body
            body = body.replace(old, '!strncmp(lump_p->longname, uname, 8)')
        if kind == 'candidate' and mutation == 'cache':
            old = 'return W_CACHE_NUM(lumpnumcacheindex);'
            assert old in body
            body = body.replace(old, 'return W_CACHE_NUM(lumpnumcacheindex) ^ 1;')
        fallback = '''
#ifndef W_CACHE_HASH
#define W_CACHE_HASH(i) (lumpnumcache[(i)].hash)
#define W_CACHE_NUM(i) (lumpnumcache[(i)].lumpnum)
#define W_CACHE_NAME(i) (lumpnumcache[(i)].lumpname)
#endif
'''
        wrappers = f'''
void {kind}_index(wadfile_t *wf) {{ W_BuildNameIndex(wf); }}
UINT16 {kind}_long(const char *n, UINT16 w, UINT16 s) {{ return {kind}_long_actual(n,w,s); }}
UINT16 {kind}_patch(const char *n, UINT16 w, boolean l) {{ return W_CheckNumForPatchNamePwad(n,w,l); }}
void {kind}_cache_reset(void) {{ W_InvalidateLumpnumCache(); }}
void {kind}_cache_add(lumpnum_t n, const char *s, boolean l) {{ AddLumpToCache(n,s,l); }}
lumpnum_t {kind}_cache_check(const char *n, boolean l) {{ return CheckLumpInCache(n,l); }}
size_t {kind}_cache_bytes(void) {{ return sizeof lumpnumcache; }}
void {kind}_cache_state(next2_cache_state *s) {{
    unsigned i; memset(s,0,sizeof *s); s->cursor=lumpnumcacheindex;
    for(i=0;i<64;i++) {{ s->hash[i]=W_CACHE_HASH(i); s->number[i]=W_CACHE_NUM(i); memcpy(s->name[i],W_CACHE_NAME(i),32); }}
}}
'''
        prelude = '''#include "next2_lookup_common.h"
static boolean TestValidLump(UINT16 w, UINT16 l) {
    return w < numwadfiles && wadfiles[w] && l < wadfiles[w]->numlumps;
}
'''
        src = out / (kind + '.c')
        src.write_text(prelude + body + fallback + wrappers)
        sources.append(src)
    return sources


def run(cmd, log):
    p = subprocess.run(list(map(str, cmd)), cwd=ROOT, capture_output=True, text=True)
    log.write_text(' '.join(map(str, cmd)) + '\n' + p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(f'{p.returncode}: {log}\n{p.stdout}{p.stderr}')
    return p.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT / 'build/nov14-next/baseline-src/src/w_wad.c')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next/lookup')
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--negative-controls', action='store_true')
    ap.add_argument('--ee', action='store_true')
    ap.add_argument('--assets', type=Path)
    a = ap.parse_args()
    out = a.out.resolve()
    sources = prepare(out, a.baseline)
    common = ['-O2', '-std=gnu17', '-fwrapv', '-DNDEBUG', '-DNOHW', '-DPS2_PROFILE',
              '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-stringop-truncation',
              '-I' + str(TOOLS), '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'build/host-gen')]
    if a.sanitize:
        common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    exe = out / 'lookup'
    command = [os.environ.get('CC', 'cc'), *common, *sources, TOOLS / 'next2_lookup_hosttest.c', '-o', exe]
    run(command, out / 'build.log')
    test_args = []
    if a.assets:
        names = []
        for archive in sorted(a.assets.glob('*.pk3')):
            with zipfile.ZipFile(archive) as z:
                for name in z.namelist():
                    basename = name.rsplit('/', 1)[-1]
                    longname = basename.rsplit('.', 1)[0] if '.' in basename else basename
                    names.append(longname.encode('utf-8'))
        if not names or len(names) > 65535:
            raise RuntimeError('Expected 1..65535 asset names')
        path = out / 'asset-names.bin'
        path.write_bytes(struct.pack('<I', len(names)) + b''.join(struct.pack('<I', len(n)) + n for n in names))
        test_args.append(path)
        print(f'Asset names: {len(names)}')
    print(run([exe, *test_args], out / 'test.log'), end='')
    if not a.sanitize:
        print(run([exe, 'bench'], out / 'benchmark.log'), end='')
    if a.negative_controls:
        for mutation in ['prefix', 'cache']:
            work = out / mutation
            paths = prepare(work, a.baseline, mutation)
            neg = work / 'lookup'
            run([os.environ.get('CC', 'cc'), *common, *paths, TOOLS / 'next2_lookup_hosttest.c', '-o', neg], work / 'build.log')
            p = subprocess.run([str(neg)], capture_output=True, text=True)
            (work / 'test.log').write_text(p.stdout + p.stderr)
            if p.returncode != 1 or 'LOOKUP FAIL' not in p.stderr:
                raise RuntimeError('Mutation not detected: ' + mutation)
            print('Detected ' + mutation + ' mutation')
    if a.ee:
        import build as B
        ee = out / 'ee'
        ee.mkdir(exist_ok=True)
        flags = [f for f in B.CFLAGS if f not in ['-flto', '-MMD', '-MP']]
        objects = []
        for src in [*sources, TOOLS / 'next2_lookup_hosttest.c']:
            obj = ee / (src.stem + '.o')
            run([B.CC, *flags, '-DNEXT2_LOOKUP_EE', '-I' + str(TOOLS), *B.INCS, '-c', src, '-o', obj], ee / (src.stem + '-build.log'))
            objects.append(obj)
        run([B.CC, *B.LDFLAGS, *objects, '-o', ee / 'LOOKUP.ELF', '-ldebug', '-lpatches', '-lm'], ee / 'link.log')
        print('EE benchmark:', ee / 'LOOKUP.ELF')
    (out / 'inputs.json').write_text(json.dumps({'baseline': str(a.baseline), 'sources': list(map(str, sources))}, indent=2) + '\n')


if __name__ == '__main__':
    main()
