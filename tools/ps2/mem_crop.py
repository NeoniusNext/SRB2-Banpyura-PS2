#!/usr/bin/env python3
"""OPT11-MEM: a crop of a -vidshot picture, enlarged, as PNG for the report.

usage: mem_crop.py vidshot.ppm out.png [x0 y0 x1 y1 [zoom]]     (default: the lower right corner 120,148-320,200, x3)
"""
import sys
from PIL import Image

def main():
    a = sys.argv[1:]
    im = Image.open(a[0]).convert('RGB')
    box = tuple(int(v) for v in a[2:6]) if len(a) >= 6 else (120, 148, 320, 200)
    zoom = int(a[6]) if len(a) >= 7 else 3
    c = im.crop(box)
    c.resize((c.width * zoom, c.height * zoom), Image.NEAREST).save(a[1], optimize=True)

main()
