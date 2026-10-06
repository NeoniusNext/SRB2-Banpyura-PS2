"""Compile actual visplane types/functions; compare clip/split/reuse output and guarded width switches."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

import math_common as C


def function(text, name):
    match = re.search(r'(?:static )?(?:void|visplane_t \*)\s*' + name + r'\([^)]*\)\s*\{', text)
    if not match:
        raise ValueError(name)
    end, depth = match.end(), 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def build(out, reference=False, broken=None, arch='x64'):
    out.mkdir(parents=True, exist_ok=True)
    texts = {}
    for name in ('r_plane.c', 'r_plane.h'):
        texts[name] = (subprocess.run(['git', 'show', 'HEAD:src/' + name], capture_output=True,
                                      check=True, cwd=C.ROOT).stdout.decode() if reference
                       else (C.ROOT / 'src' / name).read_text())
    header = texts['r_plane.h']
    typedef = header[header.index('typedef struct visplane_s'):header.index('} visplane_t;') + len('} visplane_t;')]
    source = texts['r_plane.c']
    reset = ('static void R_ResetPlaneClip(visplane_t *pl) { memset(pl->top, 0xff, sizeof pl->top); '
             'memset(pl->bottom, 0, sizeof pl->bottom); }' if reference else function(source, 'R_ResetPlaneClip'))
    bodies = [function(source, 'new_visplane'), reset]
    bodies += [function(source, name) for name in ('R_ClearPlanes', 'R_CheckPlane', 'R_ExpandPlane', 'R_PlaneBounds')]
    (out / 'plane_type.inc').write_text(typedef)
    generated = '\n'.join(bodies)
    if broken == 'overlap-pad':
        generated = generated.replace('check->bottom = check->top + strip;', 'check->bottom = check->top + strip - 1;')
    elif broken == 'skip-growth':
        generated = generated.replace('check->clipwidth < viewwidth', '0')
    (out / 'planes.inc').write_text(generated)
    flags = ['/DPS2_PROFILE', '/I' + str(C.ROOT / 'src'), '/I' + str(out)]
    if not reference:
        flags += ['/DPLANE_DYNAMIC']
    # Engine clip-index narrowing is intentional; all new allocations compile with /WX.
    return C.msvc_build(out, 'plane_bank_hosttest', [(C.ROOT / 'tools/ps2/plane_bank_hosttest.c', 'main.obj', flags)],
                        arch=arch, cl_extra=['/WX'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    out = args.out.resolve()
    report = {}
    passed = True
    for arch in ('x64', 'x86'):
        dumps = {}
        for tag in ('reference', 'candidate'):
            work = out / arch / tag
            exe = build(work, reference=tag == 'reference', arch=arch)
            dump = work / 'clips.bin'
            rc, result = C.run(exe, [str(dump)], log=work / 'test.log')
            print(arch, tag, result.strip())
            if rc:
                raise RuntimeError(f'{work}: exit {rc}')
            dumps[tag] = dump.read_bytes()
        same = bool(dumps['reference']) and dumps['reference'] == dumps['candidate']
        row = {'equal': same, 'bytes': len(dumps['reference']),
               'sha256': {tag: hashlib.sha256(data).hexdigest() for tag, data in dumps.items()}}
        passed &= same
        if args.negative_controls:
            row['negative_controls'] = {}
            for broken in ('overlap-pad', 'skip-growth'):
                work = out / arch / broken
                exe = build(work, broken=broken, arch=arch)
                rc, result = C.run(exe, [str(work / 'clips.bin')], log=work / 'test.log')
                print(arch, broken, rc, result.strip())
                row['negative_controls'][broken] = rc
                passed &= rc != 0
        report[arch] = row
    report['passed'] = passed
    (out / 'report.json').write_text(json.dumps(report, indent=2))
    print('PLANE RESULT', report)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
