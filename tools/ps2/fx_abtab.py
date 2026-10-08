"""OPT11-FX3: the table of the round-3 goal for pairs of runs: wall and the set "sprites + shadows + HUD + sky + water on the EE" (addspr + sprites (sort + draw) + dome + water + singles imm),
mean cycles per frame of the windows 1..9.

usage: fx_abtab.py TAG_A TAG_B [DEMO ...]        (runs build/runs/<TAG>_d<N>/boot.txt; default demos 1 2 3 4, those that exist)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KEYS = [('wall', 'HWPROF.wall'), ('addspr', 'HWPROF2.addspr'), ('sprites', 'HWPROF.sprites'), ('dome', 'HWPROF12.dome'), ('water', 'HWPROF7.water'), ('imm', 'HWPROF23.imm'), ('shadow', 'HWPROF14.shadow')]


def load(path, first=1):
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
    res = {}
    for g in groups:
        for k, v in g.items():
            res.setdefault(k, []).append(v)
    return {k: sum(v) / len(v) for k, v in res.items()}


def row(d):
    v = {n: d.get(k, 0.0) for n, k in KEYS}
    v['set'] = v['addspr'] + v['sprites'] + v['dome'] + v['water'] + v['imm']
    return v


def main():
    ta, tb = sys.argv[1:3]
    demos = sys.argv[3:] or ['1', '2', '3', '4']
    print('%-4s %-9s %9s %9s %9s %9s %9s %9s %9s %9s' % ('demo', '', 'wall', 'set', 'addspr', 'sprites', 'dome', 'water', 'imm', 'shadow'))
    for n in demos:
        pa = ROOT / ('build/runs/%s_d%s/boot.txt' % (ta, n))
        pb = ROOT / ('build/runs/%s_d%s/boot.txt' % (tb, n))
        if not pa.exists() or not pb.exists():
            continue
        a, b = row(load(pa)), row(load(pb))
        for tag, v in ((ta, a), (tb, b)):
            print('D%-3s %-9s %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f' % (n, tag[-9:], v['wall'], v['set'], v['addspr'], v['sprites'], v['dome'], v['water'], v['imm'], v['shadow']))
        print('D%-3s %-9s %+8.1f%% %+8.1f%% (wall %+.0f K, set %+.0f K)' % (n, 'B-A', 100.0 * (b['wall'] / a['wall'] - 1.0), 100.0 * (b['set'] / a['set'] - 1.0) if a['set'] else 0.0, (b['wall'] - a['wall']) / 1000.0, (b['set'] - a['set']) / 1000.0))


main()
