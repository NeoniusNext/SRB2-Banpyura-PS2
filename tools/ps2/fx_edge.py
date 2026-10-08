"""OPT11-FX: structural comparison of pictures that differ in colour but must have the same edges (the water ripple of a grid texture).

usage: fx_edge.py PC.png x,y,w,h HW1.ppm [HW2.ppm ...]
Prints per picture: the correlation of the gradient magnitudes (1 = the same edges), the share of PC edge pixels that have an HW edge within 0 / 1 pixel.
"""
import sys

import numpy as np
from PIL import Image


def lum(p):
    a = np.asarray(Image.open(p).convert('RGB')).astype(np.float32)
    return a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11


def grad(l):
    gx = np.abs(np.diff(l, axis=1))[:-1, :]
    gy = np.abs(np.diff(l, axis=0))[:, :-1]
    return gx + gy


def main():
    pc = sys.argv[1]
    x, y, w, h = [int(v) for v in sys.argv[2].split(',')]
    gp = grad(lum(pc))[y:y + h, x:x + w]
    tp = gp > np.percentile(gp, 85)
    for p in sys.argv[3:]:
        gh = grad(lum(p))[y:y + h, x:x + w]
        th = gh > np.percentile(gh, 85)
        c = float(np.corrcoef(gp.ravel(), gh.ravel())[0, 1])
        near0 = float((tp & th).sum() / max(tp.sum(), 1))
        dil = th.copy()
        dil[1:, :] |= th[:-1, :]
        dil[:-1, :] |= th[1:, :]
        dil[:, 1:] |= th[:, :-1]
        dil[:, :-1] |= th[:, 1:]
        near1 = float((tp & dil).sum() / max(tp.sum(), 1))
        print(f'{p.split("/")[-2]:32s} corr {c:.3f} edge match 0px {near0:.3f} 1px {near1:.3f}')


if __name__ == '__main__':
    main()
