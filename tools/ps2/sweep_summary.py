"""Summarise a chain_sweep.py result directory.

usage: python tools/ps2/sweep_summary.py build/runs/sweep/<tag>
Prints: maps ok/total, failed maps with the reason, smallest free space after 35 frames, libc peak, slowest levels.
"""
import json
import sys
from pathlib import Path

r = json.loads((Path(sys.argv[1]) / 'results.json').read_text())
ok = {k: v for k, v in r.items() if v.get('ok')}
bad = {k: v for k, v in r.items() if not v.get('ok')}
print(f'{len(ok)}/{len(r)} maps ok')
for k, v in bad.items():
    print('FAILED MAP' + k, v.get('oom') or v.get('error') or v.get('last'))
if ok:
    print('smallest free B after 35 frames:', sorted((v['free'], 'MAP' + k) for k, v in ok.items())[:8])
    print('smallest largest-free-block B   :', sorted((v['largest'], 'MAP' + k) for k, v in ok.items())[:5])
    print('mean free B:', int(sum(v['free'] for v in ok.values()) / len(ok)), ' max gpeak B:', max(v['gpeak'] for v in ok.values()))
    print('libc peak B (max):', sorted(((v['libcpeak'], 'MAP' + k) for k, v in ok.items()), reverse=True)[:3])
    print('slowest level loads (Mcycles incl. 35 frames):', sorted(((v['mcycles'], 'MAP' + k) for k, v in ok.items()), reverse=True)[:5])
