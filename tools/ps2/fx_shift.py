"""OPT11-FX: where does a picture sit compared with the reference: block matching. For blocks of the water region the integer shift (dx, dy) in -R..R that makes the HW block
equal to the PC block best (sum of absolute differences of the luminance after a 3x3 blur, blocks without structure are skipped) is found. A water that is shifted by the ripple
the wrong way gives shifts away from (0, 0); one that matches gives (0, 0) on most blocks.

usage: fx_shift.py PC.png HW1.ppm [HW2.ppm ...] x,y,w,h
Prints for every HW picture: blocks used, share of blocks at (0,0), mean |dx|, mean |dy|, share of blocks that are off by 2 or more pixels.
"""
import sys

import numpy as np
from PIL import Image

R = 3
BW, BH = 16, 12


def lum(p):
    a = np.asarray(Image.open(p).convert('RGB')).astype(np.float32)
    l = a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11
    k = np.pad(l, 1, mode='edge')
    return sum(k[i:i + l.shape[0], j:j + l.shape[1]] for i in range(3) for j in range(3)) / 9.0


def main():
    x, y, w, h = [int(v) for v in sys.argv[-1].split(',')]
    pc = lum(sys.argv[1])
    for p in sys.argv[2:-1]:
        hw = lum(p)
        n = z = off2 = 0
        sx = sy = 0.0
        for by in range(y, y + h - BH + 1, BH):
            for bx in range(x, x + w - BW + 1, BW):
                a = pc[by:by + BH, bx:bx + BW]
                if a.std() < 6.0:
                    continue
                best = None
                for dy in range(-R, R + 1):
                    for dx in range(-R, R + 1):
                        yy, xx = by + dy, bx + dx
                        if yy < 0 or xx < 0 or yy + BH > hw.shape[0] or xx + BW > hw.shape[1]:
                            continue
                        s = float(np.abs(hw[yy:yy + BH, xx:xx + BW] - a).mean())
                        if best is None or s < best[0] - 1e-6:
                            best = (s, dx, dy)
                n += 1
                z += best[1] == 0 and best[2] == 0
                sx += abs(best[1])
                sy += abs(best[2])
                off2 += max(abs(best[1]), abs(best[2])) >= 2
        print(f'{p.split("/")[-2]:28s} blocks {n:4d}  at (0,0) {z / max(n, 1):.3f}  mean |dx| {sx / max(n, 1):.2f}  mean |dy| {sy / max(n, 1):.2f}  off >= 2 px {off2 / max(n, 1):.3f}')


if __name__ == '__main__':
    main()
