"""PS2-LOAD-11: the storage order list of the packs (cook.py --order), made from the engine's own read log.

usage: lump_order.py --out tools/ps2/lump_order.txt LOG [LOG ...]
Each LOG is an engine log (boot.txt) of a run with `-loadprof -lpreads` (opt_run.py ... -- -skipintro -zquitall 120 -loadprof -lpreads, and the same with
--map MAP01 -zquit 10): the lines "RD <wad> <lump> <size> <offset> <full name>" are the lump reads in the order the engine made them. Reads of the first
bytes of a lump (size <= 16 at offset 0: they are answered by the head table of a v2 pack) are not file reads and are not counted. The list holds every lump
that was read from the file, in the order of its first read, so cook.py can store them as one contiguous run and the start-up reads them sequentially.
The numbering of the lumps in the packs does not change.
"""
import argparse
import re
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--skip-wad', type=int, nargs='*', default=[3], help='wad numbers whose lumps are not listed (default 3: MUSIC.PAK, read on demand)')
    a = ap.parse_args()
    seen = {}
    total = 0
    for log in a.logs:
        for line in log.read_text(errors='replace').splitlines():
            m = re.match(r'RD (\d+) (\d+) (\d+) (\d+) (.*)$', line)
            if not m:
                continue
            wad, lump, size, off, name = int(m[1]), int(m[2]), int(m[3]), int(m[4]), m[5].strip()
            total += 1
            if wad in a.skip_wad or not name:
                continue
            if 0 < size <= 16 and off == 0:
                continue
            seen.setdefault((wad, name), len(seen))
    names = [n for (_w, n), _i in sorted(seen.items(), key=lambda kv: kv[1])]
    a.out.write_text('# PS2-LOAD-11: lumps read from the file at start-up, first use order (tools/ps2/lump_order.py from -lpreads logs)\n' + '\n'.join(names) + '\n')
    print(f'{total} read log lines, {len(names)} distinct lumps -> {a.out}')


if __name__ == '__main__':
    sys.exit(main())
