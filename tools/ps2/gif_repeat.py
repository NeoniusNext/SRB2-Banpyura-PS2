"""OPT14 GIF: run one net_session.py scenario N times in a row (the run directories get the suffix -rK), scan every run with gif_scan.py.

usage: python3 tools/ps2/gif_repeat.py SPEC.json N [--expect K] [--elf ELF]
A scenario that ends without its "until" lines (exit 2) counts as failed; the scan decides about the recording lines: no "Movie mode enabled", no "PS2: screenshot key" lines (--expect K: exactly K).
Stops at once on SIGTERM (kills the session's process group)."""
import argparse
import json
import os
import signal
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('spec')
    ap.add_argument('n', type=int)
    ap.add_argument('--expect', type=int, default=0)
    ap.add_argument('--elf')
    a = ap.parse_args()
    spec = json.loads(Path(a.spec).read_text())
    base = spec['name']
    cur = []

    def stop(*_):
        for p in cur:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except OSError:
                pass
        sys.exit(130)
    signal.signal(signal.SIGTERM, stop)
    rows = []
    for k in range(1, a.n + 1):
        spec['name'] = f'{base}-r{k}'
        if a.elf:
            for nd in spec['nodes']:
                if nd.get('kind') == 'ps2':
                    nd['elf'] = a.elf
        sp = Path(a.spec).with_name(f'{base}-r{k}.json')
        sp.write_text(json.dumps(spec, indent=1))
        p = subprocess.Popen([sys.executable, str(ROOT / 'tools/ps2/net_session.py'), str(sp), '--retries', '2'], cwd=ROOT, start_new_session=True,
                             stdout=open(sp.with_suffix('.log'), 'w'), stderr=subprocess.STDOUT)
        cur[:] = [p]
        rc = p.wait()
        run = ROOT / spec['out'] / spec['name']
        sc = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/gif_scan.py'), '--expect', str(a.expect), str(run)], capture_output=True, text=True)
        line = f'run {k}: session rc {rc}; ' + sc.stdout.strip().replace(str(ROOT) + '/', '')
        print(line, flush=True)
        rows.append((rc, sc.returncode))
    bad = sum(1 for rc, s in rows if rc != 0 or s != 0)
    print(f'{a.n - bad}/{a.n} runs clean')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
