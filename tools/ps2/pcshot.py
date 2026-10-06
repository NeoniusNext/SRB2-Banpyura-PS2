"""OPT10-HF: reference pictures from the PC engine of this tree with the OpenGL renderer (Xvfb + llvmpipe) at the same frame as the PS2 -vidshot.

usage: pcshot.py NAME --shots 'k300' [--warp 1] [--sw] [--out build/ref] [--cfg 'chasecam "Off"'] [--keys SPEC] [--exe PATH] [-- more engine args]
  --shots: -ps2ref-shot spec (src/ps2ref.c): t35 l70 f200 k300 K300 w5 with an optional =command (k300=map~2); several maps in one run: 'k300=map~2,K300=map~4,K300'
  --sw   : software renderer (the PC reference of the software picture) instead of OpenGL
Result: <out>/NAME/shot-<n>.png (n = order of the shot, 0-based) and <out>/NAME/pc.txt (the PS2SHOT lines).
The engine is build/pc-hf (cmake -S . -B build/pc-hf -G Ninja -DSRB2_CONFIG_STATIC_STDLIB=OFF -DSRB2_CONFIG_USE_GME=OFF -DSRB2_CONFIG_PS2REF=ON).
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def find_exe():
    c = sorted(glob.glob(str(ROOT / 'build/pc-hf/bin/*')) + glob.glob(str(ROOT / 'build/pc-hf/bin/*/*')))
    c = [x for x in c if os.access(x, os.X_OK) and not x.endswith('.so')]
    if not c:
        sys.exit('no PC engine: build build/pc-hf first')
    return c[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--shots', required=True)
    ap.add_argument('--warp', default='')
    ap.add_argument('--sw', action='store_true')
    ap.add_argument('--out', default=str(ROOT / 'build/ref'))
    ap.add_argument('--cfg', default='')
    ap.add_argument('--keys', default='')
    ap.add_argument('--exe', default='')
    ap.add_argument('--timeout', type=float, default=240)
    ap.add_argument('--size', default='320x200')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    o = Path(a.out).resolve() / a.name
    shutil.rmtree(o, ignore_errors=True)
    (o / 'home/.srb2').mkdir(parents=True)
    cfg = ''.join(x.strip() + '\n' for x in a.cfg.split(';') if x.strip())
    (o / 'home/.srb2/reference.cfg').write_text('fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\nrollingdemos "Off"\n' + cfg)
    w, h = a.size.split('x')
    exe = a.exe or find_exe()
    args = ['xvfb-run', '-a', exe, '-home', str(o / 'home'), '-win', '-width', w, '-height', h, '-software' if a.sw else '-opengl', '-skipintro',
            '-nosound', '-config', 'reference.cfg', '-ps2ref-shot', a.shots]
    if a.warp:
        args += ['-warp', a.warp]
    if a.keys:
        args += ['-ps2ref-keys', a.keys]
    args += a.extra
    env = dict(os.environ, SRB2WADDIR='/opt/srb2-assets', SDL_AUDIODRIVER='dummy', LIBGL_ALWAYS_SOFTWARE='1')
    try:
        p = subprocess.run(args, cwd=str(o), env=env, capture_output=True, text=True, timeout=a.timeout)
        out = p.stdout + p.stderr
        rc = p.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
        rc = 'timeout'
    (o / 'pc.out').write_text(out)
    lines = [l for l in out.splitlines() if l.startswith('PS2SHOT')]
    (o / 'pc.txt').write_text('\n'.join(lines) + '\n')
    shots = sorted(glob.glob(str(o / 'home/.srb2/screenshots/*')))
    for i, s in enumerate(shots):
        ext = Path(s).suffix
        dst = o / f'shot-{i}{ext}'
        shutil.copy2(s, dst)
        if ext.lower() != '.png':
            subprocess.run(['convert', str(dst), str(o / f'shot-{i}.png')])
    print(f'{a.name}: rc={rc} shots={len(shots)} lines={len(lines)}')
    for l in lines:
        print(' ', l)
    return 0 if shots else 1


if __name__ == '__main__':
    sys.exit(main())
