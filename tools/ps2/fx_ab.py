"""OPT11-FX: A/B of two HWPROF runs (boot.txt of -ps2prof runs on the same ELF with and without a -hwdbg flag): mean and max over the windows 1..9 of
the cycles per frame of the fields that matter for the water, plus the demo time.

usage: fx_ab.py A/boot.txt B/boot.txt [first_window]
HWPROF7 'emit cyc ... water=N' is the driver's cycles of the water polygons per frame; 'wall' of the HWPROF windows is the frame of the engine side (units of the HWPROF line).
"""
import re
import sys


def load(path, first):
    wins = []
    water = []
    text = open(path, errors='replace').read().splitlines()
    wi = -1
    for l in text:
        m = re.match(r'HWPROF win=(\d+) frames=(\d+) (.*)', l)
        if m:
            wi = int(m.group(1))
            if wi >= first:
                d = dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', m.group(3)))
                d['_frames'] = int(m.group(2))
                wins.append(d)
            continue
        m = re.match(r'HWPROF7 .*water=(\d+)', l)
        if m and wi >= first:
            water.append(int(m.group(1)))
    timed = re.search(r'timed (\d+) gametics in (\d+) realtics', '\n'.join(text))
    return wins, water, timed


def stats(vals):
    return (sum(vals) / len(vals), max(vals)) if vals else (0, 0)


def main():
    a, b = sys.argv[1], sys.argv[2]
    first = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    wa, wta, ta = load(a, first)
    wb, wtb, tb = load(b, first)
    print('%-12s %14s %14s %8s' % ('field', 'A mean / max', 'B mean / max', 'B/A'))
    for k in ['wall', 'bsp', 'batch', 'sprites', 'nodes', 'draw', 'polys', 'vout']:
        va = stats([w[k] / w['_frames'] for w in wa if k in w])
        vb = stats([w[k] / w['_frames'] for w in wb if k in w])
        print('%-12s %7.0f/%-7.0f %7.0f/%-7.0f %8.3f' % (k, va[0], va[1], vb[0], vb[1], vb[0] / va[0] if va[0] else 0))
    va, vb = stats(wta), stats(wtb)
    print('%-12s %7.0f/%-7.0f %7.0f/%-7.0f %8.3f' % ('water emit', va[0], va[1], vb[0], vb[1], vb[0] / va[0] if va[0] else 0))
    if ta and tb:
        print('demo time: A %s tics in %s realtics, B %s in %s' % (ta.group(1), ta.group(2), tb.group(1), tb.group(2)))


if __name__ == '__main__':
    main()
