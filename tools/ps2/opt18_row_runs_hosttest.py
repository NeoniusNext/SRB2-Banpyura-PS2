#!/usr/bin/env python3
"""Check the actual 16 KiB row classifier without a PS2 SDK or emulator.

Example:
  python3 tools/ps2/opt18_row_runs_hosttest.py --source-root . \
      --out build/opt18-row-public --sanitize --negative-controls

The default reference is reconstructed mechanically and must match the frozen
pre-row classifier SHA256. An explicit --reference-root must contain that same
reference. Every run also checks late rejection after submitted groups and the
exact unsigned32/64 quotient boundary. Negative controls mutate only generated
host copies and must fail semantically, without a compiler or sanitizer error.

The sinks check masks, source coordinates and a synthetic alpha blend. Host
CPU timings do not establish EE/GS or whole-game performance; actual GS RGBA,
Z24 and packed-vertex comparisons are separate native tests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).with_suffix('.c')
TILE_PATH = Path('src/ps2/hw/ps2_hw_tile_draw.inc')
REFERENCE_SHA256 = '6527517aa6b1bbbbacb472cea12974116574888d9caaa8a7b69309616959b8ba'
ROW_REFERENCE_SHA256 = 'aa9776d63698772560492595c872c797f35cde8ab11735a604b59278df943123'

COMMON = r'''
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef struct {float x,y,z,w,a[6];} cv_t;
typedef struct {float sb,tb;int x,y,ripple;} piece_t;
typedef struct {int width,height,extent,sw,sh,sizew,sizeh,wrapx,wrapy,region;float kx,ky,ox,oy;cv_t tri[3];piece_t piece;int inject_late;} input_t;
typedef struct {unsigned short *coverage;int *ox,*oy;u32 *alpha;size_t rectangles,groups;unsigned failed,boxes,pixels,fast,work,rejected,groups_before_rejection;} output_t;
void base_classify(const input_t*,output_t*);void fast_classify(const input_t*,output_t*);
'''

STUBS = r'''
#define TILE_SIZE 64
#define TILE_REGION_CORE 32
#define NOREC (-1)
typedef struct {int x0,x1;u16 y0,y1;} tile_mask_rect_t;
typedef struct {int x0,x1,y0,ox,oy;} tile_run_t;
typedef struct {tile_mask_rect_t rect;int ox,oy;} tile_batch_t;
typedef struct {int sw,sh,w,h,region,extent;} texrec_t;
static struct {int vw,vh;u32 sciss_x0,sciss_x1,sciss_y0,sciss_y1,serial;texrec_t rec[1];} H;
static struct {float tsx,tsy,kx,ky,ox,oy;u64 clamp;u32 serial;int wrapx,wrapy;texrec_t*r;} P;
static u32 tile_run_peak_bytes,tile_core_fast_triangles,tile_box_nodes,tile_pixel_fallbacks;
static output_t *O;
static int force_late_reject;
static float hw_minf(float a,float b){return a<b?a:b;}
static float hw_maxf(float a,float b){return a>b?a:b;}
static int tile_general_extent(const texrec_t*s){return s->extent;}
static int tile_wide_dimensions(const texrec_t*s,int*w,int*h){(void)s;(void)w;(void)h;return 0;}
static int tile_cache_get(void){return 0;}
static int tile_cache_try_borrow(void){return 0;}
static void hw_failure(const char*s){(void)s;O->failed++;}
static void tile_stencil_begin(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}
static void tile_stencil_end(void){}
static void record(int x0,int x1,int y0,int y1,int ox,int oy)
{
 O->rectangles++;
 if(x0<0||y0<0||x1>H.vw||y1>H.vh||x0>=x1||y0>=y1){O->failed++;return;}
 if(O->coverage)for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++)
 {size_t at=(size_t)y*H.vw+x;if(O->coverage[at]++)O->failed++;O->ox[at]=ox;O->oy[at]=oy;if(O->alpha)O->alpha[at]=(O->alpha[at]*127u+200u*128u)/255u;}
}
static int tile_draw_window(const cv_t*t,const piece_t*p,const texrec_t*s,int ox,int oy)
{(void)t;(void)p;(void)s;O->groups++;record(0,H.vw,0,H.vh,ox,oy);return 1;}
static int tile_draw_window_layout(const cv_t*t,const piece_t*p,const texrec_t*s,int ox,int oy,int wide)
{(void)wide;return tile_draw_window(t,p,s,ox,oy);}
static int tile_draw_group(const cv_t*t,const piece_t*p,const texrec_t*s,const tile_batch_t*g,int count)
{(void)t;(void)p;(void)s;O->groups++;for(int i=0;i<count;i++)record(g[i].rect.x0,g[i].rect.x1,g[i].rect.y0,g[i].rect.y1,g[i].ox,g[i].oy);return 1;}
'''

def digest(data):
    return hashlib.sha256(data).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('Missing or ambiguous source anchor: ' + repr(old))
    return text.replace(old, new)


def function(text, name):
    match = re.search(r'^static [^\n]*\b' + re.escape(name) + r'\([^{}]*\)\s*\{', text, re.M)
    if not match:
        raise RuntimeError('Missing actual source function: ' + name)
    start, depth = text.index('{', match.start()), 0
    for at in range(start, len(text)):
        if text[at] == '{':
            depth += 1
        elif text[at] == '}':
            depth -= 1
            if depth == 0:
                return text[match.start():at + 1] + '\n'
    raise RuntimeError('Unclosed actual source function: ' + name)


def reconstruct_reference(candidate):
    # Removing this entire newly introduced section leaves every original draw,
    # box certificate, pixel fallback and DFS byte available for the SHA check.
    anchor = '\ntypedef struct\n{\n\ttile_batch_t *batch;'
    if candidate.count(anchor) != 1:
        raise RuntimeError('The source does not contain the supported row classifier')
    start = candidate.index(anchor)
    end = candidate.index('\nstatic void tile_emit_triangle(', start)
    reference = candidate[:start] + candidate[end:]
    reference = replace_once(reference,
        'sizeof pending + sizeof batch + 2 * H.vw * sizeof(int)',
        'sizeof pending + sizeof batch')
    dispatch = ('\tconst int rows = tile_emit_rows(tri, pc, &source, &plane, &box, batch, &batch_n);\n'
                '\tif (rows < 0) goto failed;\n'
                '\tif (rows > 0) goto flush;\n')
    reference = replace_once(reference, dispatch, '')
    reference = replace_once(reference, 'flush:\n', '')
    if digest(reference.encode()) != REFERENCE_SHA256:
        raise RuntimeError('Reconstructed pre-row source differs from the frozen original; '
                           'original draw/classifier/DFS bytes changed')
    return reference


def original_row_inside(candidate):
    # Independently retain the V1 exact 64-bit row-bound function rather than
    # deriving expected bounds from the candidate's optimized quotient result.
    reference = function(candidate, 'tile_row_inside')
    reference = replace_once(reference,
        'tile_row_div_positive(-n[i] + d[i] - 1, (uint32_t)d[i])',
        '(-n[i] + d[i] - 1) / d[i]')
    reference = replace_once(reference,
        'tile_row_div_positive(n[i], (uint32_t)-d[i])', 'n[i] / -d[i]')
    if digest(reference.encode()) != ROW_REFERENCE_SHA256:
        raise RuntimeError('Reconstructed 64-bit row-bound function differs from frozen V1')
    return reference


def row_sources(reference, candidate, out, negative=None, late=False):
    (out / 'common.h').write_text(COMMON)
    generated = {}
    for tag, original in (('base', reference), ('fast', candidate)):
        text = original
        if tag == 'fast' and negative:
            mutations = {
                'source': ('entry->ox = ox; entry->oy = oy;',
                           'entry->ox = ox + tile_general_extent(source); entry->oy = oy;'),
                'mask': ('entry->rect.x0 = x0; entry->rect.x1 = x1;',
                         'entry->rect.x0 = x0 + 1; entry->rect.x1 = x1;'),
                'merge': ('&& entry->ox == ox && entry->oy == oy)', ')'),
            }
            text = replace_once(text, *mutations[negative])
        if tag == 'fast' and late:
            text = replace_once(text, '\ttile_box_nodes++;',
                '\ttile_box_nodes++;\n'
                '\tif(force_late_reject && b->y0 >= H.vh/2 && O->groups)'
                '{O->rejected++;O->groups_before_rejection=(unsigned)O->groups;return 0;}')
        start = text.index('static int tile_region_origin(')
        end = text.index('\nstatic void tile_emit_piece(', start)
        code = '#include "common.h"\n' + STUBS
        for name in ('tile_batch_order', 'tile_batch_flush', 'tile_batch_add'):
            code += function(text, name)
        code += text[start:end]
        code += f'''
void {tag}_classify(const input_t *i, output_t *out)
{{
 O=out;force_late_reject=i->inject_late;
 tile_run_peak_bytes=tile_core_fast_triangles=tile_box_nodes=tile_pixel_fallbacks=0;
 H.vw=i->width;H.vh=i->height;H.sciss_x0=H.sciss_y0=0;
 H.sciss_x1=i->width-1;H.sciss_y1=i->height-1;
 H.rec[0]=(texrec_t){{i->sw,i->sh,i->sizew,i->sizeh,i->region,i->extent}};
 P.kx=i->kx;P.ky=i->ky;P.ox=i->ox;P.oy=i->oy;
 P.wrapx=i->wrapx;P.wrapy=i->wrapy;P.r=&H.rec[0];
 tile_emit_triangle(i->tri,&i->piece,0);
 out->boxes=tile_box_nodes;out->pixels=tile_pixel_fallbacks;
 out->fast=tile_core_fast_triangles;out->work=tile_run_peak_bytes;
}}
'''
        path = out / (tag + '.c')
        path.write_text(code)
        generated[path.name] = digest(path.read_bytes())
    return generated


def quotient_sources(candidate, out, negative=False):
    start = candidate.index('typedef struct\n{\n\tint x[3], y[3];')
    end = candidate.index('} tile_plane_t;', start) + len('} tile_plane_t;')
    declaration = candidate[start:end]
    (out / 'common.h').write_text(
        '#include <stdint.h>\n#include <math.h>\n' + declaration + '\n'
        'void v1_inside(const tile_plane_t*,int,int,int,int*,int*);\n'
        'void v2_inside(const tile_plane_t*,int,int,int,int*,int*);\n'
        'int64_t v2_divide(int64_t,uint32_t);\n')
    reference = original_row_inside(candidate)
    generated = {}
    for tag in ('v1', 'v2'):
        code = '#include "common.h"\n' + function(candidate, 'tile_plane_edges')
        if tag == 'v2':
            helper = function(candidate, 'tile_row_div_positive')
            if negative:
                helper = replace_once(helper,
                    'if ((uint64_t)numerator <= UINT32_MAX) return (uint32_t)numerator / denominator;',
                    'return (uint32_t)numerator / denominator;')
            code += helper + '\nint64_t v2_divide(int64_t n,uint32_t d)'
            code += '{return tile_row_div_positive(n,d);}\n'
            code += function(candidate, 'tile_row_inside')
        else:
            code += reference
        code += f'\nvoid {tag}_inside(const tile_plane_t*p,int x0,int x1,int y,int*l,int*r)'
        code += '{tile_row_inside(p,x0,x1,y,l,r);}\n'
        path = out / (tag + '.c')
        path.write_text(code)
        generated[path.name] = digest(path.read_bytes())
    return generated


def execute_case(reference, candidate, out, sanitize, kind, negative=None):
    out.mkdir(parents=True, exist_ok=False)
    quotient = kind == 'quotient'
    late = kind == 'late'
    if quotient:
        generated = quotient_sources(candidate, out, negative=bool(negative))
        units = [out / 'v1.c', out / 'v2.c']
    else:
        generated = row_sources(reference, candidate, out, negative=negative, late=late)
        units = [out / 'base.c', out / 'fast.c']
    flags = ['-std=gnu17', '-O2', '-g', '-fno-lto', '-ffp-contract=off',
             '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-Wno-unused-variable', '-Wno-unused-parameter', '-Wno-unused-but-set-variable']
    if quotient:
        flags.append('-DROW_RUNS_QUOTIENT_PROPERTY')
    if late:
        flags.append('-DROWS_FORCE_LATE')
    if sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    command = ['cc', *flags, '-I' + str(out), *map(str, units), str(FIXTURE),
               '-o', str(out / 'test'), '-lm']
    (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    built = subprocess.run(command, capture_output=True, text=True)
    (out / 'build.log').write_text(built.stdout + built.stderr)
    if built.returncode or built.stdout or built.stderr:
        raise RuntimeError('Host build must succeed without diagnostics: ' + str(out / 'build.log'))
    ran = subprocess.run([str(out / 'test')], capture_output=True, text=True,
                         env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
    (out / 'test.log').write_text(ran.stdout + ran.stderr)
    pattern = r'^DIV COMPLETE .* failures=(\d+)$' if quotient else r'^ROWS COMPLETE .* failures=(\d+)$'
    complete = re.search(pattern, ran.stdout, re.M)
    failures = int(complete[1]) if complete else None
    passed = ran.returncode == 0 and failures == 0 and not ran.stderr
    # A crash, compile error or sanitizer report cannot satisfy a negative.
    witness = {
        'source': 'ROWS SOURCE ',
        'mask': 'a.coverage[at]==1&&b.coverage[at]==1',
        'merge': 'ROWS SOURCE ',
        'quotient': 'DIV FAIL numerator=4294967296 ',
    }.get(negative)
    detected = bool(negative and ran.returncode == 1 and failures is not None
                    and failures > 0 and witness in ran.stdout and not ran.stderr)
    result = {'passed': passed, 'negative_control_detected': detected,
              'returncode': ran.returncode, 'failures': failures, 'kind': kind,
              'negative': negative, 'sanitize': sanitize,
              'forced_late_after_submitted_group': late,
              'generated_source_sha256': generated,
              'fixture_sha256': digest(FIXTURE.read_bytes()),
              'command': command, 'output': ran.stdout, 'stderr': ran.stderr}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    summary = complete[0] if complete else 'missing completion line'
    print(out.name + ': ' + summary + (' [expected fault detected]' if detected else ''), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--reference-root', type=Path,
                        help='Optional explicit pre-row tree; its tile file must match the frozen original SHA')
    parser.add_argument('--out', type=Path, required=True, help='Fresh output directory')
    parser.add_argument('--sanitize', action='store_true', help='Enable AddressSanitizer and UBSan for every run')
    parser.add_argument('--negative-controls', action='store_true', help='Also require four deliberate faults to be detected')
    args = parser.parse_args()
    source = args.source_root.resolve() / TILE_PATH
    candidate_bytes = source.read_bytes()
    candidate = candidate_bytes.decode()
    reference = reconstruct_reference(candidate)
    reference_kind = 'mechanically reconstructed from source; exact frozen SHA required'
    if args.reference_root:
        explicit = (args.reference_root.resolve() / TILE_PATH).read_bytes()
        if digest(explicit) != REFERENCE_SHA256 or explicit.decode() != reference:
            raise RuntimeError('Explicit reference does not match the frozen pre-row source')
        reference_kind = 'explicit reference tree, also verified against mechanical reconstruction'
    # Verify the independent row-bound reference before creating any test output.
    original_row_inside(candidate)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'reference-tile-draw.inc').write_text(reference)
    (out / 'candidate-tile-draw.inc').write_bytes(candidate_bytes)
    proof = {'source': str(source), 'source_sha256': digest(candidate_bytes),
             'reference_sha256': REFERENCE_SHA256, 'reference_kind': reference_kind,
             'original_row_bound_function_sha256': ROW_REFERENCE_SHA256,
             'original_draw_classifier_DFS_bytes_unchanged': True,
             'helper_sha256': digest(Path(__file__).read_bytes()),
             'fixture_sha256': digest(FIXTURE.read_bytes()),
             'scope': 'Complete actual triangle classifier, original batching and numerical certificates; '
                      'GS draw sinks replaced with full viewport masks and independent source-coordinate checks. '
                      'Unsupported Q/huge/NaN cases require exact original masks/keys/counts. '
                      'Late rejection is injected only after half-height and an already submitted group. '
                      'Synthetic alpha and host timings do not replace actual EE/GS validation.'}
    (out / 'source-proof.json').write_text(json.dumps(proof, indent=2) + '\n')
    results = {}
    for kind in ('positive', 'late', 'quotient'):
        results[kind] = execute_case(reference, candidate, out / kind, args.sanitize, kind)
    if args.negative_controls:
        for negative in ('source', 'mask', 'merge', 'quotient'):
            kind = 'quotient' if negative == 'quotient' else 'positive'
            results['negative-' + negative] = execute_case(
                reference, candidate, out / ('negative-' + negative), args.sanitize, kind, negative)
    source_unchanged = source.read_bytes() == candidate_bytes
    positives = all(results[k]['passed'] for k in ('positive', 'late', 'quotient'))
    negatives = all(value['negative_control_detected'] for key, value in results.items() if key.startswith('negative-'))
    passed = positives and negatives and source_unchanged
    final = {'passed': passed, 'source_inputs_unchanged': source_unchanged,
             'sanitize': args.sanitize, 'negative_controls_requested': args.negative_controls,
             'source_proof': proof, 'runs': results, 'emulator_used': False,
             'scope': 'Host actual-source correctness/properties only; no native performance claim.'}
    (out / 'results.json').write_text(json.dumps(final, indent=2) + '\n')
    print('ROW RUNS HOST COMPLETE passed=' + str(passed).lower(), flush=True)
    return int(not passed)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, UnicodeError) as error:
        print('ROW RUNS HOST ERROR: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
