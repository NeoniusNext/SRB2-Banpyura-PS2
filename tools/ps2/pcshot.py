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


def grid_share(path):
    """share of the pixels on the RGB565 crush grid (palette rendering with gr_palettedepth 16 produces 100%)"""
    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        return 1.0
    r = {int(v / 31 * 255) for v in range(32)}
    g = {int(v / 63 * 255) for v in range(64)}
    a = np.asarray(Image.open(path).convert('RGB'))
    return float((np.isin(a[..., 0], list(r)) & np.isin(a[..., 1], list(g)) & np.isin(a[..., 2], list(r))).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--shots', required=True)
    ap.add_argument('--warp', default='')
    ap.add_argument('--sw', action='store_true')
    ap.add_argument('--demo', default='', help='attract demo DEMO_001..4 played with -timedemo')
    ap.add_argument('--out', default=str(ROOT / 'build/ref'))
    ap.add_argument('--cfg', default='')
    ap.add_argument('--keys', default='')
    ap.add_argument('--cmd', default='', help='console commands on the third frame: con_hudlines~0;gr_paletterendering~Off')
    ap.add_argument('--exe', default='')
    ap.add_argument('--timeout', type=float, default=240)
    ap.add_argument('--size', default='320x200')
    ap.add_argument('--tree', default='', help='a directory copied into the home of the engine (models.dat, models/*.md3 ...)')
    ap.add_argument('--nogrid', action='store_true', help='do not check that the picture was made with palette rendering (the default look); a run without it is repeated')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    o = Path(a.out).resolve() / a.name
    shutil.rmtree(o, ignore_errors=True)
    (o / 'home/.srb2').mkdir(parents=True)
    if a.tree:
        shutil.copytree(a.tree, o / 'home/.srb2', dirs_exist_ok=True)
    cfg = ''.join(x.strip() + '\n' for x in a.cfg.split(';') if x.strip())
    (o / 'home/.srb2/reference.cfg').write_text('fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\nrollingdemos "Off"\n' + cfg)
    w, h = a.size.split('x')
    exe = a.exe or find_exe()
    args = ['xvfb-run', '-a', exe, '-home', str(o / 'home'), '-win', '-width', w, '-height', h, '-software' if a.sw else '-opengl', '-skipintro',
            '-nosound', '-config', 'reference.cfg', '-ps2ref-shot', a.shots]
    if a.demo:
        shutil.copy2(ROOT / 'golden/phase0-v2' / (a.demo + '.lmp'), o / 'home/.srb2' / (a.demo + '.lmp'))
        args += ['-timedemo', a.demo + '.lmp']
    if a.warp:
        args += ['-warp', a.warp]
    if a.cmd:
        args += ['-ps2ref-cmd', a.cmd]
    if a.keys:
        args += ['-ps2ref-keys', a.keys]
    args += a.extra
    env = dict(os.environ, SRB2WADDIR='/opt/srb2-assets', SDL_AUDIODRIVER='dummy', LIBGL_ALWAYS_SOFTWARE='1')
    for attempt in range(3):
        try:
            p = subprocess.run(args, cwd=str(o), env=env, capture_output=True, text=True, timeout=a.timeout)
            out = p.stdout + p.stderr
            rc = p.returncode
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
            rc = 'timeout'
        shots = sorted(glob.glob(str(o / 'home/.srb2/screenshots/*')))
        pal_off = 'gr_paletterendering' in (a.cfg + a.cmd + a.shots)
        if a.nogrid or a.sw or pal_off or not shots or min(grid_share(x) for x in shots[-1:]) > 0.9 or attempt == 2:
            break
        # OpenGL (llvmpipe) sometimes starts without the shaders under load: the picture is then not the palette rendered default; make it again
        shutil.rmtree(o / 'home/.srb2/screenshots', ignore_errors=True)
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
