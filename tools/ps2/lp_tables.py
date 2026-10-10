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
    """EE cycles per frame of a timed demo: the ZSTAT line of -zquit N (cycles over N level frames); the add-on is already loaded when frame 1 starts"""
    out = {}
    for d in sorted(Path(runs).glob(prefix + '*')):
        m = re.match(re.escape(prefix) + r'(base|cur)_d(\d)_(none|hooks|big)$', d.name)
        if not m:
            continue
        t = (d / 'boot.txt').read_text(errors='replace') if (d / 'boot.txt').exists() else ''
        r = re.findall(r'^ZSTAT .*?levelframes=(\d+) .*?cycles=(\d+)', t, re.M)
        if r and int(r[-1][0]):
            out[(m.group(1), int(m.group(2)), m.group(3))] = (int(r[-1][0]), int(r[-1][1]))
    print('| demo | add-on | base k cycles / frame | now k cycles / frame | base - no add-on | now - no add-on | ratio |')
    print('|---|---|---:|---:|---:|---:|---:|')
    for dm in (1, 2, 3, 4):
        zero = {w: out.get((w, dm, 'none')) for w in ('base', 'cur')}
        for ad in ('none', 'hooks', 'big'):
            b, c = out.get(('base', dm, ad)), out.get(('cur', dm, ad))
            if not b and not c:
                continue
            per = lambda x: x[1] / x[0] if x else None
            f = lambda x: ('%.0f' % (x / 1e3)) if x else '-'
            extra = lambda x, z: ('%+.0f' % ((per(x) - per(z)) / 1e3)) if x and z else '-'
            ratio = ('%.2fx' % (per(b) / per(c))) if b and c else '-'
            print('| D%d | %s | %s | %s | %s | %s | %s |' % (dm, {'none': 'none', 'hooks': 'HOOKS.pk3', 'big': 'BIG.pk3'}[ad], f(per(b)), f(per(c)), extra(b, zero['base']) if ad != 'none' else '', extra(c, zero['cur']) if ad != 'none' else '', ratio))


def levels(sweepdir, names):
    """per map name: (cycles of P_LoadLevel, cycles of the wipe, hash). The sessions are read in order and the n-th P_LoadLevel report is the n-th name of the chain
    (a map that dies before its report shifts the rest: the counts are checked)"""
    seq = []
    for boot in sorted(Path(sweepdir).glob('*/boot.txt'), key=lambda p: int(re.search(r'-s(\d+)$', p.parent.name).group(1))):
        text = boot.read_text(errors='replace')
        hashes = re.findall(r'^LHASH map\.all ([0-9a-f]+)', text, re.M)
        totals = re.findall(r'^LP map\d+ LV_TOTAL (\d+)', text, re.M)
        wipes = {i: int(c) for i, c in enumerate(re.findall(r'^LP map\d+ LV_PRE4 (\d+)', text, re.M))}
        for i, c in enumerate(totals):
            if i < len(hashes):
                seq.append((int(c), wipes.get(i, 0), hashes[i]))
    return {n: seq[i] for i, n in enumerate(names) if i < len(seq)}, len(seq)


def sweep(base, cur, top, names):
    names = [n for n in names.split(',') if n]
    b, nb = levels(base, names)
    c, nc = levels(cur, names)
    maps = [n for n in names if n in b and n in c]
    print('maps: %d asked, %d reports in the baseline sweep, %d in the candidate sweep' % (len(names), nb, nc))
    same = sum(1 for m in maps if b[m][2] == c[m][2])
    print('maps in both sweeps: %d, level structure hash equal: %d, different: %d' % (len(maps), same, len(maps) - same))
    for m in maps:
        if b[m][2] != c[m][2]:
            print('  DIFFERENT map %s: %s vs %s' % (m, b[m][2], c[m][2]))
    if not maps:
        return
    tb = sum(b[m][0] for m in maps)
    tc = sum(c[m][0] for m in maps)
    wc = sum(c[m][1] for m in maps)
    print('sum of P_LoadLevel over these maps: base %.0f M, now %.0f M (%.2fx); the wipe of the candidate runs %.0f M of it (the baseline log has no wipe slot: its wipe is taken as the same), without the wipe: %.0f M -> %.0f M (%.2fx)'
          % (tb / 1e6, tc / 1e6, tb / tc, wc / 1e6, (tb - wc) / 1e6, (tc - wc) / 1e6, (tb - wc) / (tc - wc)))
    print()
    print('| map | base M | now M | base M without wipe | now M without wipe | ratio | level hash |')
    print('|---|---:|---:|---:|---:|---:|---|')
    for m in sorted(maps, key=lambda m: -(c[m][0] - c[m][1]))[:top]:
        wipe = c[m][1]
        print('| MAP%s | %.1f | %.1f | %.1f | %.1f | %.2fx | %s |' % (m, b[m][0] / 1e6, c[m][0] / 1e6, (b[m][0] - wipe) / 1e6, (c[m][0] - wipe) / 1e6, (b[m][0] - wipe) / max(1, c[m][0] - wipe), 'equal' if b[m][2] == c[m][2] else 'DIFFERENT'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what')
    ap.add_argument('a')
    ap.add_argument('b', nargs='?')
    ap.add_argument('--prefix', default='rel3_')
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--names', default='11,M3,MH,23,MG,ME,05,08,41,01,03,63,MB')
    a = ap.parse_args()
    if a.what == 'ticks':
        ticks(a.a, a.prefix)
    else:
        sweep(a.a, a.b, a.top, a.names)


if __name__ == '__main__':
    sys.exit(main())
