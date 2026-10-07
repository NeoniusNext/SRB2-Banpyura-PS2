"""OPT11-STAB: video outputs (NTSC / PAL / 480p / the frame modes) x internal resolutions x both renderers: the level must start, draw and quit cleanly.

usage: stab_video.py --elf SRB2.ELF --tag NAME [--outputs ntsc,pal,480p,ntscframe,palframe,576p] [--modes 0,1,3,7,9,10] [--renderers Software,Hardware]
Per combination: `-skipintro -warp MAP01 -zquit 90 -vidshot l60 -<output> -vidmode N [-renderer Hardware]`; ok = ZQUIT DONE, no I_Error/OOM/WATCHDOG, a picture of the size of the mode
(the PPM header). The hardware renderer shows NTSC, PAL and 480p only: other outputs keep software there (the log line says so), that is a result as well.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAK = '/home/user/SRB2-Banpyura-PS2/build/pak'
SIZES = {0: (320, 200), 1: (320, 224), 2: (320, 240), 3: (320, 256), 4: (400, 300), 5: (512, 384), 6: (512, 448), 7: (640, 400), 8: (640, 448), 9: (640, 480), 10: (640, 512)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--outputs', default='ntsc,pal,480p')
    ap.add_argument('--modes', default='0,1,3,7,9,10')
    ap.add_argument('--renderers', default='Software,Hardware')
    ap.add_argument('--timeout', type=int, default=600)
    a = ap.parse_args()
    bad = 0
    for out in a.outputs.split(','):
        for rd in a.renderers.split(','):
            for m in [int(x) for x in a.modes.split(',')]:
                name = f'{a.tag}-{out}-{m}-{rd[:2].lower()}'
                extra = ['-skipintro', '-warp', 'MAP01', '-zquit', '90', '-zck', '-vidshot', 'l60', '-' + out, '-vidmode', str(m)]
                if rd == 'Hardware':
                    extra += ['-renderer', 'Hardware']
                cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', name, '--elf', a.elf, '--pak', PAK, '--out', str(ROOT / 'build/runs'),
                       '--timeout', str(a.timeout), '--until', 'ZQUIT DONE', '--'] + extra
                subprocess.run(cmd, capture_output=True, text=True)
                d = ROOT / 'build/runs' / name
                text = (d / 'boot.txt').read_text(errors='replace') if (d / 'boot.txt').exists() else ''
                errs = [l.strip()[:140] for l in text.splitlines() if 'I_Error' in l or 'OOM:' in l or 'WATCHDOG' in l or 'HEAP CHECK FAILED' in l]
                shots = sorted(list(d.glob('vidshot-*.ppm')) + list((d / '.srb2').glob('vidshot-*.ppm')))
                dim = None
                if shots:
                    with open(shots[0], 'rb') as f:
                        hdr = f.read(20).split()
                        dim = (int(hdr[1]), int(hdr[2])) if len(hdr) > 2 else None
                ok = 'ZQUIT DONE' in text and not errs and dim == SIZES[m]
                notes = [l.strip()[:120] for l in text.splitlines() if 'hardware renderer shows' in l.lower() or 'Output format' in l or l.startswith('Output ')]
                bad += not ok
                print(f'{name}: {"ok" if ok else "FAIL"} picture={dim} want={SIZES[m]} {errs[:1]} {notes[:1]}', flush=True)
    print('video grid:', 'all ok' if not bad else f'{bad} failed')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
