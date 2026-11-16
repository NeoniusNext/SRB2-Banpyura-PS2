#!/usr/bin/env python3
"""Prove truthful skip ownership without changing renderer decisions or output.

Compiles the actual scalar clipper, planner, selection/upload functions, draw
preparation/emission and delete callback with the existing bounded host GIF
capture. The old and fixed sources must emit identical complete GIF streams and
counter/return records. Sanitizers also exercise a deleted diagnostic owner.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

import hw_hosttest

ROOT = Path(__file__).resolve().parents[2]
PLAN_FIX = '\t\t\tskip_tex = m;\n\t\t\tskip_why = "immediate polygon outside view";\n'
DELETE_FIX = '\tif (skip_tex == TexInfo)\n\t{\n\t\tskip_tex = NULL;\n\t\tskip_why = NULL;\n\t}\n'
INPUTS = ('src/ps2/hw/ps2_hw_plan.inc', 'src/ps2/hw/ps2_hwd.c',
          'tools/ps2/hw_hosttest.c', 'tools/ps2/hw_hosttest.py',
          'tools/ps2/opt18_skip_bookkeeping_hosttest.c',
          'tools/ps2/opt18_skip_bookkeeping_hosttest.py')


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError('Missing unique source anchor: ' + before)
    return text.replace(before, after, 1)


def function(text, name):
    # Locate a definition rather than a prototype; balance braces after masking
    # comments and literals, then return the untouched production source bytes.
    match = re.search(r'(?m)^static [^\n]*\b' + re.escape(name) + r'\([^;\n]*\)\s*\{', text)
    if not match:
        raise RuntimeError('Missing definition: ' + name)
    start, at = match.start(), match.end() - 1
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/',
                    lambda m: ' ' * len(m[0]), text)
    depth = 0
    for end in range(at, len(masked)):
        depth += masked[end] == '{'
        depth -= masked[end] == '}'
        if depth == 0:
            return text[start:end + 1] + '\n'
    raise RuntimeError('Unbalanced function: ' + name)


def prepare(root, work, variant):
    work.mkdir(parents=True)
    old_root, old_hw = hw_hosttest.ROOT, hw_hosttest.HW
    hw_hosttest.ROOT, hw_hosttest.HW = root, root / 'src/ps2/hw'
    try:
        hw_hosttest.make_shim(work)
        hw_hosttest.copy_sources(work, False)
    finally:
        hw_hosttest.ROOT, hw_hosttest.HW = old_root, old_hw
    plan = (work / 'ps2_hw_plan.inc').read_text()
    driver = (root / 'src/ps2/hw/ps2_hwd.c').read_text()
    if plan.count(PLAN_FIX) != 1 or driver.count(DELETE_FIX) != 1:
        raise RuntimeError('Expected exactly the two reviewed bookkeeping edits')
    if variant in ('baseline', 'negative-owner'):
        plan = plan.replace(PLAN_FIX, '', 1)
    if variant in ('baseline', 'negative-delete'):
        driver = driver.replace(DELETE_FIX, '', 1)
    (work / 'ps2_hw_plan.inc').write_text(plan)
    original_fixture = (root / 'tools/ps2/hw_hosttest.c').read_text()
    prefix = original_fixture.split('/* ---- test framework ---- */')[0]
    prefix = replace_once(prefix,
        'static void host_log(const char *format, ...) { (void)format; }',
        '#define host_log fixture_log')
    prefix = replace_once(prefix,
        'const char *HWR_PS2_TexName(const GLMipmap_t *m) { (void)m; return "host texture"; }',
        'const char *HWR_PS2_TexName(const GLMipmap_t *m) { return m->regen_kind ? (m->regen_id == 1 ? "map A" : "map B") : "patch"; }')
    prefix = replace_once(prefix,
        'static u32 tex_want(const GLMipmap_t *m, int *visible) { (void)m; *visible = 1; return 0; }',
        'static u32 tex_want(const GLMipmap_t *m, int *visible);')
    prefix = replace_once(prefix,
        'static void imm_prepare(const FOutVector *verts, unsigned int n) { (void)verts; (void)n; }',
        'static void imm_prepare(const FOutVector *verts, unsigned int n);')
    prefix = replace_once(prefix,
        'static void pk_ref(const void *data, u32 qwc)\n{\n\t(void)data; (void)qwc;\n}',
        'static void pk_ref(const void *data, u32 qwc) { fixture_capture_ref(data, (size_t)qwc * 16u); }')
    prefix = replace_once(prefix,
        'static void pk_ref_tail(const void *data, u32 bytes) { (void)data; (void)bytes; }',
        'static void pk_ref_tail(const void *data, u32 bytes) { fixture_capture_ref(data, bytes); }')
    prefix = replace_once(prefix, '#include "ps2_hw_tex.inc"', '''
#define HWR_PS2_CPU_TILED_SOURCE 16u
static unsigned regenerate_calls;
static void HWR_PS2_RegenerateMipmap(GLMipmap_t *m) { (void)m; regenerate_calls++; }
static void HWR_PS2_ReleaseMipmapData(GLMipmap_t *m) { (void)m; }
static void HWR_PS2_FlatUnpin(const u8 *p, size_t n) { (void)p; (void)n; }
static const u8 *HWR_PS2_FlatPin(const GLMipmap_t *m) { (void)m; return NULL; }
static boolean HWR_PS2_FlatStreamable(const GLMipmap_t *m) { (void)m; return 0; }
static boolean HWR_PS2_FlatRows(const GLMipmap_t *m, UINT32 row, UINT32 n, UINT8 *dst) { (void)m; (void)row; (void)n; (void)dst; return 0; }
static size_t Z_ArenaFree(void) { return 32u << 20; }
#include "ps2_hw_tex.inc"''')
    prefix += '\nUINT32 hwr_geo_off;\n#include "ps2_hw_plan.inc"\n'
    prefix += '#define TRACING() 0\nstatic struct { int on; } TT;\n'
    prefix += 'static struct { int in_flush; } SPR;\nstatic void tt_capture(GLMipmap_t *m) { (void)m; }\n'
    prefix += ''.join(function(driver, name) for name in ('twin_rec', 'settex_now', 'hw_SetTexture', 'hw_DeleteTexture'))
    prefix += function(original_fixture, 'host_init')
    fixture = (root / 'tools/ps2/opt18_skip_bookkeeping_hosttest.c').read_text()
    (work / 'fixture.c').write_text(replace_once(fixture, '/* PRODUCTION_DRIVER */', prefix))
    actual_functions = {
        'src/ps2/hw/ps2_hwd.c': (driver, ('twin_rec', 'settex_now', 'hw_SetTexture', 'hw_DeleteTexture')),
        'src/ps2/hw/ps2_hw_plan.inc': (plan, ('plan_measure', 'plan_measure_impl', 'plan_measure_after', 'imm_prepare', 'plan_forget')),
        'src/ps2/hw/ps2_hw_tex.inc': ((work / 'ps2_hw_tex.inc').read_text(), ('tex_upload', 'tex_drop', 'clut_get', 'dc_drop')),
        'src/ps2/hw/ps2_hw_draw.inc': ((work / 'ps2_hw_draw.inc').read_text(), ('begin_draw_inner', 'emit_fan', 'ramp_clut')),
    }
    return {path + ':' + name: hashlib.sha256(function(text, name).encode()).hexdigest()
            for path, (text, names) in actual_functions.items() for name in names}


def invoke(command, log, expected=0, environment=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                            env=environment)
    log.write_text(' '.join(map(str, command)) + '\n' + result.stdout + result.stderr)
    if expected is not None and result.returncode != expected:
        raise RuntimeError(f'Unexpected status {result.returncode}: {log}\n' + result.stdout + result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in INPUTS}
    report = {'schema': 'ps2-opt18-skip-bookkeeping-host-v1', 'inputs_sha256': hashes,
              'fixture_boundaries': ['Actual production scalar transform, clipper, planner, selection/upload, begin_draw_inner, emit_fan, and DeleteTexture callback',
                                     'Bounded host capture records every inline GIF byte and referenced texture payload in submission order; GS execution and DMA waits are platform stubs',
                                     'Engine memory leases are no-op host hooks; the synthetic texture pixels and metadata have real heap lifetimes'],
              'results': {}, 'negative_controls': {}}
    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    for variant in ('baseline', 'candidate', 'negative-owner', 'negative-delete'):
        work = out / variant
        production_hashes = prepare(root, work, variant)
        report.setdefault('compiled_production_functions_sha256', {})[variant] = production_hashes
        for sanitize in (False, True):
            if variant.startswith('negative') and not sanitize:
                continue
            name = 'fixture-sanitize' if sanitize else 'fixture'
            binary = work / name
            command = ['gcc', '-std=gnu17', '-O2', '-g', '-ffp-contract=off', '-DPS2', '-DPS2_PROFILE',
                       '-Wall', '-Wextra', '-Werror', '-Wformat=2', '-Wno-unused-function',
                       '-Wno-unused-variable', '-Wno-unused-parameter', '-Wno-unused-but-set-variable',
                       '-DEXPECT_FIXED=' + str(int(variant != 'baseline')), '-I' + str(work),
                       '-I' + str(root / 'src/hardware')]
            if sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
            command += [work / 'fixture.c', '-o', binary, '-lm']
            invoke(command, work / (name + '-build.log'))
            packets = work / (name + '-packets.bin')
            result = invoke([binary, packets], work / (name + '-run.log'),
                            expected=None if variant.startswith('negative') else 0, environment=env)
            if variant.startswith('negative'):
                required = 'current immediate texture owns culled diagnostic' if variant == 'negative-owner' else 'matching live deletion clears diagnostic owner'
                if result.returncode == 0 or required not in result.stderr:
                    raise RuntimeError('Negative control did not reject the intended defect: ' + variant)
                report['negative_controls'][variant] = {'detected': True, 'assertion': required}
                continue
            data = json.loads(result.stdout)
            data['packets_sha256'] = hashlib.sha256(packets.read_bytes()).hexdigest()
            data['packet_bytes'] = packets.stat().st_size
            report['results'][variant + ('-sanitize' if sanitize else '')] = data
            if sanitize:
                uaf = invoke([binary, '--freed-owner'], work / (name + '-freed-owner.log'),
                             expected=None, environment=env)
                if variant == 'baseline':
                    if uaf.returncode == 0 or 'heap-use-after-free' not in uaf.stderr:
                        raise RuntimeError('Old-source freed-owner control did not expose heap-use-after-free')
                    report['negative_controls']['baseline-freed-owner'] = {'detected': True, 'sanitizer': 'heap-use-after-free'}
                elif uaf.returncode != 0:
                    raise RuntimeError('Fixed callback retained a freed diagnostic owner')
                else:
                    report['results']['candidate-freed-owner'] = json.loads(uaf.stdout)
    for suffix in ('', '-sanitize'):
        a, b = (report['results'][v + suffix] for v in ('baseline', 'candidate'))
        if a['behavior'] != b['behavior'] or a['packets_sha256'] != b['packets_sha256']:
            raise RuntimeError('Bookkeeping changed a rendering decision, counter, or emitted GIF byte')
    if hashes != {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in INPUTS}:
        raise RuntimeError('Input sources changed during testing')
    report['differential_behavior_and_full_packets_equal'] = True
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
