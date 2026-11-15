#!/usr/bin/env python3
"""Actual-header EE slope ABI and frozen consumer object comparison, serial."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

import build as B
from audio_storage_next7_finalbind import Object
from next7_lumplocation_objectcheck import frames
from next11_memory_replay import validate_candidate

ROOT = Path(__file__).resolve().parents[2]
FIELDS = 'id next o normal d zdelta zangle xydirection dorigin dnormdir dzdelta moved flags'.split()
SOURCES = ['src/p_slopes.c', 'src/p_mobj.c', 'src/r_plane.c', 'src/r_fps.c',
           'src/p_saveg.c', 'src/lua_maplib.c', 'src/hardware/hw_main.c', 'src/ps2/ps2_mem.c']


def run(command, log):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=300)
    log.write_text(result.stdout + result.stderr)
    assert result.returncode == 0, str(log) + ': ' + result.stderr[-4000:]
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next11/baseline-src')
    parser.add_argument('--candidate', type=Path, default=ROOT / 'build/nov15-next11/memory/byte-pool/candidate-src')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--header-only-proof', type=Path)
    parser.add_argument('--abi-only', action='store_true')
    parser.add_argument('--analyze-only', action='store_true', help='reuse compiled artifacts after checking their source pins')
    args = parser.parse_args()
    stage = validate_candidate(args.candidate)
    args.out = (args.out or ROOT / ('build/nov15-next11/memory/replay-' + stage + '/objects')).resolve()
    if not args.out.is_relative_to(ROOT / 'build') or args.out == ROOT / 'build':
        parser.error('--out must be an isolated child of build/')
    if not args.analyze_only and (args.out / 'report.json').exists():
        parser.error('Completed object evidence exists; use --analyze-only or choose a fresh --out')
    args.out.mkdir(parents=True, exist_ok=True)
    previous = json.loads((args.out / 'report.json').read_text()) if args.analyze_only else None
    isolated = args.out / 'isolated-src'
    shutil.copytree(args.baseline.resolve() / 'src', isolated / 'src', dirs_exist_ok=True)
    shutil.copyfile(args.candidate.resolve() / 'src/r_defs.h', isolated / 'src/r_defs.h')
    shutil.copyfile(args.candidate.resolve() / 'src/p_slopes.c', isolated / 'src/p_slopes.c')
    flags = [f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP', '-DNOHW')]
    flags += ['-DHWRENDER', '-Werror', '-Wno-format-truncation']
    objcopy = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objcopy' + B.EXE)
    objdump = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objdump' + B.EXE)
    readelf = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-readelf' + B.EXE)
    expressions = ['sizeof(pslope_t)', '_Alignof(pslope_t)', 'sizeof(boolean)', 'sizeof(void*)',
                   '(sizeof(pslope_t)+15)&~15u', 'sizeof(vertex_t)', 'sizeof(drawseg_t)',
                   'sizeof(mobj_t)', 'sizeof(sector_t)', 'sizeof(zlevelpool_t)']
    expressions += ['offsetof(pslope_t,' + f + ')' for f in FIELDS]
    expressions += ['sizeof(((pslope_t*)0)->' + f + ')' for f in FIELDS]
    abi, objects, stack, hashes, relocs, records, object_hashes = {}, {}, {}, {}, {}, {}, {}
    for label, root, extra in [('baseline', args.baseline.resolve(), []), ('current', isolated, []),
                               ('old-switch', isolated, ['-DPS2_NOOPT_SLOPE_LAYOUT']),
                               ('pool-old', isolated, ['-DPS2_NOOPT_SLOPE_POOL_BYTES'])]:
        out = args.out / label
        out.mkdir(exist_ok=True)
        includes = ['-I' + str(root / 'src'), '-I' + str(root / 'src/ps2')]
        includes += [f for f in B.INCS if f not in ('-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/ps2'))]
        source, obj, binary = (out / ('abi' + suffix) for suffix in ('.c', '.o', '.bin'))
        source.write_text('#include "doomdef.h"\n#include "r_defs.h"\n#include "z_zone.h"\n#include <stddef.h>\n'
                          'const unsigned abi[] __attribute__((section(".meta_abi"),used))={'
                          + ','.join(expressions) + '};\n')
        if args.analyze_only:
            assert previous['source_sha256'][label]['src/r_defs.h'] == hashlib.sha256((root / 'src/r_defs.h').read_bytes()).hexdigest()
        else:
            run([B.CC, *flags, *includes, *extra, '-c', source, '-o', obj], out / 'abi-build.log')
            run([objcopy, '-O', 'binary', '--only-section=.meta_abi', obj, binary], out / 'abi-copy.log')
        values = struct.unpack('<' + 'I' * (len(binary.read_bytes()) // 4), binary.read_bytes())[:len(expressions)]
        abi[label] = dict(zip(expressions, values))
        objects[label], stack[label], hashes[label], relocs[label] = {}, {}, {}, {}
        records[label], object_hashes[label] = {}, {}
        for relative in ([] if args.abi_only else SOURCES):
            stem = relative.replace('/', '__').replace('.c', '')
            obj = out / (stem + '.o')
            if args.analyze_only:
                assert previous['source_sha256'][label][relative] == hashlib.sha256((root / relative).read_bytes()).hexdigest()
            else:
                run([B.CC, *flags, *B.UNIT_FLAGS.get(relative, []), *includes, *extra, '-c', root / relative, '-o', obj],
                    out / (stem + '-build.log'))
            assembly = run([objdump, '-dr', obj], out / (stem + '.s'))
            relocation_text = run([readelf, '-rW', obj], out / (stem + '-relocations.txt'))
            relocs[label][relative] = hashlib.sha256(relocation_text.encode()).hexdigest()
            elf = Object(obj)
            objects[label][relative] = {name: elf.signature(name) for name, header in elf.sections.items() if header[2] & 2}
            records[label][relative] = elf.relocations
            object_hashes[label][relative] = hashlib.sha256(obj.read_bytes()).hexdigest()
            stack[label].update(frames(assembly, relative))
            hashes[label][relative] = hashlib.sha256((root / relative).read_bytes()).hexdigest()
        hashes[label]['src/r_defs.h'] = hashlib.sha256((root / 'src/r_defs.h').read_bytes()).hexdigest()
        print(label, 'slope', abi[label]['sizeof(pslope_t)'], 'align', abi[label]['_Alignof(pslope_t)'],
              'pool_stride', abi[label]['(sizeof(pslope_t)+15)&~15u'], flush=True)
    assert abi['baseline'] == abi['old-switch'], 'opt-out actual header ABI differs'
    assert abi['pool-old'] == abi['current'], 'pool-only opt-out changed the112-byte header ABI'
    assert abi['baseline']['sizeof(pslope_t)'] == 120 and abi['current']['sizeof(pslope_t)'] == 112
    for label in abi:
        assert abi[label]['_Alignof(pslope_t)'] == 8
        assert abi[label]['sizeof(boolean)'] == 1 and abi[label]['sizeof(void*)'] == 4
    assert abi['baseline']['(sizeof(pslope_t)+15)&~15u'] == 128
    assert abi['current']['(sizeof(pslope_t)+15)&~15u'] == 112
    for key in abi['baseline']:
        if key not in ['sizeof(pslope_t)', '(sizeof(pslope_t)+15)&~15u',
                        'offsetof(pslope_t,moved)', 'offsetof(pslope_t,flags)']:
            assert abi['baseline'][key] == abi['current'][key], key + ' changed'
    assert abi['current']['offsetof(pslope_t,moved)'] == 2 and abi['current']['offsetof(pslope_t,flags)'] == 3
    assert objects['baseline'] == objects['old-switch'], 'opt-out complete allocated object sections differ'
    assert relocs['baseline'] == relocs['old-switch'], 'opt-out ordered object relocations differ'
    assert records['baseline'] == records['old-switch'], 'opt-out full ordered relocation targets/addends differ'
    header_only = args.header_only_proof or ROOT / 'build/nov15-next11/memory/objects'
    header_only = header_only.resolve()
    header_only_compared = False
    if not args.abi_only and stage == 'header-only':
        assert objects['pool-old'] == objects['current'] and records['pool-old'] == records['current']
        header_only_compared = True
    elif not args.abi_only and header_only != args.out and (header_only / 'allocated-sections.json').is_file():
        prior_objects = json.loads((header_only / 'allocated-sections.json').read_text())
        prior_records = json.loads((header_only / 'ordered-relocations.json').read_text())
        assert objects['pool-old'] == prior_objects['current'], '64-slot counterfactual differs from header-only consumer objects'
        assert records['pool-old'] == prior_records['current'], '64-slot counterfactual differs from header-only ordered relocations'
        header_only_compared = True
    elif args.header_only_proof and not args.abi_only:
        raise RuntimeError('Requested header-only proof does not contain complete prior object records')
    summary = {}
    for relative in objects['baseline']:
        def usage(label, prefix):
            return sum(s['bytes'] for name, s in objects[label][relative].items() if name.startswith('.' + prefix))
        summary[relative] = {kind: {'before': usage('baseline', kind), 'after': usage('current', kind)}
                             for kind in ('text', 'rodata', 'data', 'sdata', 'bss', 'sbss')}
    differences = {name: [before, stack['current'].get(name)] for name, before in stack['baseline'].items()
                   if before != stack['current'].get(name)}
    report = {'candidate_stage': stage, 'abi': abi, 'isolated_object_summary': summary, 'isolated_frame_differences': differences,
              'object_checks_executed': not args.abi_only,
              'old_switch_allocated_sections_equal': None if args.abi_only else True,
              'source_sha256': hashes, 'old_switch_ordered_relocations_equal': None if args.abi_only else True,
              'object_sha256': object_hashes,
              'ordered_relocation_sha256': relocs,
              'pool_optout_header_only_sections_and_ordered_relocations_equal': True if header_only_compared else None,
              'isolation': 'Frozen pass 11 baseline consumers plus only candidate r_defs.h and p_slopes.c; peer edits excluded.',
              'compiler_flags': flags, 'unit_flags': {s: B.UNIT_FLAGS.get(s, []) for s in SOURCES}}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.out / 'allocated-sections.json').write_text(json.dumps(objects, indent=2) + '\n')
    (args.out / 'ordered-relocations.json').write_text(json.dumps(records, indent=2) + '\n')
    print('ABI-only; consumer object checks not run.' if args.abi_only else
          'Opt-out complete allocated sections equal baseline; isolated frame changes: ' + str(differences), flush=True)


if __name__ == '__main__':
    main()
