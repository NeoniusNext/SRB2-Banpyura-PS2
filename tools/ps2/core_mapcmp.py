"""OPT11-CORE: compare two core_mapsweep.sh outputs (the tic state hash of every map): usage core_mapcmp.py DIR_A DIR_B
Prints one line per map that differs (first differing tic) or is missing in one of them, and a summary; exit 1 when anything differs."""
import sys
from pathlib import Path

a, b = Path(sys.argv[1]), Path(sys.argv[2])
bad = 0
same = 0
maps = sorted({p.name for p in a.iterdir() if p.is_dir()} | {p.name for p in b.iterdir() if p.is_dir()})
for m in maps:
    fa, fb = a / m / 'tics.csv', b / m / 'tics.csv'
    if not fa.exists() or not fb.exists() or not (a / m / 'complete.txt').exists() or not (b / m / 'complete.txt').exists():
        print('MAP%s: missing (%s %s)' % (m, 'A' if fa.exists() else '-', 'B' if fb.exists() else '-'))
        bad += 1
        continue
    la, lb = fa.read_text().splitlines(), fb.read_text().splitlines()
    n = min(len(la), len(lb))
    first = next((i for i in range(n) if la[i] != lb[i]), None)
    if first is None and len(la) == len(lb):
        same += 1
    else:
        bad += 1
        print('MAP%s: DIFFERENT at row %s of %d/%d: %s | %s' % (m, first, len(la) - 1, len(lb) - 1, la[first] if first is not None else '-', lb[first] if first is not None else '-'))
print('maps identical %d, different or missing %d' % (same, bad))
sys.exit(1 if bad else 0)
