"""Function-level profile table from an engine log made by a build.py --ps2ref --fprof ELF run with -ps2prof.

usage: python tools/ps2/fprof_report.py --elf SRB2.ELF --log boot.txt [--skip 1] [--top 40] [--json out.json]
Lines "FP <addr> <calls> <exclusive cycles>" (windows of 105 rendered frames, src/ps2/ps2_prof.c) are summed over the windows
after the first --skip ones (window 0 is the level load) and divided by the number of frames. The hooks cost cycles per call:
read the table as a ranking, not as absolute cost. Names come from `nm` of the ELF.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

import os
IS_WIN = os.name == 'nt'
NM = 'D:/ps2dev/ee/bin/mips64r5900el-ps2-elf-nm.exe' if IS_WIN else os.environ.get('PS2DEV', '/opt/ps2dev-x/ps2dev') + '/ee/bin/mips64r5900el-ps2-elf-nm'


def symbols(elf):
    out = subprocess.run([NM, '-n', '-S', '--defined-only', str(elf)], capture_output=True, text=True,
                         env={'PATH': 'D:/ps2dev/ee/bin;C:/Windows/System32'} if IS_WIN else None).stdout
    syms = []
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 4 and p[2] in 'tTwW':
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    return syms


def lookup(syms, addr):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best[2] if best else '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--skip', type=int, default=1)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--json', default='')
    a = ap.parse_args()
    syms = symbols(a.elf)
    win = -1
    tot = {}
    frames = 0
    total = 0
    for line in Path(a.log).read_text(errors='replace').splitlines():
        m = re.match(r'PROF win=(\d+) frames=(\d+)', line)
        if m:
            win = int(m.group(1))
            if win >= a.skip:
                frames += int(m.group(2))
            continue
        if win < a.skip:
            continue
        if line.startswith('FPTOTAL'):
            total += int(line.split()[1])
        elif line.startswith('FP '):
            _, ad, calls, cyc = line.split()
            e = tot.setdefault(int(ad, 16), [0, 0])
            e[0] += int(calls)
            e[1] += int(cyc)
    if not frames:
        raise SystemExit('no PROF/FP windows after skip')
    rows = sorted(((lookup(syms, ad), c, cy) for ad, (c, cy) in tot.items()), key=lambda r: -r[2])
    print(f'frames {frames}, instrumented cycles/frame {total / frames:.0f}')
    print(f'{"function":38s} {"calls/frame":>11s} {"cycles/frame":>13s} {"%":>6s} {"cyc/call":>9s}')
    for name, c, cy in rows[:a.top]:
        print(f'{name:38s} {c / frames:11.1f} {cy / frames:13.0f} {100 * cy / total:6.2f} {cy / max(c, 1):9.1f}')
    if a.json:
        Path(a.json).write_text(json.dumps({'frames': frames, 'total': total, 'rows': rows[:200]}, indent=1))


if __name__ == '__main__':
    main()
