"""Every internal video mode in PCSX2: pictures of the title and of a level, and the per-phase EE cost (COP0 Count).

usage: python tools/ps2/video_modes_batch.py [--out build/agent-vid-modes] [--modes 0,1,..] [--outfmt ARG] [--elf FILE]
For each mode N: run_video.py twice (title: t35,t245 ; level 1: l70,l455) with -vidmode N -ps2prof. The engine must be the
--ps2ref build (tools/ps2/build.py --ps2ref) so that the profiler wrappers exist. Results: <out>/mNN/*.png, <out>/modes.json
(cycles per rendered frame by phase; window 0 = loading is dropped) and a table on stdout.
Cycle numbers are PCSX2 instruction cost without the EE caches (docs/VIDEO_MODES.md section 4): an estimate, not hardware.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PHASES = ['gtick', 'ptick', 'render', 'bsp', 'planes', 'masked', 'hud', 'gs', 'audio', 'sleep', 'other']


def parse_prof(boot):
    wins = []
    for line in boot.read_text(errors='replace').splitlines():
        if not line.startswith('PROF win='):
            continue
        kv = dict(p.split('=', 1) for p in line.split()[1:])
        w = {'win': int(kv['win']), 'frames': int(kv['frames']), 'tics': int(kv['tics']), 'total': int(kv['total'])}
        for ph in PHASES:
            w[ph] = int(kv[ph].split('/')[0])
        wins.append(w)
    return wins


def summarise(wins):
    use = [w for w in wins if w['win'] >= 1 and w['frames'] > 0]
    frames = sum(w['frames'] for w in use)
    if not frames:
        return None
    s = {'windows': len(use), 'frames': frames}
    for ph in PHASES + ['total']:
        s[ph] = sum(w[ph] for w in use) / frames
    s['work'] = sum(s[ph] for ph in PHASES if ph not in ('other', 'sleep'))
    s['draw'] = s['render'] + s['bsp'] + s['planes'] + s['masked']
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, default=ROOT / 'build/agent-vid-modes')
    ap.add_argument('--modes', default='0,1,2,3,4,5,6,7,8,9,10')
    ap.add_argument('--outfmt', default='')
    ap.add_argument('--elf', type=Path, default=ROOT / 'build/agent-vid-build/SRB2.ELF')
    ap.add_argument('--timeout', type=int, default=600)
    a = ap.parse_args()
    from PIL import Image
    result = {}
    for m in [int(x) for x in a.modes.split(',')]:
        res = {}
        for scen, args, tags in (('title', ['-skipintro', '-vidshot', 't35,t245'], ['t35', 't245']),
                                 ('level', ['-skipintro', '-warp', '1', '-vidshot', 'l70,l455'], ['l70', 'l455'])):
            run = a.out / f'm{m:02d}-{scen}'
            extra = ['-vidmode', str(m), '-ps2prof'] + (['-' + a.outfmt] if a.outfmt else []) + args
            cmd = [sys.executable, str(ROOT / 'tools/ps2/run_video.py'), '--run', str(run), '--elf', str(a.elf), '--timeout', str(a.timeout), '--'] + extra
            p = subprocess.run(cmd, capture_output=True, text=True)
            (run / 'run.log').write_text(p.stdout + p.stderr, encoding='utf-8')
            boot = run / 'boot.txt'
            wins = parse_prof(boot) if boot.exists() else []
            res[scen] = {'rc': p.returncode, 'summary': summarise(wins), 'windows': len(wins)}
            for f in run.glob('vidshot-*.ppm'):
                Image.open(f).save(f.with_suffix('.png'))
            s = res[scen]['summary']
            print(f'mode {m:2d} {scen:5s} rc={p.returncode} ' + (f"frames={s['frames']} draw={s['draw']:.0f} work={s['work']:.0f} total={s['total']:.0f} cyc/frame" if s else 'no PROF windows'), flush=True)
        result[m] = res
        (a.out / 'modes.json').write_text(json.dumps(result, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
