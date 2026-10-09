#!/usr/bin/env python3
"""OPT13 RFRONT: HWREGEN lines (a texture the engine had to compose again: > 2 M cycles each, printed by the driver) of a run: how many, the sum, the part inside the first 100 frames (level start), the largest.
usage: rf_regen.py boot.txt [...]"""
import re, sys

for fn in sys.argv[1:]:
    ev = []
    for l in open(fn, errors='replace'):
        m = re.match(r'HWREGEN f=(\d+) (\S+) (\S+) kind=(\d) made again in (\d+) cycles \(zone free (\d+) K\) uploads before=(\d+) dropped by=(\d+)', l)
        if m:
            ev.append((int(m.group(1)), m.group(2), m.group(3), int(m.group(5)), int(m.group(6)), int(m.group(8))))
    first = [e for e in ev if e[0] <= 100]
    later = [e for e in ev if e[0] > 100]
    print(f'{fn}: {len(ev)} events, {sum(e[3] for e in ev)/1e6:.0f} M cycles; first 100 frames: {len(first)} events {sum(e[3] for e in first)/1e6:.0f} M; later: {len(later)} events {sum(e[3] for e in later)/1e6:.0f} M')
    for e in sorted(ev, key=lambda e: -e[3])[:4]:
        print(f'   f={e[0]} {e[1]} {e[2]} {e[3]/1e6:.1f} M (zone free {e[4]} K, dropped by {e[5]})')
