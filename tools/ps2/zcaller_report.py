"""Summarise the "[zcaller]" lines of an engine log (-zcaller [bytes], release build) by calling function.

usage: python tools/ps2/zcaller_report.py --elf SRB2.ELF --log boot.txt [--tags 50,51,1] [--top 40] [--after-marker TEXT]
Every allocation / free of at least the threshold prints "[zcaller] <kind> <size> tag <t> caller <addr>"; the caller address is
the return address in the function that called Z_Malloc / Z_Free, resolved here with mips64r5900el-ps2-elf-nm. The table counts
events per (kind, tag, function) and the bytes they moved: where blocks that leave holes come from (a free of a long-lived
tag between other long-lived blocks is a hole in the level region).
"""
import argparse
import bisect
import collections
import re
import subprocess
from pathlib import Path

import os
NM = 'D:/ps2dev/ee/bin/mips64r5900el-ps2-elf-nm.exe' if os.name == 'nt' else os.path.join(os.environ.get('PS2DEV', '/opt/ps2dev-x/ps2dev'), 'ee/bin/mips64r5900el-ps2-elf-nm')


def symbols(elf):
    out = subprocess.run([NM, '-n', '--defined-only', str(elf)], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in 'tTwWiI':
            addrs.append(int(f[0], 16))
            names.append(f[2])
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--tags', default='')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--after-marker', default='')
    a = ap.parse_args()
    addrs, names = symbols(a.elf)
    want = set(int(t) for t in a.tags.split(',') if t)
    rx = re.compile(r'\[zcaller\] (\w+) (\d+) tag (\d+) caller (?:0x)?([0-9a-fA-F]+)')
    agg = collections.defaultdict(lambda: [0, 0])
    text = Path(a.log).read_text(errors='replace')
    if a.after_marker:
        i = text.find(a.after_marker)
        text = text[i:] if i >= 0 else text
    for m in rx.finditer(text):
        kind, size, tag, addr = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4), 16)
        if want and tag not in want:
            continue
        k = bisect.bisect_right(addrs, addr) - 1
        fn = names[k] if k >= 0 else '?'
        e = agg[(kind, tag, fn)]
        e[0] += 1
        e[1] += size
    rows = sorted(agg.items(), key=lambda kv: -kv[1][1])[:a.top]
    print(f'{"kind":9} {"tag":>3} {"count":>6} {"bytes":>10}  function')
    for (kind, tag, fn), (n, b) in rows:
        print(f'{kind:9} {tag:3} {n:6} {b:10}  {fn}')


if __name__ == '__main__':
    main()
