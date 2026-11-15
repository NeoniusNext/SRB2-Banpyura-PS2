"""Compile full real TUs for default-v2 identity and exact target opt-out restore.

Consumes one granted EE worker, serially. These matching-header object proofs
supplement, and do not replace, the actual whole-default-LTO graph binding.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

from next7_render_fog_codecheck import Elf32
from next10_sw_bounds_codecheck import frame, symbol_tables

ROOT = Path(__file__).resolve().parents[2]
UNITS = ('hardware/hw_cache.c', 'ps2/hw/ps2_hwd.c', 'ps2/hw/ps2_hwd_null.c')


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    log.write_text(' '.join(map(str, command)) + '\n' + result.stdout + result.stderr)
    if result.returncode: raise SystemExit('Target policy command failed: ' + str(log))
    return result.stdout


def signature(obj):
    result = {}; tables = symbol_tables(obj)
    for index, (name, section) in enumerate(zip(obj.names, obj.sections)):
        if not section[2] & 2: continue
        records = []
        for rela in obj.sections:
            if rela[1] not in (4, 9) or rela[7] != index: continue
            if rela[1] != 4: raise SystemExit('Explicit target object RELAs required')
            for at in range(rela[4], rela[4] + rela[5], rela[9]):
                offset, info, addend = struct.unpack_from('<IIi', obj.data, at)
                target = tables[rela[6]][info >> 8]
                records.append((offset, info & 255, target['name'], target['value'], target['type'], addend))
        result[name] = {'type': section[1], 'flags': section[2], 'bytes': section[5], 'alignment': section[8],
                        'raw_sha256': None if section[1] == 8 else hashlib.sha256(obj.bytes(section)).hexdigest(),
                        'ordered_explicit_RELAs': records}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next12/baseline-src/src')
    parser.add_argument('--v2', type=Path, default=ROOT / 'build/nov15-next12/lighttable-v2/candidate-source/src')
    parser.add_argument('--candidate', type=Path, default=ROOT / 'build/nov15-next12/lighttable-v2b-src/src')
    args = parser.parse_args(); out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable policy output required')
    out.mkdir(parents=True); pinned = out / 'pinned-tools'; pinned.mkdir()
    for name in ('next12_lighttable_policy.py', 'build.py', 'next10_sw_bounds_codecheck.py', 'next7_render_fog_codecheck.py'):
        shutil.copyfile(ROOT / 'tools/ps2' / name, pinned / name)
    os.environ['SRB2_PS2_HW'] = '1'
    import build as B
    flags = [x for x in B.CFLAGS if x not in ('-MMD', '-MP', '-flto')] + ['-fno-lto']
    paths = {'baseline': args.baseline.resolve(), 'v2': args.v2.resolve(), 'candidate': args.candidate.resolve()}
    for name, path in paths.items(): shutil.copytree(path, out / 'sources' / name)
    report = {'scope': 'Complete real-TU default body unchanged by opt-out guards versus supplied guard-free control; full target opt-out code/data/RELAs/frame restored to original baseline',
              'flags': flags, 'policies': {}}
    for policy, pair, extra in (('default', ('v2', 'candidate'), []), ('optout', ('baseline', 'candidate'), ['-DPS2_NOOPT_LIGHTTABLE_INDEXED'])):
        units = {}
        for unit in UNITS:
            artifacts = {}; objects = {}
            for version in pair:
                source = out / 'sources' / version; stem = policy + '-' + version + '-' + Path(unit).stem
                target = out / (stem + '.o')
                includes = ['-I' + str(source), '-I' + str(source / 'ps2'),
                            '-I' + str(ROOT / 'build/nov15-next12/baseline/hardware/gen'),
                            *[x for x in B.INCS if not x.startswith('-I' + str(ROOT / 'src')) and x != '-I' + str(B.GEN)]]
                command = [B.CC, *flags, *extra, *includes]
                run([*command, '-c', source / unit, '-o', target], out / (stem + '-build.log'))
                text = run([*command, '-E', '-P', source / unit], out / (stem + '.i'))
                obj = Elf32(target); objects[version] = obj
                tokens = re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_0-9]+|[^\s]', text)
                artifacts[version] = {'object_sha256': sha(target), 'preprocessed_tokens_sha256': hashlib.sha256(json.dumps(tokens).encode()).hexdigest(),
                                      'complete_allocated_sections': signature(obj)}
            a, b = pair
            if artifacts[a]['preprocessed_tokens_sha256'] != artifacts[b]['preprocessed_tokens_sha256']:
                raise SystemExit('Actual complete preprocessed target policy tokens differ: ' + policy + ':' + unit)
            if artifacts[a]['complete_allocated_sections'] != artifacts[b]['complete_allocated_sections']:
                raise SystemExit('Actual complete allocated target sections/RELAs differ: ' + policy + ':' + unit)
            names = {s['name'] for table in symbol_tables(objects[a]).values() for s in table if s['type'] == 2 and s['section']}
            frames = {n: frame(objects[a], n) for n in sorted(names)}
            if any(frame(objects[b], n) != value for n, value in frames.items()):
                raise SystemExit('Complete target frame changed: ' + policy + ':' + unit)
            units[unit] = {'versions': artifacts, 'all_complete_code_data_BSS_capacities_ordered_RELAs_frames_and_preprocessed_tokens_equal': True,
                           'complete_frames': frames}
        report['policies'][policy] = units
    source = out / 'sources/candidate'
    includes = ['-I' + str(source), '-I' + str(source / 'ps2'),
                '-I' + str(ROOT / 'build/nov15-next12/baseline/hardware/gen'),
                *[x for x in B.INCS if not x.startswith('-I' + str(ROOT / 'src')) and x != '-I' + str(B.GEN)]]
    abi = out / 'rgba-abi.c'
    abi.write_text('''#include <stddef.h>
#include "doomtype.h"
const UINT32 next12_rgba_abi[] = {sizeof(RGBA_t),_Alignof(RGBA_t),offsetof(RGBA_t,s.red),offsetof(RGBA_t,s.green),offsetof(RGBA_t,s.blue),offsetof(RGBA_t,s.alpha),__BYTE_ORDER__,__ORDER_LITTLE_ENDIAN__,sizeof(void *)};
''')
    target = out / 'rgba-abi.o'
    run([B.CC, *flags, *includes, '-c', abi, '-o', target], out / 'rgba-abi-build.log')
    parsed = Elf32(target); section = parsed.sections[parsed.index['.rodata.next12_rgba_abi']]
    values = list(struct.unpack('<9I', parsed.bytes(section)))
    if values != [4, 1, 0, 1, 2, 3, 1234, 1234, 4]: raise SystemExit('Actual EE RGBA ABI/endianness witness differs')
    report['actual_EE_ABI_witness'] = {'sizeof_RGBA': values[0], 'alignof_RGBA': values[1], 'channel_offsets': values[2:6],
        'compiler_byte_order': values[6], 'compiler_little_endian': values[7], 'sizeof_pointer': values[8],
        'source_sha256': sha(abi), 'target_object_sha256': sha(target),
        'no_stronger_pointer_alignment_or_pointer_cast_introduced': True}
    report['all_source_sha256'] = {str(p.relative_to(out)): sha(p) for p in sorted((out / 'sources').rglob('*')) if p.is_file()}
    report['pinned_tool_sha256'] = {p.name: sha(p) for p in pinned.iterdir()}
    (out / 'policy-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Complete real target default-v2 identity and original opt-out restoration passed: ' + str(out))


if __name__ == '__main__': main()
