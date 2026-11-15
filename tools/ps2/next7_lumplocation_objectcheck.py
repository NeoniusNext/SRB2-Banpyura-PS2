#!/usr/bin/env python3
"""EE ABI and isolated object/stack cost proof for the compact lump location.

Uses one serial EE compiler worker. The assembler/object results are isolated
non-LTO measurements, not final linked engine or runtime stack high-water.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import build as B

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ['src/w_wad.c', 'src/w_pack.c', 'src/filesrch.c',
           'src/ps2/ps2_texc.c', 'src/ps2/i_sound.c']


def run(command, log):
    p = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True, timeout=120)
    log.write_text(p.stdout + p.stderr)
    assert p.returncode == 0, str(log) + ': ' + p.stderr[-4000:]
    return p.stdout


def allocated_sections(path):
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x01\x01', 'expected a little-endian ELF32 object'
    offset = struct.unpack_from('<I', data, 32)[0]
    stride, count, string_index = struct.unpack_from('<HHH', data, 46)
    headers = [struct.unpack_from('<10I', data, offset + i * stride) for i in range(count)]
    strings = headers[string_index]
    names = data[strings[4]:strings[4] + strings[5]]
    result = {}
    for header in headers:
        name = names[header[0]:names.index(0, header[0])].decode()
        if header[2] & 2:  # SHF_ALLOC
            content = data[header[4]:header[4] + header[5]] if header[1] != 8 else b''
            result[name] = {'size': header[5], 'sha256': hashlib.sha256(content).hexdigest()}
    return result


def frames(assembly, source):
    result, current = {}, None
    for line in assembly.splitlines():
        match = re.match(r'[0-9a-f]+ <([^>]+)>:', line)
        if match:
            current = source + ':' + match[1]
            result[current] = 0
        match = re.search(r'(?:d?addiu)\s+sp,sp,-(\d+)', line)
        if match and current:
            result[current] = max(result[current], int(match[1]))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT / 'build/nov14-next7/baseline-src')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next7/memory-location-objects')
    a = ap.parse_args()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    # Isolate the metadata change from the independent composite-retry helper
    # added after the first compact-layout integration stress comparison.
    flags = [f for f in B.CFLAGS if f not in ['-flto', '-MMD', '-MP']] + ['-DPS2_NOOPT_TEXRETRY']
    objcopy = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objcopy' + B.EXE)
    objdump = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objdump' + B.EXE)
    nm = B.DEV / ('ee/bin/mips64r5900el-ps2-elf-nm' + B.EXE)
    abi, sections, stack, hashes = {}, {}, {}, {}
    for label, root, defines in [('baseline', a.baseline.resolve(), []), ('current', ROOT, []),
                                 ('old-switch', ROOT, ['-DPS2_NOOPT_LUMPLOCATION'])]:
        out = a.out / label
        out.mkdir(exist_ok=True)
        includes = ['-I' + str(root / 'src')] + [i for i in B.INCS if i != '-I' + str(ROOT / 'src')]
        fields = ['disksize', 'name', 'compression', 'hash', 'longname', 'fullname', 'size']
        expressions = ['sizeof(lumpinfo_t)', '_Alignof(lumpinfo_t)', 'sizeof(unsigned long)', 'sizeof(char *)']
        expressions += ['offsetof(lumpinfo_t,' + field + ')' for field in fields]
        if label == 'current':
            expressions += ['offsetof(lumpinfo_t,location)', 'offsetof(lumpinfo_t,location_is_path)',
                            'sizeof(((lumpinfo_t*)0)->location.filepos)', 'sizeof(((lumpinfo_t*)0)->location.path)']
        else:
            expressions += ['offsetof(lumpinfo_t,position)', 'offsetof(lumpinfo_t,diskpath)',
                            'sizeof(((lumpinfo_t*)0)->position)', 'sizeof(((lumpinfo_t*)0)->diskpath)']
        source, obj, binary = out / 'abi.c', out / 'abi.o', out / 'abi.bin'
        source.write_text('#include "doomdef.h"\n#include "w_wad.h"\n#include <stddef.h>\n'
                          'const unsigned abi[] __attribute__((section(".meta_abi"),used))={'
                          + ','.join(expressions) + '};\n')
        run([B.CC, *flags, *includes, *defines, '-c', source, '-o', obj], out / 'abi-build.log')
        run([objcopy, '-O', 'binary', '--only-section=.meta_abi', obj, binary], out / 'abi-copy.log')
        values = struct.unpack('<' + 'I' * (len(binary.read_bytes()) // 4), binary.read_bytes())[:len(expressions)]
        abi[label] = dict(zip(expressions, values))
        sections[label], stack[label], hashes[label] = {}, {}, {}
        for name in SOURCES:
            stem = Path(name).stem
            obj = out / (stem + '.o')
            run([B.CC, *flags, *includes, *defines, '-c', root / name, '-o', obj], out / (stem + '-build.log'))
            asm = run([objdump, '-dr', obj], out / (stem + '.s'))
            run([nm, '-S', '--size-sort', obj], out / (stem + '-nm.txt'))
            sections[label][stem] = allocated_sections(obj)
            stack[label].update(frames(asm, stem))
            hashes[label][name] = hashlib.sha256((root / name).read_bytes()).hexdigest()
        print(label, 'sizeof', abi[label]['sizeof(lumpinfo_t)'], flush=True)
    assert abi['baseline']['sizeof(lumpinfo_t)'] == 40 and abi['current']['sizeof(lumpinfo_t)'] == 36
    assert abi['baseline'] == abi['old-switch'], 'old switch ABI differs'
    assert sections['baseline'] == sections['old-switch'], 'old switch allocated sections differ'
    summary = {}
    for source in sections['baseline']:
        def usage(label, prefix):
            return sum(v['size'] for k, v in sections[label][source].items() if k.startswith(prefix))
        summary[source] = {kind + '_' + label: usage(version, '.' + kind)
                           for kind in ('text', 'rodata', 'data', 'bss')
                           for label, version in [('before', 'baseline'), ('after', 'current')]}
    differences = {f: [value, stack['current'].get(f)] for f, value in stack['baseline'].items()
                   if value != stack['current'].get(f)}
    (a.out / 'abi-report.json').write_text(json.dumps(abi, indent=2) + '\n')
    (a.out / 'alloc-section-proof.json').write_text(json.dumps(sections, indent=2) + '\n')
    (a.out / 'section-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    (a.out / 'frame-summary.json').write_text(json.dumps({'frames': stack, 'differences': differences}, indent=2) + '\n')
    (a.out / 'source-sha256.json').write_text(json.dumps(hashes, indent=2) + '\n')
    (a.out / 'isolation-flags.json').write_text(json.dumps({'extra_defines': ['PS2_NOOPT_TEXRETRY']}, indent=2) + '\n')
    print('Old switch matches all baseline allocated sections; stack differences:', differences, flush=True)
    print('Isolated text delta:', sum(v['text_after'] - v['text_before'] for v in summary.values()), 'bytes', flush=True)


if __name__ == '__main__':
    main()
