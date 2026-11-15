#!/usr/bin/env python3
"""Serial EE ABI/object evidence for sprite-frame member ordering."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

import build as B
from next7_lumplocation_objectcheck import allocated_sections, frames

ROOT = Path(__file__).resolve().parents[2]


def run(cmd, log):
    result = subprocess.run(list(map(str, cmd)), cwd=ROOT, capture_output=True, text=True, timeout=180)
    log.write_text(result.stdout + result.stderr)
    assert result.returncode == 0, str(log) + ': ' + result.stderr[-4000:]
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next8/baseline-src')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next8/memory/objects')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    flags = [flag for flag in B.CFLAGS if flag not in ('-flto', '-MMD', '-MP')]
    objcopy = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objcopy' + B.EXE)
    objdump = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objdump' + B.EXE)
    abi, objects, stack = {}, {}, {}
    fields = ['rotate', 'flip', 'lumppat', 'lumpid', 'rotated']
    expressions = ['sizeof(spriteframe_t)', '_Alignof(spriteframe_t)', 'sizeof(void*)']
    expressions += ['offsetof(spriteframe_t,' + field + ')' for field in fields]
    sources = ['src/r_things.c', 'src/r_patchrotation.c', 'src/ps2/ps2_limits.c', 'src/r_skins.c']
    hashes = {}
    for label, root, defines in [('baseline', args.baseline.resolve(), []), ('current', ROOT, []),
                                 ('old-switch', ROOT, ['-DPS2_NOOPT_SPRFRAME_LAYOUT'])]:
        work = args.out / label
        work.mkdir(exist_ok=True)
        includes = ['-I' + str(root / 'src')] + [item for item in B.INCS if item != '-I' + str(ROOT / 'src')]
        abi[label] = {}
        for rotate in (True, False):
            name = 'abi' if rotate else 'abi-no-rotation'
            exprs = expressions if rotate else expressions[:-1]
            source, obj, binary = (work / (name + suffix) for suffix in ('.c', '.o', '.bin'))
            source.write_text('#include "doomdef.h"\n'
                              + ('' if rotate else '#undef ROTSPRITE\n')
                              + '#include "r_defs.h"\n#include <stddef.h>\n'
                              + 'const unsigned abi[] __attribute__((section(".frame_abi"),used))={'
                              + ','.join(exprs) + '};\n')
            run([B.CC, *flags, *includes, *defines, '-c', source, '-o', obj], work / (name + '-build.log'))
            run([objcopy, '-O', 'binary', '--only-section=.frame_abi', obj, binary], work / (name + '-copy.log'))
            values = struct.unpack('<' + 'I' * (len(binary.read_bytes()) // 4), binary.read_bytes())[:len(exprs)]
            abi[label]['rotation' if rotate else 'no-rotation'] = dict(zip(exprs, values))
        objects[label], stack[label], hashes[label] = {}, {}, {}
        for relative in sources:
            stem = Path(relative).stem
            obj = work / (stem + '.o')
            run([B.CC, *flags, *includes, *defines, '-c', root / relative, '-o', obj], work / (stem + '-build.log'))
            assembly = run([objdump, '-dr', obj], work / (stem + '.s'))
            objects[label][relative] = allocated_sections(obj)
            stack[label].update(frames(assembly, relative))
            hashes[label][relative] = hashlib.sha256((root / relative).read_bytes()).hexdigest()
        hashes[label]['src/r_defs.h'] = hashlib.sha256((root / 'src/r_defs.h').read_bytes()).hexdigest()
        print(label, abi[label], flush=True)
    assert abi['baseline'] == abi['old-switch']
    assert objects['baseline'] == objects['old-switch'], 'old-switch object sections differ from frozen source'
    for profile, oldsize, newsize in [('rotation', 108, 104), ('no-rotation', 104, 100)]:
        before, after = abi['baseline'][profile], abi['current'][profile]
        assert before['sizeof(spriteframe_t)'] == oldsize and after['sizeof(spriteframe_t)'] == newsize
        assert before['_Alignof(spriteframe_t)'] == after['_Alignof(spriteframe_t)'] == 4
        for field in ('rotate', 'lumppat', 'lumpid'):
            assert before['offsetof(spriteframe_t,' + field + ')'] == after['offsetof(spriteframe_t,' + field + ')']
        assert after['offsetof(spriteframe_t,flip)'] == 2
    assert abi['current']['rotation']['offsetof(spriteframe_t,rotated)'] == 100
    summary = {}
    for source in sources:
        def size(label, prefix):
            return sum(section['size'] for name, section in objects[label][source].items() if name.startswith('.' + prefix))
        summary[source] = {kind: {'before': size('baseline', kind), 'after': size('current', kind)}
                           for kind in ('text', 'rodata', 'data', 'bss')}
    changes = {name: [before, stack['current'].get(name)] for name, before in stack['baseline'].items()
               if before != stack['current'].get(name)}
    report = {'abi': abi, 'objects': summary, 'frame_differences': changes,
              'old_switch_allocated_sections_equal': True, 'source_sha256': hashes,
              'compiler_flags': flags}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.out / 'allocated-sections.json').write_text(json.dumps(objects, indent=2) + '\n')
    print('Old-switch objects exactly match frozen source; frames:', changes, flush=True)
    print('Isolated summary:', summary, flush=True)


if __name__ == '__main__':
    main()
