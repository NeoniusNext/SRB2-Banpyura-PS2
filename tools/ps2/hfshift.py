"""OPT10-HF: sub-pixel registration of the PS2-HW picture against the PC-OpenGL one, per region.

usage: hfshift.py --pc a.png --hw b.ppm [--region name=y0,y1,x0,x1 ...]
For every region the HW picture is sampled with a bilinear shift (fx, fy in 1/4 pixel steps, +-1 px) and the shift with the smallest mean absolute
difference of the luminance is printed with the MAD at shift 0 for comparison: a systematic half-pixel offset of the HW pipeline shows up as the same
best shift in every region (HUD, walls, floor).
"""
import argparse

import numpy as np
from PIL import Image


def shifted(a, fx, fy):
    h, w = a.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    xs = np.clip(xs + fx, 0, w - 1.001)
    ys = np.clip(ys + fy, 0, h - 1.001)
    x0 = np.floor(xs).astype(int)
    y0 = np.floor(ys).astype(int)
    tx = xs - x0
    ty = ys - y0
    return a[y0, x0] * (1 - tx) * (1 - ty) + a[y0, x0 + 1] * tx * (1 - ty) + a[y0 + 1, x0] * (1 - tx) * ty + a[y0 + 1, x0 + 1] * tx * ty


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pc', required=True)
    ap.add_argument('--hw', required=True)
    ap.add_argument('--region', action='append', default=[])
    a = ap.parse_args()
    pc = np.asarray(Image.open(a.pc).convert('L')).astype(np.float32)
    hw = np.asarray(Image.open(a.hw).convert('L')).astype(np.float32)
    regions = {'all': (4, 196, 4, 316)}
    for r in a.region:
        n, v = r.split('=')
        regions[n] = tuple(int(x) for x in v.split(','))
    for name, (y0, y1, x0, x1) in regions.items():
        best = None
        m0 = float(np.abs(pc[y0:y1, x0:x1] - hw[y0:y1, x0:x1]).mean())
        for fy in np.arange(-1.0, 1.01, 0.25):
            for fx in np.arange(-1.0, 1.01, 0.25):
                s = shifted(hw, fx, fy)
                m = float(np.abs(pc[y0:y1, x0:x1] - s[y0:y1, x0:x1]).mean())
                if best is None or m < best[0]:
                    best = (m, fx, fy)
        print(f'{name}: MAD at 0 = {m0:.2f}; best shift fx={best[1]:+.2f} fy={best[2]:+.2f} MAD {best[0]:.2f}')


if __name__ == '__main__':
    main()
