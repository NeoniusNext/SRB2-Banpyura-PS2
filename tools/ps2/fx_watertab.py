"""OPT11-FX: the table of the final water check: for every scene of fx_water.sh the picture of the PC (build/ref/fx_<scene>), the PS2 with the OPT10 water
(runs <tagA>_<scene>) and with the new water (runs <tagB>_<scene>): MAD of the whole picture, MAD of the region of the water, edge correlation of the water region.

usage: fx_watertab.py TAGB TAGA      (e.g. a3 a3o)
The water region of a scene is a fixed box per scene (BOX below).
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
# the lower part of the picture is the water in these views (320x200)
BOX = {'g2': (0, 70, 320, 130), 'g3': (0, 60, 320, 140), 'grid_s5': (0, 110, 320, 90), 'dsz_e': (0, 110, 320, 90), 'dsz_w': (0, 110, 320, 90), 'dsz_n15': (0, 110, 320, 90),
       'dsz_s5': (0, 110, 320, 90), 'under1': (0, 70, 320, 130), 'under2': (0, 70, 320, 130)}


def grad(a):
    l = a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11
    return np.abs(np.diff(l, axis=1))[:-1, :] + np.abs(np.diff(l, axis=0))[:, :-1]


def load_hw(tag, scene):
    d = ROOT / 'build/runs' / f'fx_{tag}_{scene}'
    f = sorted(d.glob('vidshot-*.ppm'), key=lambda x: int(x.stem.split('_')[-1]))
    return np.asarray(Image.open(f[-1]).convert('RGB')).astype(np.float32) if f else None


def main():
    tb, ta = sys.argv[1], sys.argv[2]
    print('scene       MAD all  A(OPT10) B(new) | MAD water A / B | edge corr water A / B')
    for sc, (x, y, w, h) in BOX.items():
        ref = sorted((ROOT / 'build/ref' / f'fx_{sc}').glob('shot-*.png'), key=lambda p: int(p.stem.split('-')[1]))
        a, b = load_hw(ta, sc), load_hw(tb, sc)
        if not ref or a is None or b is None:
            print(f'{sc:10s} missing')
            continue
        pc = np.asarray(Image.open(ref[-1]).convert('RGB')).astype(np.float32)
        r = (slice(y, y + h), slice(x, x + w))
        m = lambda p, q: float(np.abs(p - q).mean())
        e = lambda p, q: float(np.corrcoef(grad(p[r]).ravel(), grad(q[r]).ravel())[0, 1])
        print(f'{sc:10s}  {m(pc, a):6.2f} {m(pc, b):6.2f}  | {m(pc[r], a[r]):6.2f} {m(pc[r], b[r]):6.2f}  | {e(pc, a):.3f} {e(pc, b):.3f}')


if __name__ == '__main__':
    main()
