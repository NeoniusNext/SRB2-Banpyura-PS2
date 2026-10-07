"""OPT11-STAB: missing / truncated / corrupted data packs and a damaged config or save: the game must say what is wrong and stop (or go on without the part),
never hang or run into a crash with an unreadable message.

usage: stab_packs.py --elf SRB2.ELF --tag NAME [--only nopaks,trunc] [--timeout 240]
Variants are made in build/pakbad/<name>/ (hard links of the good packs, one pack replaced by a damaged copy), run with `-skipintro -warp MAP01 -zquit 120`
(a start that works ends with ZQUIT DONE). Result per variant: how the run ended (done / error text / hang = timeout without the end of the log).
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GOOD = ROOT / 'build/pakx'  # hard links of the cooked packs + FINEACON.DAT (python3 tools/ps2/net_env.py makes it)
BASE = ROOT / 'build/pakbad'
PACKS = ['SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK']


def link_all(d, skip=()):
    d.mkdir(parents=True, exist_ok=True)
    for f in PACKS + ['FINEACON.DAT']:
        if f in skip:
            continue
        dst = d / f
        if dst.exists():
            dst.unlink()
        try:
            os.link(GOOD / f, dst)
        except OSError:
            shutil.copy2(GOOD / f, dst)


def damaged_copy(d, name, how):
    dst = d / name
    if dst.exists():
        dst.unlink()
    src = GOOD / name
    if how == 'trunc':
        with open(src, 'rb') as a, open(dst, 'wb') as b:
            b.write(a.read(20 << 20))
    else:
        shutil.copy2(src, dst)
        size = dst.stat().st_size
        with open(dst, 'r+b') as f:
            if how == 'magic':
                f.seek(0)
                f.write(b'XXXX')
            elif how == 'header':  # the offsets of the layout: table offset and lump count
                f.seek(16)
                f.write(b'\xff' * 12)
            elif how == 'table':  # the middle of the lump table
                f.seek(int.from_bytes(open(src, 'rb').read(64)[20:24], 'little') + 4096)
                f.write(os.urandom(2048))
            elif how == 'body':  # the data of some lumps
                for k in range(1, 40):
                    f.seek(size * k // 41)
                    f.write(os.urandom(4096))
            elif how == 'zero':
                f.seek(size // 2)
                f.write(b'\0' * (1 << 20))


VARIANTS = {
    'nopaks': lambda d: link_all(d, skip=PACKS + ['FINEACON.DAT']),
    'nosrb2': lambda d: link_all(d, skip=['SRB2.PAK']),
    'nozones': lambda d: link_all(d, skip=['ZONES.PAK']),
    'nochars': lambda d: link_all(d, skip=['CHARS.PAK']),
    'nomusic': lambda d: link_all(d, skip=['MUSIC.PAK']),
    'nofine': lambda d: link_all(d, skip=['FINEACON.DAT']),
    'trunc-srb2': lambda d: (link_all(d, skip=['SRB2.PAK']), damaged_copy(d, 'SRB2.PAK', 'trunc')),
    'magic-srb2': lambda d: (link_all(d, skip=['SRB2.PAK']), damaged_copy(d, 'SRB2.PAK', 'magic')),
    'header-srb2': lambda d: (link_all(d, skip=['SRB2.PAK']), damaged_copy(d, 'SRB2.PAK', 'header')),
    'table-srb2': lambda d: (link_all(d, skip=['SRB2.PAK']), damaged_copy(d, 'SRB2.PAK', 'table')),
    'body-srb2': lambda d: (link_all(d, skip=['SRB2.PAK']), damaged_copy(d, 'SRB2.PAK', 'body')),
    'magic-zones': lambda d: (link_all(d, skip=['ZONES.PAK']), damaged_copy(d, 'ZONES.PAK', 'magic')),
    'body-zones': lambda d: (link_all(d, skip=['ZONES.PAK']), damaged_copy(d, 'ZONES.PAK', 'body')),
    'zero-zones': lambda d: (link_all(d, skip=['ZONES.PAK']), damaged_copy(d, 'ZONES.PAK', 'zero')),
    'trunc-chars': lambda d: (link_all(d, skip=['CHARS.PAK']), damaged_copy(d, 'CHARS.PAK', 'trunc')),
    'magic-music': lambda d: (link_all(d, skip=['MUSIC.PAK']), damaged_copy(d, 'MUSIC.PAK', 'magic')),
    'body-music': lambda d: (link_all(d, skip=['MUSIC.PAK']), damaged_copy(d, 'MUSIC.PAK', 'body')),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--only', default='')
    ap.add_argument('--timeout', type=int, default=240)
    ap.add_argument('--extra', default='', help='more engine arguments (e.g. "-renderer Hardware")')
    a = ap.parse_args()
    names = [n for n in (a.only.split(',') if a.only else VARIANTS) if n]
    for n in names:
        d = BASE / n
        VARIANTS[n](d)
        run = f'{a.tag}-{n}'
        cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', run, '--elf', a.elf, '--pak', str(d), '--out', str(ROOT / 'build/runs'),
               '--timeout', str(a.timeout), '--until', 'end of logstream', '--', '-skipintro', '-warp', 'MAP01', '-zquit', '120'] + a.extra.split()
        subprocess.run(cmd, capture_output=True, text=True)
        text = (ROOT / 'build/runs' / run / 'boot.txt').read_text(errors='replace') if (ROOT / 'build/runs' / run / 'boot.txt').exists() else ''
        done = 'ZQUIT DONE' in text
        ended = 'end of logstream' in text
        err = [l.strip()[:200] for l in text.splitlines() if 'I_Error' in l or 'ERROR' in l or 'corrupt' in l.lower()]
        verdict = 'works' if done else ('stopped with a message' if ended and err else ('ended without a message' if ended else 'HANG (no end of log)'))
        print(f'{n:14} {verdict:26} {err[:2]}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
