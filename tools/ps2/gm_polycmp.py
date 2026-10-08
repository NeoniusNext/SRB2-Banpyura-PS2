#!/usr/bin/env python3
"""Compare the polygon streams of two runs made with -hwpolyhash (OPT11 GEOM, hardware/hw_batching.c).

usage: gm_polycmp.py RUN_A RUN_B [--runs DIR] [--show N] [--order] [--world]
Reads the HWPH lines (frame, polygon count, 64 bit hash of every polygon the engine handed to HWR_ProcessPolygon in the frame: flags, shader, texture
identity, surface, vertices) of build/runs/RUN/boot.txt. Prints the number of frames compared, the number that differ and the first differences.
With --order the order of the polygons after the sort of HWR_RenderBatches (o=, OPT11 round 2) is compared too: the order of the batches must not depend on the
memory layout of the build.
With --world only the polygons with a map texture or flat (w=, the world: walls, planes) are compared, not the sprites: a static view of a map has random particles,
so two runs of one map differ in the sprites whatever the build does.
Exit code 0 when the streams are equal for every frame both runs have (the first frame is skipped: the switch is read at its end).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(path):
    d = {}
    for line in Path(path).read_text(errors='replace').splitlines():
        m = re.match(r'HWPH f=(\d+) n=(\d+) h=([0-9a-f]+)(?: o=([0-9a-f]+))?(?: w=(\d+):([0-9a-f]+))?(?: s=([0-9a-f]+) v=(\S+))?', line)
        if m:
            d[int(m.group(1))] = (int(m.group(2)), m.group(3), m.group(4), m.group(5), m.group(6), m.group(7), m.group(8))
    return d


def main():
    args = sys.argv[1:]
    runs = ROOT / 'build/runs'
    show = 10
    order = False
    world = False
    names = []
    i = 0
    while i < len(args):
        if args[i] == '--runs':
            runs = Path(args[i + 1]); i += 2
        elif args[i] == '--world':
            world = True; i += 1
        elif args[i] == '--order':
            order = True; i += 1
        elif args[i] == '--show':
            show = int(args[i + 1]); i += 2
        else:
            names.append(args[i]); i += 1
    a = load(runs / names[0] / 'boot.txt')
    b = load(runs / names[1] / 'boot.txt')
    common = sorted(set(a) & set(b))
    common = [f for f in common if f > 1]
    if world:
        bad = [f for f in common if a[f][3:5] != b[f][3:5]]
    else:
        bad = [f for f in common if a[f][:2] != b[f][:2] or (order and a[f][2] != b[f][2])]
    print(f'{names[0]}: {len(a)} frames, {names[1]}: {len(b)} frames, compared {len(common)}, differ {len(bad)}')
    for f in bad[:show]:
        if a[f][5] and b[f][5]:
            print(f'  frame {f}: sectors {a[f][5]} / {b[f][5]}  view {a[f][6]} / {b[f][6]}')
        print(f'  frame {f}: polygons {a[f][0]} / {b[f][0]}  hash {a[f][1]} / {b[f][1]}  order {a[f][2]} / {b[f][2]}  world {a[f][3]}:{a[f][4]} / {b[f][3]}:{b[f][4]}')
    return 1 if bad or not common else 0


if __name__ == '__main__':
    sys.exit(main())
