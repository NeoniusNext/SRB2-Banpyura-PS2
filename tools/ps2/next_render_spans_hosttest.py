"""Actual NPOT drawer equivalence at framebuffer boundaries, with guarded extra bytes."""
import argparse
from pathlib import Path
import re
import math_common as C
from rdraw_bench import lighting


def function(text, name):
    match = re.search(r'void\s+' + name + r'\s*\(void\)\s*\{', text)
    end, depth = match.end(), 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end].replace(name, 'Ref_' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--base-src', type=Path, required=True)
    parser.add_argument('--negative-control', action='store_true')
    args = parser.parse_args()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    text = (args.base_src / 'r_draw8_npo2.c').read_text()
    names = ['R_Draw' + middle + '_NPO2_8' for middle in ['Span', 'TranslucentSpan', 'WaterSpan', 'Splat',
                                                      'TranslucentSplat', 'FloorSprite', 'TranslucentFloorSprite']]
    (work / 'reference.inc').write_text('\n'.join(function(text, name) for name in names))
    (work / 'lighting.inc').write_text(lighting((C.ROOT / 'src/r_draw.c').read_text()))
    for arch in ['x86', 'x64']:
        flags = ['/DPS2_PROFILE', '/I' + str(work), '/I' + str(C.ROOT / 'tools/ps2'), '/I' + str(C.ROOT / 'src')]
        exe = C.msvc_build(work / arch, 'spans', [(C.ROOT / 'tools/ps2/next_render_spans_hosttest.c', 'main.obj', flags)],
                           arch=arch, cl_extra=['/WX'])
        rc, result = C.run(exe, log=work / arch / 'run.log')
        print(arch, rc, result.strip())
        if rc:
            raise SystemExit(rc)
        if args.negative_control:
            rc, result = C.run(exe, ['broken'], log=work / arch / 'negative.log')
            print(arch, 'negative detected' if rc == 1 else 'negative FAILED')
            if rc != 1:
                raise SystemExit(1)


if __name__ == '__main__':
    main()
