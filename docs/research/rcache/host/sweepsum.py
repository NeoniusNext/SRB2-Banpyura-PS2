#!/usr/bin/env python3
"""OPT13-RCACHE: summary of a set of HW runs (HWPROF win= lines of build/runs/<prefix>*/boot.txt): average and maximum wall of windows 1..9 (M cycles per frame), number of windows with wall > 1.25 x median,
regenerations/evictions per window, realtics of the timedemo. usage: sweepsum.py PREFIX [PREFIX...]"""
import glob, re, statistics, sys, os
def parse(run):
    wins = {}
    rt = None
    for l in open(os.path.join(run, 'boot.txt'), errors='replace'):
        if l.startswith('HWPROF win='):
            d = dict(re.findall(r'(\w+)=([\d/]+)', l))
            wins[int(d['win'])] = d
        m = re.search(r'timed \d+ gametics in (\d+) realtics', l)
        if m: rt = int(m.group(1))
    return wins, rt
print('%-14s %7s %8s %8s %6s %7s %7s %7s %6s' % ('run', 'realtic', 'avg M', 'max M', 'spikes', 'regen', 'evict', 'upload', 'free?'))
for pfx in sys.argv[1:]:
    for run in sorted(glob.glob('build/runs/%s*' % pfx), key=lambda p: [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', p)]):
        if not os.path.isdir(run) or not os.path.exists(run + '/boot.txt'): continue
        w, rt = parse(run)
        ws = [int(w[i]['wall']) / 1e6 for i in range(1, 10) if i in w]
        if not ws: continue
        med = statistics.median(ws)
        reg = sum(int(w[i]['regen']) for i in range(1, 10) if i in w)
        ev = sum(int(w[i]['evict']) for i in range(1, 10) if i in w)
        up = sum(int(w[i]['uploads']) for i in range(1, 10) if i in w)
        print('%-14s %7s %8.3f %8.2f %6d %7d %7d %7d' % (os.path.basename(run), rt, sum(ws) / len(ws), max(ws), sum(1 for x in ws if x > 1.25 * med), reg, ev, up))
