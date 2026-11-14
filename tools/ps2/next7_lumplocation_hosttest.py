#!/usr/bin/env python3
"""Exact preceding/current lump locations, real constructors/readers/arena proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def function(text, start):
    begin = text.index(start)
    first = text.index('{', begin)
    depth, end = 1, first + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end] + '\n'


def parts(root):
    h = (root / 'src/w_wad.h').read_text()
    w = (root / 'src/w_wad.c').read_text()
    f = (root / 'src/filesrch.c').read_text()
    doom = (root / 'src/doomdef.h').read_text()
    functions = [
        'void W_Shutdown(void)',
        'static lumpinfo_t* ResGetLumpsStandalone',
        'static lumpinfo_t* ResGetLumpsWad',
        'static boolean ResFindSignature',
        'static lumpinfo_t* ResGetLumpsZipFast',
        'static boolean W_ResolveZipPosition',
        'static boolean TestValidLump',
        'static void W_BuildNameIndex',
        'static UINT16 W_FindName8',
        'UINT16 W_CheckNumForNamePwad',
        'UINT16 W_CheckNumForLongNamePwad',
        'UINT16 W_CheckNumForFullNamePK3',
        'size_t W_LumpLengthPwad',
        'void zerr(int ret)',
    ]
    production = function(doom, 'UINT32 quickncasehash')
    production += '#ifdef PS2_PROFILE\n' + section(w, '#define ZIP_LAZYPOS', '/** Create a lumpinfo_t array for a PKZip file.')
    production += section(f, '#define dirpathlen', 'filestatus_t filesearch')
    production += ''.join(function(f, name) for name in (
        'static void initdirpath', 'static int lumpnamecompare', 'lumpinfo_t *getdirectoryfiles'))
    production += ''.join(function(w, name) for name in functions)
    # Skip the forward declaration and extract the actual full reader.
    start = w.index('static size_t W_ReadLumpHeaderPwad_', w.index('static struct { UINT16 wad, lump;'))
    production += function(w[start:], 'static size_t W_ReadLumpHeaderPwad_')
    types = section(h, 'typedef struct\n{\n\tUINT32 filepos;', '// Available compression methods')
    types += section(h, 'typedef enum\n{\n\tCM_NOCOMPRESSION',
                     '// =========================================================================\n//                         \'VIRTUAL\' RESOURCES')
    wadtype = section(h, '#define lumpcache_t', '#define WADFILENUM')
    return {'LL_TYPES': types, 'LL_WADTYPE': wadtype, 'LL_PRODUCTION': production}


def source_byte(ident, offset):
    return ((offset * 37 + ident * 17) ^ (offset >> 7)) & 255


def fixtures(out):
    out.mkdir(exist_ok=True)
    folder = out / 'folder'
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir()
    names = ['Empty.bin', 'RootNameWithoutAnExtension', 'MixCase/AbCd1234.long.ext',
             'Dup/Test.bin', 'dup/test.BIN', 'Folder/Tiny.bin']
    sizes = [0, 1, 257, 4097, 65537, 80000]
    records = [(name, bytes(source_byte(i + 1, j) for j in range(sizes[i])), i + 1)
               for i, name in enumerate(names)]
    for name, data, _ in records:
        p = folder / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def oracle(name, rows):
        (out / (name + '.oracle')).write_text(''.join(
            f'{filename}\t{len(data)}\t{ident}\n' for filename, data, ident in rows))

    oracle('folder', sorted(records))
    for compressed, filename in [(False, 'plain.wad'), (True, 'compressed.wad')]:
        payload, entries, rows = bytearray(), [], []
        for i, (_, data, ident) in enumerate(records):
            name = f'ENTRY{i:03}'
            if compressed:
                # Independent literal-only LZF stream, valid even when it does
                # not save disk space; decoding still uses production lzf.c.
                encoded = b''.join(bytes([len(data[j:j + 32]) - 1]) + data[j:j + 32]
                                   for j in range(0, len(data), 32))
                stored = struct.pack('<I', len(data)) + encoded
            else:
                stored = data
            position = 12 + len(payload)
            # A zero-size raw record also checks all 32 bits of filepos without
            # seeking the deliberately absent payload.
            if not compressed and not data:
                position = 0xFFFFFFFF
            entries.append(struct.pack('<II8s', position, len(stored), name.encode()))
            rows.append((name, data, ident))
            payload += stored
        (out / filename).write_bytes(struct.pack('<4sII', b'ZWAD' if compressed else b'PWAD',
                                                len(entries), 12 + len(payload)) + payload + b''.join(entries))
        oracle(filename, rows)
    with zipfile.ZipFile(out / 'mixed.pk3', 'w') as archive:
        for i, (name, data, _) in enumerate(records):
            info = zipfile.ZipInfo(name)
            info.compress_type = zipfile.ZIP_DEFLATED if i & 1 else zipfile.ZIP_STORED
            info.extra = b'\xfe\xca\x03\x00XYZ'
            info.comment = b'per-entry fixture comment'
            archive.writestr(info, data)
        archive.comment = b'central directory fixture comment'
    oracle('mixed.pk3', records)
    data = bytes(source_byte(9, j) for j in range(8193))
    (out / 'standalone.soc').write_bytes(data)
    oracle('standalone.soc', [('OBJCTCFG', data, 9)])
    return out


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
    ap.add_argument('--baseline', type=Path, default=ROOT / 'build/nov14-next7/baseline-src')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next7/memory-location')
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--negative-controls', action='store_true')
    a = ap.parse_args()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    before, after = parts(a.baseline), parts(ROOT)
    fixture_dir = fixtures(a.out / 'fixtures')
    report = {'sources': {}}
    for name in ('src/w_wad.h', 'src/w_wad.c', 'src/w_pack.c', 'src/filesrch.c',
                 'src/ps2/ps2_texc.c', 'src/ps2/i_sound.c'):
        report['sources'][name] = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()

    def test(label, source, extra=(), reject=False):
        work = a.out / label
        work.mkdir(exist_ok=True)
        defines = []
        for key, content in source.items():
            inc = work / (key.lower() + '.inc')
            inc.write_text(content)
            defines.append('-D' + key + '="' + str(inc) + '"')
        exe = work / 'test'
        command = ['cc', '-std=gnu17', '-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-function',
                   '-Wno-clobbered', '-Wno-sign-compare', '-fwrapv', '-DPARANOIA',
                   '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/ps2'), *defines, *extra,
                   TOOLS / 'next7_lumplocation_hosttest.c', '-lz', '-o', exe]
        if a.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        run(command, work / 'build.log')
        text = run([exe, fixture_dir], work / 'test.log', reject=reject)
        if reject:
            return None
        rows = [tuple(map(int, m[:-1])) + (m[-1],) for m in re.findall(
            r'^case n=(\d+) guard=(\d+) sides=(\d+) fragmented=(\d+) request=(\d+) used=(\d+) peak=(\d+) hash=([0-9a-f]+)$', text, re.M)]
        constructors = [(*m[:1], *map(int, m[1:-1]), m[-1]) for m in re.findall(
            r'^constructor name=(\S+) guard=(\d+) sides=(\d+) count=(\d+) calls=(\d+) used=(\d+) peak=(\d+) hash=([0-9a-f]+)$', text, re.M)]
        assert len(rows) == 176 and len(constructors) == 20, (len(rows), len(constructors))
        tail = re.search(r'LUMPLOCATION7 DONE cases=176 constructors=20 checks=(\d+) sizeof=(\d+) align=(\d+) hash=([0-9a-f]+)', text)
        assert tail, text[-1000:]
        result = dict(zip(('checks', 'sizeof', 'align'), map(int, tail.groups()[:3])))
        result.update(hash=tail[4], rows=rows, constructors=constructors)
        print(label, {k: v for k, v in result.items() if k not in ('rows', 'constructors')}, flush=True)
        return result

    report['baseline'] = test('baseline', before)
    report['current'] = test('current', after)
    report['old-switch'] = test('old-switch', after, ['-DPS2_NOOPT_LUMPLOCATION'])
    assert report['baseline']['hash'] == report['current']['hash'] == report['old-switch']['hash']
    assert report['baseline'] == report['old-switch'], 'old switch must preserve every preceding allocator result'
    for old, new in zip(report['baseline']['rows'], report['current']['rows']):
        assert old[:4] == new[:4] and old[-1] == new[-1], (old, new)
        assert all(b <= a for a, b in zip(old[4:7], new[4:7])), (old, new)
    for old, new in zip(report['baseline']['constructors'], report['current']['constructors']):
        assert old[:5] == new[:5] and old[-1] == new[-1], (old, new)
        assert all(b <= a for a, b in zip(old[5:7], new[5:7])), (old, new)
    if a.negative_controls:
        controls = {
            'folder-position': ('LL_TYPES', 'return lump->location_is_path ? 0 : lump->location.filepos;',
                                'return lump->location.filepos;'),
            'archive-null-store': ('LL_TYPES', 'else if (lump->location_is_path)', 'else'),
            'folder-tag': ('LL_TYPES', 'lump->location_is_path = 1;', 'lump->location_is_path = 0;'),
            'truncated-position': ('LL_TYPES', 'return lump->location_is_path ? 0 : lump->location.filepos;',
                                   'return lump->location_is_path ? 0 : (UINT16)lump->location.filepos;'),
            'folder-zero-store': ('LL_TYPES', 'if (position || !lump->location_is_path)', 'if (1)'),
        }
        for name, (key, old, new) in controls.items():
            mutated = dict(after)
            assert old in mutated[key], name
            mutated[key] = mutated[key].replace(old, new)
            test('negative-' + name, mutated, reject=True)
        report['negative_controls'] = list(controls)
    # Public non-PS2 type and header interface remain exactly the same after
    # preprocessing; macro-only call-site adapters add no declarations there.
    interfaces = {}
    for label, root in [('baseline', a.baseline.resolve()), ('current', ROOT)]:
        p = subprocess.run(['cc', '-E', '-P', '-DNOHW', '-DHAVE_ZLIB', '-I' + str(root / 'src'),
                            '-I' + str(ROOT / 'build/host-gen'), '-x', 'c', '-'],
                           input='#include "doomdef.h"\n#include "w_wad.h"\n', text=True, capture_output=True)
        (a.out / (label + '-nonps2.i')).write_text(p.stdout)
        assert p.returncode == 0, p.stderr
        interfaces[label] = p.stdout
        if shutil.which('c++'):
            p = subprocess.run(['c++', '-std=gnu++17', '-fsyntax-only', '-DPS2_PROFILE', '-DNOHW', '-DHAVE_ZLIB',
                                '-I' + str(root / 'src'), '-I' + str(ROOT / 'build/host-gen'), '-x', 'c++', '-'],
                               input='#include "doomdef.h"\n#include "w_wad.h"\n', text=True, capture_output=True)
            (a.out / (label + '-cpp.log')).write_text(p.stdout + p.stderr)
            assert p.returncode == 0, p.stderr
    assert interfaces['baseline'] == interfaces['current'], 'non-PS2 header interface differs'
    report['nonps2_interface_identical'] = True
    report['cpp_header_syntax_checked'] = bool(shutil.which('c++'))
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('All semantic/read digests equal; no measured arena request/peak increase', flush=True)


if __name__ == '__main__':
    main()
