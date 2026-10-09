"""Percentiles of the per-tic NETLATTRACE lines (engine options -netlat -netlattrace) of one or more runs.

usage: python3 tools/ps2/net_trace.py RUNDIR [RUNDIR ...]   (RUNDIR/cli/boot.txt)
arrive->take: the datagram reached the socket -> the game took it off the ring; take->run: -> the tic was run; arrive->run: the sum (what a tic waits for in the client);
backlog: tics received and not run yet when a tic starts to run (1 = the tic itself); pass: clock / early.
"""
import re
import sys
from pathlib import Path

L = re.compile(r'NETLATTRACE tic=(\d+) arrive->take=(\d+) take->run=(\d+) arrive->run=(\d+) us backlog=(\d+) pass=(\w+) realtics=(\d+)')


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p / 100))] if v else 0


def main():
    rows = []
    for d in sys.argv[1:]:
        for ln in Path(d, 'cli', 'boot.txt').read_text(errors='replace').splitlines():
            m = L.search(ln)
            if m:
                rows.append(tuple(int(x) if x.isdigit() else x for x in m.groups()))
    if not rows:
        print('no NETLATTRACE lines')
        return
    print(f'{len(rows)} tics from {len(sys.argv) - 1} runs; early passes: {sum(1 for r in rows if r[5] == "early")}')
    for name, idx, div in (('arrive->take', 1, 1000), ('take->run', 2, 1000), ('arrive->run', 3, 1000), ('backlog', 4, 1)):
        v = [r[idx] / div for r in rows]
        print(f'  {name:13s} p10 {pct(v, 10):7.1f}  p25 {pct(v, 25):7.1f}  median {pct(v, 50):7.1f}  p75 {pct(v, 75):7.1f}  p90 {pct(v, 90):7.1f}  p99 {pct(v, 99):7.1f}  mean {sum(v) / len(v):7.1f}')


if __name__ == '__main__':
    main()
