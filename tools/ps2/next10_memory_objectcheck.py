#!/usr/bin/env python3
"""Serial actual-header EE32 drawseg ABI and frozen consumer object comparison."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

import build as B
from next7_lumplocation_objectcheck import allocated_sections, frames
from next10_memory_hosttest import FIELDS

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ['src/r_segs.c', 'src/r_things.c', 'src/r_bsp.c', 'src/r_main.c',
           'src/ps2/ps2_mem.c', 'src/hardware/hw_main.c']


def run(command, log):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=240)
    log.write_text(result.stdout + result.stderr)
    assert result.returncode == 0, str(log) + ': ' + result.stderr[-4000:]
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next10/baseline-src')
    parser.add_argument('--candidate', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next10/memory/objects')
    parser.add_argument('--abi-only', action='store_true')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    isolated = args.out / 'isolated-src'
    shutil.copytree(args.baseline.resolve() / 'src', isolated / 'src', dirs_exist_ok=True)
    shutil.copyfile(args.candidate.resolve() / 'src/r_defs.h', isolated / 'src/r_defs.h')
    flags = [f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP', '-DNOHW')]
    if '-DHWRENDER' not in flags:
        flags += ['-DHWRENDER', '-Werror', '-Wno-format-truncation']
    objcopy = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objcopy' + B.EXE)
    objdump = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objdump' + B.EXE)
    readelf = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-readelf' + B.EXE)
    expressions = ['sizeof(vertex_t)', '_Alignof(vertex_t)', 'sizeof(drawseg_t)', 'sizeof(polyobj_t)', 'sizeof(void*)']
    expressions += ['offsetof(vertex_t,' + f + ')' for f in FIELDS]
    expressions += ['sizeof(((vertex_t*)0)->' + f + ')' for f in FIELDS]
    expressions += ['offsetof(drawseg_t,leftpos)', 'offsetof(drawseg_t,rightpos)', 'offsetof(drawseg_t,ffloorplanes)',
                    'offsetof(polyobj_t,centerPt)', 'offsetof(polyobj_t,zdist)',
                    'sizeof(((drawseg_t*)0)->leftpos)', 'sizeof(((drawseg_t*)0)->rightpos)',
                    'sizeof(((drawseg_t*)0)->leftpos.x)', 'sizeof(((drawseg_t*)0)->leftpos.y)',
                    'sizeof(((drawseg_t*)0)->rightpos.x)', 'sizeof(((drawseg_t*)0)->rightpos.y)']
    abi, objects, stack, hashes, relocs = {}, {}, {}, {}, {}
    for label, root, extra in [('baseline', args.baseline.resolve(), []), ('current', isolated, []),
                               ('old-switch', isolated, ['-DPS2_NOOPT_DRAWSEG_XY'])]:
        out = args.out / label
        out.mkdir(exist_ok=True)
        includes = ['-I' + str(root / 'src'), '-I' + str(root / 'src/ps2')]
        includes += [f for f in B.INCS if f not in ('-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/ps2'))]
        source, obj, binary = (out / ('abi' + suffix) for suffix in ('.c', '.o', '.bin'))
        source.write_text('#include "doomdef.h"\n#include "r_defs.h"\n#include "p_polyobj.h"\n#include <stddef.h>\n'
                          'const unsigned abi[] __attribute__((section(".meta_abi"),used))={'
                          + ','.join(expressions) + '};\n')
        run([B.CC, *flags, *includes, *extra, '-c', source, '-o', obj], out / 'abi-build.log')
        run([objcopy, '-O', 'binary', '--only-section=.meta_abi', obj, binary], out / 'abi-copy.log')
        values = struct.unpack('<' + 'I' * (len(binary.read_bytes()) // 4), binary.read_bytes())[:len(expressions)]
        abi[label] = dict(zip(expressions, values))
        objects[label], stack[label], hashes[label], relocs[label] = {}, {}, {}, {}
        for relative in ([] if args.abi_only else SOURCES):
            stem = relative.replace('/', '__').replace('.c', '')
            obj = out / (stem + '.o')
            run([B.CC, *flags, *B.UNIT_FLAGS.get(relative, []), *includes, *extra, '-c', root / relative, '-o', obj],
                out / (stem + '-build.log'))
            assembly = run([objdump, '-dr', obj], out / (stem + '.s'))
            relocation_text = run([readelf, '-rW', obj], out / (stem + '-relocations.txt'))
            relocs[label][relative] = hashlib.sha256(relocation_text.encode()).hexdigest()
            objects[label][relative] = allocated_sections(obj)
            stack[label].update(frames(assembly, relative))
            hashes[label][relative] = hashlib.sha256((root / relative).read_bytes()).hexdigest()
        hashes[label]['src/r_defs.h'] = hashlib.sha256((root / 'src/r_defs.h').read_bytes()).hexdigest()
        print(label, 'vertex', abi[label]['sizeof(vertex_t)'], 'drawseg', abi[label]['sizeof(drawseg_t)'],
              'polyobj', abi[label]['sizeof(polyobj_t)'], flush=True)
    assert abi['baseline'] == abi['old-switch'], 'opt-out actual header ABI differs'
    assert abi['baseline']['sizeof(vertex_t)'] == abi['current']['sizeof(vertex_t)'] == 20
    assert abi['baseline']['sizeof(drawseg_t)'] == 444 and abi['current']['sizeof(drawseg_t)'] == 420
    assert abi['current']['sizeof(polyobj_t)'] == abi['baseline']['sizeof(polyobj_t)'] == 196
    for key in ('_Alignof(vertex_t)', 'sizeof(void*)'):
        assert abi['baseline'][key] == abi['current'][key] == 4
    for f in ('x', 'y', 'floorz', 'ceilingz'):
        assert abi['baseline']['sizeof(((vertex_t*)0)->' + f + ')'] == abi['current']['sizeof(((vertex_t*)0)->' + f + ')'] == 4
        assert abi['current']['offsetof(vertex_t,' + f + ')'] % 4 == 0
    for f, offset in [('x', 0), ('y', 4)]:
        assert abi['baseline']['offsetof(vertex_t,' + f + ')'] == abi['current']['offsetof(vertex_t,' + f + ')'] == offset
    for f in ('floorzset', 'ceilingzset'):
        assert abi['baseline']['sizeof(((vertex_t*)0)->' + f + ')'] == abi['current']['sizeof(((vertex_t*)0)->' + f + ')'] == 1
    for key in abi['baseline']:
        if 'vertex_t' in key or 'polyobj_t' in key:
            assert abi['baseline'][key] == abi['current'][key], 'persistent vertex/polyobject ABI changed'
    for member in ('leftpos', 'rightpos'):
        assert abi['baseline']['sizeof(((drawseg_t*)0)->' + member + ')'] == 20
        assert abi['current']['sizeof(((drawseg_t*)0)->' + member + ')'] == 8
        for f in ('x', 'y'):
            assert abi['baseline']['sizeof(((drawseg_t*)0)->' + member + '.' + f + ')'] == abi['current']['sizeof(((drawseg_t*)0)->' + member + '.' + f + ')'] == 4
    assert objects['baseline'] == objects['old-switch'], 'opt-out complete allocated object sections differ'
    assert relocs['baseline'] == relocs['old-switch'], 'opt-out ordered object relocations differ'
    summary = {}
    for relative in objects['baseline']:
        def usage(label, prefix):
            return sum(s['size'] for name, s in objects[label][relative].items() if name.startswith('.' + prefix))
        summary[relative] = {kind: {'before': usage('baseline', kind), 'after': usage('current', kind)}
                             for kind in ('text', 'rodata', 'data', 'sdata', 'bss', 'sbss')}
    differences = {name: [before, stack['current'].get(name)] for name, before in stack['baseline'].items()
                   if before != stack['current'].get(name)}
    report = {'abi': abi, 'isolated_object_summary': summary, 'isolated_frame_differences': differences,
              'object_checks_executed': not args.abi_only,
              'old_switch_allocated_sections_equal': True, 'source_sha256': hashes,
              'old_switch_ordered_relocations_equal': True, 'ordered_relocation_sha256': relocs,
              'isolation': 'Frozen pass baseline consumer sources plus live r_defs.h only; independent renderer edits excluded.',
              'compiler_flags': flags, 'unit_flags': {s: B.UNIT_FLAGS.get(s, []) for s in SOURCES}}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.out / 'allocated-sections.json').write_text(json.dumps(objects, indent=2) + '\n')
    print('Opt-out complete allocated sections equal frozen baseline; frame changes:', differences, flush=True)
    print('Isolated text delta:', sum(s['text']['after'] - s['text']['before'] for s in summary.values()), flush=True)


if __name__ == '__main__':
    main()
