#!/usr/bin/env python3
"""OPT13 RFRONT: summarise HWSPIKE lines (i_video.c patch, -hwspike): per window p50/p90/p99/max of the frame (EE cycles) and the 3 worst frames with the stage that makes them.
The first frame of a window (f=0) carries the cost of printing the previous window's HWPROF lines to the host console: it is skipped.
usage: rf_spikes.py boot.txt [more]
"""
import re
import sys

def kv(line):
    return {m.group(1): int(m.group(2)) for m in re.finditer(r'(\w+)=(\d+)', line)}

def main():
    for fn in sys.argv[1:]:
        wins = []
        cur = None
        for l in open(fn, errors='replace'):
            if l.startswith('HWSPIKE win='):
                cur = {'w': kv(l), 'f': []}
                wins.append(cur)
            elif l.startswith('HWSPIKE  f=') and cur is not None:
                cur['f'].append(kv(l))
        print(fn)
        tot = 0
        for w in wins:
            if w['w']['win'] == 0:
                continue
            p = w['w']
            sp = [f for f in w['f'] if f['f'] != 0]
            line = f"  win {p['win']}: p50 {p['p50']/1e6:5.2f} p90 {p['p90']/1e6:5.2f} p99 {p['p99']/1e6:5.2f} max {p['max']/1e6:5.2f}"
            for f in sorted(sp, key=lambda x: -x['wall'])[:2]:
                # the biggest of the stage laps explains the frame: drv tex (texture conversion/upload), nodes, clear/setup (skybox view), batch, bsp
                parts = {'tex': f['tex'], 'nodes': f['nodes'], 'setup(sky view)': f['setup'], 'batch': f['batch'], 'bsp': f['bsp'], 'spr': f['spr'], 'wait': f['wait'] + f['flipwait']}
                big = max(parts, key=lambda k: parts[k])
                line += f" | f{f['f']} {f['wall']/1e6:5.2f}M [{big} {parts[big]/1e6:.2f}M upl={f['uploads']} ev={f['evict']}]"
            print(line)
main()
