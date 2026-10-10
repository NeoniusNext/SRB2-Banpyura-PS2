#!/usr/bin/env python3
"""OPT13 IS: per run (build/runs/NAME/boot.txt) the mean over HWPROF windows 1..9 (M cycles a frame) of wall, the sprite laps of HWPROF2 (addspr, sprsort, sprdraw) and
their sum, the 'sprites' stage of HWPROF, the realtics of the time demo.   usage: is_sum.py [--first N] [--last N] NAME [NAME ...]"""
import re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
def kv(line):
    return {m.group(1): m.group(2) for m in re.finditer(r'(\w+)=([^\s|]+)', line)}
def main():
    a = sys.argv[1:]; first, last = 1, 9; names = []; i = 0
    while i < len(a):
        if a[i] == '--first': first = int(a[i+1]); i += 2
        elif a[i] == '--last': last = int(a[i+1]); i += 2
        else: names.append(a[i]); i += 1
    print(f'{"run":<16}{"wall":>8}{"addspr":>8}{"sprsort":>8}{"sprdraw":>8}{"SUM":>8}{"bsp":>8}{"sprites":>8}{"tics":>6}   | medians of the windows: {"wall":>7}{"addspr":>8}{"sprdraw":>8}{"SUM":>8}')
    for n in names:
        boot = ROOT / 'build/runs' / n / 'boot.txt'
        if not boot.exists(): print(n, 'missing'); continue
        w, l2, tm = {}, {}, ''
        for l in boot.read_text(errors='replace').splitlines():
            if l.startswith('HWPROF win='): d = kv(l); w[int(d['win'])] = d
            elif l.startswith('HWPROF2 win='): d = kv(l); l2[int(d['win'])] = d
            else:
                m = re.search(r'timed (\d+) gametics in (\d+) realtics', l)
                if m: tm = m.group(2)
        ws = [x for x in sorted(w) if first <= x <= last and x in l2]
        if not ws: print(n, 'no windows'); continue
        def mean(src, k): return sum(float(src[x][k]) for x in ws) / len(ws) / 1e6
        def mean2(k): return sum(float(l2[x][k]) for x in ws) / len(ws) / 1e6
        wall = mean(w, 'wall'); bsp = mean(w, 'bsp'); sp = mean(w, 'sprites')
        ad, so, dr = mean2('addspr'), mean2('sprsort'), mean2('sprdraw')
        def med(vals):
            v = sorted(vals)
            return v[len(v) // 2]
        mw = med([float(w[x]['wall']) / 1e6 for x in ws])
        ma = med([float(l2[x]['addspr']) / 1e6 for x in ws])
        md = med([float(l2[x]['sprdraw']) / 1e6 for x in ws])
        msu = med([(float(l2[x]['addspr']) + float(l2[x]['sprsort']) + float(l2[x]['sprdraw'])) / 1e6 for x in ws])
        print(f'{n:<16}{wall:8.3f}{ad:8.3f}{so:8.3f}{dr:8.3f}{ad+so+dr:8.3f}{bsp:8.3f}{sp:8.3f}{tm:>6}   {"":>22}{mw:7.3f}{ma:8.3f}{md:8.3f}{msu:8.3f}')
main()
