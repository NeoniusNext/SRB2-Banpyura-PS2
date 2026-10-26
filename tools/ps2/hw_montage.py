#!/usr/bin/env python3
"""OPT12 HWDRV: contact sheet of the -vidshot pictures of one run: hw_montage.py RUNDIR OUT.png [--scale 2] [--cols 3]"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw


def main():
    a = sys.argv[1:]
    scale, cols = 2, 3
    while '--scale' in a:
        i = a.index('--scale'); scale = int(a[i + 1]); del a[i:i + 2]
    while '--cols' in a:
        i = a.index('--cols'); cols = int(a[i + 1]); del a[i:i + 2]
    run, out = Path(a[0]), a[1]
    fs = sorted(run.glob('vidshot-*.ppm'), key=lambda p: int(p.stem.split('_')[-1]))
    ims = [Image.open(f).convert('RGB') for f in fs]
    if not ims:
        sys.exit('no pictures')
    w, h = ims[0].size
    w, h = w * scale, h * scale
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * w, rows * (h + 12)), (20, 20, 20))
    d = ImageDraw.Draw(sheet)
    for i, (f, im) in enumerate(zip(fs, ims)):
        x, y = (i % cols) * w, (i // cols) * (h + 12)
        sheet.paste(im.resize((w, h), Image.NEAREST), (x, y + 12))
        d.text((x + 2, y), f.stem.replace('vidshot-', ''), fill=(255, 255, 0))
    sheet.save(out)
    print(out, sheet.size)


if __name__ == '__main__':
    main()
