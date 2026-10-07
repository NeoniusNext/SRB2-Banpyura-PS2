"""Summarise the "[zmap]" runs of an OOM/PS2Mem_Map report as an arena picture (OPT10-S).

usage: python tools/ps2/zmap_view.py boot.txt [bucket_KiB=512]
One line per bucket of the arena: bytes of free (F), level (L), static (S), cache (C), patch/sprite (P), render work (W), other (?) blocks, as a bar.
Reads the runs the engine prints after an out-of-memory error (-zmap, PS2Mem_Map): "+offset TYPE bytes N blocks".
"""
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(errors='replace')
bucket = (int(sys.argv[2]) if len(sys.argv) > 2 else 512) << 10
runs = []
for m in re.finditer(r'^\[zmap\] \+([0-9a-f]+) (\S) +(\d+) B +(\d+) blocks', text, re.M):
    runs.append((int(m.group(1), 16), m.group(2), int(m.group(3))))
if not runs:
    sys.exit('no [zmap] lines')
base = runs[0][0]
end = max(o + s for o, t, s in runs)
nb = (end - base + bucket - 1) // bucket
acc = [dict() for _ in range(nb)]
for o, t, s in runs:
    pos = o - base
    left = s
    while left > 0:
        b = min(pos // bucket, nb - 1)
        take = min(left, (b + 1) * bucket - pos)
        acc[b][t] = acc[b].get(t, 0) + take
        pos += take
        left -= take
print('legend: F free, L level, S static, C cache, P patch/sprite, W render work, other letters as printed; each cell ~ 1/32 of the bucket')
for i, d in enumerate(acc):
    cells = ''
    tot = sum(d.values()) or 1
    for t, v in sorted(d.items(), key=lambda kv: -kv[1]):
        cells += t * max(1 if v else 0, round(32 * v / tot))
    print(f'{base + i * bucket:#010x} {cells[:40]:<40} ' + ' '.join(f'{t}={v >> 10}K' for t, v in sorted(d.items(), key=lambda kv: -kv[1])))
