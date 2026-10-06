"""Actual drawseg type, lazy frontscale allocator and growth, forced moving reallocs."""
import argparse
import re
from pathlib import Path
import math_common as C


def braces(text, start):
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    header = (C.ROOT / 'src/r_defs.h').read_text()
    begin = header.index('typedef struct drawseg_s')
    datatype = header[begin:header.index('} drawseg_t;', begin) + len('} drawseg_t;')]
    reference = re.sub(r'#ifdef PS2_PROFILE\s+fixed_t \*frontscale;[^\n]*\n\s+INT32 frontscalewidth;\s+#else\s+fixed_t frontscale\[MAXVIDWIDTH\];\s+#endif',
                       'fixed_t frontscale[MAXVIDWIDTH];', datatype)
    if reference == datatype:
        raise RuntimeError('lazy frontscale type not found')
    reference = reference.replace('drawseg_s', 'inline_drawseg_s').replace('drawseg_t', 'inline_drawseg_t')
    (out / 'drawseg_type.inc').write_text(datatype + '\n' + reference)
    source = (C.ROOT / 'src/r_segs.c').read_text()
    allocate = braces(source, source.index('void R_AllocDrawSegFrontScale'))
    grow = braces(source, source.index('if (ds_p == drawsegs+maxdrawsegs)'))
    vertexend = header.index('} vertex_t;') + len('} vertex_t;')
    vertexstart = header.rfind('typedef struct', 0, vertexend)
    (out / 'vertex_type.inc').write_text(header[vertexstart:vertexend])
    for variant in ['candidate', 'drop-zero-firstseg', 'reset-stale']:
        if variant != 'candidate' and not args.negative_controls:
            continue
        functions = allocate + '\nstatic void grow_drawsegs(void) {\n' + grow + '\n}\n'
        if variant == 'drop-zero-firstseg':
            functions = functions.replace('if (hadfirstseg)', 'if (hadfirstseg && firstpos)')
        elif variant == 'reset-stale':
            functions = functions.replace('if (ds->frontscalewidth < viewwidth)', 'if (ds->frontscale) memset(ds->frontscale, 0, (size_t)ds->frontscalewidth * sizeof (*ds->frontscale));\n\tif (ds->frontscalewidth < viewwidth)')
        work = out / variant
        work.mkdir(exist_ok=True)
        (work / 'drawseg_functions.inc').write_text(functions)
        for arch in ['x86', 'x64']:
            flags = ['/DPS2_PROFILE', '/I' + str(C.ROOT / 'src'), '/I' + str(out), '/I' + str(work)]
            exe = C.msvc_build(work / arch, 'drawsegs', [(C.ROOT / 'tools/ps2/next_render_drawseg_hosttest.c', 'main.obj', flags)],
                               arch=arch, cl_extra=['/WX'])
            rc, result = C.run(exe, log=work / arch / 'run.log')
            print(variant, arch, rc, result.strip())
            if variant == 'candidate' and rc:
                raise SystemExit(rc)
            if variant != 'candidate' and rc != 1:
                raise SystemExit('negative control failed')


if __name__ == '__main__':
    main()
