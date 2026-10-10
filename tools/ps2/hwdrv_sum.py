#!/usr/bin/env python3
"""OPT12 HWDRV: the driver side of a HWPROF run in one table (windows 1..9 unless --skip N): wall, work, flip wait, driver total (HWPROF23, all driver
calls except the waits) + planner, settex, singles, the texture subsystem counters.  usage: hwdrv_sum.py [--skip N] NAME [NAME...]  (build/runs/NAME/boot.txt)"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def num(s):
    try:
        return float(s)
    except ValueError:
        return 0.0


def main():
    a = sys.argv[1:]
    skip = 1
    if a and a[0] == '--skip':
        skip = int(a[1])
        a = a[2:]
    for name in a:
        boot = ROOT / 'build/runs' / name / 'boot.txt'
        if not boot.exists():
            print(name, 'no boot.txt')
            continue
        win = {}
        for line in boot.read_text(errors='replace').splitlines():
            m = re.match(r'HWPROF win=(\d+) .*?wall=(\d+)', line)
            if m:
                w = int(m.group(1))
                d = win.setdefault(w, {})
                d['wall'] = num(m.group(2))
                m2 = re.search(r'flipwait=(\d+)', line)
                d['flip'] = num(m2.group(1)) if m2 else 0
                m2 = re.search(r' draw=(\d+) tex=(\d+) wait=(\d+)', line)
                if m2:
                    d['draw'], d['tex'], d['wait'] = num(m2.group(1)), num(m2.group(2)), num(m2.group(3))
                continue
            m = re.match(r'HWPROF0 win=(\d+) work=(\d+)', line)
            if m:
                win.setdefault(int(m.group(1)), {})['work'] = num(m.group(2))
                continue
            m = re.match(r'HWPROF23 driver total=(\d+) \(\+ plan (\d+).*?settex n=(\d+) cyc=(\d+)', line)
            if m:
                # HWPROF23 follows its window's HWPROF line: attach to the latest window
                w = max(win) if win else 0
                d = win.setdefault(w, {})
                d['drv'], d['plan'], d['setn'], d['setcyc'] = num(m.group(1)), num(m.group(2)), num(m.group(3)), num(m.group(4))
        print(f'== {name}')
        print('win     wall     work   flipwait  drvtotal   +plan  settex(n)  settex_cyc')
        rows = []
        for w in sorted(win):
            d = win[w]
            if w < skip or 'drv' not in d:
                continue
            rows.append(d)
            print(f"{w:3d} {d['wall'] / 1e6:8.2f} {d.get('work', 0) / 1e6:8.2f} {d['flip'] / 1e6:9.2f} {d['drv'] / 1e6:9.2f} {d['plan'] / 1e6:7.2f} {d['setn']:9.0f} {d['setcyc'] / 1e6:11.2f}")
        if rows:
            n = len(rows)
            m = lambda k: sum(r.get(k, 0) for r in rows) / n / 1e6
            print(f"mean {m('wall'):7.2f} {m('work'):8.2f} {m('flip'):9.2f} {m('drv'):9.2f} {m('plan'):7.2f}  driver+planner mean {m('drv') + m('plan'):.2f} max {max(r['drv'] + r['plan'] for r in rows) / 1e6:.2f}")


if __name__ == '__main__':
    main()
