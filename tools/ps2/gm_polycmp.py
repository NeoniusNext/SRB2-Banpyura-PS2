#!/usr/bin/env python3
"""Compare the polygon streams of two runs made with -hwpolyhash (OPT11 GEOM, hardware/hw_batching.c).

usage: gm_polycmp.py RUN_A RUN_B [--runs DIR] [--show N]
Reads the HWPH lines (frame, polygon count, 64 bit hash of every polygon the engine handed to HWR_ProcessPolygon in the frame: flags, shader, texture
identity, surface, vertices) of build/runs/RUN/boot.txt. Prints the number of frames compared, the number that differ and the first differences.
Exit code 0 when the streams are equal for every frame both runs have (the first frame is skipped: the switch is read at its end).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(path):
    d = {}
    for line in Path(path).read_text(errors='replace').splitlines():
        m = re.match(r'HWPH f=(\d+) n=(\d+) h=([0-9a-f]+)', line)
        if m:
            d[int(m.group(1))] = (int(m.group(2)), m.group(3))
    return d


def main():
    args = sys.argv[1:]
    runs = ROOT / 'build/runs'
    show = 10
    names = []
    i = 0
    while i < len(args):
        if args[i] == '--runs':
            runs = Path(args[i + 1]); i += 2
        elif args[i] == '--show':
            show = int(args[i + 1]); i += 2
        else:
            names.append(args[i]); i += 1
    a = load(runs / names[0] / 'boot.txt')
    b = load(runs / names[1] / 'boot.txt')
    common = sorted(set(a) & set(b))
    common = [f for f in common if f > 1]
    bad = [f for f in common if a[f] != b[f]]
    print(f'{names[0]}: {len(a)} frames, {names[1]}: {len(b)} frames, compared {len(common)}, differ {len(bad)}')
    for f in bad[:show]:
        print(f'  frame {f}: polygons {a[f][0]} / {b[f][0]}  hash {a[f][1]} / {b[f][1]}')
    return 1 if bad or not common else 0


if __name__ == '__main__':
    sys.exit(main())
