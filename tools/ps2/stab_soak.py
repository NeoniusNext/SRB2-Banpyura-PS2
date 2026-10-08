"""OPT11-STAB: soak test: many levels and trips to the title in ONE session of the final ELF, one scripted pad, one game tic per displayed frame (-zsingle),
heap check after every level (-zck, ZCHAIN lines), the zone/libc/stack numbers at the end.

usage: stab_soak.py --elf SRB2.ELF --tag NAME [--renderer Software|Hardware] [--frames-per-level 1500] [--minutes 20] [--maps 01,02,...] [--timeout 14400]
The game time is frames / 35 s: --minutes 20 is 42 000 frames (levels x frames-per-level, the list is repeated until the total is reached).
Result: build/runs/<tag>-<renderer>/boot.txt (grep ZCHAIN / ps2_hwfb / OOM), summary printed: levels done, frames, libcfree start->end, errors.
"""
import argparse
import random
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAK = '/home/user/SRB2-Banpyura-PS2/build/pak'
MAPS = '01,02,03,04,05,06,07,08,09,10,11,12,x,13,14,15,16,22,23,25,26,27,30,31,x,32,33,40,41,42,50,51,60,70,71,F0,M0,N0'


def padscript(total, seed=7):
    r = random.Random(seed)
    out = ['1:1:ly=0']
    t = 1
    nextjump = 30
    while t < total:
        lx = r.choice((128, 128, 255, 0, 200, 60))
        out.append(f'{t}:1:lx={lx}')
        t2 = t + r.randint(25, 60)
        while nextjump < t2:
            out.append(f'{nextjump}:1:+cross')
            out.append(f'{nextjump + 6}:1:-cross')
            nextjump += r.randint(40, 70)
        t = t2
    return ','.join(sorted(out, key=lambda s: (int(s.split(':')[0]), s)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--renderer', default='Software')
    ap.add_argument('--frames-per-level', type=int, default=1500)
    ap.add_argument('--minutes', type=float, default=20)
    ap.add_argument('--maps', default=MAPS)
    ap.add_argument('--timeout', type=int, default=14400)
    ap.add_argument('--extra', default='')
    a = ap.parse_args()
    total = int(a.minutes * 60 * 35)
    maps = [m for m in a.maps.split(',') if m]
    seq = []
    frames = 0
    while frames < total:
        for m in maps:
            seq.append(m)
            frames += 150 if m == 'x' else a.frames_per_level
            if frames >= total:
                break
    run = f'{a.tag}-{a.renderer[:2].lower()}'
    d = ROOT / 'build/runs' / run
    d.mkdir(parents=True, exist_ok=True)
    (d / 'pad.txt').write_text(padscript(frames + 2000))
    first = seq[0]
    if first == 'x':
        first = '01'
    cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', run, '--elf', a.elf, '--pak', PAK, '--out', str(ROOT / 'build/runs'), '--map', 'MAP' + first,
           '--timeout', str(a.timeout), '--until', 'ZQUIT DONE', '--', '-zck', '-zsingle', '-zstack', '-zquit', str(a.frames_per_level), '-zchain', ','.join(seq[1:]),
           '-padscript', 'file:pad.txt']
    if a.renderer == 'Hardware':
        cmd += ['-renderer', 'Hardware']
    cmd += a.extra.split()
    print(f'soak {run}: {len(seq)} levels, {frames} frames = {frames / 35 / 60:.1f} game minutes', flush=True)
    subprocess.run(cmd)
    text = (d / 'boot.txt').read_text(errors='replace') if (d / 'boot.txt').exists() else ''
    zc = [l for l in text.splitlines() if l.startswith('ZCHAIN')]
    bad = [l.strip()[:160] for l in text.splitlines() if 'I_Error' in l or 'OOM:' in l or 'HEAP CHECK FAILED' in l or 'WATCHDOG' in l or 'NULLGUARD' in l]
    libc = [int(m.group(1)) for l in zc for m in [re.search(r'libcfree=(\d+)', l)] if m]
    print(f'{run}: levels reported {len(zc)} of {len(seq)}; done={"ZQUIT DONE" in text}; libcfree {libc[0] if libc else "?"} -> {libc[-1] if libc else "?"}; problems {bad[:3]}')
    fb = [l for l in text.splitlines() if l.startswith('ps2_hwfb:')]
    for l in fb[-3:]:
        print('  ', l[:200])
    return 0 if 'ZQUIT DONE' in text and not bad else 1


if __name__ == '__main__':
    sys.exit(main())
