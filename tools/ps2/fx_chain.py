"""OPT11-FX: a chain of levels in one PS2 run (map after map without a restart) against fresh PC loads of the same maps: finds state that one level leaves behind
for the next (the light tables of PS2-HW-123 were such a case).

usage: fx_chain.py NAME 1,7,2,13,10 [--tick 300] [--elf build/out/SRB2.ELF] [--extra '-hwdbg 0']
Needs the cached PC references build/ref/fx_m<N>_k<tick> (fx_sweep.py / fx_pair.py --ref m<N>_k<tick> make them). Prints MAD and the edge correlation per link.
"""
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
T = ROOT / 'tools/ps2'


def lum(a):
    return a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11


def edges(a):
    l = lum(a)
    return np.abs(np.diff(l, axis=1))[:-1, :] + np.abs(np.diff(l, axis=0))[:, :-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('maps')
    ap.add_argument('--tick', type=int, default=300)
    ap.add_argument('--elf', default=str(ROOT / 'build/out/SRB2.ELF'))
    ap.add_argument('--extra', default='')
    ap.add_argument('--timeout', type=float, default=1500)
    a = ap.parse_args()
    maps = [m for m in a.maps.split(',') if m]
    spec = []
    for i, m in enumerate(maps):
        kind = 'k' if i == 0 else 'K'
        nxt = f'=map~{maps[i + 1]}' if i + 1 < len(maps) else ''
        spec.append(f'{kind}{a.tick}{nxt}')
    cmd = [sys.executable, str(T / 'hf_run.py'), 'fx_' + a.name, '--elf', a.elf, '--timeout', str(a.timeout), '--cfg', 'chasecam "Off"', '--',
           '-skipintro', '-warp', maps[0], '-renderer', 'Hardware', '-zreserve', '1536', '-vidshot', ','.join(spec), '-hwfbh', '200', '-vidcmd', 'con_hudlines~0'] + a.extra.split()
    p = subprocess.run(cmd, capture_output=True, text=True)
    print((p.stdout + p.stderr).strip().splitlines()[-1] if (p.stdout + p.stderr).strip() else 'no output')
    shots = sorted((ROOT / 'build/runs' / ('fx_' + a.name)).glob('vidshot-*.ppm'), key=lambda x: int(x.stem.split('_')[-1]))
    for i, m in enumerate(maps):
        ref = ROOT / f'build/ref/fx_m{m}_k{a.tick}/shot-0.png'
        if i >= len(shots) or not ref.exists():
            print(f'link {i} map {m}: no picture or no PC reference ({ref.name})')
            continue
        hw = np.asarray(Image.open(shots[i]).convert('RGB')).astype(np.float32)
        pc = np.asarray(Image.open(ref).convert('RGB')).astype(np.float32)
        mad = float(np.abs(hw - pc).mean())
        gp, gh = edges(pc), edges(hw)
        corr = float(np.corrcoef(gp.ravel(), gh.ravel())[0, 1])
        print(f'link {i} map {m}: MAD {mad:.2f}  edge corr {corr:.3f}  mean PC {pc.reshape(-1, 3).mean(0).round(1)} HW {hw.reshape(-1, 3).mean(0).round(1)}')


if __name__ == '__main__':
    main()
