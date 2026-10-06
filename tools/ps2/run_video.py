"""Run the PS2 build in PCSX2 with video options and pick up the pictures (-vidshot) and the per-phase profile (-ps2prof).

usage: python tools/ps2/run_video.py --run build/agent-vid-shots [--elf build/agent-vid-build/SRB2.ELF] [--timeout 300]
                                     [--until "VIDSHOT COMPLETE"] [--cfg "fpscap \\"35\\"\\n..."] -- <engine arguments>
examples:
  ... -- -skipintro -vidmode 9 -vidshot t35            title screen of 640x480, picture of its 35th frame
  ... -- -skipintro -warp 1 -vidmode 7 -vidshot l70    level 1 at 640x400, 70th level frame
  ... -- -skipintro -720p -vidmode 0 -vidshot t35      output format 720p
The work directory gets the ELF, the pk3 hard links and the packs (like tools/ps2/run_ps2_ref.py); host: is that directory,
so the pictures land there as vidshot-<W>x<H>-<tag>.ppm and the engine log is boot.txt. PCSX2 runs under the shared lock.
Exit code 0 only when the wanted text appeared in boot.txt.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def prepare(run, elf, cfg):
    run.mkdir(parents=True, exist_ok=True)
    for name in ['srb2', 'zones', 'characters', 'music']:
        dst = run / f'{name}.pk3'
        if not dst.exists():
            try:
                os.link(ROOT / 'srb2-assets' / f'{name}.pk3', dst)
            except OSError:
                shutil.copy2(ROOT / 'srb2-assets' / f'{name}.pk3', dst)
    for pak in (ROOT / 'build/pak').glob('*.PAK') if (ROOT / 'build/pak').exists() else []:
        shutil.copy2(pak, run / pak.name)
    (run / '.srb2').mkdir(exist_ok=True)
    (run / '.srb2/reference.cfg').write_text(cfg.replace('\\n', '\n'))
    shutil.copy2(elf, run / 'SRB2.ELF')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', required=True, type=Path)
    ap.add_argument('--elf', type=Path, default=ROOT / 'build/agent-vid-build/SRB2.ELF')
    ap.add_argument('--timeout', type=float, default=300)
    ap.add_argument('--until', default='VIDSHOT COMPLETE')
    ap.add_argument('--cfg', default='fpscap "35"\\nfullscreen "Off"\\nshowfps "No"\\nshowping "Off"\\nrollingdemos "Off"\\n')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    run = a.run.resolve()
    prepare(run, a.elf, a.cfg)
    for old in run.glob('vidshot-*.ppm'):
        old.unlink()
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt'] + a.extra
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(run / 'SRB2.ELF'), '--log', str(run / 'pcsx2.log'),
           '--args=' + ' '.join(args), '--timeout', str(a.timeout), '--until-file', str(run / 'boot.txt'), '--until', a.until]
    print(' '.join(cmd))
    r = subprocess.run(cmd)
    boot = run / 'boot.txt'
    text = boot.read_text(errors='replace') if boot.exists() else ''
    ok = a.until in text
    print('wrapper exit', r.returncode, '| found', repr(a.until), ':', ok)
    if not ok:
        print('\n'.join(text.splitlines()[-15:]))
    for f in sorted(run.glob('vidshot-*.ppm')):
        print('picture', f.name, f.stat().st_size)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
