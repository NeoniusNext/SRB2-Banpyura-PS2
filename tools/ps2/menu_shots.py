"""PS2-338 (OPT11 NETUI): pictures of typical menus with the button hints, Software and Hardware side by side.

usage: python3 tools/ps2/menu_shots.py --elf build/out1/SRB2.ELF [--out docs/GATES/g1/opt11-NETUI] [--only NAME ...] [--scale 1.5] [--extra "-menuhints 0"]
Each scenario drives the menus with -vidkeys (key presses by frame number of I_FinishUpdate) and takes one -vidshot, in both renderers, through
tools/ps2/netui_run.py (a private PCSX2 copy, one run at a time). The two pictures of a scenario become one JPEG: Software on the left, Hardware on the right.
"""
import argparse
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
MAIN = '100:enter,140:enter'
OPTIONS = MAIN + ',170:down,180:down,190:down,200:enter'
SCENARIOS = {
    # name: (engine arguments before -vidkeys, vidkeys, shot frame)
    'main': ([], MAIN, 180),
    'options': ([], OPTIONS, 240),
    'banpyura': ([], OPTIONS + ',240:down,246:down,252:down,258:down,264:down,270:down,276:down,290:enter', 340),
    'controls': ([], OPTIONS + ',240:enter,275:enter', 310),
    'address': ([], MAIN + ',170:down,200:enter,240:down', 280),
    'quit': ([], MAIN + ',170:down,175:down,180:down,185:down,200:enter', 245),
    'addons': ([], MAIN + ',170:down,175:down,200:enter', 245),
    'pause': (['-warp', '1'], '150:esc', 190),
}


def run(name, renderer, elf, out, extra):
    args, keys, shot = SCENARIOS[name]
    run_name = 'mh-%s-%s' % (name, renderer)
    cmd = [sys.executable, str(ROOT / 'tools/ps2/netui_run.py'), '--name', run_name, '--elf', elf, '--timeout', '240', '--until', 'VIDSHOT COMPLETE', '--', '-skipintro']
    if renderer == 'hw':
        cmd += ['-renderer', 'Hardware', '-zreserve', '1536']
    cmd += args + extra + ['-vidkeys', keys, '-vidshot', 'f%d' % shot]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return ROOT / 'build/runs' / run_name / ('vidshot-320x200-f%d.ppm' % shot)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--out', default=str(ROOT / 'docs/GATES/g1/opt11-NETUI'))
    ap.add_argument('--only', nargs='*', default=[])
    ap.add_argument('--scale', type=float, default=1.5)
    ap.add_argument('--prefix', default='hints-')
    ap.add_argument('--extra', default='')
    a = ap.parse_args()
    extra = a.extra.split()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for name in SCENARIOS:
        if a.only and name not in a.only:
            continue
        pics = []
        for r in ('sw', 'hw'):
            pics.append(Image.open(run(name, r, a.elf, out, extra)).convert('RGB'))
        w, h = pics[0].size
        sheet = Image.new('RGB', (w * 2 + 4, h), (255, 255, 255))
        sheet.paste(pics[0], (0, 0))
        sheet.paste(pics[1], (w + 4, 0))
        if a.scale != 1:
            sheet = sheet.resize((int(sheet.width * a.scale), int(sheet.height * a.scale)), Image.NEAREST)
        dst = out / (a.prefix + name + '.jpg')
        sheet.save(dst, quality=85)
        # the two renderers should agree in what the hints say: report the number of different pixels outside the 3D-free menu background is not meaningful, so only note the size
        print(name, dst, sheet.size, flush=True)


if __name__ == '__main__':
    main()
