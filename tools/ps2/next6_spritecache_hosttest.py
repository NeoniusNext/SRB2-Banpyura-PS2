#!/usr/bin/env python3
"""Actual preceding/current sprite-header stores, getters, reserve and arena proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def function(text, start):
    begin = text.index(start)
    first = text.index('{', begin)
    depth = 1
    end = first + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end] + '\n'


def parts(root):
    state = (root / 'src/r_state.h').read_text()
    things = (root / 'src/r_things.c').read_text()
    data = (root / 'src/r_data.c').read_text()
    return {
        'SC_TYPES': section(state, '// needed for pre rendering (fracs)', 'extern sprcache_t *spritecachedinfo;'),
        'SC_INIT': function(data, 'static void R_InitSpriteLumps(void)'),
        'SC_WRITE': section(things, '\t\t\t// store sprite info in lookup tables', '\t\t\t//----------------------------------------------------'),
        'SC_GROW': function(things, '\t\t\tif (++numspritelumps >= max_spritelumps)'),
    }


def run(command, log, *, reject=False):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True, timeout=240)
    log.write_text(result.stdout + result.stderr)
    if reject:
        assert result.returncode == 1 and 'FAIL:' in result.stderr, str(log) + ': negative control did not fail'
    elif result.returncode:
        raise RuntimeError(str(log) + ': ' + result.stdout[-2000:] + result.stderr[-4000:])
    return result.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT / 'build/nov14-next6/baseline-src')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next6/spritecache')
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--negative-controls', action='store_true')
    ap.add_argument('--ee-abi', action='store_true')
    a = ap.parse_args()
    a.out = a.out.resolve(); a.out.mkdir(parents=True, exist_ok=True)
    before, after = parts(a.baseline), parts(ROOT)
    assert before['SC_INIT'] == after['SC_INIT'] and before['SC_GROW'] == after['SC_GROW'], 'reserve policy changed'
    report = {'reserve_policy_identical': True, 'sources': {}}
    for name in ('src/r_state.h', 'src/r_data.c', 'src/r_things.c', 'src/hardware/hw_main.c'):
        report['sources'][name] = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()

    def test(label, source, extra=(), reject=False):
        work = a.out / label; work.mkdir(exist_ok=True)
        defines = []
        for key, content in source.items():
            inc = work / (key.lower() + '.inc'); inc.write_text(content)
            defines.append('-D' + key + '="' + str(inc) + '"')
        exe = work / 'test'
        command = ['cc', '-std=gnu17', '-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-function',
                   '-Wno-clobbered', '-fwrapv', '-DPARANOIA', '-I' + str(ROOT / 'src'),
                   '-I' + str(ROOT / 'src/ps2'), *defines, *extra, TOOLS / 'next6_spritecache_hosttest.c', '-o', exe]
        if a.sanitize:
            # The unchanged reference shifts negative signed pixels. Preserve
            # its target wrapping semantics without normalizing its source.
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
            if label in ('baseline', 'old-switch'):
                command += ['-fno-sanitize=shift']
        run(command, work / 'build.log')
        text = run([exe], work / 'test.log', reject=reject)
        if reject: return None
        rows = [tuple(map(int, m[:-1])) + (m[-1],) for m in re.findall(
            r'^case n=(\d+) sides=(\d+) guard=(\d+) fragmented=(\d+) cap=(\d+) request=(\d+) used=(\d+) peak=(\d+) allocs=(\d+) moves=(\d+) hash=([0-9a-f]+)$', text, re.M)]
        assert len(rows) == 80
        tail = re.search(r'SPRCACHE6 DONE cases=(\d+) checks=(\d+) sizeof=(\d+) align=(\d+) hash=([0-9a-f]+)', text)
        assert tail
        result = dict(zip(('cases', 'checks', 'sizeof', 'align'), map(int, tail.groups()[:4])))
        result['hash'] = tail[5]
        result['rows'] = rows
        print(label, {k: v for k, v in result.items() if k != 'rows'}, flush=True)
        return result

    report['baseline'] = test('baseline', before)
    report['current'] = test('current', after)
    report['old_switch'] = test('old-switch', after, ['-DPS2_NOOPT_SPRCACHE'])
    for name in ('current', 'old_switch'):
        old, new = report['baseline'], report[name]
        assert old['hash'] == new['hash'], name + ' field digest differs'
        for reference, candidate in zip(old['rows'], new['rows']):
            assert reference[:5] == candidate[:5] and reference[-1] == candidate[-1]
            assert candidate[5] <= reference[5] and candidate[6] <= reference[6] and candidate[7] <= reference[7], (reference, candidate)
            assert candidate[8] <= reference[8], (reference, candidate)
            if name == 'old_switch': assert reference == candidate
    assert report['baseline']['sizeof'] == 16 and report['current']['sizeof'] == 8
    if a.negative_controls:
        controls = {
            'top-adjust': {**after, 'SC_WRITE': after['SC_WRITE'].replace('(UINT32)(FEETADJUST / FRACUNIT)', '0u')},
            'top-high-bit': {**after, 'SC_TYPES': after['SC_TYPES'].replace('(fixed_t)(c).topoffset_pixels', '(fixed_t)((c).topoffset_pixels & 0x7fff)')},
            'height-field': {**after, 'SC_TYPES': after['SC_TYPES'].replace('#define SPRCACHE_HEIGHT(c) ((fixed_t)(c).height_pixels', '#define SPRCACHE_HEIGHT(c) ((fixed_t)(c).width_pixels')},
            'reserve-count': {**after, 'SC_INIT': after['SC_INIT'].replace('max_spritelumps = 8192;', 'max_spritelumps = 4096;')},
        }
        for name, source in controls.items():
            assert source != after
            test('negative-' + name, source, reject=True)
        report['negative_controls_detected'] = list(controls)
    interface = {}
    for label, root in (('baseline', a.baseline), ('current', ROOT)):
        source = '#include "doomdef.h"\n#include "r_state.h"\n'
        p = subprocess.run(['cc', '-E', '-P', '-DNOHW', '-DHAVE_ZLIB', '-I' + str(root / 'src'),
                            '-I' + str(ROOT / 'build/host-gen'), '-x', 'c', '-'], input=source, text=True, capture_output=True)
        (a.out / (label + '-nonps2.i')).write_text(p.stdout)
        assert p.returncode == 0, p.stderr
        interface[label] = p.stdout
        if shutil.which('c++'):
            # r_state.h includes pre-existing C-only _Static_assert uses in
            # p_mobj.h. Check the actual changed type/getters as C++ directly.
            cpp_source = '#include <cstdint>\ntypedef int32_t fixed_t;\ntypedef int16_t INT16;\n#define FRACUNIT 65536\n' + parts(root)['SC_TYPES']
            cpp = subprocess.run(['c++', '-std=gnu++17', '-fsyntax-only', '-DPS2_PROFILE', '-x', 'c++', '-'],
                                 input=cpp_source, text=True, capture_output=True)
            (a.out / (label + '-cpp.log')).write_text(cpp.stdout + cpp.stderr)
            assert cpp.returncode == 0, cpp.stderr
    assert interface['baseline'] == interface['current'], 'non-PS2 header interface differs'
    report['nonps2_header_identical'] = True
    report['cpp_scope'] = 'actual extracted type/getters; full header has preceding C-only p_mobj.h assertions'
    if a.ee_abi:
        import build as B
        for label, root in (('baseline', a.baseline), ('current', ROOT)):
            work = a.out / label
            source = work / 'abi.c'; obj = work / 'abi.o'; binary = work / 'abi.bin'
            field = 'width' if label == 'baseline' else 'width_pixels'
            source.write_text('#include "doomdef.h"\n#include "r_state.h"\n#include <stddef.h>\n'
                              'typedef char fixed_width_check[(sizeof(fixed_t)==4 && FRACBITS==16)?1:-1];\n'
                              'typedef char signed_pixel_check[((INT16)(UINT16)65535==-1 && (INT16)(UINT16)32768==-32768)?1:-1];\n'
                              'const unsigned spritecache_abi[] __attribute__((section(".spritecache_abi"),used))={'
                              'sizeof(sprcache_t),__alignof__(sprcache_t),sizeof(((sprcache_t*)0)->' + field + ')};\n')
            flags = [f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP')]
            includes = ['-I' + str(root / 'src'), *[i for i in B.INCS if i != '-I' + str(ROOT / 'src')]]
            run([B.CC, *flags, *includes, '-c', source, '-o', obj], work / 'ee-abi-build.log')
            run([B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objcopy' + B.EXE), '-O', 'binary', '--only-section=.spritecache_abi', obj, binary], work / 'ee-abi-copy.log')
            report[label]['ee_abi'] = dict(zip(('sizeof', 'align', 'fieldbytes'), struct.unpack('<3I', binary.read_bytes()[:12])))
        assert report['baseline']['ee_abi'] == {'sizeof': 16, 'align': 4, 'fieldbytes': 4}
        assert report['current']['ee_abi'] == {'sizeof': 8, 'align': 2, 'fieldbytes': 2}
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('All 262144 signed header values and reserve/lifecycle digests equal; no measured arena peak increase', flush=True)


if __name__ == '__main__':
    main()
