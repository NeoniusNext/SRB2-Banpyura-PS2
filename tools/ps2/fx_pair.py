"""OPT11-FX: one scene, two renderers: PC OpenGL (llvmpipe, reference) and PS2-HW (PCSX2) at the same frame, panel PC | HW | diff + metrics.

usage: fx_pair.py NAME --map 7 [--tick 300] [--cmd 'devmode~1;teleport~-x~100~-y~200~-z~300~-ang~90'] [--cfg 'chasecam "Off"'] [--elf build/out/SRB2.ELF]
                  [--zoom 3] [--crop x,y,w,h] [--pcargs '...'] [--hwargs '...'] [--norun] [--refonly] [--nopc]
  --cmd   console commands run on the third frame on BOTH sides ('~' = space, ';' = next command): the same camera on both
  --shot  full -vidshot / -ps2ref-shot spec instead of k<tick> (e.g. 'k300', 'k100=cmd~x,k200')
Outputs: build/fx/NAME.png (the panel; the PC picture is cached in build/ref/fx_NAME), metrics line (MAD, blurred MAD, share > 48) on stdout.
The PC engine is the one of this tree (build/pc-ref, PS2REF=ON). Every picture is 320x200 (-hwfbh 200: one GS pixel per engine pixel).
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
T = ROOT / 'tools/ps2'
PCEXE = Path(os.environ.get('SRB2_PCEXE', str(ROOT / 'build/pc-ref/bin/lsdlsrb2_claude/lucid-mayer-1izlqe')))  # OPT11-MODEL: SRB2_PCEXE = own PC build


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    return p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--map', required=True)
    ap.add_argument('--tick', type=int, default=300)
    ap.add_argument('--lt', type=int, default=-1, help='-hwlt: the level time of the water phase (default = tick + 1: the PC picture of the k tick is taken one frame later)')
    ap.add_argument('--shot', default='')
    ap.add_argument('--cmd', default='')
    ap.add_argument('--pre', default='', help='camera command run after the first shot (<= 60 chars, ~ = space), e.g. teleport~-x~0~-y~0~-z~100~-ang~90~-aim~-10')
    ap.add_argument('--cfg', default='chasecam "Off"')
    ap.add_argument('--elf', default=str(ROOT / 'build/out/SRB2.ELF'))
    ap.add_argument('--zoom', type=int, default=3)
    ap.add_argument('--crop', default='')
    ap.add_argument('--pcargs', default='')
    ap.add_argument('--addon', default='', help='a PWAD/pk3 loaded with -file on both sides (tools/ps2/make_fxflat.py)')
    ap.add_argument('--tree', default='', help='a directory copied into the home of the PC engine and next to the PS2 ELF (models.dat, models/*.md3: tools/ps2/make_fxmodel.py)')
    ap.add_argument('--hwargs', default='')
    ap.add_argument('--zreserve', default='1536')
    ap.add_argument('--timeout', type=float, default=900)
    ap.add_argument('--norun', action='store_true')
    ap.add_argument('--refonly', action='store_true')
    ap.add_argument('--nopc', action='store_true', help='no PC reference (HW picture only)')
    ap.add_argument('--out', default=str(ROOT / 'build/fx'))
    ap.add_argument('--emu', default='')
    ap.add_argument('--pcsw', action='store_true', help='PC reference = PC software renderer')
    ap.add_argument('--all', action='store_true', help='a panel and metrics for every shot of the spec (shot i of the PC run against shot i of the PS2 run)')
    ap.add_argument('--ref', default='', help='name of the cached PC reference (default: the run name): scenes of different builds share it')
    a = ap.parse_args()
    spec = a.shot or f'k{a.tick}'
    if a.pre:  # console commands after an early shot (the camera): devmode on frame 3, then '<pre>' right after the k20 shot, then the real shot
        spec = f'k20={a.pre};devmode~0,{spec}'
        a.cmd = (a.cmd + ';' if a.cmd else '') + 'con_hudlines~0;devmode~1'
    Path(a.out).mkdir(parents=True, exist_ok=True)
    refname = f'fx_{a.ref or a.name}'
    refdir = ROOT / 'build/ref' / refname
    def lastshot():
        c = sorted(refdir.glob('shot-*.png'), key=lambda x: int(x.stem.split('-')[1]))
        return c[-1] if c else refdir / 'shot-0.png'
    ref = lastshot()
    if not a.norun and not a.nopc and not (refdir / 'shot-0.png').exists():
        cmd = [sys.executable, str(T / 'pcshot.py'), refname, '--shots', spec, '--warp', a.map, '--cfg', a.cfg, '--exe', str(PCEXE), '--size', '320x200']
        if a.cmd:
            cmd += ['--cmd', a.cmd]
        if a.pcsw:
            cmd += ['--sw']
        if a.tree:
            cmd += ['--tree', str(Path(a.tree).resolve())]
        pcx = a.pcargs.split() + [y for f in a.addon.split(',') if f for y in ('-file', str(Path(f).resolve()))]  # OPT11-MODEL: --addon a.lua,b.lua
        if pcx:
            cmd += ['--'] + pcx
        rc, out = run(cmd)
        print(out.strip().splitlines()[0] if out.strip() else 'pcshot: no output', flush=True)
    if a.refonly:
        return 0
    runname = f'fx_{a.name}'
    if not a.norun:
        cmd = [sys.executable, str(T / 'hf_run.py'), runname, '--elf', a.elf, '--timeout', str(a.timeout), '--cfg', a.cfg]
        if a.addon:
            cmd += ['--files', ','.join(str(Path(f).resolve()) for f in a.addon.split(',') if f)]
        if a.tree:
            cmd += ['--tree', str(Path(a.tree).resolve())]
        if a.emu:
            cmd += ['--emu', a.emu]
        cmd += ['--', '-skipintro', '-warp', a.map, '-renderer', 'Hardware', '-zreserve', a.zreserve, '-vidshot', spec, '-hwfbh', '200', '-hwlt', str(a.lt if a.lt >= 0 else a.tick + 1)]
        if a.cmd:
            cmd += ['-vidcmd', a.cmd]
        cmd += a.hwargs.split()
        rc, out = run(cmd)
        print(out.strip(), flush=True)
    ref = lastshot()
    hw = sorted((ROOT / 'build/runs' / runname).glob('vidshot-*.ppm'), key=lambda x: int(x.stem.split('_')[-1]))
    hw = hw[::-1]
    if not hw:
        print('no HW picture')
        return 1
    if a.all:
        hws = sorted((ROOT / 'build/runs' / runname).glob('vidshot-*.ppm'), key=lambda x: int(x.stem.split('_')[-1]))
        pcs = sorted(refdir.glob('shot-*.png'), key=lambda x: int(x.stem.split('-')[1]))
        for i, (h, c) in enumerate(zip(hws, pcs)):
            pn = Path(a.out) / f'{a.name}_{i}.png'
            rc, out = run([sys.executable, str(T / 'hfpanel.py'), '--pc', str(c), '--hw', str(h), '--out', str(pn), '--label', f'MAP{a.map} shot {i} ({h.stem.split("-")[-1]})', '--json', '--scale', str(a.zoom)])
            print(i, h.name, out.strip().splitlines()[-1] if out.strip() else 'hfpanel: no output')
        return 0
    panel = Path(a.out) / f'{a.name}.png'
    if a.nopc or not ref.exists():
        from PIL import Image
        im = Image.open(hw[0]).convert('RGB')
        im = im.resize((im.width * a.zoom, im.height * a.zoom), Image.NEAREST)
        im.save(panel)
        print(f'panel {panel} (HW only)')
        return 0
    cmd = [sys.executable, str(T / 'hfpanel.py'), '--pc', str(ref), '--hw', str(hw[0]), '--out', str(panel), '--label', f'MAP{a.map} {spec}', '--json', '--scale', str(a.zoom)]
    rc, out = run(cmd)
    print(out.strip().splitlines()[-1] if out.strip() else 'hfpanel: no output')
    return 0


if __name__ == '__main__':
    sys.exit(main())
