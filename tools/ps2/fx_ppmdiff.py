"""OPT11-FX2: a picture of the differences of two vidshot PPMs: A | B | difference (x2, differing pixels white on black), written as PNG, with the bounding box and a histogram of the rows of the differing pixels.

usage: fx_ppmdiff.py RUN_A RUN_B SHOT_NAME OUT.png     (SHOT_NAME e.g. vidshot-320x200-f301.ppm)
"""
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def main():
    ra, rb, name, out = sys.argv[1:5]
    a = Image.open(ROOT / 'build/runs' / ra / name).convert('RGB')
    b = Image.open(ROOT / 'build/runs' / rb / name).convert('RGB')
    w, h = a.size
    pa, pb = a.load(), b.load()
    d = Image.new('RGB', (w, h))
    pd = d.load()
    xs, ys = [], []
    rows = {}
    for y in range(h):
        for x in range(w):
            if pa[x, y] != pb[x, y]:
                pd[x, y] = (255, 255, 255)
                xs.append(x)
                ys.append(y)
                rows[y // 20] = rows.get(y // 20, 0) + 1
    if xs:
        print('differing pixels %d, bounding box x %d..%d y %d..%d' % (len(xs), min(xs), max(xs), min(ys), max(ys)))
        print('per 20-row band:', sorted(rows.items()))
    else:
        print('identical')
    sheet = Image.new('RGB', (w * 3, h))
    sheet.paste(a, (0, 0))
    sheet.paste(b, (w, 0))
    sheet.paste(d, (2 * w, 0))
    sheet = sheet.resize((w * 3 * 2, h * 2), Image.NEAREST)
    sheet.save(out)


if __name__ == '__main__':
    main()
