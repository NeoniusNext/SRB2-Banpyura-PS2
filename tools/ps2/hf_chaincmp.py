"""OPT10-HF: pictures of a map-change chain against themselves: shot i of map M against the first shot of the same map (period = length of the map list; the first occurrence is left out: it is the load from the title).
A map drawn for the 8th time after 50 map changes must look like the first time (same frame of the level; garbage, stale textures, wrong palette would show).

usage: hf_chaincmp.py run_dir [period=7] [tag=k5]
"""
import sys
from pathlib import Path

from PIL import Image


def load(p):
    return Image.open(p).convert('RGB')


def mad(a, b):
    pa, pb = a.tobytes(), b.tobytes()
    return sum(abs(x - y) for x, y in zip(pa, pb)) / float(len(pa))


def main():
    d = Path(sys.argv[1])
    period = int(sys.argv[2]) if len(sys.argv) > 2 else 7
    tag = sys.argv[3] if len(sys.argv) > 3 else 'k5'
    shots = {}
    for p in d.glob('vidshot-*-%s_*.ppm' % tag):
        shots[int(p.stem.rsplit('_', 1)[1])] = p
    n = max(shots) + 1
    worst = {}
    for i in range(2 * period, n):  # the first shot of a family is the load from the title / the previous map: compare the repeats with each other
        if i not in shots or (i - period) not in shots:
            continue
        m = mad(load(shots[i - period]), load(shots[i]))
        worst.setdefault(i % period, []).append((m, i))
    for k in sorted(worst):
        ms = [m for m, _ in worst[k]]
        print('family %d (shots %s...): %d repeats, MAD to the previous repeat: mean %.2f max %.2f (shot %d)' % (k, k, len(ms), sum(ms) / len(ms), max(ms), max(worst[k])[1]))


if __name__ == '__main__':
    main()
