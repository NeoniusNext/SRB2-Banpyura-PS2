#!/usr/bin/env python3
"""Compare actual tile row-run conversion with the preceding column-map source.

Reuses the column proof's host types/stubs and source extractor. The separate C
fixture retains its original matrix and adds ALPHA_8/PSMT8, run descriptors and
fallback checks. No candidate source is edited and no emulator is started.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def sibling(name):
    path = HERE / (name + '.py')
    spec = importlib.util.spec_from_file_location('tile_runs_' + name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


columns = sibling('tile_columns_hosttest')
extract = sibling('highres_state_hosttest')
sha = columns.sha
COMMON = columns.COMMON + '''
u32 fast_plan(const input_t *, u32 *, u32 *);
u32 base_context_size(void);
u32 fast_context_size(void);
'''


def wrapper(source, tag, negative):
    text = source.read_text()
    start = text.index('typedef struct\n{', text.index('static int tile_coord('))
    end = text.index('} tile_fill_t;', start) + len('} tile_fill_t;')
    code = '#include "common.h"\n' + columns.STUBS
    code += extract.function(text, 'tile_coord') + text[start:end] + '\n'
    prepared = 'static void tile_fill_columns(' in text
    if prepared:
        prepare = extract.function(text, 'tile_fill_columns')
        if negative and tag == 'fast':
            old = 'c->x + dx, c->source.w'
            if old not in prepare:
                raise RuntimeError('Candidate-only negative-control anchor is missing.')
            prepare = prepare.replace(old, 'c->x + dx + 1, c->source.w')
        code += prepare
    code += extract.function(text, 'tile_fill')
    code += f'''
__attribute__((noinline)) void {tag}_fill(const input_t *i, u8 *out)
{{
 tile_fill_t c;memset(&c,0,sizeof c);c.source=i->r;c.source.owner=(GLMipmap_t*)&i->m;
 c.data=i->data;c.x=i->x;c.y=i->y;c.wrapx=i->wrapx;c.wrapy=i->wrapy;c.region=i->region;
 H.fbw=i->fbw;H.fb32=i->fb32;memcpy(H.pal,i->pal,sizeof H.pal);
 {'tile_fill_columns(&c);' if prepared else ''}
 for(u32 row=0;row<TILE_SIZE;row++)tile_fill(&c,row,out+row*TILE_SIZE*4u);
}}
u32 {tag}_context_size(void) {{ return (u32)sizeof(tile_fill_t); }}
'''
    if tag == 'fast':
        if not prepared or 'run_count' not in text[start:end]:
            raise RuntimeError('Candidate must contain the upload-local row-run plan.')
        code += r'''
u32 fast_plan(const input_t *i, u32 *columns_out, u32 *plan)
{
 tile_fill_t c;memset(&c,0,sizeof c);c.source=i->r;c.source.owner=(GLMipmap_t*)&i->m;
 c.data=i->data;c.x=i->x;c.y=i->y;c.wrapx=i->wrapx;c.wrapy=i->wrapy;c.region=i->region;
 tile_fill_columns(&c);
 for(int x=0;x<TILE_SIZE;x++)columns_out[x]=(u32)c.sx[x];
 for(u32 r=0;r<c.run_count;r++){
  plan[r*4]=c.runs[r].sx;plan[r*4+1]=c.runs[r].dst;
  plan[r*4+2]=c.runs[r].count;plan[r*4+3]=c.runs[r].same;
 }
 return c.run_count;
}
'''
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative', action='store_true', help='Shift candidate columns only; exact assertions must fail.')
    parser.add_argument('--ee-objects', action='store_true', help='Compile both extracted wrappers for EE using PS2DEV.')
    args = parser.parse_args()
    base, candidate, out = args.base.resolve(), args.candidate.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'common.h').write_text(COMMON)
    for tag, source in [('base', base), ('fast', candidate)]:
        (out / (tag + '.c')).write_text(wrapper(source, tag, args.negative))
    caller = extract.function(candidate.read_text(), 'tile_load_impl')
    if caller.count('tile_fill_columns(&c);') != 1 or not caller.index('if (!hit)') < caller.index('tile_fill_columns(&c);') < caller.index('upload_bands('):
        raise RuntimeError('Run plan must be prepared once, only on an upload miss.')
    fixture = Path(__file__).with_suffix('.c')
    proof = {
        'base': str(base), 'base_sha256': sha(base),
        'candidate': str(candidate), 'candidate_sha256': sha(candidate),
        'runner_sha256': sha(Path(__file__)), 'fixture_sha256': sha(fixture),
        'column_helpers_sha256': sha(HERE / 'tile_columns_hosttest.py'),
        'extraction_helpers_sha256': sha(HERE / 'highres_state_hosttest.py'),
        'negative_mutation_target': 'candidate only' if args.negative else None,
        'base_wrapper_sha256': sha(out / 'base.c'), 'candidate_wrapper_sha256': sha(out / 'fast.c'),
        'scope': 'Actual tile_coord, tile_fill_t, tile_fill_columns and tile_fill. Packed AP88 accessor mocked as in the column proof.'
    }
    (out / 'source-proof.json').write_text(json.dumps(proof, indent=2) + '\n')
    flags = ['-std=gnu17', '-O2', '-g', '-fwrapv', '-Wall', '-Wextra', '-Werror']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    command = [*shlex.split(os.environ.get('CC', 'cc')), *flags, '-I' + str(out), str(out / 'base.c'),
               str(out / 'fast.c'), str(fixture), '-o', str(out / 'test')]
    (out / 'host-command.json').write_text(json.dumps(command, indent=2) + '\n')
    built = subprocess.run(command, text=True, capture_output=True)
    (out / 'build.log').write_text(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError(built.stdout + built.stderr)
    result = subprocess.run([str(out / 'test')], text=True, capture_output=True)
    (out / 'test.log').write_text(result.stdout + result.stderr)
    totals = re.search(r'TC COMPLETE checks=(\d+) failures=(\d+) compared_bytes=(\d+)', result.stdout)
    exact_count = totals is not None and int(totals[1]) == 10880 and int(totals[3]) == 178257920
    passed = result.returncode == 0 and exact_count and int(totals[2]) == 0 and not result.stderr
    matched = (result.returncode == 1 and exact_count and 'TC FAIL' in result.stdout and not result.stderr) if args.negative else passed
    ee_commands, ee_objects = [], []
    if args.ee_objects:
        os.environ['SRB2_PS2_OUT'] = str(out / 'sdkcfg')
        sys.path.insert(0, str(ROOT / 'tools/ps2'))
        import build as build_ps2
        build_ps2.gen_config()
        for tag in ('base', 'fast'):
            obj = out / (tag + '.o')
            command = [str(build_ps2.CC), *[f for f in build_ps2.CFLAGS if f not in ('-flto', '-MMD', '-MP')],
                       '-fno-lto', '-fstack-usage', '-Werror', '-I' + str(out), '-c', str(out / (tag + '.c')), '-o', str(obj)]
            built = subprocess.run(command, text=True, capture_output=True, env=build_ps2.ENV)
            (out / (tag + '-ee.log')).write_text(built.stdout + built.stderr)
            if built.returncode or built.stdout or built.stderr:
                raise RuntimeError(built.stdout + built.stderr)
            ee_commands.append(command)
            ee_objects.append({'path': str(obj), 'sha256': sha(obj), 'stack_usage': str(out / (tag + '.su'))})
    report = {'passed': passed, 'matched_expectation': matched, 'negative': args.negative, 'sanitize': args.sanitize,
              'returncode': result.returncode, 'checks': int(totals[1]) if totals else None,
              'compared_bytes': int(totals[3]) if totals else None, 'source_proof': proof,
              'output': result.stdout, 'stderr': result.stderr, 'ee_compile_commands': ee_commands,
              'ee_objects': ee_objects, 'emulator_used': False}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(result.stdout + result.stderr)
    return int(not matched)


if __name__ == '__main__':
    raise SystemExit(main())
