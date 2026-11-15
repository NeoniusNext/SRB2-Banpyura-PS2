#!/usr/bin/env python3
"""Serial actual-header EE32 ABI and isolated mipmap consumer object gate."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

import build as B
from next7_lumplocation_objectcheck import allocated_sections, frames
from next9_memory_hosttest import FIELDS

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ['src/r_patch.c', 'src/hardware/hw_cache.c', 'src/hardware/hw_batching.c',
           'src/hardware/hw_main.c', 'src/hardware/hw_light.c', 'src/hardware/hw_md2.c',
           'src/ps2/ps2_models.c', 'src/ps2/hw/ps2_hwd.c']


def run(command, log):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=240)
    log.write_text(result.stdout + result.stderr)
    assert result.returncode == 0, str(log) + ': ' + result.stderr[-4000:]
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next9/baseline-src')
    parser.add_argument('--candidate', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next9/memory/objects')
    parser.add_argument('--abi-only', action='store_true')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    # Freeze the unrelated consumers at this pass's source reference. Only the
    # live production metadata header is overlaid, so simultaneous independent
    # driver-kernel edits cannot contaminate the layout-only object comparison.
    isolated = args.out / 'isolated-src'
    shutil.copytree(args.baseline.resolve() / 'src', isolated / 'src', dirs_exist_ok=True)
    shutil.copyfile(args.candidate.resolve() / 'src/hardware/hw_data.h', isolated / 'src/hardware/hw_data.h')
    flags = [f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP', '-DNOHW')]
    if '-DHWRENDER' not in flags:
        flags += ['-DHWRENDER', '-Werror', '-Wno-format-truncation']
    objcopy = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objcopy' + B.EXE)
    objdump = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objdump' + B.EXE)
    expressions = ['sizeof(GLMipmap_t)', '_Alignof(GLMipmap_t)', 'sizeof(GLMapTexture_t)',
                   '_Alignof(GLMapTexture_t)', 'sizeof(void*)', 'offsetof(GLMapTexture_t,mipmap)',
                   'offsetof(GLMapTexture_t,scaleX)', 'offsetof(GLMapTexture_t,scaleY)']
    expressions += ['offsetof(GLMipmap_t,' + f + ')' for f in FIELDS]
    expressions += ['sizeof(((GLMipmap_t*)0)->' + f + ')' for f in FIELDS]
    abi, objects, stack, source_hashes = {}, {}, {}, {}
    for label, root, extra in [('baseline', args.baseline.resolve(), []), ('current', isolated, []),
                               ('old-switch', isolated, ['-DPS2_NOOPT_MIPMAP_LAYOUT'])]:
        out = args.out / label
        out.mkdir(exist_ok=True)
        includes = ['-I' + str(root / 'src'), '-I' + str(root / 'src/ps2')]
        includes += [f for f in B.INCS if f not in ('-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/ps2'))]
        source, obj, binary = (out / ('abi' + suffix) for suffix in ('.c', '.o', '.bin'))
        source.write_text('#include "doomdef.h"\n#include "hardware/hw_data.h"\n#include <stddef.h>\n'
                          'const unsigned abi[] __attribute__((section(".meta_abi"),used))={'
                          + ','.join(expressions) + '};\n')
        run([B.CC, *flags, *includes, *extra, '-c', source, '-o', obj], out / 'abi-build.log')
        run([objcopy, '-O', 'binary', '--only-section=.meta_abi', obj, binary], out / 'abi-copy.log')
        values = struct.unpack('<' + 'I' * (len(binary.read_bytes()) // 4), binary.read_bytes())[:len(expressions)]
        abi[label] = dict(zip(expressions, values))
        objects[label], stack[label], source_hashes[label] = {}, {}, {}
        for relative in ([] if args.abi_only else SOURCES):
            stem = relative.replace('/', '__').replace('.c', '')
            obj = out / (stem + '.o')
            run([B.CC, *flags, *B.UNIT_FLAGS.get(relative, []), *includes, *extra, '-c', root / relative, '-o', obj],
                out / (stem + '-build.log'))
            assembly = run([objdump, '-dr', obj], out / (stem + '.s'))
            objects[label][relative] = allocated_sections(obj)
            stack[label].update(frames(assembly, relative))
            source_hashes[label][relative] = hashlib.sha256((root / relative).read_bytes()).hexdigest()
        source_hashes[label]['src/hardware/hw_data.h'] = hashlib.sha256((root / 'src/hardware/hw_data.h').read_bytes()).hexdigest()
        print(label, 'mipmap', abi[label]['sizeof(GLMipmap_t)'], 'map', abi[label]['sizeof(GLMapTexture_t)'], flush=True)
    assert abi['baseline'] == abi['old-switch'], 'opt-out actual header ABI differs'
    assert abi['baseline']['sizeof(GLMipmap_t)'] == 88 and abi['current']['sizeof(GLMipmap_t)'] == 76
    assert abi['baseline']['sizeof(GLMapTexture_t)'] == 96 and abi['current']['sizeof(GLMapTexture_t)'] == 84
    for key in ('_Alignof(GLMipmap_t)', '_Alignof(GLMapTexture_t)', 'sizeof(void*)'):
        assert abi['baseline'][key] == abi['current'][key] == 4
    for field in FIELDS:
        key = 'sizeof(((GLMipmap_t*)0)->' + field + ')'
        assert abi['baseline'][key] == abi['current'][key], field + ' width changed'
        width = abi['current'][key]
        assert abi['current']['offsetof(GLMipmap_t,' + field + ')'] % min(width, 4) == 0, field + ' not naturally aligned'
    for field in ('format', 'data', 'flags', 'height', 'width', 'downloaded', 'nextcolormap', 'colormap'):
        key = 'offsetof(GLMipmap_t,' + field + ')'
        assert abi['baseline'][key] == abi['current'][key], 'generic leading field moved'
    assert objects['baseline'] == objects['old-switch'], 'opt-out complete allocated object sections differ'
    summary = {}
    for relative in objects['baseline']:
        def usage(label, prefix):
            return sum(s['size'] for name, s in objects[label][relative].items() if name.startswith('.' + prefix))
        summary[relative] = {kind: {'before': usage('baseline', kind), 'after': usage('current', kind)}
                             for kind in ('text', 'rodata', 'data', 'sdata', 'bss', 'sbss')}
    differences = {name: [before, stack['current'].get(name)] for name, before in stack['baseline'].items()
                   if before != stack['current'].get(name)}
    report = {'abi': abi, 'isolated_object_summary': summary, 'isolated_frame_differences': differences,
              'old_switch_allocated_sections_equal': True, 'source_sha256': source_hashes,
              'isolation': 'Frozen pass baseline consumer sources plus live candidate hw_data.h only; independent kernel edits are excluded.',
              'compiler_flags': flags, 'unit_flags': {s: B.UNIT_FLAGS.get(s, []) for s in SOURCES}}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.out / 'allocated-sections.json').write_text(json.dumps(objects, indent=2) + '\n')
    print('Opt-out complete allocated sections equal frozen baseline; frame changes:', differences, flush=True)
    print('Isolated summaries:', summary, flush=True)


if __name__ == '__main__':
    main()
