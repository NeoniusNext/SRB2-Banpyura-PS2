#!/usr/bin/env python3
"""OPT12 HWFRONT: where two vidshots differ: bounding boxes of the clusters of differing pixels (rows below SKIP), and optionally a PNG with A, B and the difference side by side.

usage: hwf_ppmdiff.py RUN_A RUN_B SHOT_NAME [SKIP_TOP_ROWS] [OUT.png]
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read_ppm(p):
    data = p.read_bytes()
    parts = data.split(b'\n', 3)
    w, h = [int(x) for x in parts[1].split()]
    return w, h, parts[3]


def main():
    a, b, name = sys.argv[1], sys.argv[2], sys.argv[3]
    skip = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    out = sys.argv[5] if len(sys.argv) > 5 else None
    w, h, da = read_ppm(ROOT / 'build/runs' / a / name)
    _, _, db = read_ppm(ROOT / 'build/runs' / b / name)
    pts = []
    for y in range(skip, h):
        for x in range(w):
            i = (y * w + x) * 3
            if da[i:i + 3] != db[i:i + 3]:
                pts.append((x, y))
    # clusters: a pixel joins the cluster whose box is within 6 pixels
    boxes = []
    for x, y in pts:
        for bx in boxes:
            if bx[0] - 6 <= x <= bx[2] + 6 and bx[1] - 6 <= y <= bx[3] + 6:
                bx[0] = min(bx[0], x); bx[1] = min(bx[1], y); bx[2] = max(bx[2], x); bx[3] = max(bx[3], y); bx[4] += 1
                break
        else:
            boxes.append([x, y, x, y, 1])
    print('%d differing pixels, %d clusters (x0 y0 x1 y1 count):' % (len(pts), len(boxes)))
    for bx in sorted(boxes, key=lambda t: -t[4])[:12]:
        print('  ', bx)
    if out:
        from PIL import Image
        ia = Image.frombytes('RGB', (w, h), da)
        ib = Image.frombytes('RGB', (w, h), db)
        idf = Image.new('RGB', (w, h), (0, 0, 0))
        px = idf.load()
        for x, y in pts:
            px[x, y] = (255, 255, 255)
        sheet = Image.new('RGB', (w * 3, h))
        sheet.paste(ia, (0, 0)); sheet.paste(ib, (w, 0)); sheet.paste(idf, (2 * w, 0))
        sheet = sheet.resize((w * 3 * 2, h * 2), Image.NEAREST)
        sheet.save(out)
        print('written', out)


if __name__ == '__main__':
    main()
