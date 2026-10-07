"""Hottest source lines of one file in a sampler report (tools/ps2/sample_report.py --lines 8000 output), with the source text.
usage: python3 tools/ps2/sw_hotlines.py REPORT.txt src/file.c [N] [first_line last_line]   (run from the repository root)"""
import re
import sys
from collections import defaultdict

rep, f = sys.argv[1], sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
lo = int(sys.argv[4]) if len(sys.argv) > 4 else 0
hi = int(sys.argv[5]) if len(sys.argv) > 5 else 10**9
agg = defaultdict(float)
for line in open(rep):
    m = re.match(r'(\S+?):(\d+)(?: \(discriminator \d+\))?\s+(\d+)\s+([\d.]+)', line)
    if m and m.group(1) == f:
        agg[int(m.group(2))] += float(m.group(4))
src = open(f).read().split('\n')
print('total in range (kcyc/frame)', round(sum(v for k, v in agg.items() if lo <= k < hi), 1))
for v, k in sorted(((v, k) for k, v in agg.items() if lo <= k < hi), reverse=True)[:n]:
    print(k, round(v, 1), src[k - 1].strip()[:110] if k <= len(src) else '?')
