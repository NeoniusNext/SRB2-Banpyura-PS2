"""OPT10-HF: automatic glitch finder between two pictures of the same frame (HW against software or PC), without a per-pixel comparison.

usage: hfglitch.py --ref a.png --hw b.ppm [--out annotated.png] [--block 8] [--thr 40] [--skip-top 0]
The pictures are compared in blocks (default 8x8): a block is flagged when its mean luminance differs by more than --thr (0..255) or when one picture is flat
(luminance standard deviation < 3) and the other is textured (> 12): a missing / wrong texture, a hole in a polygon, garbage. Texture phase, filtering and
the palette/light model change single pixels, not block means. Prints the number of flagged blocks and writes the annotated picture (flagged blocks outlined red).
"""
import argparse

import numpy as np
from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', required=True)
    ap.add_argument('--hw', required=True)
    ap.add_argument('--out', default='')
    ap.add_argument('--block', type=int, default=8)
    ap.add_argument('--thr', type=float, default=40.0)
    ap.add_argument('--skip-top', type=int, default=0)
    a = ap.parse_args()
    ref = Image.open(a.ref).convert('RGB')
    hw = Image.open(a.hw).convert('RGB')
    if hw.size != ref.size:
        hw = hw.resize(ref.size, Image.BILINEAR)
    r = np.asarray(ref.convert('L')).astype(np.float32)
    h = np.asarray(hw.convert('L')).astype(np.float32)
    B = a.block
    H, W = r.shape
    flagged = []
    for y in range(a.skip_top, H - B + 1, B):
        for x in range(0, W - B + 1, B):
            rb, hb = r[y:y + B, x:x + B], h[y:y + B, x:x + B]
            dm = abs(float(rb.mean() - hb.mean()))
            rs, hs = float(rb.std()), float(hb.std())
            flat = (rs < 3 and hs > 12) or (hs < 3 and rs > 12)
            if dm > a.thr or flat:
                flagged.append((x, y, dm, rs, hs))
    print(f'flagged {len(flagged)} of {((H - a.skip_top) // B) * (W // B)} blocks')
    if a.out:
        out = Image.new('RGB', (W * 2, H))
        out.paste(ref, (0, 0))
        out.paste(hw, (W, 0))
        d = ImageDraw.Draw(out)
        for x, y, *_ in flagged:
            d.rectangle([W + x, y, W + x + B - 1, y + B - 1], outline=(255, 0, 0))
            d.rectangle([x, y, x + B - 1, y + B - 1], outline=(255, 0, 0))
        out.save(a.out)
    for x, y, dm, rs, hs in flagged[:12]:
        print(f'  block ({x},{y}) dmean={dm:.0f} std ref={rs:.0f} hw={hs:.0f}')


if __name__ == '__main__':
    main()
