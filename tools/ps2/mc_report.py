"""Report of the memcpy/memset caller statistics (build.py --memprof): who copies how much per frame.

usage: python tools/ps2/mc_report.py --elf SRB2.ELF --log boot.txt [--frames 945] [--top 25]
Sums the MCR lines (window >= 1) per return address and kind (0 memcpy, 1 memset, 2 memmove), resolves the caller with addr2line.
"""
import argparse
import re
import subprocess
from collections import defaultdict
from pathlib import Path

import os
IS_WIN = os.name == 'nt'
BIN = Path('D:/ps2dev/ee/bin' if IS_WIN else os.environ.get('PS2DEV', '/opt/ps2dev-x/ps2dev') + '/ee/bin')
ENVP = {'PATH': str(BIN) + (';C:/Windows/System32' if IS_WIN else ':/usr/bin:/bin')}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    acc = defaultdict(lambda: [0, 0])
    tot = defaultdict(lambda: [0, 0])
    frames = 0
    win = -1
    for line in Path(a.log).read_text(errors='replace').splitlines():
        line = re.sub(r'^(?:\[[^\]\r\n]*\]\s*)?', '', line)
        if line.startswith('PROF win='):
            kv = dict(x.split('=', 1) for x in line.split()[1:])
            win = int(kv['win'])
            if win >= 1:
                frames += int(kv['frames'])
        elif line.startswith('MC win='):
            win = int(line.split()[1].split('=')[1])
            if win >= 1:
                for key, name in (('cpy', 'memcpy'), ('set', 'memset'), ('mov', 'memmove')):
                    m = re.search(key + r'=(\d+)/(\d+)', line)
                    tot[name][0] += int(m.group(1))
                    tot[name][1] += int(m.group(2))
        elif line.startswith('MCR ') and win >= 1:
            _, ra, kind, calls, nbytes = line.split()
            k = (int(ra), int(kind))
            acc[k][0] += int(calls)
            acc[k][1] += int(nbytes)
    print('frames', frames)
    for name, (c, b) in tot.items():
        print('%-8s calls/frame %8.1f  bytes/frame %10.0f' % (name, c / frames, b / frames))
    rows = sorted(acc.items(), key=lambda x: -x[1][1])[:a.top]
    addrs = ''.join('%x\n' % (ra - 8) for (ra, _), _ in rows)  # the call instruction is before the delay slot
    out = subprocess.run([str(BIN / ('mips64r5900el-ps2-elf-addr2line' + ('.exe' if IS_WIN else ''))), '-e', a.elf, '-f'], input=addrs, capture_output=True, text=True, env=ENVP).stdout.splitlines()
    print('\n%-6s %9s %10s %9s  %s' % ('kind', 'calls/frm', 'bytes/frm', 'avg', 'caller'))
    i = 0
    for (ra, kind), (c, b) in rows:
        fn = out[i] if i < len(out) else '?'
        loc = out[i + 1] if i + 1 < len(out) else '?'
        i += 2
        loc = re.sub(r'^.*/(src/)', r'\1', loc.replace(chr(92), '/'))
        print('%-6s %9.1f %10.0f %9.0f  %s %s' % (('cpy', 'set', 'mov')[kind], c / frames, b / frames, b / max(c, 1), fn, loc))


if __name__ == '__main__':
    main()
