"""Compile actual source against the pre-edit drawers; complete/truncated post boundaries."""
import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as C
from rdraw_bench import lighting


def function(text, name):
    begin = text.index('void ' + name + '(void)')
    brace = text.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end].replace(name, 'Ref_' + name)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--base-src', type=Path, required=True)
    ap.add_argument('--negative-control', action='store_true')
    args = ap.parse_args()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = (args.base_src / 'r_draw8.c').read_text()
    names = ['R_DrawColumnClamped_8', 'R_DrawTranslucentColumnClamped_8']
    (work / 'reference.inc').write_text('\n'.join(function(source, n) for n in names))
    (work / 'lighting.inc').write_text(lighting((C.ROOT / 'src/r_draw.c').read_text()))
    extra = ['/DPS2_PROFILE', '/I' + str(work), '/I' + str(C.ROOT / 'tools/ps2'), '/I' + str(C.ROOT / 'src')]
    for arch in ['x86', 'x64']:
        exe = C.msvc_build(work / arch, 'columns', [(C.ROOT / 'tools/ps2/next_render_columns_hosttest.c', 'columns.obj', extra)], arch=arch, cl_extra=['/WX'])
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
