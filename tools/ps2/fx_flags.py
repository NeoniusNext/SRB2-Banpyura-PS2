"""OPT11-FX2: the A/B table of single switches of -hwfx on one ELF: for every run <prefix><flag>_d<N> the mean cycles per frame (windows 1..9) of wall, sprites, bsp, sprite parts and the
dome, as a difference to the run with all paths on (<prefix>0) and to the run with all paths off (<prefix><all_off>).

usage: fx_flags.py PREFIX DEMO ALLOFF FLAG...      e.g.  fx_flags.py r2x 1 381 1 4 8 16 32 64 256
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(path):
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
    groups = [g for g in groups if g['_win'] >= 1]
    res = {}
    for k in groups[0]:
        if k == '_win':
            continue
        vals = [g[k] for g in groups if k in g]
        res[k] = sum(vals) / len(vals)
    return res


def main():
    prefix, demo, alloff = sys.argv[1], sys.argv[2], sys.argv[3]
    flags = sys.argv[4:]
    keys = ['HWPROF.wall', 'HWPROF.bsp', 'HWPROF.sprites', 'HWPROF2.addspr', 'HWPROF2.sprsort', 'HWPROF2.sprdraw', 'HWPROF12.dome', 'HWPROF14.shadow']

    def run(f):
        return load(ROOT / 'build/runs' / ('%s%s_d%s' % (prefix, f, demo)) / 'boot.txt')

    on, off = run('0'), run(alloff)
    print('%-22s %10s %10s' % ('', 'all on', 'all off'))
    for k in keys:
        print('%-22s %10.0f %10.0f   (on - off = %+.0f)' % (k, on[k], off[k], on[k] - off[k]))
    print()
    print('single switch OFF (-hwfx N): the cost relative to all paths on (positive = the path saves that much)')
    print('%-8s' % 'flag' + ''.join('%16s' % k.split('.')[1] for k in keys))
    for f in flags:
        r = run(f)
        print('%-8s' % f + ''.join('%+16.0f' % (r[k] - on[k]) for k in keys))


if __name__ == '__main__':
    main()
