"""OPT11-FX2: mean cycles per frame of every key=value of the HWPROF lines (windows first..9) of one or two -ps2prof runs (boot.txt), A/B ratio.

usage: fx_prof.py A/boot.txt [B/boot.txt] [--first N] [--min V] [--grep REGEX]
Keys are LINE.key (HWPROF.wall, HWPROF2.sprdraw, HWPROF14.shadow ...), values are per-frame cycles as the engine prints them (the window sums are divided by 105 frames).
HWPROF3/HWPROF4 counters (polygons, batches) are counts per frame, not cycles.
"""
import re
import sys


def load(path, first):
    groups = []
    cur = None
    for l in open(path, errors='replace'):
        m = re.match(r'(HWPROF\d*) (win=(\d+) )?(.*)', l)
        if not m:
            continue
        name, _, win, rest = m.groups()
        if name == 'HWPROF' and win is not None:
            cur = {'_win': int(win)}
            groups.append(cur)
        if cur is None:
            continue
        for k, v in re.findall(r'(\w+)=(\d+)', rest):
            cur[name + '.' + k] = int(v)
    groups = [g for g in groups if g['_win'] >= first]
    keys = []
    for g in groups:
        for k in g:
            if k not in keys and k != '_win':
                keys.append(k)
    res = {}
    for k in keys:
        vals = [g[k] for g in groups if k in g]
        res[k] = (sum(vals) / len(vals), max(vals))
    timed = None
    for l in open(path, errors='replace'):
        m = re.search(r'timed (\d+) gametics in (\d+) realtics', l)
        if m:
            timed = (int(m.group(1)), int(m.group(2)))
    return res, len(groups), timed


def main():
    args = sys.argv[1:]
    first, vmin, rx = 1, 0, None
    files = []
    i = 0
    while i < len(args):
        if args[i] == '--first':
            first = int(args[i + 1]); i += 2
        elif args[i] == '--min':
            vmin = float(args[i + 1]); i += 2
        elif args[i] == '--grep':
            rx = re.compile(args[i + 1]); i += 2
        else:
            files.append(args[i]); i += 1
    a, na, ta = load(files[0], first)
    b, nb, tb = load(files[1], first) if len(files) > 1 else (None, 0, None)
    print('windows A %d%s  demo A %s' % (na, (' B %d' % nb) if b else '', ta))
    if b:
        print('demo B %s' % (tb,))
    for k, (v, mx) in a.items():
        if rx and not rx.search(k):
            continue
        if v < vmin and (not b or b.get(k, (0, 0))[0] < vmin):
            continue
        if b:
            vb = b.get(k, (0, 0))[0]
            print('%-22s %12.0f %12.0f %8.3f' % (k, v, vb, vb / v if v else 0))
        else:
            print('%-22s %12.0f  (max %.0f)' % (k, v, mx))


if __name__ == '__main__':
    main()
