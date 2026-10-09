#!/usr/bin/env python3
"""OPT13 RFRONT: per-frame means over the windows 1..9 of the HWPROF lines of a --hwdetail run (AddLine calls, ProjectSprite/DrawSprite/DropShadow/RenderPlane parts, sprite census, wall/plane census).
usage: rf_detail.py boot.txt"""
import re, sys
from collections import defaultdict

def main():
    fn = sys.argv[1]
    acc = defaultdict(lambda: defaultdict(list))
    wins = defaultdict(int)
    for l in open(fn, errors='replace'):
        m = re.match(r'(HWPROF\d+)( \w+)?', l)
        if not m:
            continue
        key = m.group(1) + (m.group(2) or '')
        wins[key] += 1
        w = wins[key]
        if w < 2 or w > 10:   # window 0 is the load; windows 1..9
            continue
        for k, v in re.findall(r'([A-Za-z_+/]+)=(\d+)', l):
            acc[key][k].append(int(v))
    for key in sorted(acc):
        if key.startswith(('HWPROF31', 'HWPROF33', 'HWPROF35', 'HWPROF36', 'HWPROF37', 'HWPROF40 sprites', 'HWPROF60', 'HWPROF61', 'HWPROF34', 'HWPROF4 ', 'HWPROF2 ', 'HWPROF14', 'HWPROF10')):
            print(key, {k: int(sum(v) / len(v)) for k, v in acc[key].items()})
main()
