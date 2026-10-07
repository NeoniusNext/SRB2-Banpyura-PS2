"""Per-map OOM breakdown of a chain_sweep.py result directory (OPT10-S): the failing request and the zone tags that held the arena at that moment.

usage: python tools/ps2/oom_summary.py build/runs/sweep/<tag> [min_KB]
Reads the "OOM: request ..." block (PS2Mem_Report) of the session log of every failed map; tags below min_KB (default 150) are left out.
"""
import json
import re
import sys
from pathlib import Path

base = Path(sys.argv[1])
minb = int(sys.argv[2]) * 1024 if len(sys.argv) > 2 else 150 * 1024
res = json.loads((base / 'results.json').read_text())
ok = sorted(m for m, v in res.items() if v.get('ok'))
bad = [m for m, v in res.items() if not v.get('ok')]
print(f'{len(ok)} maps ok, {len(bad)} failed: {" ".join("MAP" + m for m in bad)}')
for m in bad:
    sess = res[m].get('session')
    if not sess:
        continue
    t = (base / sess / 'boot.txt').read_text(errors='replace').splitlines()
    for i, l in enumerate(t):
        if not l.startswith('OOM:'):
            continue
        out = [l[:120]]
        for x in t[i + 1:i + 40]:
            mm = re.match(r'ps2_mem: tag\s+(\d+) (\w+)\s+(\d+) blocks\s+(\d+) B', x)
            if mm and int(mm.group(4)) > minb:
                out.append(f'{mm.group(2)} {int(mm.group(4)) / 1048576:.2f} MB')
            if x.startswith('ps2_mem: arena'):
                f = re.search(r'free (\d+) B in (\d+) blocks, largest (\d+)', x)
                if f:
                    out.append(f'free {int(f.group(1)) / 1048576:.2f} MB, largest {int(f.group(3)) / 1048576:.2f} MB')
        print(f'MAP{m}: ' + ' | '.join(out))
        break
