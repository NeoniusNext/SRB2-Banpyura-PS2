#!/usr/bin/env python3
"""Actual frozen/current lumpinfo layout, field semantics and PS2 arena accounting."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def run(cmd, log):
    p = subprocess.run(list(map(str, cmd)), capture_output=True, text=True, cwd=ROOT)
    log.write_text(p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(str(log) + ': ' + p.stdout[-1000:] + p.stderr[-2000:])
    return p.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT / 'build/nov14-next4/baseline-src')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next4/lumpinfo')
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--ee-abi', action='store_true')
    a = ap.parse_args()
    a.out = a.out.resolve(); a.out.mkdir(parents=True, exist_ok=True)
    if not (a.baseline / 'src/w_wad.h').exists():
        subprocess.run([sys.executable, TOOLS / 'prepare_next4_baseline.py', '--out', a.baseline], check=True)
    report = {}; cases = {}
    for label, root in [('baseline', a.baseline.resolve()), ('current', ROOT)]:
        work = a.out / label; work.mkdir(exist_ok=True)
        h = (root / 'src/w_wad.h').read_text()
        start = h.index('typedef enum\n{\n\tCM_NOCOMPRESSION')
        end = h.index('// =========================================================================\n//                         \'VIRTUAL\' RESOURCES', start)
        types = work / 'lumpinfo.inc'; types.write_text(h[start:end])
        exe = work / 'test'
        cmd = ['cc', '-O2', '-g', '-std=gnu17', '-Wall', '-Wextra', '-Wno-unused-function', '-DPARANOIA', '-DHAVE_ZLIB',
               '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/ps2'), '-DLUMPINFO_TYPES="' + str(types) + '"',
               TOOLS / 'next4_lumpinfo_hosttest.c', '-o', exe]
        if a.sanitize:
            cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        run(cmd, work / 'build.log')
        text = run([exe], work / 'test.log')
        rows = re.findall(r'^case n=(\d+) guard=(\d) sides=(\d) used=(\d+) peak=(\d+) hash=([0-9a-f]+)$', text, re.M)
        assert len(rows) == 88, len(rows)
        cases[label] = rows
        tail = re.search(r'LUMPINFO4 DONE cases=(\d+) checks=(\d+) sizeof=(\d+) align=(\d+) codecbytes=(\d+)', text)
        assert tail, text[-1000:]
        report[label] = dict(zip(['cases', 'checks', 'sizeof', 'align', 'codecbytes'], map(int, tail.groups())))
        print(label, report[label], flush=True)
        if a.ee_abi:
            import build as B
            source = work / 'abi.c'
            offsets = ['position','disksize','name','hash','longname','fullname','diskpath','size','compression']
            expressions = ['sizeof(lumpinfo_t)', '_Alignof(lumpinfo_t)', 'sizeof(((lumpinfo_t*)0)->compression)'] + ['offsetof(lumpinfo_t,' + field + ')' for field in offsets]
            source.write_text('#include "doomdef.h"\n#include "w_wad.h"\n#include <stddef.h>\nconst unsigned meta_abi[] __attribute__((section(".meta_abi"),used))={' + ','.join(expressions) + '};\n')
            obj = work / 'abi.o'; binary = work / 'abi.bin'
            flags = [f for f in B.CFLAGS if f not in ['-flto', '-MMD', '-MP']]
            includes = ['-I' + str(root / 'src'), *[i for i in B.INCS if i != '-I' + str(ROOT / 'src')]]
            run([B.CC, *flags, *includes, '-c', source, '-o', obj], work / 'ee-abi-build.log')
            run([B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objcopy' + B.EXE), '-O', 'binary', '--only-section=.meta_abi', obj, binary], work / 'ee-abi-copy.log')
            values = struct.unpack('<' + 'I' * (len(binary.read_bytes()) // 4), binary.read_bytes())
            # Alignment may pad the section; the declared array entries come first.
            values = values[:len(expressions)]
            report[label]['ee_abi'] = dict(zip(['sizeof','align','codecbytes',*offsets], values))
            print(label, 'actual EE ABI', report[label]['ee_abi'], flush=True)
    for old, new in zip(cases['baseline'], cases['current']):
        assert old[:3] == new[:3] and old[-1] == new[-1], (old,new)
        assert all(int(b) <= int(a) for a,b in zip(old[3:5],new[3:5])), (old,new)
    if a.ee_abi:
        before=report['baseline']['ee_abi']; after=report['current']['ee_abi']
        assert before['sizeof']==44 and after['sizeof']==40 and after['codecbytes']==1
        assert all(before[k]==after[k] for k in before if k not in ['sizeof','codecbytes','compression'])
    interface = {}
    for label, root in [('baseline', a.baseline.resolve()), ('current', ROOT)]:
        source = '#include "doomdef.h"\n#include "w_wad.h"\n'
        pre = subprocess.run(['cc','-E','-P','-DNOHW','-DHAVE_ZLIB','-I'+str(root/'src'),
                              '-I'+str(ROOT/'build/host-gen'),'-x','c','-'], input=source, text=True, capture_output=True)
        (a.out/(label+'-nonps2.i')).write_text(pre.stdout)
        assert pre.returncode == 0, pre.stderr
        interface[label] = pre.stdout
        if shutil.which('c++'):
            cpp = subprocess.run(['c++','-std=gnu++17','-fsyntax-only','-DPS2_PROFILE','-DNOHW','-DHAVE_ZLIB',
                                  '-I'+str(root/'src'),'-I'+str(ROOT/'build/host-gen'),'-x','c++','-'],
                                 input=source, text=True, capture_output=True)
            (a.out/(label+'-cpp.log')).write_text(cpp.stdout+cpp.stderr)
            assert cpp.returncode == 0, cpp.stderr
    assert interface['baseline'] == interface['current'], 'non-PS2 header interface differs'
    report['nonps2_interface_identical'] = True
    report['cpp_header_syntax_checked'] = bool(shutil.which('c++'))
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('All field digests equal; no arena peak/request increase', flush=True)


if __name__ == '__main__':
    main()
