"""Load many maps one after the other in as few emulator sessions as possible (OPT10-S): a -zchain session per run; when a map dies (OOM, I_Error, hang)
the failure is recorded for exactly that map and the sweep continues with the next map in a fresh session.

usage: python tools/ps2/chain_sweep.py --elf SRB2.ELF --tag NAME [--out build/runs/sweep] [--pak build/pak] [--kinds SP,MP-special,Match,CTF] [--maps 01,11,..]
                                       [--frames 35] [--timeout 900] [-- extra engine args, e.g. -renderer Hardware]
Map list: build/ps2-sw-continuation-map-inventory.json (tools/ps2/map_inventory.py --json). Per map: ZCHAIN line (heap check, used/peak/free/largest block,
evicted bytes, libc spare, EE cycles); failures: OOM request/tag or the last engine log line. Writes <out>/<tag>/results.json and results.md.
Every session loads the first remaining map with -warp and the others with -zchain (a `map MAPxx -force` level change after -zquit frames of each).
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INV = ROOT / 'build/ps2-sw-continuation-map-inventory.json'


def parse(text):
    steps = []
    for line in text.splitlines():
        if line.startswith('ZCHAIN n='):
            steps.append({k: v for k, v in (x.split('=', 1) for x in line.split()[1:] if '=' in x)})
    oom = None
    err = None
    for line in text.splitlines():
        m = re.match(r'OOM: request (\d+) B tag (\d+) \((\w+)\)', line)
        if m:
            oom = f'{m.group(1)} B {m.group(3)}'
        if 'I_Error' in line and not err:
            err = line.strip()[:200]
    heap = 'HEAP CHECK FAILED' in text
    return steps, oom, err, heap, 'ZQUIT DONE' in text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--out', default=str(ROOT / 'build/runs/sweep'))
    ap.add_argument('--pak', default='/home/user/SRB2-Banpyura-PS2/build/pak')
    ap.add_argument('--kinds', default='SP,MP-special,Match,CTF')
    ap.add_argument('--maps', default='')
    ap.add_argument('--frames', type=int, default=35)
    ap.add_argument('--timeout', type=int, default=1200)
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    inv = json.loads(INV.read_text())
    kinds = a.kinds.split(',')
    maps = [m for m in inv if m['kind'] in kinds]
    if a.maps:
        want = a.maps.split(',')
        maps = sorted([m for m in maps if m['map'] in want], key=lambda m: want.index(m['map']))
    else:
        maps.sort(key=lambda m: m['map'])
    names = [m['map'] for m in maps]
    outdir = Path(a.out) / a.tag
    outdir.mkdir(parents=True, exist_ok=True)
    results = {}
    session = 0
    remaining = list(names)
    t_all = time.time()
    while remaining:
        session += 1
        run = f'{a.tag}-s{session}'
        cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', run, '--elf', a.elf, '--out', str(outdir), '--pak', a.pak,
               '--map', 'MAP' + remaining[0], '--timeout', str(a.timeout), '--until', 'ZQUIT DONE', '--', '-zck', '-zquit', str(a.frames)]
        if len(remaining) > 1:
            cmd += ['-zchain', ','.join(remaining[1:])]
        cmd += a.extra
        t0 = time.time()
        for _try in range(20):
            p = subprocess.run(cmd, capture_output=True, text=True)
            rl = outdir / run / 'run.log'
            if not (rl.exists() and 'could not get the PCSX2 lock' in rl.read_text(errors='replace')):
                break
        boot = outdir / run / 'boot.txt'
        text = boot.read_text(errors='replace') if boot.exists() else ''
        steps, oom, err, heap, done = parse(text)
        n = len(steps)
        for i, s in enumerate(steps):
            results[remaining[i]] = {'ok': s.get('check') == 'ok', 'check': s.get('check'), 'peak': int(s['peak']), 'gpeak': int(s['gpeak']), 'free': int(s['free']),
                                     'largest': int(s['largest']), 'evicted': int(s['evictedbytes']), 'libcfree': int(s['libcfree']), 'libcpeak': int(s.get('libcpeak', 0)),
                                     'mcycles': int(s['mcycles']), 'session': run}
        if done and n == len(remaining):
            remaining = []
        else:
            bad = remaining[n] if n < len(remaining) else remaining[-1]
            last = [l for l in text.splitlines() if l.strip()][-1:] or ['(no log)']
            results[bad] = {'ok': False, 'oom': oom, 'error': err, 'heapcheck': heap, 'last': last[0][:160], 'session': run, 'rc': p.returncode}
            remaining = remaining[n + 1:]
        print(f"[{time.time() - t0:5.0f}s] {run}: {n} maps ok, " + ('done' if not remaining else f"FAILED {bad}: oom={oom} err={err}") + f", {len(remaining)} left", flush=True)
        (outdir / 'results.json').write_text(json.dumps(results, indent=1))
    md = ['| map | kind | ok | gpeak B | free B | largest B | evicted B | libc spare B | libc peak B | Mcycles | failure |', '|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|']
    kind = {m['map']: m['kind'] for m in maps}
    for n in names:
        r = results.get(n, {})
        if r.get('ok'):
            md.append(f"| MAP{n} | {kind[n]} | yes | {r['gpeak']} | {r['free']} | {r['largest']} | {r['evicted']} | {r['libcfree']} | {r['libcpeak']} | {r['mcycles']} | |")
        else:
            md.append(f"| MAP{n} | {kind[n]} | **NO** | | | | | | | | {r.get('oom') or r.get('error') or r.get('last')} |")
    (outdir / 'results.md').write_text('\n'.join(md) + '\n')
    bad = [n for n in names if not results.get(n, {}).get('ok')]
    print(f"sweep {a.tag}: {len(names) - len(bad)}/{len(names)} maps ok in {time.time() - t_all:.0f}s; failed: {bad if bad else 'none'}")
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
