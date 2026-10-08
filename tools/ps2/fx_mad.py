"""OPT11-FX: mean absolute difference and mean colours of regions of two pictures.

usage: fx_mad.py A.png B.ppm x,y,w,h [x,y,w,h ...]     (no region = the whole picture)
"""
import sys

import numpy as np
from PIL import Image


def main():
    a = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(np.float32)
    b = np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(np.float32)
    regs = [[int(v) for v in r.split(',')] for r in sys.argv[3:]] or [[0, 0, a.shape[1], a.shape[0]]]
    for x, y, w, h in regs:
        ra, rb = a[y:y + h, x:x + w], b[y:y + h, x:x + w]
        print('region %d,%d,%d,%d: MAD %.2f  mean A %s  B %s' % (x, y, w, h, float(np.abs(ra - rb).mean()), ra.reshape(-1, 3).mean(0).round(1), rb.reshape(-1, 3).mean(0).round(1)))


if __name__ == '__main__':
    main()
