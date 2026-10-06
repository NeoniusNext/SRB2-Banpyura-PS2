"""Compare the NETSYNC lines (src/netcode/d_clisrv.c, -netsync) of two machines of one network game.

usage: python tools/ps2/netsync_compare.py A.log B.log [--min-players 2] [--json OUT]
Every line has gametic, leveltime, players, state (hash of all players' position/momentum/angle/rings/score), cons (the engine's own 16-bit
consistancy value) and rnd (P_GetRandSeed). For each gametic present in both logs everything but nothing else must be equal: any difference is a
desynchronisation. Prints the counts and exits 0 only if at least one common tic was compared (--min-common N) and there is no difference.
"""
import argparse
import json
import re
import sys

LINE = re.compile(r'NETSYNC gametic=(\d+) leveltime=(\d+) players=(\d+) state=([0-9a-f]+) cons=(\d+) rnd=(\d+)')


def parse(path):
    out = {}
    for m in LINE.finditer(open(path, errors='replace').read()):
        g = int(m.group(1))
        out.setdefault(g, []).append(m.groups()[1:])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--min-players', type=int, default=1)
    ap.add_argument('--min-common', type=int, default=5)
    ap.add_argument('--json', default='')
    a = ap.parse_args()
    A, B = parse(a.a), parse(a.b)
    common = sorted(set(A) & set(B))
    diffs, multi = [], 0
    compared = 0
    for g in common:
        la, lb = A[g], B[g]
        # the same tic can be printed twice after a resync/map change: compare the last of each
        if len(la) > 1 or len(lb) > 1:
            multi += 1
        va, vb = la[-1], lb[-1]
        if int(va[1]) < a.min_players:
            continue
        compared += 1
        if va != vb:
            diffs.append({'gametic': g, 'a': va, 'b': vb})
    res = {'a_lines': sum(map(len, A.values())), 'b_lines': sum(map(len, B.values())), 'common_tics': len(common), 'compared': compared,
           'repeated_tics': multi, 'differences': len(diffs), 'first_differences': diffs[:5],
           'range': [common[0], common[-1]] if common else None}
    print(json.dumps(res, indent=1))
    if a.json:
        open(a.json, 'w').write(json.dumps(res, indent=1))
    return 0 if compared >= a.min_common and not diffs else 1


if __name__ == '__main__':
    sys.exit(main())
