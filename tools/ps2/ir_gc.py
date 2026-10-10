#!/usr/bin/env python3
"""OPT13 IR: the stored-record counters of a time demo run (build/runs/NAME/boot.txt), mean over the HWPROF windows FIRST..LAST (default 1..9).

HWPROF32 (gcache: hits, misses, key and replay cycles, arena), HWPROF70 (v3 records: polygons replayed as blocks, culled by their box, ...), HWPROF2 laps (seg, plane).
usage: ir_gc.py [--first N] [--last N] NAME [NAME ...]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def nums(line):
    return {m.group(1): m.group(2) for m in re.finditer(r'(\w+)=([^\s|]+)', line)}


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
    for n in names:
        boot = ROOT / 'build/runs' / n / 'boot.txt'
        if not boot.exists():
            print(n, 'missing')
            continue
        win = -1
        acc = {}
        cnt = {}
        for l in boot.read_text(errors='replace').splitlines():
            if l.startswith('HWPROF win='):
                win = int(nums(l)['win'])
                continue
            if win < first or win > last:
                pass
            key = None
            if l.startswith('HWPROF32'):
                key = 'gc'
            elif l.startswith('HWPROF70'):
                key = 'v3'
            elif l.startswith('HWPROF2 '):
                key = 'lap'
            if key is None:
                continue
            # these lines are printed after the 'HWPROF win=' line of their window
            if win < first or win > last:
                continue
            d = nums(l)
            for k, v in d.items():
                if re.fullmatch(r'-?\d+', v):
                    acc[(key, k)] = acc.get((key, k), 0) + int(v)
            cnt[key] = cnt.get(key, 0) + 1
            if key == 'gc':
                m = re.search(r'arena=(\d+)/(\d+)', l)
                if m:
                    acc[('gc', 'arena_used')] = acc.get(('gc', 'arena_used'), 0) + int(m.group(1))
                    acc[('gc', 'arena_half')] = acc.get(('gc', 'arena_half'), 0) + int(m.group(2))
                for tag, rx in (('seg_hit', r'seg hit=(\d+)'), ('seg_miss', r'seg hit=\d+ miss=(\d+)'), ('seg_skip', r'seg hit=\d+ miss=\d+ skip=(\d+)'),
                                ('pl_hit', r'plane hit=(\d+)'), ('pl_miss', r'plane hit=\d+ miss=(\d+)'), ('pl_skip', r'plane hit=\d+ miss=\d+ skip=(\d+)'),
                                ('c_key', r'cycles key=(\d+)'), ('c_hit', r'\) hit=(\d+)'), ('c_miss', r'\) hit=\d+ miss=(\d+)'), ('bad', r'bad=(\d+)')):
                    m = re.search(rx, l)
                    if m:
                        acc[('gc', tag)] = acc.get(('gc', tag), 0) + int(m.group(1))
        def mean(key, k):
            c = cnt.get(key, 0)
            return acc.get((key, k), 0) / c if c else 0.0
        print(f'{n}: windows gc={cnt.get("gc", 0)} v3={cnt.get("v3", 0)} lap={cnt.get("lap", 0)}')
        print('   seg hit %6.0f miss %5.0f skip %5.0f | plane hit %5.0f miss %5.0f skip %5.0f | key %7.0f K  replay %7.0f K  miss %6.0f K | arena %.0f/%.0f KB bad=%d' % (
            mean('gc', 'seg_hit'), mean('gc', 'seg_miss'), mean('gc', 'seg_skip'), mean('gc', 'pl_hit'), mean('gc', 'pl_miss'), mean('gc', 'pl_skip'),
            mean('gc', 'c_key') / 1e3, mean('gc', 'c_hit') / 1e3, mean('gc', 'c_miss') / 1e3, mean('gc', 'arena_used'), mean('gc', 'arena_half'), acc.get(('gc', 'bad'), 0)))
        if cnt.get('lap'):
            print('   laps (K cycles/frame): ' + ' '.join('%s=%.0f' % (k, mean('lap', k) / 1e3) for k in ('seg', 'plane', 'addspr', 'subsec', 'nodedraw')))
        if cnt.get('v3'):
            print('   v3: ' + ' '.join('%s=%.1f' % (k[1], mean('v3', k[1])) for k in sorted(acc) if k[0] == 'v3'))


if __name__ == '__main__':
    main()
