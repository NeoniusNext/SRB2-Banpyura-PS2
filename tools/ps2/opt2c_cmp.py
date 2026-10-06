"""Frame-exact comparison of two PS2 demo runs made with `-ps2ref-hashall` (src/ps2ref.c: allhash.csv = FNV-1a of every
rendered level frame) plus the sampled golden frames.

usage: python tools/ps2/opt2c_cmp.py BASE_REFOUT CAND_REFOUT [--golden DIR]
  BASE_REFOUT / CAND_REFOUT : the `refout` directories of two runs (tools/ps2/run_ps2_ref.py ... -- -ps2ref-hashall)
  --golden DIR              : golden/phase0-v2/run1/DEMO_00n; the sampled frame-*.idx of the candidate are compared with it
                              pixel by pixel (count and largest palette-index difference)
Prints: tics.csv equality (game state), number of frames whose hash differs from the base run, first differing frames,
and for the golden frames the number of differing pixels. Exit code 0 only if tics.csv is identical (frames may differ
within the stated tolerance: the caller judges the numbers).
"""
import argparse
import csv
import sys
from pathlib import Path


def load_hash(d):
    f = d / 'allhash.csv'
    if not f.exists():
        return None
    return {int(r['seq']): r['fnv1a32'] for r in csv.DictReader(f.open())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base', type=Path)
    ap.add_argument('cand', type=Path)
    ap.add_argument('--golden', type=Path)
    ap.add_argument('--pixels', action='store_true', help='compare every frame-*.idx present in both runs (-ps2ref-every 1 dumps)')
    a = ap.parse_args()
    rc = 0
    tb, tc = (a.base / 'tics.csv').read_bytes(), (a.cand / 'tics.csv').read_bytes()
    same_tics = tb == tc
    print('tics.csv:', 'identical' if same_tics else 'DIFFERENT')
    if not same_tics:
        rc = 1
    hb, hc = load_hash(a.base), load_hash(a.cand)
    if hb is None or hc is None:
        print('allhash.csv missing in', 'base' if hb is None else 'cand')
        rc = 1
    else:
        common = sorted(set(hb) & set(hc))
        diff = [s for s in common if hb[s] != hc[s]]
        print('frames hashed: base %d cand %d common %d; differing from base: %d' % (len(hb), len(hc), len(common), len(diff)))
        if diff:
            print('  first differing frames:', diff[:12])
    if a.pixels:
        nf = nd_frames = tot = worst = maxd = 0
        diffs = []
        for g in sorted(a.base.glob('frame-*.idx')):
            c = a.cand / g.name
            if not c.exists():
                continue
            gb, cb = g.read_bytes(), c.read_bytes()
            nf += 1
            idx = [i for i in range(len(gb)) if gb[i] != cb[i]]
            if idx:
                nd_frames += 1
                tot += len(idx)
                worst = max(worst, len(idx))
                maxd = max(maxd, max(abs(gb[i] - cb[i]) for i in idx))
                diffs.append((g.name, len(idx)))
        print('pixel compare: %d frames, %d differ, %d pixels in total, worst frame %d px, max index delta %d' % (nf, nd_frames, tot, worst, maxd))
        if diffs:
            print('  frames:', ', '.join('%s:%d' % d for d in diffs[:20]))
    if a.golden:
        tot = 0
        worst = 0
        for g in sorted(a.golden.glob('frame-*.idx')):
            c = a.cand / g.name
            if not c.exists():
                print('  missing', g.name)
                rc = 1
                continue
            gb, cb = g.read_bytes(), c.read_bytes()
            nd = sum(1 for i in range(len(gb)) if gb[i] != cb[i])
            if nd:
                md = max(abs(gb[i] - cb[i]) for i in range(len(gb)) if gb[i] != cb[i])
                print('  %s: %d pixels differ (max index delta %d)' % (g.name, nd, md))
                tot += nd
                worst = max(worst, nd)
        print('golden frames: total differing pixels %d, worst frame %d' % (tot, worst))
    return rc


if __name__ == '__main__':
    sys.exit(main())
