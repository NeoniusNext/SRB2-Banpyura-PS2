"""OPT10-HF: pictures of a map-change chain against themselves. The maps repeat (period = length of the map list), but the shot a map lands on can shift when
a frame is missed, so every shot is compared with all the others: a map drawn for the 8th time after 50 map changes must look like one of its earlier
showings (garbage, stale textures, a wrong palette have no counterpart). Prints per shot the best MAD to another shot and the shot it is; shots whose best
MAD is above the limit are listed as suspicious.

usage: hf_chaincmp.py run_dir [limit=6] [tag=k5]
"""
import sys
from pathlib import Path

from PIL import Image


def load(p):
    # the console overlay of the first rows (VIDSHOT / Speeding off lines) is not part of the picture: compare below row 40
    im = Image.open(p).convert('RGB')
    return im.crop((0, 40, im.width, im.height)).tobytes()


def mad(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / float(len(a))


def main():
    d = Path(sys.argv[1])
    limit = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
    tag = sys.argv[3] if len(sys.argv) > 3 else 'k5'
    shots = {}
    for p in d.glob('vidshot-*-%s_*.ppm' % tag):
        shots[int(p.stem.rsplit('_', 1)[1])] = load(p)
    ids = sorted(shots)
    bad = []
    best_all = []
    for i in ids:
        best, bj = 1e9, -1
        for j in ids:
            if j == i:
                continue
            m = mad(shots[i], shots[j])
            if m < best:
                best, bj = m, j
        best_all.append(best)
        if best > limit:
            bad.append((i, best, bj))
    print('%d shots; best MAD to another shot: mean %.2f max %.2f' % (len(ids), sum(best_all) / len(best_all), max(best_all)))
    for i, b, j in bad:
        print('  suspicious: shot %d best MAD %.2f (to shot %d)' % (i, b, j))
    if not bad:
        print('  every shot has a counterpart below %.1f: no garbage frames' % limit)


if __name__ == '__main__':
    main()
