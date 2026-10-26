"""OPT14 GIF: several net_session.py scenarios (each N times) one after the other under ONE hold of the machine-wide net lock, every run scanned with gif_scan.py.

usage: python3 tools/ps2/gif_batch.py [--elf ELF] [--expect K] SPEC.json:N [SPEC.json:N ...]
The run directories get the suffix -rK (the scenario name of the spec stays the prefix; "run/<name>/..." paths in node arguments, e.g. the log of the mock master server, follow the rename).
Why not tools/ps2/gif_repeat.py for everything: every net_session.py waits for the lock separately and other agents' sessions take 15 minutes each, a wait of an hour per run (2026-10-10).
This tool takes the lock (tools/ps2/run_pcsx2.py NETLOCK) once, keeps it fresh and runs the sessions in-process with the lock calls of net_session.py switched off; the lock file is removed at the end."""
import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
import run_pcsx2  # noqa: E402
import net_session as NS  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('specs', nargs='+')
    ap.add_argument('--elf')
    ap.add_argument('--expect', type=int, default=0)
    ap.add_argument('--lock-wait', type=int, default=10800)
    ap.add_argument('--dry', action='store_true', help='only print what would run (no lock, no session)')
    a = ap.parse_args()
    if a.dry:
        for item in a.specs:
            path, _, n = item.rpartition(':')
            spec0 = json.loads(Path(path).read_text())
            base = spec0['name']
            for k in range(1, int(n) + 1):
                text = json.dumps(spec0).replace(f'/run/{base}/', f'/run/{base}-r{k}/')
                spec = NS.sub(json.loads(text))
                print(f'{base}-r{k}:', [x for nd in spec['nodes'] for x in nd.get('args', []) if 'mock.jsonl' in str(x)], [nd.get('elf') for nd in spec['nodes'] if nd.get('kind') == 'ps2'])
        return
    real = run_pcsx2.NETLOCK
    real_acquire = run_pcsx2.acquire
    real_acquire(a.lock_wait, real)
    print(f'[{time.strftime("%H:%M:%S")}] net lock taken', flush=True)
    stop = threading.Event()
    dummy = Path(tempfile.mkdtemp()) / 'dummy.lock'

    def keep_fresh():
        while not stop.wait(30):
            try:
                os.utime(real)
            except OSError:
                pass
    threading.Thread(target=keep_fresh, daemon=True).start()

    def bye(*_):
        stop.set()
        try:
            real.unlink()
        except OSError:
            pass
        os._exit(130)
    signal.signal(signal.SIGTERM, bye)
    summary = []
    try:
        for item in a.specs:
            path, _, n = item.rpartition(':')
            n = int(n)
            spec0 = json.loads(Path(path).read_text())
            base = spec0['name']
            for k in range(1, n + 1):
                text = json.dumps(spec0).replace(f'/run/{base}/', f'/run/{base}-r{k}/')
                spec = NS.sub(json.loads(text))
                spec['name'] = f'{base}-r{k}'
                if a.elf:
                    for nd in spec['nodes']:
                        if nd.get('kind') == 'ps2':
                            nd['elf'] = a.elf
                run_pcsx2.NETLOCK = dummy
                run_pcsx2.acquire = lambda *x, **y: None
                try:
                    code = 3
                    for attempt in range(3):
                        code = NS.run_session(spec)
                        if code not in (3, 7):
                            break
                        time.sleep(3)
                finally:
                    run_pcsx2.NETLOCK = real
                    run_pcsx2.acquire = real_acquire
                run = ROOT / spec.get('out', 'build/opt14-gif/run') / spec['name']
                sc = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/gif_scan.py'), '--expect', str(a.expect), str(run)], capture_output=True, text=True)
                line = f'{spec["name"]}: session rc {code}; ' + sc.stdout.strip().replace(str(ROOT) + '/', '')
                print('RESULT ' + line, flush=True)
                summary.append(line)
    finally:
        stop.set()
        try:
            real.unlink()
        except OSError:
            pass
    print('\n'.join(summary))


if __name__ == '__main__':
    main()
