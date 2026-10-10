#!/usr/bin/env python3
"""OPT13 IS: per-window A/B of two runs of one demo (build/runs/NAME/boot.txt): the difference B - A of the sprite laps (HWPROF2 addspr, sprsort, sprdraw), of the 'sprites' stage and of the wall,
window by window (windows 1..9, M cycles a frame), with the median and the mean of the differences. The windows in which the two runs differ by a texture rebuild (HWREGEN) or a patch
read (a memory layout lottery, 4..20 M cycles once) show as outliers: the median is the figure to read.   usage: is_ab.py A B"""
import re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
def kv(l): return {m.group(1): m.group(2) for m in re.finditer(r'(\w+)=([^\s|]+)', l)}
def load(n):
    w, l2, tm = {}, {}, ''
    for l in (ROOT / 'build/runs' / n / 'boot.txt').read_text(errors='replace').splitlines():
        if l.startswith('HWPROF win='): d = kv(l); w[int(d['win'])] = d
        elif l.startswith('HWPROF2 win='): d = kv(l); l2[int(d['win'])] = d
        else:
            m = re.search(r'timed (\d+) gametics in (\d+) realtics', l)
            if m: tm = int(m.group(2))
    return w, l2, tm
def med(v): v = sorted(v); n = len(v); return v[n // 2] if n % 2 else 0.5 * (v[n // 2 - 1] + v[n // 2])
def main():
    a, b = sys.argv[1], sys.argv[2]
    wa, la, ta = load(a); wb, lb, tb = load(b)
    ws = [x for x in range(1, 10) if x in wa and x in wb and x in la and x in lb]
    cols = [('wall', lambda w, l, x: float(w[x]['wall'])), ('addspr', lambda w, l, x: float(l[x]['addspr'])), ('sprdraw', lambda w, l, x: float(l[x]['sprdraw'])),
            ('SUM', lambda w, l, x: float(l[x]['addspr']) + float(l[x]['sprsort']) + float(l[x]['sprdraw']))]
    print(f'{b} - {a}   (realtics {ta} -> {tb})   M cycles a frame, per window 1..9')
    print(f'{"":<8}' + ''.join(f'{x:>8}' for x in ws) + f'{"median":>9}{"mean":>9}{"mean(no outlier)":>18}')
    for name, f in cols:
        d = [(f(wb, lb, x) - f(wa, la, x)) / 1e6 for x in ws]
        m = med(d)
        keep = [v for v in d if abs(v - m) < 0.5]
        print(f'{name:<8}' + ''.join(f'{v:8.3f}' for v in d) + f'{m:9.3f}{sum(d) / len(d):9.3f}{(sum(keep) / len(keep) if keep else 0):18.3f}')
main()
