"""OPT11-FX2: compare the vidshot PPMs of two runs (same names in both run directories): pixels that differ, largest channel difference, mean absolute difference.

usage: fx_ppmcmp.py RUN_A RUN_B [SKIP_TOP_ROWS]   (names under build/runs/; the top rows hold the console lines of -ps2prof runs, which differ between runs: pass 40 to leave them out)
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read_ppm(p):
    data = p.read_bytes()
    parts = data.split(b'\n', 3)
    w, h = [int(x) for x in parts[1].split()]
    return w, h, parts[3]


def main():
    a, b = ROOT / 'build/runs' / sys.argv[1], ROOT / 'build/runs' / sys.argv[2]
    skip = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    names = sorted(p.name for p in a.glob('vidshot-*.ppm') if (b / p.name).exists())
    if not names:
        print('no common vidshots')
        return
    tot_diff = 0
    for n in names:
        w, h, da = read_ppm(a / n)
        w2, h2, db = read_ppm(b / n)
        if (w, h) != (w2, h2) or len(da) != len(db):
            print('%s: size differs' % n)
            continue
        nd = 0
        mx = 0
        sm = 0
        for i in range(skip * w * 3, len(da), 3):
            if da[i:i + 3] != db[i:i + 3]:
                nd += 1
                d = max(abs(da[i] - db[i]), abs(da[i + 1] - db[i + 1]), abs(da[i + 2] - db[i + 2]))
                mx = max(mx, d)
                sm += d
        tot_diff += nd
        print('%-34s pixels %d, differing %d, max channel diff %d, mean over differing %.1f' % (n, w * h, nd, mx, (sm / nd) if nd else 0.0))
    print('TOTAL differing pixels: %d in %d shots' % (tot_diff, len(names)))


if __name__ == '__main__':
    main()
