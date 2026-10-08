#!/usr/bin/env python3
"""OPT12 HWDRV: one line per run (windows 1..9 of the HWPROF lines): wall mean/max, work mean, FPS at mean wall, flip wait, heavy HWREGEN events, missing draws.
usage: hwdrv_ab.py RUN [RUN...]   (build/runs/RUN/boot.txt)"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    print(f"{'run':24s} {'wall':>6s} {'max':>6s} {'work':>6s} {'FPS':>5s} {'flip':>5s} {'drv+pl':>6s} {'evict':>5s} {'regen>20M':>9s} {'missing':>7s} {'ticks/real':>12s}")
    for name in sys.argv[1:]:
        boot = ROOT / 'build/runs' / name / 'boot.txt'
        if not boot.exists():
            print(f'{name:24s} no boot.txt')
            continue
        wall, work, flip, drv, evict, miss = {}, {}, {}, {}, {}, {}
        heavy = 0
        tr = ''
        last = -1
        for line in boot.read_text(errors='replace').splitlines():
            m = re.match(r'HWPROF win=(\d+) .*?wall=(\d+)', line)
            if m:
                w = int(m.group(1)); last = w
                wall[w] = float(m.group(2))
                flip[w] = float((re.search(r'flipwait=(\d+)', line) or [0, 0])[1])
                evict[w] = float((re.search(r' evict=(\d+)', line) or [0, 0])[1])
                miss[w] = float((re.search(r' missing=(\d+)', line) or [0, 0])[1])
                continue
            m = re.match(r'HWPROF0 win=(\d+) work=(\d+)', line)
            if m:
                work[int(m.group(1))] = float(m.group(2))
                continue
            m = re.match(r'HWPROF23 driver total=(\d+) \(\+ plan (\d+)', line)
            if m and last >= 0:
                drv[last] = float(m.group(1)) + float(m.group(2))
                continue
            m = re.search(r'HWREGEN .* made again in (\d+) cycles', line)
            if m and int(m.group(1)) > 20000000:
                heavy += 1
            m = re.search(r'timed (\d+) gametics in (\d+) realtics', line)
            if m:
                tr = f'{m.group(1)}/{m.group(2)}'
        ws = [w for w in sorted(wall) if w >= 1]
        if not ws:
            print(f'{name:24s} no windows')
            continue
        mean = lambda d: sum(d.get(w, 0) for w in ws) / len(ws)
        print(f"{name:24s} {mean(wall) / 1e6:6.2f} {max(wall[w] for w in ws) / 1e6:6.2f} {mean(work) / 1e6:6.2f} {294.912e6 / mean(wall):5.1f} {mean(flip) / 1e6:5.2f} {mean(drv) / 1e6:6.2f} {sum(evict.get(w, 0) for w in ws):5.0f} {heavy:9d} {sum(miss.get(w, 0) for w in ws):7.0f} {tr:>12s}")


if __name__ == '__main__':
    main()
