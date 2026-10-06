"""Compare a box of two -vidshot pictures (OPT9-F: the Lua HUD of ZL.pk3 in the software and in the hardware renderer).

usage: hud_compare.py A.ppm B.ppm [--box x0,y0,x1,y1] [--png out.png]
The pictures are the 320x200 RGB PPMs of -vidshot. Prints, for the box, the number of pixels, how many differ and the largest / mean channel difference;
--png writes a side-by-side enlargement (A | B | difference) of the box. Exit code 0 when the box is identical.
"""
import argparse
import sys

from PIL import Image, ImageChops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--box', default='8,8,128,22')
    ap.add_argument('--png', default='')
    ap.add_argument('--scale', type=int, default=4)
    a = ap.parse_args()
    box = tuple(int(v) for v in a.box.split(','))
    ia = Image.open(a.a).convert('RGB').crop(box)
    ib = Image.open(a.b).convert('RGB').crop(box)
    diff = ImageChops.difference(ia, ib)
    px = ia.size[0] * ia.size[1]
    da, db = list(ia.getdata()), list(ib.getdata())
    bad = sum(1 for x, y in zip(da, db) if x != y)
    worst = max((max(abs(p - q) for p, q in zip(x, y)) for x, y in zip(da, db)), default=0)
    mean = sum(sum(abs(p - q) for p, q in zip(x, y)) for x, y in zip(da, db)) / (3 * px)
    print(f'box {box}: {px} pixels, {bad} differ, largest channel difference {worst}, mean {mean:.2f}')
    if a.png:
        w, h = ia.size
        s = a.scale
        out = Image.new('RGB', (w * s * 3 + 8, h * s), (64, 64, 64))
        out.paste(ia.resize((w * s, h * s), Image.NEAREST), (0, 0))
        out.paste(ib.resize((w * s, h * s), Image.NEAREST), (w * s + 4, 0))
        out.paste(diff.point(lambda v: min(255, v * 4)).resize((w * s, h * s), Image.NEAREST), (w * s * 2 + 8, 0))
        out.save(a.png)
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
