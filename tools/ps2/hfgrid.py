"""OPT10-HF: is a picture made with palette rendering? Share of pixels whose colour is on the RGB565 crush grid of gr_palettedepth 16.

usage: hfgrid.py pic.png [pic2.png ...]
With palette rendering the final picture is built from the screen palette, whose entries are crushed to 5/6/5 bits: R and B are int((v >> 3) / 31 * 255),
G is int((v >> 2) / 63 * 255). Pictures of the non palette path (RGB blends) are mostly off the grid.
"""
import sys

import numpy as np
from PIL import Image

R = {int((v) / 31 * 255) for v in range(32)}
G = {int((v) / 63 * 255) for v in range(64)}
for f in sys.argv[1:]:
    a = np.asarray(Image.open(f).convert('RGB'))
    on = np.isin(a[..., 0], list(R)) & np.isin(a[..., 1], list(G)) & np.isin(a[..., 2], list(R))
    print(f'{f}: {on.mean() * 100:.1f}% of the pixels on the 565 grid')
