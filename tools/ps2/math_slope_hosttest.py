"""Host test: integer slope-plane setup (src/r_slopeq.h, PS2_OPT) against the original double code of r_plane.c.

python tools/ps2/math_slope_hosttest.py [--out DIR] [--cases N] [--negative-controls]
The reference is extracted from `git show HEAD:src/r_plane.c` (R_GetSlopeZAt .. DoSlopeLightCrossProduct) so that it
cannot drift from the original. Negative controls compile deliberately broken copies of r_slopeq.h and must fail.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as C  # noqa: E402

ROOT = C.ROOT


def extract_reference(work):
    text = subprocess.run(['git', 'show', 'HEAD:src/r_plane.c'], cwd=ROOT, capture_output=True, check=True).stdout.decode('utf-8').replace('\r\n', '\n')
    start = text.index('// Returns the height of the sloped plane at (x, y) as a double')
    end = text.index('static void CalcSlopePlaneVectors(visplane_t *pl, fixed_t xoff, fixed_t yoff)\n{', start)
    body = text[start:end]
    (work / 'ref_slope.inc').write_text(body, encoding='utf-8')
    return body


def build(work, hdr_dir=None):
    work.mkdir(parents=True, exist_ok=True)
    extract_reference(work)
    incs = ['/I' + str(ROOT / 'src'), '/I' + str(ROOT / 'src/ps2'), '/I' + str(work)]
    if hdr_dir:
        incs.insert(0, '/I' + str(hdr_dir))
    units = [(ROOT / 'tools/ps2/math_slope_hosttest.c', 'main.obj',
              ['/DPS2_PROFILE', '/DPS2_OPT_SLOPE', '/DPS2_OPT_SEGS', *incs])]
    return C.msvc_build(work, 'math_slope_hosttest', units)


CONTROLS = {
    'cross-no-borrow': ('hi = h1 - h2 - (l1 < l2);', 'hi = h1 - h2;'),
    'zdelta-q48-as-q40': ('q->dz = RQ_FromDouble(zdelta, 48);', 'q->dz = RQ_FromDouble(zdelta, 40);'),
    'light-sign': ('lu.z = -cs;', 'lu.z = cs;'),
    'scaled-fine-angle': ('RQ_SinCos(RQ_Ang2Rad(plangle), &sinp, &cosp);', 'RQ_SinCos(RQ_Ang2Rad(plangle >> 19), &sinp, &cosp);'),
    'focal-on-wrong-axis': ('out->su[2] = RQ_CrossToFloat(o.x, v.y, o.y, v.x, ex, in->focallength);', 'out->su[2] = RQ_CrossToFloat(o.x, v.y, o.y, v.x, ex, 1.0f);'),
    'sincos-quadrant': ('case 1: *sinq = c; *cosq = -s; break;', 'case 1: *sinq = c; *cosq = s; break;'),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, default=ROOT / 'build/agent-b-opt/math-slope-hosttest')
    ap.add_argument('--cases', type=int, default=2000000)
    ap.add_argument('--negative-controls', action='store_true')
    ap.add_argument('--strict-equivalence', action='store_true', help='reject any difference; include near-parallel rays')
    a = ap.parse_args()
    a.out = a.out.resolve()
    exe = build(a.out / 'cand')
    args = [f'--cases={a.cases}'] + (['--strict-equivalence'] if a.strict_equivalence else [])
    rc, text = C.run(exe, args, log=a.out / 'test.log')
    print(text)
    if rc:
        print('candidate FAILED')
        return 1
    if a.negative_controls:
        src = (ROOT / 'src/r_slopeq.h').read_text().replace('\r\n', '\n')
        for name, (old, new) in CONTROLS.items():
            if src.count(old) != 1:
                print('control anchor not unique/missing:', name)
                return 1
            d = a.out / ('neg-' + name)
            (d / 'hdr').mkdir(parents=True, exist_ok=True)
            (d / 'hdr/r_slopeq.h').write_text(src.replace(old, new))
            exe = build(d, d / 'hdr')
            rc, text = C.run(exe, ['--cases=200000'], log=d / 'test.log')
            res = re.search(r'RESULT \w+', text)
            print(f'negative control {name}: exit={rc} {res.group(0) if res else text[-200:]}')
            if rc == 0:
                print('NEGATIVE CONTROL NOT DETECTED:', name)
                return 1
        print('all negative controls detected')
    return 0


if __name__ == '__main__':
    sys.exit(main())
