#!/usr/bin/env python3
"""OPT11-MODEL: every model of models.dat on the screen, in batches (PC OpenGL against PS2 HW; fixture tools/ps2/mdlscene.lua).

usage: python3 tools/ps2/md_all.py ELF [--batch 25] [--only N,N] [--dat /opt/srb2-assets-models/models.dat] [--hwargs "-hwmodel 4"]
Each batch is a Lua file (build/mdall/set_N.lua, MDL_SET = the 4 letter sprite names that have a model) loaded before mdlscene.lua by tools/ps2/fx_pair.py
with gr_models On on both sides; the last line of every run (MAD of the whole picture) is printed, the panel is build/fx/mdall_N.png,
the PS2 counters (HWPROF40/41: models drawn, triangles, damaged, refused) come from build/runs/fx_mdall_N/boot.txt.
SRB2_PCWADDIR (a private directory of the PC engine with models/ and models.dat) must be set, see docs/GATES/g1/opt11-MODEL.md.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def sprites(dat):
    names = []
    for line in Path(dat).read_text().splitlines():
        t = line.split()
        if len(t) >= 2 and not line.startswith('#') and len(t[0]) == 4 and t[0] not in names:
            names.append(t[0])
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--batch', type=int, default=25)
    ap.add_argument('--only', default='')
    ap.add_argument('--dat', default='/opt/srb2-assets-models/models.dat')
    ap.add_argument('--tree', default='', help='loose files instead of MODELS.PAK (build/pak-m)')
    ap.add_argument('--hwargs', default='')
    ap.add_argument('--map', default='1')
    ap.add_argument('--nopc', action='store_true')
    a = ap.parse_args()
    names = sprites(a.dat)
    out = ROOT / 'build/mdall'
    out.mkdir(parents=True, exist_ok=True)
    nb = (len(names) + a.batch - 1) // a.batch
    only = {int(x) for x in a.only.split(',') if x}
    print(f'{len(names)} sprite names, {nb} batches', flush=True)
    for b in range(nb):
        if only and b not in only:
            continue
        f = out / f'set_{b}.lua'
        chunk = names[b * a.batch:(b + 1) * a.batch]
        f.write_text('rawset(_G, "MDL_SET", {' + ', '.join(f'"{n}"' for n in chunk) + '})\n')
        cmd = [sys.executable, str(ROOT / 'tools/ps2/fx_pair.py'), f'mdall_{b}', '--elf', a.elf, '--map', a.map, '--tick', '300', '--cfg', 'chasecam "On"',
               '--cmd', 'gr_models~On;con_hudlines~0', '--addon', f'{f},{ROOT}/tools/ps2/mdlscene.lua']
        cmd += ['--tree', a.tree] if a.tree else ['--pak', str(ROOT / 'build/pak-m')]
        if a.hwargs:
            cmd += ['--hwargs=' + a.hwargs]
        if a.nopc:
            cmd += ['--nopc']
        r = subprocess.run(cmd, capture_output=True, text=True)
        last = (r.stdout.strip().splitlines() or ['no output'])[-1]
        boot = ROOT / f'build/runs/fx_mdall_{b}/boot.txt'
        extra = ''
        if boot.exists():
            txt = boot.read_text(errors='replace')
            m = re.findall(r'HWPROF41[^\n]*', txt)
            extra = (m[-1] if m else 'no HWPROF41')
            bad = [l for l in txt.splitlines() if re.search(r'exception|panic|I_Error|Assert|TLB|Bus error', l, re.I)]
            if bad:
                extra += '  !! ' + bad[0][:120]
        print(f'batch {b} [{chunk[0]}..{chunk[-1]}]: {last}\n    {extra}', flush=True)


if __name__ == '__main__':
    main()
