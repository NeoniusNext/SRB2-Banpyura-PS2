#!/usr/bin/env python3
"""Compare actual tile row conversion before/after an upload-local column map."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

COMMON = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
#define TILE_SIZE 64
#define PSM_T8 19
#define PSM_CT32 0
#define TF_CHROMAKEYED 1
enum {GL_TEXFMT_P_8,GL_TEXFMT_ALPHA_8,GL_TEXFMT_AP_88,GL_TEXFMT_RGBA,GL_TEXFMT_ALPHA_INTENSITY_88};
typedef struct {int width,height,format,flags,packed;const u8*data;}GLMipmap_t;
typedef struct {GLMipmap_t*owner;int w,h,screen,psm;const u32*tile_palette;}texrec_t;
typedef struct {GLMipmap_t m;texrec_t r;const u8*data;int x,y,wrapx,wrapy,region,fbw,fb32;u32 pal[256];}input_t;
void base_fill(const input_t*,u8*);void fast_fill(const input_t*,u8*);
'''
STUBS = r'''
#define HWR_PS2_PACKED_PATCHES 1
static struct {int fbw,fb32;u32 pal[256];}H;
static u32 a8_to_gs(u32 a){return(a+1u)/2u;}
static int HWR_PS2_IsPackedPatch(const GLMipmap_t*m){return m->packed;}
static u16 HWR_PS2_ReadAP88(const GLMipmap_t*m,u32 x,u32 y){return((const u16*)(const void*)m->data)[(size_t)y*m->width+x];}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--negative', action='store_true', help='Shift one prepared source column; must fail exact comparison.')
    p.add_argument('--ee-objects', action='store_true', help='Also compile both actual source wrappers for EE; no emulator.')
    a = p.parse_args()
    work = a.out.resolve(); work.mkdir(parents=True, exist_ok=True)
    helper = Path(__file__).with_name('highres_state_hosttest.py')
    spec = importlib.util.spec_from_file_location('helpers', helper)
    h = importlib.util.module_from_spec(spec); spec.loader.exec_module(h)
    (work / 'common.h').write_text(COMMON)
    for tag, source in [('base', a.base), ('fast', a.candidate)]:
        text = source.read_text()
        start = text.index('typedef struct\n{', text.index('static int tile_coord('))
        end = text.index('} tile_fill_t;', start) + len('} tile_fill_t;')
        code = '#include "common.h"\n' + STUBS + h.function(text, 'tile_coord') + text[start:end] + '\n'
        prepared = 'static void tile_fill_columns(' in text
        if prepared:
            code += h.function(text, 'tile_fill_columns')
            if a.negative and tag == 'fast':
                code = code.replace('c->x + dx, c->source.w', 'c->x + dx + 1, c->source.w')
        code += h.function(text, 'tile_fill')
        code += f'''\n__attribute__((noinline)) void {tag}_fill(const input_t*i,u8*out)
{{
 tile_fill_t c;memset(&c,0,sizeof c);c.source=i->r;c.source.owner=(GLMipmap_t*)&i->m;
 c.data=i->data;c.x=i->x;c.y=i->y;c.wrapx=i->wrapx;c.wrapy=i->wrapy;c.region=i->region;
 H.fbw=i->fbw;H.fb32=i->fb32;memcpy(H.pal,i->pal,sizeof H.pal);
 {'tile_fill_columns(&c);' if prepared else ''}
 for(u32 row=0;row<TILE_SIZE;row++)tile_fill(&c,row,out+row*TILE_SIZE*4u);
}}
'''
        (work / (tag + '.c')).write_text(code)
    # Check the real caller, not only the standalone conversion wrapper.
    caller = h.function(a.candidate.read_text(), 'tile_load_impl')
    if caller.count('tile_fill_columns(&c);') != 1 or not caller.index('if (!hit)') < caller.index('tile_fill_columns(&c);') < caller.index('upload_bands('):
        raise RuntimeError('Column map must be initialized once, only on the upload miss path.')
    fixture = Path(__file__).with_suffix('.c')
    proof = {'base': str(a.base.resolve()), 'base_sha256': sha(a.base), 'candidate': str(a.candidate.resolve()),
             'candidate_sha256': sha(a.candidate), 'fixture_sha256': sha(fixture),
             'scope': 'Actual tile_coord, tile_fill_t, tile_fill_columns and tile_fill; complete 64-row output byte comparison. Packed AP88 accessor mocked, format branches unchanged.'}
    (work / 'source-proof.json').write_text(json.dumps(proof, indent=2) + '\n')
    flags = ['-std=gnu17', '-O2', '-g', '-fwrapv', '-Wall', '-Wextra', '-Werror']
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    cmd = ['cc', *flags, '-I' + str(work), str(work / 'base.c'), str(work / 'fast.c'), str(fixture), '-o', str(work / 'test')]
    r = subprocess.run(cmd, text=True, capture_output=True)
    (work / 'build.log').write_text(r.stdout + r.stderr)
    if r.returncode:
        raise RuntimeError(r.stdout + r.stderr)
    r = subprocess.run([str(work / 'test')], text=True, capture_output=True)
    passed = r.returncode == 0 and 'failures=0' in r.stdout and not r.stderr
    matched = (not passed and r.returncode == 1 and 'TC FAIL' in r.stdout and not r.stderr) if a.negative else passed
    (work / 'test.log').write_text(r.stdout + r.stderr)
    ee_commands = []
    if a.ee_objects:
        import os, sys
        root = Path(__file__).resolve().parents[3]
        os.environ['SRB2_PS2_OUT'] = str(work / 'sdkcfg')
        sys.path.insert(0, str(root / 'tools/ps2'))
        import build as B
        B.gen_config()
        for tag in ('base', 'fast'):
            cmd = [str(B.CC), *[f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP')],
                   '-fno-lto', '-Werror', '-I' + str(work), '-c', str(work / (tag + '.c')), '-o', str(work / (tag + '.o'))]
            e = subprocess.run(cmd, text=True, capture_output=True, env=B.ENV)
            (work / (tag + '-ee.log')).write_text(e.stdout + e.stderr)
            if e.returncode or e.stdout or e.stderr:
                raise RuntimeError(e.stdout + e.stderr)
            ee_commands.append(cmd)
    report = {'passed': passed, 'matched_expectation': matched, 'negative': a.negative, 'sanitize': a.sanitize,
              'returncode': r.returncode, 'source_proof': proof, 'output': r.stdout, 'stderr': r.stderr,
              'ee_compile_commands': ee_commands, 'emulator_used': False}
    (work / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(r.stdout + r.stderr)
    return int(not matched)


if __name__ == '__main__':
    raise SystemExit(main())
