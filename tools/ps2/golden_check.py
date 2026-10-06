"""Compare PS2REF dumps (tics.csv, frames.csv, frame-*.idx) of a run against a reference directory.

usage: golden_check.py --run <refout dir> --ref <dir> [--demo DEMO_001]
  --ref golden/phase0-v2/run1/DEMO_00n   the PC reference (tools/ps2/make_golden_linux.sh): the PHYSICS reference. tics.csv must match;
                                         pixels differ by small palette deltas on Linux/gcc-vs-newlib (not investigated; same for HEAD).
  --ref golden/ps2-head/DEMO_00n         the PS2 software output of the tree at the start of OPT10 (self-golden): software-renderer
                                         changes must keep it BIT-EXACT ("0 differ").
Exit 0 only if tics.csv (compared over the common length, last row may be missing) and, with --pixels, every frame-*.idx match.
"""
import argparse
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', required=True)
    ap.add_argument('--ref', required=True)
    ap.add_argument('--pixels', action='store_true', help='frames must be byte-identical (self-golden); otherwise only report the pixel difference')
    a = ap.parse_args()
    run, ref = Path(a.run), Path(a.ref)
    bad = 0
    ta = (ref / 'tics.csv').read_text().splitlines()
    tb = (run / 'tics.csv').read_text().splitlines()
    n = min(len(ta), len(tb))
    first = next((i for i in range(n) if ta[i] != tb[i]), None)
    print(f'tics: ref {len(ta) - 1} run {len(tb) - 1} rows, ' + ('identical over %d rows' % (n - 1) if first is None else f'FIRST DIFFERENCE at row {first}: {ta[first]} | {tb[first]}'))
    if first is not None or abs(len(ta) - len(tb)) > 2:
        bad += 1
    frames = sorted(p.name for p in ref.glob('frame-*.idx'))
    differ = 0
    pix = 0
    for f in frames:
        x = (ref / f).read_bytes()
        yp = run / f
        if not yp.exists():
            differ += 1
            continue
        y = yp.read_bytes()
        if x != y:
            differ += 1
            pix += sum(1 for p, q in zip(x, y) if p != q)
    print(f'frames: {len(frames)} reference, {differ} differ' + (f', {pix} pixels in total ({pix / max(1, len(frames)) :.0f} per frame)' if differ else ''))
    if a.pixels and differ:
        bad += 1
    print('RESULT', 'FAIL' if bad else 'OK')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
