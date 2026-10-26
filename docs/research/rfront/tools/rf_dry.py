#!/usr/bin/env python3
"""OPT13 RFRONT: summarise HWDRY lines (hw_rf2.inc, -hwdry): for each margin variant the mean size of the walk of frame t-1 against the exact walk of frame t (inflation), and the mean/p95/max share of the exact result of frame t
that the walk of t-1 (miss1) or t-2 (miss2) did not contain (holes). Leaves = subsectors walked, segs = segs that reach HWR_ProcessSeg."""
import re, sys

def main():
    for fn in sys.argv[1:]:
        rows = []
        for l in open(fn, errors='replace'):
            if not l.startswith('HWDRY'):
                continue
            parts = l.split('|')
            m = re.search(r'E leaf=(\d+) seg=(\d+)', parts[0])
            E = (int(m.group(1)), int(m.group(2)))
            row = {'E': E}
            for p in parts[1:]:
                name = p.split()[0]
                nums = [int(x) for x in re.findall(r'=(\d+)', p)]
                row[name] = nums  # W leaf, W seg, miss1 leaf, miss1 seg, miss2 leaf, miss2 seg
            rows.append(row)
        print(fn, len(rows), 'frames')
        names = [k for k in rows[0] if k != 'E']
        for n in names:
            infl_l = [r[n][0] / max(1, r['E'][0]) for r in rows]
            infl_s = [r[n][1] / max(1, r['E'][1]) for r in rows]
            m1l = sorted(r[n][2] / max(1, r['E'][0]) for r in rows)
            m1s = sorted(r[n][3] / max(1, r['E'][1]) for r in rows)
            m2s = sorted(r[n][5] / max(1, r['E'][1]) for r in rows)
            f = lambda v: sum(v) / len(v)
            p = lambda v: v[int(0.95 * len(v))]
            print(f'  {n:8s} size/E: leaf {f(infl_l):5.2f} seg {f(infl_s):5.2f} | holes (seg) stride1: mean {100*f(m1s):5.2f}% p95 {100*p(m1s):5.2f}% max {100*m1s[-1]:5.1f}% | stride2: mean {100*f(m2s):5.2f}% p95 {100*p(m2s):5.2f}%')
main()
