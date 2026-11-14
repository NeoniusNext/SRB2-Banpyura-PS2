#!/usr/bin/env python3
"""Serial real EE objects for the default/old-switch sprite-header layouts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import build as B

ROOT = Path(__file__).resolve().parents[2]


def run(command, log):
    p = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True, timeout=240)
    log.write_text(' '.join(map(str, command)) + '\n' + p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(str(log) + ': ' + p.stdout[-1000:] + p.stderr[-3000:])
    return p.stdout


def sections(path):
    data = path.read_bytes()
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    assert header[0][:5] == b'\x7fELF\x01'
    off, stride, count, names_id = header[6], header[11], header[12], header[13]
    entries = [struct.unpack_from('<10I', data, off + i * stride) for i in range(count)]
    names = data[entries[names_id][4]:entries[names_id][4] + entries[names_id][5]]
    return {names[e[0]:names.index(b'\0', e[0])].decode(): (e[5], data[e[4]:e[4]+e[5]])
            for e in entries if e[0]}


def summary(items):
    return {kind: sum(size for name, (size, _) in items.items() if name.startswith(prefixes))
            for kind, prefixes in [('text', ('.text',)), ('rodata', ('.rodata',)),
                                   ('data', ('.data', '.sdata')), ('bss', ('.bss', '.sbss'))]}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT / 'build/nov14-next6/baseline-src')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next6/spritecache-objects')
    a = ap.parse_args(); a.out = a.out.resolve(); a.out.mkdir(parents=True, exist_ok=True)
    report = {'scope': 'real non-LTO translation units; same release EE flags and per-unit policies', 'objects': {}}
    baseflags = [f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP')]
    for mode in ('software', 'hardware'):
        modeflags = baseflags if mode == 'software' else [f for f in baseflags if f != '-DNOHW'] + ['-DHWRENDER', '-Werror', '-Wno-format-truncation']
        sources = ['src/r_data.c', 'src/r_things.c'] + (['src/hardware/hw_main.c'] if mode == 'hardware' else [])
        for name in sources:
            key = mode + '/' + name
            objs = {}
            for label, root, extra in (('reference', a.baseline, []), ('candidate', ROOT, []), ('old-switch', ROOT, ['-DPS2_NOOPT_SPRCACHE'])):
                work = a.out / mode / label; work.mkdir(parents=True, exist_ok=True)
                obj = work / (Path(name).stem + '.o')
                includes = ['-I' + str(root / 'src'), *[i for i in B.INCS if i != '-I' + str(ROOT / 'src')]]
                run([B.CC, *modeflags, *B.UNIT_FLAGS.get(name, []), *extra, *includes, '-c', root / name, '-o', obj], obj.with_suffix('.build.log'))
                contents = sections(obj); objs[label] = contents
                asm = run([B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objdump' + B.EXE), '-dr', obj], obj.with_suffix('.asm'))
                funcs = {}
                for match in re.finditer(r'^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)', asm, re.M | re.S):
                    if match[1].startswith(('R_AddSingleSpriteDef', 'R_AddSprites', 'R_ProjectSprite', 'R_ProjectPrecipitationSprite',
                                            'HWR_ProjectPlain', 'HWR_ProjectSprite', 'HWR_ProjectPrecipitationSprite', 'HWR_FX_ExtMake')):
                        frame = re.search(r'addiu\s+sp,sp,-(\d+)', match[2])
                        funcs[match[1]] = {'text_bytes': contents['.text.' + match[1]][0],
                                          'frame': int(frame[1]) if frame else 0,
                                          'lh_instructions': len(re.findall(r'\s+lh\s', match[2])),
                                          'sll16_instructions': len(re.findall(r'\bsll\s+[^\n]*,0x10\b', match[2]))}
                report['objects'].setdefault(key, {})[label] = {'totals': summary(contents), 'functions': funcs,
                                                              'object_sha256': hashlib.sha256(obj.read_bytes()).hexdigest()}
                print(key, label, summary(contents), flush=True)
            compare = ('.text', '.rodata', '.data', '.sdata', '.bss', '.sbss')
            old = {k: v for k, v in objs['reference'].items() if k.startswith(compare)}
            switched = {k: v for k, v in objs['old-switch'].items() if k.startswith(compare)}
            assert old == switched, key + ': old-switch code/data differs from preceding production'
            report['objects'][key]['old_switch_all_code_data_identical'] = True
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
