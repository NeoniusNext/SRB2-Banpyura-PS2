#!/usr/bin/env python3
"""Build, never launch, a paired exact EE sprite-header probe and gather timing."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import build as B
from next6_spritecache_hosttest import parts

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def run(command, log):
    p = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=120)
    log.write_text(' '.join(map(str, command)) + '\n' + p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(str(log) + ': ' + p.stdout[-1000:] + p.stderr[-3000:])
    return p.stdout


def source(root, prefix):
    extracted = parts(root)
    integer = re.search(r'\tspr_width = [^\n]+\n\tspr_height = [^\n]+\n\tspr_offset = [^\n]+\n\tspr_topoffset = [^\n]+', (root / 'src/r_things.c').read_text())
    floating = re.search(r'\t+w = FIXED_TO_FLOAT[^\n]+\n\t+off = FIXED_TO_FLOAT[^\n]+\n\t+h = FIXED_TO_FLOAT[^\n]+\n\t+top = FIXED_TO_FLOAT[^\n]+', (root / 'src/hardware/hw_main.c').read_text())
    assert integer and floating
    result = '#include <stdint.h>\n#include <stddef.h>\ntypedef int16_t INT16;\ntypedef uint16_t UINT16;\ntypedef uint32_t UINT32;\ntypedef int32_t fixed_t;\n'
    result += '#define FRACBITS 16\n#define FRACUNIT 65536\n#define FEETADJUST (4*FRACUNIT)\n#define FIXED_TO_FLOAT(x) ((x)/(float)FRACUNIT)\n'
    result += extracted['SC_TYPES']
    result += 'static sprcache_t storage[8192];\nstatic sprcache_t *spritecachedinfo=storage;\nstatic size_t numspritelumps;\n'
    result += 'void ' + prefix + '_write(unsigned id, INT16 width, INT16 height, INT16 topoffset, INT16 leftoffset) { numspritelumps=id;\n' + extracted['SC_WRITE'] + '\n}\n'
    result += 'void ' + prefix + '_integer(unsigned lump, fixed_t out[4]) { fixed_t spr_width,spr_height,spr_offset,spr_topoffset;\n' + integer[0] + '\nout[0]=spr_width;out[1]=spr_height;out[2]=spr_offset;out[3]=spr_topoffset;\n}\n'
    result += 'void ' + prefix + '_float(unsigned id, float out[4]) { float w,off,h,top;\n' + floating[0] + '\nout[0]=w;out[1]=h;out[2]=off;out[3]=top;\n}\n'
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT / 'build/nov14-next6/baseline-src')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next6/spritecache-ee')
    ap.add_argument('--host-only', action='store_true')
    a = ap.parse_args(); a.out = a.out.resolve(); a.out.mkdir(parents=True, exist_ok=True)
    report = {'scope': 'actual header writer and four-field fixed/float gathers, separate translation units without LTO'}
    sources = []
    for label, root in (('reference', a.baseline), ('candidate', ROOT)):
        path = a.out / (label + '.c'); path.write_text(source(root, label)); sources.append(path)
    driver = TOOLS / 'next6_spritecache_eetest.c'
    host = a.out / 'host'
    run(['cc', '-O2', '-std=gnu17', '-D_POSIX_C_SOURCE=200809L', '-DPS2_PROFILE', '-Wall', '-Wextra', '-Werror',
         '-fwrapv', *sources, driver, '-o', host], a.out / 'host-build.log')
    text = run([host], a.out / 'host.log')
    assert 'SPRCACHE6 EE DONE' in text and 'SPRCACHE6 EE FAIL' not in text
    report['host_exact'] = next(line for line in text.splitlines() if line.startswith('SPRCACHE6 EE EXACT'))
    if not a.host_only:
        flags = [f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP')]
        objs = []
        for path in [*sources, driver]:
            obj = a.out / (path.stem + '.o')
            run([B.CC, *flags, '-Werror', *B.INCS, '-c', path, '-o', obj], obj.with_suffix('.build.log'))
            objs.append(obj)
            run([B.DEV / ('ee/bin/mips64r5900el-ps2-elf-objdump' + B.EXE), '-dr', obj], obj.with_suffix('.asm'))
        elf = a.out / 'SPRCACHE.ELF'
        run([B.CC, *B.LDFLAGS, *objs, '-o', elf, '-ldebug', '-lpatches', '-lm'], a.out / 'link.log')
        report['elf_sha256'] = hashlib.sha256(elf.read_bytes()).hexdigest()
        negative_source = a.out / 'candidate-negative.c'
        negative_source.write_text(sources[1].read_text().replace('#define SPRCACHE_HEIGHT(c) ((fixed_t)(c).height_pixels', '#define SPRCACHE_HEIGHT(c) ((fixed_t)(c).width_pixels'))
        assert negative_source.read_text() != sources[1].read_text()
        negative_obj = a.out / 'candidate-negative.o'
        run([B.CC, *flags, '-Werror', *B.INCS, '-c', negative_source, '-o', negative_obj], a.out / 'negative-build.log')
        negative_elf = a.out / 'SPRCACHE-NEGATIVE.ELF'
        run([B.CC, *B.LDFLAGS, objs[0], negative_obj, objs[2], '-o', negative_elf, '-ldebug', '-lpatches', '-lm'], a.out / 'negative-link.log')
        report['negative_elf_sha256'] = hashlib.sha256(negative_elf.read_bytes()).hexdigest()
        print('Built', elf, 'and', negative_elf, flush=True)
    (a.out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
