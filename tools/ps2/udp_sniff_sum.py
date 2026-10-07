"""Summary of a udp_sniff.py log (OPT10-X): python3 tools/ps2/udp_sniff_sum.py LOG [bucket seconds]
Packets per time bucket and direction (every datagram is seen twice on lo: halved), with the length range, to see when a flow stops."""
import collections
import re
import sys

log = sys.argv[1]
bucket = float(sys.argv[2]) if len(sys.argv) > 2 else 5
rows = collections.defaultdict(lambda: collections.defaultdict(list))
for line in open(log, errors='replace'):
    m = re.match(r'\s*([\d.]+)\s+(\S+)\s+(\S+) -> (\S+) len=(\d+)', line)
    if not m:
        continue
    t = float(m.group(1))
    rows[int(t // bucket)][(m.group(3), m.group(4))].append(int(m.group(5)))
for b in sorted(rows):
    parts = []
    for k, v in sorted(rows[b].items()):
        parts.append(f'{k[0].split(":")[1]}->{k[1].split(":")[1]}: {len(v) // 2} pk len {min(v)}..{max(v)}')
    print(f'{b * bucket:7.0f}s  ' + ' | '.join(parts))
