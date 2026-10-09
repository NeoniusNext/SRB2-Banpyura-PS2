"""Tables for the OPT12-LOAD report from the logs of the runs: tick cost of hooks (timed demos), chain sweep of many maps (load time and level hash per map).

usage: lp_tables.py ticks RUNS_DIR [--prefix rel3_]      "timed N gametics in M realtics" of rel3_<base|cur>_d<n>_<none|hooks|big>
       lp_tables.py sweep BASE_SWEEP_DIR CUR_SWEEP_DIR [--top 12]   per map: P_LoadLevel cycles without the wipe, level structure hash (LHASH map.all), both builds
"""
import argparse
import json
import re
import sys
from pathlib import Path


def ticks(runs, prefix):
    out = {}
    for d in sorted(Path(runs).glob(prefix + '*')):
        m = re.match(re.escape(prefix) + r'(base|cur)_d(\d)_(none|hooks|big)$', d.name)
        if not m:
            continue
        t = (d / 'boot.txt').read_text(errors='replace') if (d / 'boot.txt').exists() else ''
        r = re.findall(r'timed (\d+) gametics in (\d+) realtics', t)
        if r:
            out[(m.group(1), int(m.group(2)), m.group(3))] = (int(r[-1][0]), int(r[-1][1]))
    print('| demo | add-on | base realtics | now realtics | base ms / tic | now ms / tic | ratio |')
    print('|---|---|---:|---:|---:|---:|---:|')
    for dm in (1, 2, 3, 4):
        for ad in ('none', 'hooks', 'big'):
            b, c = out.get(('base', dm, ad)), out.get(('cur', dm, ad))
            if not b and not c:
                continue
            f = lambda x: ('%d' % x[1]) if x else '-'
            g = lambda x: ('%.2f' % (x[1] * 1000.0 / 35 / x[0])) if x else '-'
            ratio = ('%.2fx' % (b[1] / c[1])) if b and c else '-'
            print('| D%d | %s | %s | %s | %s | %s | %s |' % (dm, {'none': 'none', 'hooks': 'HOOKS.pk3 (100 scripts, hooks)', 'big': 'BIG.pk3 (100 scripts, 400 pictures)'}[ad], f(b), f(c), g(b), g(c), ratio))


def levels(sweepdir):
    """per map: (cycles of P_LoadLevel, hash) in the order of the sessions"""
    res = {}
    for boot in sorted(Path(sweepdir).glob('*/boot.txt')):
        text = boot.read_text(errors='replace')
        hashes = re.findall(r'^LHASH map\.all ([0-9a-f]+)', text, re.M)
        totals = re.findall(r'^LP map(\d+) LV_TOTAL (\d+)', text, re.M)
        wipes = re.findall(r'^LP map(\d+) LV_PRE4 (\d+)', text, re.M)
        wipe = {m: int(c) for m, c in wipes}
        for i, (m, c) in enumerate(totals):
            if i < len(hashes):
                res[m] = (int(c), wipe.get(m, 0), hashes[i])
    return res


def sweep(base, cur, top):
    b, c = levels(base), levels(cur)
    maps = sorted(set(b) & set(c), key=int)
    same = sum(1 for m in maps if b[m][2] == c[m][2])
    print('maps in both sweeps: %d, level structure hash equal: %d, different: %d' % (len(maps), same, len(maps) - same))
    for m in maps:
        if b[m][2] != c[m][2]:
            print('  DIFFERENT map %s: %s vs %s' % (m, b[m][2], c[m][2]))
    tb = sum(b[m][0] for m in maps)
    tc = sum(c[m][0] for m in maps)
    wc = sum(c[m][1] for m in maps)
    print('sum of P_LoadLevel over these maps: base %.0f M, now %.0f M (%.2fx); the wipe of the candidate runs %.0f M of it, without the wipe (the baseline log has no wipe slot: its wipe is taken as the same): %.0f M -> %.0f M (%.2fx)'
          % (tb / 1e6, tc / 1e6, tb / tc, wc / 1e6, (tb - wc) / 1e6, (tc - wc) / 1e6, (tb - wc) / (tc - wc)))
    print()
    print('| map | base M | now M | base M without wipe | now M without wipe | ratio | hash |')
    print('|---|---:|---:|---:|---:|---:|---|')
    rows = sorted(maps, key=lambda m: -(c[m][0] - c[m][1]))[:top]
    for m in rows:
        wipe = c[m][1]
        print('| MAP%02d | %.1f | %.1f | %.1f | %.1f | %.2fx | %s |' % (int(m), b[m][0] / 1e6, c[m][0] / 1e6, (b[m][0] - wipe) / 1e6, (c[m][0] - wipe) / 1e6, (b[m][0] - wipe) / max(1, c[m][0] - wipe), 'equal' if b[m][2] == c[m][2] else 'DIFFERENT'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what')
    ap.add_argument('a')
    ap.add_argument('b', nargs='?')
    ap.add_argument('--prefix', default='rel3_')
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    if a.what == 'ticks':
        ticks(a.a, a.prefix)
    else:
        sweep(a.a, a.b, a.top)


if __name__ == '__main__':
    sys.exit(main())
