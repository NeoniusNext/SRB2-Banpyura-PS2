"""Summarise the NETLAT lines (src/netcode/netlat.c, engine option -netlat) of a net_session run.

usage: python3 tools/ps2/net_lat.py RUNDIR [--from-gametic N] [--json]
RUNDIR = build/opt12-net/run/<scenario> (nodes srv/cli with boot.txt or out.txt). Windows (2 s of game tics each) before the gametic given by --from-gametic (default 140: the join and
the level load are over) are skipped. Per quantity: weighted mean of the window averages (weights = sample counts), the mean of the window 95th percentiles and the largest maximum, all in ms.
 rtt      server: a game tic is made -> the client's packet that confirms it arrives (round trip as the server sees it)
 cmd-gap  server: time between two packets of one client (the client sends once per tic: 28.6 ms)
 frame    both: time between two passes of the main loop
 poll     both: time between two looks at the socket (the longest time a datagram can wait)
 tics-gap client: time between two packets of tics from the server
 run-wait client: a tic has arrived -> it is run
 ping     the client's ping as the server reports it (the figure on the screen)
"""
import json
import re
import sys
from pathlib import Path

LINE = re.compile(r'NETLAT (?P<what>[a-z-]+) (?P<name>\S*) ?n=(?P<n>\d+) min=(?P<min>\d+) avg=(?P<avg>\d+) p95=(?P<p95>\d+) max=(?P<max>\d+) us')
HEAD = re.compile(r'NETLAT --- t=(?P<t>\d+) gametic=(?P<g>\d+) maketic=\d+ neededtic=\d+ ping=(?P<ping>\d+) ms sent=(?P<sent>\d+) \((?P<bytes>\d+) B\)(?: buf=(?P<buf>\d+) starves=(?P<st>\d+))?')


LOSS = re.compile(r'NETLAT loss cmd-missed=(\d+) \(total (\d+)\) tic-holes=(\d+) \(total (\d+)\)')


def losses(path):
    """cmds the server had to repeat and holes in the client's tic stream: the totals of the last line"""
    last = None
    try:
        for line in Path(path).read_text(errors='replace').splitlines():
            m = LOSS.search(line)
            if m:
                last = (int(m[2]), int(m[4]))
    except OSError:
        pass
    return last


def parse(path, from_gametic):
    acc = {}
    pings = []
    gametic = 0
    skip = True
    try:
        text = Path(path).read_text(errors='replace')
    except OSError:
        return {}, []
    for line in text.splitlines():
        m = HEAD.search(line)
        if m:
            gametic = int(m['g'])
            skip = gametic < from_gametic
            if not skip:
                pings.append(int(m['ping']))
            continue
        if skip:
            continue
        m = LINE.search(line)
        if not m:
            continue
        key = m['what'] + (' ' + m['name'] if m['name'] else '')
        a = acc.setdefault(key, {'n': 0, 'sum': 0, 'p95': [], 'max': 0, 'min': 10 ** 12})
        n, avg = int(m['n']), int(m['avg'])
        a['n'] += n
        a['sum'] += n * avg
        a['p95'].append(int(m['p95']))
        a['max'] = max(a['max'], int(m['max']))
        a['min'] = min(a['min'], int(m['min']))
    out = {}
    for k, a in acc.items():
        if a['n']:
            out[k] = {'n': a['n'], 'min_ms': round(a['min'] / 1000, 1), 'avg_ms': round(a['sum'] / a['n'] / 1000, 1),
                      'p95_ms': round(sum(a['p95']) / len(a['p95']) / 1000, 1), 'max_ms': round(a['max'] / 1000, 1)}
    return out, pings


def main():
    run = Path(sys.argv[1])
    from_g = 140
    if '--from-gametic' in sys.argv:
        from_g = int(sys.argv[sys.argv.index('--from-gametic') + 1])
    res = {}
    for node in ('srv', 'cli'):
        for f in ('boot.txt', 'out.txt'):
            p = run / node / f
            if p.exists():
                stats, pings = parse(p, from_g)
                if stats or pings:
                    res[node] = {'losses': losses(p), 'stats': stats, 'ping_ms': {'avg': round(sum(pings) / len(pings), 1), 'max': max(pings), 'n': len(pings)} if pings else None}
                break
    if '--json' in sys.argv:
        print(json.dumps(res, indent=1))
        return
    for node, r in res.items():
        if r.get('losses'):
            print(f'[{node}]  cmds repeated by the server: {r["losses"][0]}, holes in the tic stream of the client: {r["losses"][1]}')
        print(f'[{node}]' + (f'  ping shown: avg {r["ping_ms"]["avg"]} max {r["ping_ms"]["max"]} ms ({r["ping_ms"]["n"]} windows)' if r['ping_ms'] else ''))
        for k, s in sorted(r['stats'].items()):
            print(f'  {k:22s} n={s["n"]:5d}  min {s["min_ms"]:7.1f}  avg {s["avg_ms"]:7.1f}  p95 {s["p95_ms"]:7.1f}  max {s["max_ms"]:8.1f} ms')


if __name__ == '__main__':
    main()
