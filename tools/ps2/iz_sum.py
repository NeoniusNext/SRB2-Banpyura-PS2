#!/usr/bin/env python3
"""OPT13 IZ: one line per timedemo run (build/runs/NAME/boot.txt): realtics, mean HW frame (M cycles, HWPROF windows 1..9), the worst frame of a window (HWPROF0 wmax: wall cycles of one
frame, includes the vsync wait), frames above 15 M, the HWREGEN events (textures made again for more than 2 M cycles: number, sum), the ZMODE counters (first-try failures, frontier calls,
cycles in them), the geometry cache state per window and the TEXC counters.
usage: iz_sum.py NAME-or-glob ...   (run from the repository root)"""
import glob
import os
import re
import sys


def key(p):
    return [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', p)]


def summarize(run):
    b = os.path.join(run, 'boot.txt')
    if not os.path.exists(b):
        return None
    walls, wmax, spikes, rt = [], [], 0, None
    hist = [0] * 40
    nreg, sreg = 0, 0
    slow = [0, 0]
    lru = [0, 0]
    front = [0, 0, 0, 0]
    gcs = []
    texc = [0, 0, 0, 0, 0]
    regen_dev = 0
    for l in open(b, errors='replace'):
        m = re.match(r'HWPROF win=(\d+) .* wall=(\d+)', l)
        if m and 1 <= int(m.group(1)) <= 9:
            walls.append(int(re.search(r' wall=(\d+)', l).group(1)) / 1e6)
        m = re.match(r'HWFH win=(\d+) (.*)', l)
        if m and 1 <= int(m.group(1)) <= 9:
            for k, c in enumerate(m.group(2).split()):
                hist[k] += int(c)
        m = re.match(r'HWPROF0 win=(\d+) .* wmax=(\d+) spikes=(\d+)', l)
        if m and 1 <= int(m.group(1)) <= 9:
            wmax.append(int(m.group(2)) / 1e6)
            spikes += int(m.group(3))
        m = re.search(r'timed \d+ gametics in (\d+) realtics', l)
        if m:
            rt = int(m.group(1))
        m = re.match(r'HWREGEN f=\d+ .* made again in (\d+) cycles', l)
        if m:
            nreg += 1
            sreg += int(m.group(1))
        m = re.match(r'ZMODE cap=.*\| slow=(\d+) \((\d+) cyc\) front calls=(\d+) failed=(\d+)(?: \((\d+) from memory\))? walked=(\d+) \((\d+) cyc\)', l)
        if m:
            slow[0] += int(m.group(1))
            slow[1] += int(m.group(2))
            front[0] += int(m.group(3))
            front[1] += int(m.group(4))
            front[2] += int(m.group(7))
            front[3] += int(m.group(5) or 0)
        m = re.search(r'lru last resort (\d+) \(partial (\d+)\)', l)
        if m and l.startswith('ZMODE'):
            lru[0] += int(m.group(1))
            lru[1] += int(m.group(2))
        if l.startswith('HWPROF32'):
            on = re.search(r'on=(\d)', l)
            rec = re.search(r'reclaim=(\d+)', l)
            gcs.append('R' if rec and int(rec.group(1)) else ('.' if on and on.group(1) == '1' else 'o'))
        m = re.match(r'HWTEXC pack=\d+ textures: composites used (\d+) \(stored form in memory (\d+), read from the pack (\d+)\) cycles (\d+)', l)
        if m:
            for i in range(4):
                texc[i] += int(m.group(i + 1))
    if not walls:
        return None
    tot = sum(hist)
    pct = []
    for q in (0.5, 0.9, 0.99):
        acc, v = 0, 0
        for k, c in enumerate(hist):
            acc += c
            if tot and acc >= q * tot:
                v = k + 1
                break
        pct.append(v)
    return dict(pct=pct, rt=rt, avg=sum(walls) / len(walls), mx=max(walls), wmax=max(wmax) if wmax else 0.0, spikes=spikes, nreg=nreg, sreg=sreg / 1e6,
                slow=slow, lru=lru, front=front, gc=''.join(gcs), texc=texc)


def main():
    print('%-22s %7s %7s %7s %7s %6s %9s | %5s %7s | %6s %9s | %5s %6s %6s %7s | %-10s | %s' % ('run', 'realtic', 'avg M', 'win M', 'wmax M', '>15M', 'p50/90/99', 'regen', 'sum M', 'slow', 'cyc', 'fcall', 'ffail', 'fneg', 'fcyc', 'geom', 'lru fl/part  texc used/res/read/cyc'))
    for pat in sys.argv[1:]:
        runs = sorted(glob.glob(os.path.join('build/runs', pat)), key=key)
        for run in runs:
            if not os.path.isdir(run):
                continue
            r = summarize(run)
            if not r:
                continue
            print('%-22s %7s %7.3f %7.2f %7.1f %6d %9s | %5d %7.1f | %6d %9d | %5d %6d %6d %7d | %-10s | %s' % (os.path.basename(run), r['rt'], r['avg'], r['mx'], r['wmax'], r['spikes'], '%d/%d/%d' % tuple(r['pct']), r['nreg'], r['sreg'],
                  r['slow'][0], r['slow'][1], r['front'][0], r['front'][1], r['front'][3], r['front'][2], r['gc'], '%d/%d  ' % tuple(r['lru']) + '/'.join(str(x) for x in r['texc'])))


if __name__ == '__main__':
    main()
