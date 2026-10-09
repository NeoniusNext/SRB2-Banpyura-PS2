#!/usr/bin/env python3
"""OPT12 HWFRONT: one line per run (build/runs/NAME/boot.txt): mean over the HWPROF windows FIRST..LAST (default 1..9) of wall and the stages of the render
(M cycles per frame), their sum bsp+batch+sprites+nodes, the real tics of the time demo, and the cull counters of HWPROF62.

usage: hwf_sum.py [--first N] [--last N] [--max] NAME [NAME ...]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def kv(line):
    return {m.group(1): m.group(2) for m in re.finditer(r'(\w+)=([^\s|]+)', line)}


def main():
    a = sys.argv[1:]
    first, last, mx = 1, 9, False
    names = []
    i = 0
    while i < len(a):
        if a[i] == '--first':
            first = int(a[i + 1]); i += 2
        elif a[i] == '--last':
            last = int(a[i + 1]); i += 2
        elif a[i] == '--max':
            mx = True; i += 1
        else:
            names.append(a[i]); i += 1
    cols = ['wall', 'clear', 'bsp', 'batch', 'sprites', 'nodes']
    print(f'{"run":<14}{"wins":>5} ' + ''.join(f'{c:>9}' for c in cols) + f'{"sum":>9}{"tics":>7}  cull(planes/segs/subs tested>culled per frame)')
    for n in names:
        boot = ROOT / 'build/runs' / n / 'boot.txt'
        if not boot.exists():
            print(n, 'missing')
            continue
        wins, cull, timed = {}, {}, ''
        for l in boot.read_text(errors='replace').splitlines():
            if l.startswith('HWPROF win='):
                d = kv(l)
                wins[int(d['win'])] = d
            elif l.startswith('HWPROF62'):
                m = re.findall(r'(planes|segs|subsectors) tested=(\d+) culled=(\d+)', l)
                cull[len(cull)] = m
            else:
                m = re.search(r'timed (\d+) gametics in (\d+) realtics', l)
                if m:
                    timed = m.group(2)
        ws = [w for w in sorted(wins) if first <= w <= last]
        if not ws:
            print(n, 'no windows')
            continue
        vals = {c: [float(wins[w][c]) / 1e6 for w in ws] for c in cols}
        mean = {c: sum(v) / len(v) for c, v in vals.items()}
        s = sum(mean[c] for c in ('bsp', 'batch', 'sprites', 'nodes'))
        cs = ''
        cl = [cull[k] for k in sorted(cull)][first:last + 1]
        if cl:
            agg = {}
            for m in cl:
                for name, t, c in m:
                    x = agg.setdefault(name, [0, 0])
                    x[0] += int(t); x[1] += int(c)
            cs = ' '.join(f'{k}={v[0] // len(cl)}>{v[1] // len(cl)}' for k, v in agg.items())
        print(f'{n:<14}{len(ws):>5} ' + ''.join(f'{mean[c]:>9.3f}' for c in cols) + f'{s:>9.3f}{timed:>7}  {cs}')
        if mx:
            print(f'{"  max":<19}' + ''.join(f'{max(vals[c]):>9.3f}' for c in cols))


if __name__ == '__main__':
    main()
