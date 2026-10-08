#!/usr/bin/env python3
"""usage: ppdiff.py RUN_A RUN_B [maxprint]: compares the HWPP lines (-hwpolyhash 2 LO HI) of two runs polygon by polygon (frame c, index i): the first differences"""
import re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
def load(n):
    d = {}
    for l in (ROOT / 'build/runs' / n / 'boot.txt').read_text(errors='replace').splitlines():
        m = re.match(r'HWPP c=(\d+) i=(\d+) (.*)', l)
        if m:
            d[(int(m.group(1)), int(m.group(2)))] = m.group(3)
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
mx = int(sys.argv[3]) if len(sys.argv) > 3 else 12
keys = sorted(set(a) | set(b))
nd = 0
for k in keys:
    if a.get(k) != b.get(k):
        nd += 1
        if nd <= mx:
            print(k, '\n  A', a.get(k), '\n  B', b.get(k))
print('polygons', len(a), len(b), 'differ', nd)
