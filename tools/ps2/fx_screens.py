"""OPT11-FX (copy of hfscreens.py for this tree: PC engine build/pc-ref, zreserve 1536): parity of the non-level screens (title, menu, console, intermission, wipe): PC OpenGL against PS2-HW, same frame specs on both sides.

usage: hfscreens.py --tag scr [--elf build/outl/SRB2.ELF] [--only title,menu] [--emu AppRun]
Scenarios: title (t100,t220), menu (key enter at frame 150, f400), console (key ` at frame 20, f120), inter (k40=exitlevel, i4, i10: the intermission lasts a few seconds, the PS2 draws a few frames a second), wipe (a map change at k20, wipe frames w1,w3,w5).
Result: build/panels/<tag>_<scenario>_<n>.png and <tag>.json (MAD etc., the title and wipe frames depend on the timing of the animation: look at the pictures).
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
T = ROOT / 'tools/ps2'

# name: (shots, pc keys, ps2 keys, warp, extra tags of the shots in the file names)
SCEN = {
    'title': ('t100,t220', '', '', '', ['t100', 't220']),
    'menu': ('f400', '150:enter', '150:enter', '', ['f400']),
    'console': ('f120', '20:console', '20:console', '', ['f120']),
    'inter': ('k40=exitlevel,i4,i10', '', '', '1', ['k40_0', 'i4_', 'i10_']),
    'wipe': ('k20=map~4,w1,w3,w5', '', '', '1', ['k20_0', 'w1_', 'w3_', 'w5_']),
}


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', required=True)
    ap.add_argument('--elf', default=str(ROOT / 'build/outl/SRB2.ELF'))
    ap.add_argument('--only', default='')
    ap.add_argument('--emu', default='')
    ap.add_argument('--timeout', default='1200')
    a = ap.parse_args()
    res = {}
    names = [n for n in (a.only.split(',') if a.only else SCEN) if n]
    for n in names:
        shots, pkeys, hkeys, warp, tags = SCEN[n]
        pc = f'scr_{n}'
        cmd = [sys.executable, str(T / 'pcshot.py'), pc, '--shots', shots, '--cmd', 'con_hudlines~0', '--exe', str(ROOT / 'build/pc-ref/bin/lsdlsrb2_claude/lucid-mayer-1izlqe'), '--size', '320x200']
        if warp:
            cmd += ['--warp', warp]
        if pkeys:
            cmd += ['--keys', pkeys]
        print(run(cmd).strip().splitlines()[0], flush=True)
        name = f'{a.tag}_{n}'
        cmd = [sys.executable, str(T / 'hf_run.py'), name, '--elf', a.elf, '--timeout', a.timeout]
        if a.emu:
            cmd += ['--emu', a.emu]
        cmd += ['--', '-skipintro', '-renderer', 'Hardware', '-zreserve', '1536', '-hwfbh', '200', '-vidcmd', 'con_hudlines~0', '-vidshot', shots]
        if warp:
            cmd += ['-warp', warp]
        if hkeys:
            cmd += ['-vidkeys', hkeys]
        print(run(cmd).strip().splitlines()[0], flush=True)
        for i, tag in enumerate(tags):
            hw = sorted((ROOT / 'build/runs' / name).glob(f'vidshot-*-{tag}*.ppm'))
            pcs = ROOT / 'build/ref' / pc / f'shot-{i}.png'
            if not hw or not pcs.exists():
                res[f'{n}_{i}'] = {'error': 'missing'}
                continue
            out = run([sys.executable, str(T / 'hfpanel.py'), '--pc', str(pcs), '--hw', str(hw[0]), '--out', str(ROOT / 'build/panels' / f'{a.tag}_{n}_{i}.png'), '--label', f'{n} {tag}', '--json'])
            try:
                res[f'{n}_{i}'] = json.loads(out.strip().splitlines()[-1])
            except Exception:
                res[f'{n}_{i}'] = {'error': out.strip()[-120:]}
            print(n, i, res[f'{n}_{i}'], flush=True)
        (ROOT / 'build/panels' / f'{a.tag}.json').write_text(json.dumps(res, indent=1))


if __name__ == '__main__':
    main()
