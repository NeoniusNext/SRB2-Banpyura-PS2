"""Per-map memory table from a chain_sweep.py result directory (level data, statics, free space at the end of the load, at the end of 35 frames).

usage: python tools/ps2/sweep_levels.py build/runs/sweep/<tag> [maps...]
Reads the "[zck] precache" line (end of the level load) and the ZCHAIN line (after the frames) of every level of every session log.
"""
import json
import re
import sys
from pathlib import Path

base = Path(sys.argv[1])
res = json.loads((base / 'results.json').read_text())
inv = {m['map']: m for m in json.loads((Path(__file__).resolve().parents[2] / 'build/ps2-sw-continuation-map-inventory.json').read_text())}
rows = {}
for sess in sorted(base.glob('*-s*')):
    text = (sess / 'boot.txt').read_text(errors='replace')
    maps = [k for k, v in res.items() if v.get('session') == sess.name]
    # the maps of a session in chain order = the order of the results of this session (results.json keeps insertion order)
    pre = [dict(re.findall(r'(\w+)=(\d+)', l)) for l in text.splitlines() if l.startswith('[zck] precache')]
    ch = [dict(re.findall(r'(\w+)=(\d+)', l)) for l in text.splitlines() if l.startswith('ZCHAIN n=')]
    for name, p in zip(maps, pre):
        rows[name] = p
want = sys.argv[2:] or list(rows)
print('| map | lines | things | level MB | static MB | free after load MB | free after 35 frames MB |')
print('|---|---:|---:|---:|---:|---:|---:|')
for n in want:
    p = rows.get(n)
    r = res.get(n, {})
    if not p:
        continue
    i = inv.get(n, {})
    print(f"| MAP{n} | {i.get('n_linedefs', '')} | {i.get('n_things', '')} | {int(p['level']) / 1048576:.2f} | {int(p['static']) / 1048576:.2f} | {int(p['free']) / 1048576:.2f} | "
          f"{(r.get('free', 0)) / 1048576:.2f} |")
