#!/usr/bin/env python3
"""OPT12 HWFRONT: the fine laps of HWPROF2 (K cycles of the frame the window samples, mean over the windows FIRST..LAST) of several runs side by side: setup sky seg plane addspr subsec light sprsort sprdraw nodesort nodedraw.
The laps are cycle counters inside the frame (not the wall time of the run): a change of one stage shows without the layout noise of the whole run.

usage: hwf_laps.py [--first N] [--last N] NAME [NAME ...]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KEYS = ['setup', 'sky', 'seg', 'plane', 'addspr', 'subsec', 'light', 'sprsort', 'sprdraw', 'nodesort', 'nodedraw']


def main():
    a = sys.argv[1:]
    first, last = 1, 9
    names = []
    i = 0
    while i < len(a):
        if a[i] == '--first':
            first = int(a[i + 1]); i += 2
        elif a[i] == '--last':
            last = int(a[i + 1]); i += 2
        else:
            names.append(a[i]); i += 1
    print(f'{"run":<18}' + ''.join(f'{k:>9}' for k in KEYS))
    for n in names:
        boot = ROOT / 'build/runs' / n / 'boot.txt'
        if not boot.exists():
            print(n, 'missing')
            continue
        rows = []
        for l in boot.read_text(errors='replace').splitlines():
            if l.startswith('HWPROF2 win='):
                w = int(re.search(r'win=(\d+)', l).group(1))
                if first <= w <= last:
                    rows.append({k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', l) if k in KEYS})
        if not rows:
            print(n, 'no windows')
            continue
        print(f'{n:<18}' + ''.join(f'{sum(r.get(k, 0) for r in rows) / len(rows) / 1000:>9.1f}' for k in KEYS))


if __name__ == '__main__':
    main()
