"""Run several network scenarios one after another and compare the NETSYNC lines of the 'srv' and 'cli' nodes (OPT10-X).

usage: python3 tools/ps2/net_batch.py [--specs build/opt10-x/specs] [--out build/opt10-x/run] [--retries 2] [--min-players 1] NAME [NAME ...]
For every NAME: net_session.py <specs>/NAME.json (own process, the lock is taken per session), then netsync_compare.py on the engine logs of the nodes
"srv" and "cli" (boot.txt for PS2 nodes, out.txt for PC nodes) when both exist and have NETSYNC lines. Prints one line per scenario and appends it to
<out>/batch-results.jsonl: {"name", "rc", "seconds", "common", "differences", "range"}.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
import netsync_compare  # noqa: E402


def logof(rundir, nid):
    for f in ('boot.txt', 'out.txt'):
        p = rundir / nid / f
        if p.exists():
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--specs', default='build/opt10-x/specs')
    ap.add_argument('--out', default='build/opt10-x/run')
    ap.add_argument('--retries', type=int, default=2)
    ap.add_argument('--min-players', type=int, default=1)
    ap.add_argument('names', nargs='+')
    a = ap.parse_args()
    res = []
    for name in a.names:
        t0 = time.time()
        spec = ROOT / a.specs / f'{name}.json'
        cmd = [sys.executable, str(ROOT / 'tools/ps2/net_session.py'), str(spec), '--retries', str(a.retries)]
        with open(ROOT / 'build/logs' / f'{name}.txt', 'w') as lf:
            rc = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT).returncode
        rundir = ROOT / a.out / name
        row = {'name': name, 'rc': rc, 'seconds': round(time.time() - t0)}
        s, c = logof(rundir, 'srv'), logof(rundir, 'cli')
        if s and c:
            A, B = netsync_compare.parse(str(s)), netsync_compare.parse(str(c))
            common = sorted(set(A) & set(B))
            diffs = [g for g in common if A[g][-1] != B[g][-1] and int(A[g][-1][1]) >= a.min_players]
            cmp_players = [g for g in common if int(A[g][-1][1]) >= a.min_players]
            row.update({'common': len(cmp_players), 'differences': len(diffs), 'range': [cmp_players[0], cmp_players[-1]] if cmp_players else None,
                        'first_diff': diffs[:3]})
        res.append(row)
        print(json.dumps(row), flush=True)
        with open(ROOT / a.out / 'batch-results.jsonl', 'a') as f:
            f.write(json.dumps(row) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
