#!/usr/bin/env python3
"""Differential complete PB/planner/walk proof for a checked isolated snapshot.

Builds host C only. No EE compiler or emulator is invoked. DrawBlocks captures
every logical state/index/vertex word synchronously; the complete real C driver
texture planner consumes the unchanged submitted blocks. VU/DMA/GS execution
and actual whole-game memory/cache/time gates remain separate requirements.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

import hw_hosttest
import next12_pb_scratch_replay as replay

ROOT = replay.ROOT
TOOLS = ROOT / 'tools/ps2'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def section(text, start, end):
    if text.count(start) != 1:
        raise RuntimeError('Missing/ambiguous source anchor: ' + start)
    return text[text.index(start):text.index(end, text.index(start))]


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('Missing/ambiguous mutation anchor: ' + old)
    return text.replace(old, new, 1)


def mutation(text, name):
    if name == 'plan-order':
        return replace_once(text, 'for (k = 0; k < pb_nplan; k++)', 'for (k = pb_nplan; k-- > 0; )')
    if name == 'producer-bucket':
        old = 'HWR_PB_PLAN_BUCKET(pb_nplan) = bi;'
        if text.count(old) != 2:
            raise RuntimeError('Expected two production planner producers')
        return text.replace(old, 'HWR_PB_PLAN_BUCKET(pb_nplan) = 0;', 1)
    if name == 'walk-order':
        return replace_once(text, 'pb_walk[pos++] = pb_ent[p_].blk;',
                            'pb_walk[pos++] = pb_ent[(bk)->head].blk;')
    if name == 'replay-producer':
        old = 'HWR_PB_PLAN_BLK(pb_nplan) = blk;'
        if text.count(old) != 2:
            raise RuntimeError('Expected two production planner producers')
        at = text.rindex(old)
        return text[:at] + text[at:].replace(old, 'HWR_PB_PLAN_BLK(pb_nplan) = 0;', 1)
    if name:
        raise RuntimeError('Unknown mutation')
    return text


def prepare(work, baseline, candidate, tag, negative=None):
    work.mkdir(parents=True, exist_ok=True)
    base = baseline / 'src'
    source = base if tag == 'reference' else candidate / 'src'
    old_root = hw_hosttest.ROOT
    hw_hosttest.ROOT = baseline
    try:
        hw_hosttest.make_shim(work)
    finally:
        hw_hosttest.ROOT = old_root
    (work / 'hw_model.h').write_text((base / 'hardware/hw_model.h').read_text().replace('#include "../doomtype.h"', ''))
    for p in (base / 'ps2/hw').iterdir():
        if p.suffix in ('.inc', '.h'):
            text = p.read_text().replace('#include "../../doomtype.h"', '#include "shim.h"')
            if p.name == 'ps2_hw_prof.h':
                text = replace_once(text, '__asm__ volatile("mfc0 %0,$9" : "=r"(v));', 'v = 0; /* host platform timer */')
            (work / p.name).write_text(text)
    prefix = (TOOLS / 'hg_hosttest.c').read_text().split('/* ---- test framework ---- */', 1)[0]
    prefix = replace_once(prefix,
        'static u32 tex_want(const GLMipmap_t *m, int *visible) { (void)m; *visible = 1; return 0; }',
        'static u32 tex_want(const GLMipmap_t *m, int *visible);')
    prefix = replace_once(prefix,
        'static void imm_prepare(const FOutVector *verts, unsigned int n) { (void)verts; (void)n; } /* ps2_hw_plan.inc (HT planner): not part of the host test */',
        'static void imm_prepare(const FOutVector *verts, unsigned int n);\nstatic void settex_now(GLMipmap_t *m) { (void)m; }')
    prefix = replace_once(prefix, '#include "ps2_hw_pal.inc"', '''static void I_OutputMsg(const char *,...);
static void Z_SetRebuildCost(void **p,unsigned int cost) { (void)p;(void)cost; }
static void vu_chunk_flush(void);
static void pk_ref_tail(const void *p,u32 n) { (void)p;(void)n; }
#include "ps2_hw_fx2.h"
#include "ps2_hw_pal.inc"''')
    prefix = replace_once(prefix, '#include "ps2_hw_tex.inc"',
                          '#define PS2HWD_BatchDraw driver_BatchDraw\n#include "ps2_hw_tex.inc"\n#undef PS2HWD_BatchDraw')
    prefix += '''
#define PS2HWD_PlanBegin driver_PlanBegin
#define PS2HWD_PlanBlock driver_PlanBlock
#define PS2HWD_PlanEnd driver_PlanEnd
#include "ps2_hw_plan.inc"
#undef PS2HWD_PlanBegin
#undef PS2HWD_PlanBlock
#undef PS2HWD_PlanEnd
'''
    (work / 'driver-context.inc').write_text(prefix)
    batching = (base / 'hardware/hw_batching.c').read_text()
    capacity = section(batching, 'static int HWR_BatchCapacity(', '\n#ifdef PS2\n#include "hw_pbatch.inc"')
    (work / 'batch-capacity.inc').write_text(capacity)
    pb = (source / 'hardware/hw_pbatch.inc').read_text()
    if negative:
        pb = mutation(pb, negative)
    (work / 'production.inc').write_text(pb)
    shutil.copy2(TOOLS / 'next12_pb_scratch_hosttest.c', work / 'fixture.c')
    return {'source': str(source), 'production_sha256': hashlib.sha256(pb.encode()).hexdigest(),
            'files': {p.name: sha(p) for p in sorted(work.iterdir()) if p.is_file()}}


def run(command, log, expected=0):
    result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True, text=True,
                            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                                 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
    log.write_text(result.stdout + result.stderr)
    if result.returncode != expected:
        raise RuntimeError('Command failed with %s; inspect %s' % (result.returncode, log))
    return result.stdout


def build_run(work, tag, sanitize=False):
    binary = work / ('fixture-sanitize' if sanitize else 'fixture')
    command = ['gcc', '-std=gnu17', '-O2', '-g', '-fstrict-aliasing', '-ffp-contract=off',
               '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
               '-Wno-unused-but-set-variable', '-Wno-sign-compare', '-Wno-pointer-sign',
               '-Wno-misleading-indentation', '-DPS2', '-DPS2_PROFILE', '-I' + str(work)]
    if tag == 'optout':
        command += ['-DPS2_NOOPT_PB_SCRATCH']
    elif tag != 'reference':
        command += ['-DNEXT12_SHARED']
    if sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    command += [work / 'fixture.c', '-Wl,--gc-sections', '-o', binary, '-lm']
    run(command, work / (binary.name + '-build.log'))
    outputs = {name: work / (binary.name + '.' + name) for name in ('events', 'packets', 'memory')}
    stdout = run([binary, *outputs.values()], work / (binary.name + '-run.log'))
    if 'PB COMPLETE' not in stdout:
        raise RuntimeError('Fixture did not complete')
    return {'binary_sha256': sha(binary), 'command': [str(x) for x in command], 'stdout': stdout.strip(),
            'outputs': {name: {'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha(path)}
                        for name, path in outputs.items()}}


def complete_output_equal(a, b):
    # Read full streams, not selected hashes/pixels or a normalized summary.
    return {name: Path(a['outputs'][name]['path']).read_bytes() ==
                  Path(b['outputs'][name]['path']).read_bytes() for name in ('events', 'packets')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=replay.DEFAULT_BASE)
    parser.add_argument('--source', type=Path, default=replay.DEFAULT_CANDIDATE)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next12/pb-scratch/host')
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    baseline, source, out = args.baseline.resolve(), args.source.resolve(), args.out.resolve()
    replay.validate(baseline, False)
    replay.validate(source, True)
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build':
        parser.error('--out must be an isolated child of build/')
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'report.json').exists():
        raise RuntimeError('Preserve completed proof output; choose a fresh --out')
    report = {'kind': __doc__, 'source_revision': replay.REVISION, 'source': str(source),
              'source_manifest_sha256': sha(source / 'manifest.json'),
              'baseline_manifest_sha256': sha(baseline / 'manifest.json'),
              'tools': {name: sha(TOOLS / name) for name in ('next12_pb_scratch_hosttest.py',
                        'next12_pb_scratch_hosttest.c', 'next12_pb_scratch_replay.py',
                        'next12_pb_scratch_candidate.patch', 'hw_hosttest.py', 'hg_hosttest.c')},
              'compiler': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0],
              'variants': {}, 'negative_controls': {}}
    for tag in ('reference', 'candidate', 'optout'):
        work = out / tag
        prepared = prepare(work, baseline, source, tag)
        regular = build_run(work, tag)
        sanitize = build_run(work, tag, True)
        same = complete_output_equal(regular, sanitize)
        if not all(same.values()):
            raise RuntimeError('Sanitizer complete output differs: ' + tag)
        report['variants'][tag] = {'prepared': prepared, 'regular': regular, 'ASan_UBSan': sanitize,
                                   'sanitized_complete_output_equal': same}
    oracle = report['variants']['reference']['regular']
    report['complete_output_equal'] = {
        tag: complete_output_equal(oracle, report['variants'][tag]['regular'])
        for tag in ('candidate', 'optout')}
    if not all(all(v.values()) for v in report['complete_output_equal'].values()):
        raise RuntimeError('Complete reference/candidate/optout output differs')
    if args.negative_controls:
        for name in ('plan-order', 'producer-bucket', 'walk-order', 'replay-producer'):
            work = out / ('negative-' + name)
            prepared = prepare(work, baseline, source, 'candidate', name)
            result = build_run(work, 'candidate')
            equal = complete_output_equal(oracle, result)
            if all(equal.values()):
                raise RuntimeError('Executed mutation was not rejected: ' + name)
            report['negative_controls'][name] = {'prepared': prepared, 'run': result,
                                                'complete_output_equal': equal, 'rejected': True}
    report['status'] = 'PASS; host component proof only, full Hardware process gate pending'
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'report': str(out / 'report.json'),
                      'complete_output_equal': report['complete_output_equal'],
                      'negative_controls': len(report['negative_controls'])}, indent=2))


if __name__ == '__main__':
    main()
