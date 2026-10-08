"""OPT11-MODEL: where a model is, PC against PS2. The model region = the pixels where the PC picture with models differs from the PC picture of the same scene without them.

usage: md_cmp.py PC_ON.png PC_OFF.png HW.ppm [--out panel.png] [--zoom 3]
prints: MAD over the model region (dilated by 2 px), share of the region, MAD of the whole picture, the share of region pixels over 48
"""
import argparse
import numpy as np
from PIL import Image


def dilate(m, n):
    out = m.copy()
    for _ in range(n):
        o = out.copy()
        o[1:, :] |= out[:-1, :]
        o[:-1, :] |= out[1:, :]
        o[:, 1:] |= out[:, :-1]
        o[:, :-1] |= out[:, 1:]
        out = o
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pc_on')
    ap.add_argument('pc_off')
    ap.add_argument('hw')
    ap.add_argument('--out', default='')
    ap.add_argument('--zoom', type=int, default=3)
    ap.add_argument('--thr', type=int, default=8)
    a = ap.parse_args()
    on = np.asarray(Image.open(a.pc_on).convert('RGB')).astype(np.int16)
    off = np.asarray(Image.open(a.pc_off).convert('RGB')).astype(np.int16)
    hw = np.asarray(Image.open(a.hw).convert('RGB')).astype(np.int16)
    h = min(on.shape[0], hw.shape[0]); w = min(on.shape[1], hw.shape[1])
    on, off, hw = on[:h, :w], off[:h, :w], hw[:h, :w]
    mask = dilate((np.abs(on - off).max(axis=2) > a.thr), 2)
    d = np.abs(on - hw)
    n = int(mask.sum())
    mad_r = float(d[mask].mean()) if n else 0.0
    over = float((d.max(axis=2)[mask] > 48).mean() * 100) if n else 0.0
    print(f'region {n} px ({100 * n / (h * w):.2f}%) MAD {mad_r:.2f} over48 {over:.1f}% | whole MAD {float(d.mean()):.2f} | PC-on vs PC-off whole {float(np.abs(on - off).mean()):.2f}')
    if a.out:
        z = a.zoom
        ys, xs = np.nonzero(mask)
        if n:
            y0, y1, x0, x1 = max(ys.min() - 4, 0), min(ys.max() + 5, h), max(xs.min() - 4, 0), min(xs.max() + 5, w)
        else:
            y0, y1, x0, x1 = 0, h, 0, w
        tiles = [on, hw, np.clip(d * 2, 0, 255)]
        ims = [Image.fromarray(t[y0:y1, x0:x1].astype(np.uint8)).resize(((x1 - x0) * z, (y1 - y0) * z), Image.NEAREST) for t in tiles]
        im = Image.new('RGB', (sum(i.width for i in ims) + 8, ims[0].height), (40, 40, 40))
        x = 0
        for i in ims:
            im.paste(i, (x, 0)); x += i.width + 4
        im.save(a.out)


if __name__ == '__main__':
    main()
