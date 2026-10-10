"""OPT14 GIF: what the engine logs of a run say about recording that nobody asked for.

usage: python3 tools/ps2/gif_scan.py LOG_OR_RUNDIR [...]      (a run directory: its boot.txt, or <dir>/cli/boot.txt of a net_session run)
Prints one line per log: the number of "Movie mode enabled/disabled" lines, "PS2: record key N" / "PS2: screenshot key N" lines (src/m_misc.c M_ScreenshotResponder: the key that
started or stopped the recording / took the screenshot; key 0 = a phantom event), the frames the recordings wrote, and whether the game was reached (NETSYNC / level). Exit 1 when a log has
a recording or a screenshot and --expect is not given; --expect N: exactly N "Movie mode enabled" lines are expected (the scenarios that start the recording on purpose).
"""
import argparse
import re
import sys
from pathlib import Path


def find(p):
    p = Path(p)
    if p.is_file():
        return p
    for c in (p / 'cli' / 'boot.txt', p / 'boot.txt', p / 'srv' / 'boot.txt'):
        if c.exists():
            return c
    raise SystemExit(f'no engine log in {p}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--expect', type=int, default=0)
    a = ap.parse_args()
    bad = 0
    for p in a.logs:
        f = find(p)
        text = f.read_text(errors='replace')
        en = len(re.findall(r'^Movie mode enabled', text, re.M))
        dis = len(re.findall(r'^Movie mode disabled', text, re.M))
        frames = [int(x) for x in re.findall(r'^Movie mode disabled; wrote (\d+) frames', text, re.M)]
        rk = re.findall(r'^PS2: record key (\d+)', text, re.M)
        sk = re.findall(r'^PS2: screenshot key (\d+)', text, re.M)
        net = len(re.findall(r'^NETSYNC gametic=', text, re.M))
        zero = sum(1 for k in rk + sk if k == '0')
        ok = en == a.expect and not (sk and not a.expect) and zero == 0
        print(f'{f}: enabled {en} disabled {dis} frames {sum(frames)} record-key lines {len(rk)} screenshot-key lines {len(sk)} key-0 events {zero} netsync lines {net} -> {"ok" if ok else "BAD"}')
        bad += 0 if ok else 1
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
