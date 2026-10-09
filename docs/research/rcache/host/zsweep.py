#!/usr/bin/env python3
"""OPT13-RCACHE: table of the zone-lottery sweep: per run the timedemo realtics, mean HW frame (M cycles, windows 1..9), failed first-try allocations / cycles in Z_MoveFrontier ([zslow] of the last -zreport),
whether the geometry cache was reclaimed (HWPROF32 reclaim=1) and the final cache state. usage: zsweep.py PREFIX [PREFIX...]   (runs in build/runs/PREFIX*)"""
import glob, os, re, sys
def key(p): return [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', p)]
print('%-12s %7s %8s %8s | %8s %10s %11s | %s' % ('run', 'realtic', 'avg M', 'max M', 'slow n', 'front Mcyc', 'make-room M', 'geometry cache'))
for pfx in sys.argv[1:]:
    for run in sorted(glob.glob('build/runs/%s*' % pfx), key=key):
        b = os.path.join(run, 'boot.txt')
        if not os.path.isdir(run) or not os.path.exists(b): continue
        walls = []; rt = None; slow = None; room = None; states = []
        for l in open(b, errors='replace'):
            m = re.match(r'HWPROF win=(\d+) .* wall=(\d+)', l)
            if m and 1 <= int(m.group(1)) <= 9: walls.append(int(re.search(r' wall=(\d+)', l).group(1)) / 1e6)
            m = re.search(r'timed \d+ gametics in (\d+) realtics', l)
            if m: rt = int(m.group(1))
            m = re.search(r'\[zslow\] failed-first-try allocations (\d+) cycles (\d+) \| frontier moves (\d+) cycles (\d+)', l)
            if m: slow = (int(m.group(1)), int(m.group(4)) / 1e6)
            m = re.search(r'make-room (\d+) cycles (\d+)', l)
            if m: room = int(m.group(2)) / 1e6
            if l.startswith('HWPROF32'):
                on = re.search(r'on=(\d)', l); rec = re.search(r'reclaim=(\d+)', l)
                states.append('R' if rec and int(rec.group(1)) else ('.' if on and on.group(1) == '1' else 'o'))
        if not walls: continue
        print('%-12s %7s %8.3f %8.2f | %8s %10s %11s | %s' % (os.path.basename(run), rt, sum(walls) / len(walls), max(walls),
              slow[0] if slow else '-', '%.0f' % slow[1] if slow else '-', '%.1f' % room if room is not None else '-', ''.join(states)))
