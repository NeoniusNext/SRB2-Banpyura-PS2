#!/usr/bin/env python3
"""usage: hp2.py RUN [RUN...]  : mean over windows 1.. of HWPROF2 fields and HWPROF4 counters of runs in build/runs/RUN/boot.txt"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def load(name):
    p2, p4, pr = {}, [], {}
    for line in (ROOT / 'build/runs' / name / 'boot.txt').read_text(errors='replace').splitlines():
        if line.startswith('HWPROF2 '):
            kv = dict(re.findall(r'(\w+)=(\d+)', line))
            p2[int(kv['win'])] = {k: int(v) for k, v in kv.items()}
        elif line.startswith('HWPROF4 '):
            p4.append({k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)})
        elif line.startswith('HWPROF '):
            kv = dict(re.findall(r'(\w+)=(\d+)', line))
            pr[int(kv['win'])] = {k: int(v) for k, v in kv.items()}
    return p2, p4, pr


for n in sys.argv[1:]:
    p2, p4, pr = load(n)
    ws = [w for w in p2 if w >= 1]
    avg = {k: sum(p2[w][k] for w in ws) / len(ws) / 1e6 for k in ['seg', 'plane', 'addspr', 'subsec', 'sprdraw', 'nodedraw', 'light', 'sprsort']}
    wall = sum(pr[w]['wall'] for w in ws) / len(ws) / 1e6
    print(n, 'wall %.2f' % wall, {k: round(v, 2) for k, v in avg.items()})
    c = p4[1:1 + len(ws)]
    print('   counts', {k: round(sum(x[k] for x in c) / len(c)) for k in ['segs', 'subsecs', 'planes', 'sprites', 'proc']})
