"""Frozen/current actual texture-to-flat pixel equality and output/source lifetime checks."""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as C


def converter(text):
    begin = text.index('void *Picture_TextureToFlat(size_t texnum)')
    end = text.index('\n#ifndef PS2_PROFILE', text.index('\n\treturn converted;', begin))
    return text[begin:end]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--base-src', type=Path, required=True)
    ap.add_argument('--candidate-file', type=Path, default=C.ROOT / 'src/r_picformats.c')
    args = ap.parse_args()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    hashes = {}
    for arch in ['x86', 'x64']:
        for variant, source in [('base', args.base_src / 'r_picformats.c'), ('cand', args.candidate_file)]:
            inc = work / (variant + '.inc')
            inc.write_text(converter(source.read_text()))
            flags = ['/I' + str(C.ROOT / 'src'), '/I' + str(C.ROOT / 'tools/ps2'),
                     '/DFLAT_SOURCE="' + inc.as_posix() + '"']
            dest = work / (arch + '-' + variant)
            exe = C.msvc_build(dest, 'flat', [(C.ROOT / 'tools/ps2/oct4_render_flat_hosttest.c', 'flat.obj', flags)],
                               arch=arch, cl_extra=['/WX', '/DPARANOIA'])
            rc, output = C.run(exe, log=dest / 'run.log')
            print(arch, variant, output.strip())
            if rc:
                raise SystemExit(rc)
            hashes[arch, variant] = re.search(r'hash=(\d+)', output)[1]
            rc, output = C.run(exe, ['broken'], log=dest / 'negative.log')
            if rc == 0:
                raise SystemExit('negative not detected')
        if hashes[arch, 'base'] != hashes[arch, 'cand']:
            raise SystemExit('pixel hashes differ')
    print('FLAT EQUALITY PASS; negative controls detected')


if __name__ == '__main__':
    main()
