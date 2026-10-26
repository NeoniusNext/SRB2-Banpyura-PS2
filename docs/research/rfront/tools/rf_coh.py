#!/usr/bin/env python3
"""OPT13 RFRONT: summarise HWCOH lines (hw_rf.inc, -hwcoh) of a run: how much of a frame's BSP-walk result was already in the walk of the frame before (A) or the two frames before (A u A2).

usage: rf_coh.py boot.txt [boot2.txt ...]
Per set (sub = subsectors walked, seg = segs that reached HWR_ProcessSeg, plane = planes built): mean |B|, mean fraction of B that was in A (reuse), mean fraction new
(not in A), mean fraction new vs A u A2, mean |A u A2| / |B| (what a two-frame union would draw in excess), 95th percentile of new fraction.
"""
import re
import sys

def main():
    for fn in sys.argv[1:]:
        rows = []
        for l in open(fn, errors='replace'):
            if not l.startswith('HWCOH'):
                continue
            d = {}
            for part in l.split('|')[1:]:
                p = part.split()
                name = p[0]
                d[name] = {k: int(v) for k, v in (x.split('=') for x in p[1:])}
            rows.append(d)
        print(f'{fn}: {len(rows)} frames')
        for name in ('sub', 'seg', 'plane'):
            B = [r[name]['B'] for r in rows if r[name]['B']]
            new = [r[name]['new'] / r[name]['B'] for r in rows if r[name]['B']]
            new2 = [r[name]['new2'] / r[name]['B'] for r in rows if r[name]['B']]
            uni = [r[name]['uni'] / r[name]['B'] for r in rows if r[name]['B']]
            srt = sorted(new)
            srt2 = sorted(new2)
            if not B:
                continue
            print(f'  {name:6s} mean|B|={sum(B)/len(B):7.1f}  new vs A: mean {100*sum(new)/len(new):5.1f}%  p95 {100*srt[int(0.95*len(srt))]:5.1f}%  max {100*srt[-1]:5.1f}% | new vs AuA2: mean {100*sum(new2)/len(new2):5.1f}%  p95 {100*srt2[int(0.95*len(srt2))]:5.1f}% | |AuA2|/|B| mean {sum(uni)/len(uni):4.2f}')

main()
