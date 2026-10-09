#!/usr/bin/env python3
"""Table of the HWPROF / HWPROF2 / HWTEX windows of one or more runs (build/runs/NAME/boot.txt).

usage: hwsum.py [--all] NAME [NAME...]       (run directories under build/runs; --runs DIR to change it)
Prints per window the wall time (EE cycles per frame, millions) and its phases, then the mean and max over the windows.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def kv(line):
    d = {}
    for m in re.finditer(r'(\w+)=([^\s|]+)', line):
        d[m.group(1)] = m.group(2)
    return d


def num(s):
    try:
        return float(s)
    except ValueError:
        return 0.0


def main():
    args = sys.argv[1:]
    runs = ROOT / 'build/runs'
    if args and args[0] == '--runs':
        runs = Path(args[1])
        args = args[2:]
    skip = 0
    if args and args[0] == '--skip':
        skip = int(args[1])
        args = args[2:]
    for name in args:
        boot = runs / name / 'boot.txt'
        if not boot.exists():
            print(name, 'no boot.txt')
            continue
        prof, prof2, tex, prof0 = {}, {}, {}, {}
        for line in boot.read_text(errors='replace').splitlines():
            if line.startswith('HWPROF2 '):
                d = kv(line)
                prof2[int(d['win'])] = d
            elif line.startswith('HWPROF0 '):  # OPT12 HWDRV: work cycles and flip cadence
                d = kv(line)
                prof0[int(d['win'])] = d
            elif line.startswith('HWPROF '):
                d = kv(line)
                prof[int(d['win'])] = d
            elif line.startswith('HWTEX '):
                d = kv(line)
                tex[int(d['win'])] = d
        print(f'== {name}: {len(prof)} windows')
        cols = ['wall', 'clear', 'bsp', 'batch', 'sprites', 'tex', 'drv_draw', 'flipwait', 'uploads', 'evict', 'regen', 'decim', 'missing']
        print('win  ' + ' '.join(f'{c:>9}' for c in cols) + '   ws        pool')
        rows = []
        for w in sorted(prof):
            d = prof[w]
            d2 = prof2.get(w, {})
            row = []
            for c in cols:
                if c == 'drv_draw':
                    # "drv draw=N" is parsed as key "draw"
                    v = num(d.get('draw', 0))
                else:
                    v = num(d.get(c, 0))
                row.append(v)
            rows.append(row)
            if w < skip:
                continue
            f = lambda c, v: f'{v / 1e6:9.2f}' if c in ('wall', 'clear', 'bsp', 'batch', 'sprites', 'tex', 'drv_draw', 'flipwait') else f'{v:9.0f}'
            print(f'{w:3d}  ' + ' '.join(f(c, v) for c, v in zip(cols, row)) + f"   {d.get('ws', '')}  {d.get('pool', '')}")
        rr = [r for i, r in enumerate(rows) if i >= skip]
        if rr:
            mean = [sum(r[i] for r in rr) / len(rr) for i in range(len(cols))]
            mx = [max(r[i] for r in rr) for i in range(len(cols))]
            f = lambda c, v: f'{v / 1e6:9.2f}' if c in ('wall', 'clear', 'bsp', 'batch', 'sprites', 'tex', 'drv_draw', 'flipwait') else f'{v:9.0f}'
            print('mean ' + ' '.join(f(c, v) for c, v in zip(cols, mean)))
            print('max  ' + ' '.join(f(c, v) for c, v in zip(cols, mx)))
            print(f'FPS at mean wall: {294.912e6 / mean[0]:.1f}')
        if prof0:
            ws = [w for w in sorted(prof0) if w >= skip]
            works = [num(prof0[w].get('work', 0)) for w in ws]
            walls = [num(prof0[w].get('wall', 0)) for w in ws]
            print('work (wall without waits and sleep), M cycles per frame: ' + ' '.join(f'{x / 1e6:.2f}' for x in works))
            if works:
                print(f'WORK mean {sum(works) / len(works) / 1e6:.2f} max {max(works) / 1e6:.2f} | wall mean {sum(walls) / len(walls) / 1e6:.2f} | FPS at mean work {294.912e6 / (sum(works) / len(works)):.1f}')
            print('cadence (flips by vblanks on screen 0/1/2/3/4+): ' + ' | '.join(prof0[w].get('cad', '?') for w in ws))
        if tex:
            print('HWTEX:')
            for w in sorted(tex):
                d = tex[w]
                print('  ', {k: d[k] for k in ('need', 'up_map', 'up_flat', 'up_t8o', 'reup', 'shared', 'regen', 'zc', 'upcyc', 'pool', 'lod', 'invis') if k in d})


if __name__ == '__main__':
    main()
