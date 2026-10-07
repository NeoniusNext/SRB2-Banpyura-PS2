#!/usr/bin/env python3
"""Compare two sets of -vidshot pictures (PPM) and write a montage.

usage: shotcmp.py RUN_A RUN_B [--runs DIR] [--out montage.png] [--thr N] [tag ...]
For every vidshot-*.ppm that both runs have: the number of pixels that differ by more than --thr (default 8 per channel), the share of pixels,
the mean absolute difference, and the largest per-channel difference. The montage shows A | B | amplified difference for each tag.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.int16)


def main():
    args = sys.argv[1:]
    runs = ROOT / 'build/runs'
    out = None
    thr = 8
    tags = []
    pos = []
    i = 0
    while i < len(args):
        if args[i] == '--runs':
            runs = Path(args[i + 1]); i += 2
        elif args[i] == '--out':
            out = args[i + 1]; i += 2
        elif args[i] == '--thr':
            thr = int(args[i + 1]); i += 2
        else:
            pos.append(args[i]); i += 1
    a, b = pos[0], pos[1]
    tags = pos[2:]
    fa = {p.name: p for p in (runs / a).glob('vidshot-*.ppm')}
    fb = {p.name: p for p in (runs / b).glob('vidshot-*.ppm')}
    names = sorted(set(fa) & set(fb))
    if tags:
        names = [n for n in names if any(n.endswith(f'-{t}.ppm') for t in tags)]
    rows = []
    for n in names:
        ia, ib = load(fa[n]), load(fb[n])
        if ia.shape != ib.shape:
            print(n, 'size differs', ia.shape, ib.shape)
            continue
        d = np.abs(ia - ib)
        mx = d.max(axis=2)
        cnt = int((mx > thr).sum())
        print(f'{n}: differ(>{thr})={cnt} ({100.0 * cnt / mx.size:.2f}%) any={int((mx > 0).sum())} mean|d|={d.mean():.3f} max={int(d.max())}')
        if out:
            diff = np.clip(d * 4, 0, 255).astype(np.uint8)
            rows.append(np.concatenate([ia.astype(np.uint8), ib.astype(np.uint8), diff], axis=1))
    if out and rows:
        h = sum(r.shape[0] for r in rows)
        Image.fromarray(np.concatenate(rows, axis=0)).save(out)
        print('montage', out, h)


if __name__ == '__main__':
    main()
