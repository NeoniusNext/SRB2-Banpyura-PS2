"""OPT11-FX: mean colour (and spread) of a region of several pictures, to see a systematic colour difference between PC and PS2.

usage: fx_region.py x,y,w,h pic1 [pic2 ...]
"""
import sys

import numpy as np
from PIL import Image


def main():
    x, y, w, h = [int(v) for v in sys.argv[1].split(',')]
    base = None
    for p in sys.argv[2:]:
        a = np.asarray(Image.open(p).convert('RGB')).astype(np.float32)[y:y + h, x:x + w].reshape(-1, 3)
        m = a.mean(0)
        sd = a.std(0)
        d = '' if base is None else '  diff to first: (%+.1f %+.1f %+.1f)' % tuple(m - base)
        if base is None:
            base = m
        print('%-40s mean (%.1f %.1f %.1f) sd (%.1f %.1f %.1f)%s' % (p.split('/')[-2] if '/' in p else p, m[0], m[1], m[2], sd[0], sd[1], sd[2], d))


if __name__ == '__main__':
    main()
