"""Build actual translucent drawers and independently frozen functions with MSVC x86/x64."""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as C
from next_render_columns_hosttest import function
from rdraw_bench import lighting


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--base-src', type=Path, required=True)
    ap.add_argument('--negative-control', action='store_true')
    args = ap.parse_args()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = (args.base_src / 'r_draw8.c').read_text()
    names = ['R_DrawTranslucentColumn_8', 'R_DrawTranslucentColumnClamped_8',
             'R_DrawTranslatedTranslucentColumn_8', 'R_Draw2sMultiPatchColumn_8',
             'R_Draw2sMultiPatchTranslucentColumn_8']
    # Rename calls too: clamped full posts must call the frozen ordinary drawer.
    chunks = []
    for name in names:
        chunk = function(source, name)
        for other in names:
            chunk = re.sub(r'(?<!Ref_)\b' + other + r'\b', 'Ref_' + other, chunk)
        chunks.append(chunk)
    (work / 'reference.inc').write_text('\n'.join(chunks))
    (work / 'lighting.inc').write_text(lighting((C.ROOT / 'src/r_draw.c').read_text()))
    extra = ['/DPS2_PROFILE', '/I' + str(work), '/I' + str(C.ROOT / 'tools/ps2'), '/I' + str(C.ROOT / 'src')]
    for arch in ['x86', 'x64']:
        exe = C.msvc_build(work / arch, 'columns',
                           [(C.ROOT / 'tools/ps2/oct4_render_columns_hosttest.c', 'columns.obj', extra)],
                           arch=arch, cl_extra=['/WX'])
        rc, output = C.run(exe, log=work / arch / 'run.log')
        print(arch, output.strip())
        if rc:
            raise SystemExit(rc)
        if args.negative_control:
            rc, output = C.run(exe, ['broken'], log=work / arch / 'negative.log')
            print(arch, 'negative detected' if rc == 1 else 'negative FAILED')
            if rc != 1:
                raise SystemExit(1)


if __name__ == '__main__':
    main()
