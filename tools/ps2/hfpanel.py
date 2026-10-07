"""OPT10-HF: panels PC-OpenGL | PS2-HW | difference (and the metrics) for the picture parity work.

usage: hfpanel.py --pc a.png --hw b.ppm --out panel.png [--label 'MAP01 k300'] [--scale 2] [--sw c.ppm]
Metrics (printed as one line, JSON with --json): mean absolute difference (all pixels, 0..255), the same on 3x3 box-blurred pictures (removes
dither/filter noise), the share of pixels that differ by more than 48 in any channel, mean colour of each picture.
The difference panel is |pc - hw| * 2 (clamped), a black pixel = equal.
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image, ImageDraw


def load(p):
    return Image.open(p).convert('RGB')


def box(a):
    b = np.pad(a.astype(np.float32), ((1, 1), (1, 1), (0, 0)), mode='edge')
    return sum(b[y:y + a.shape[0], x:x + a.shape[1]] for y in range(3) for x in range(3)) / 9.0


def metrics(pc, hw):
    h = min(pc.shape[0], hw.shape[0])
    w = min(pc.shape[1], hw.shape[1])
    pc, hw = pc[:h, :w], hw[:h, :w]
    d = np.abs(pc.astype(np.int16) - hw.astype(np.int16))
    db = np.abs(box(pc) - box(hw))
    return {'mad': round(float(d.mean()), 2), 'mad_blur': round(float(db.mean()), 2), 'over48': round(float((d.max(axis=2) > 48).mean() * 100), 2),
            'pc_mean': [round(float(x), 1) for x in pc.reshape(-1, 3).mean(0)], 'hw_mean': [round(float(x), 1) for x in hw.reshape(-1, 3).mean(0)]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pc', required=True)
    ap.add_argument('--hw', required=True)
    ap.add_argument('--sw', default='')
    ap.add_argument('--out', required=True)
    ap.add_argument('--label', default='')
    ap.add_argument('--scale', type=int, default=2)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    pc, hw = load(a.pc), load(a.hw)
    if pc.size != hw.size:
        hw = hw.resize(pc.size, Image.NEAREST)
    pa, ha = np.asarray(pc), np.asarray(hw)
    m = metrics(pa, ha)
    diff = np.clip(np.abs(pa.astype(np.int16) - ha.astype(np.int16)) * 2, 0, 255).astype(np.uint8)
    panels = [pc, hw, Image.fromarray(diff)]
    names = ['PC OpenGL', 'PS2 HW', 'diff x2']
    if a.sw:
        sw = load(a.sw).resize(pc.size, Image.NEAREST)
        panels.insert(2, sw)
        names.insert(2, 'PS2 SW')
    s = a.scale
    W, H = pc.size
    top = 14
    img = Image.new('RGB', (W * s * len(panels), H * s + top), (24, 24, 24))
    dr = ImageDraw.Draw(img)
    for i, (p, n) in enumerate(zip(panels, names)):
        img.paste(p.resize((W * s, H * s), Image.NEAREST), (i * W * s, top))
        dr.text((i * W * s + 3, 1), n, fill=(255, 255, 255))
    dr.text((W * s * len(panels) - 330, 1), f"{a.label} MAD {m['mad']} blur {m['mad_blur']} >48: {m['over48']}%", fill=(255, 255, 0))
    img.save(a.out)
    print(json.dumps(m) if a.json else f"{a.label}: mad={m['mad']} blur={m['mad_blur']} over48={m['over48']}% pc={m['pc_mean']} hw={m['hw_mean']}")


if __name__ == '__main__':
    sys.exit(main())
