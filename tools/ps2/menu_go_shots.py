"""PS2-339 (OPT11 NETUI): pictures of chosen menus of the crawler's list with the button hints, one emulator run for all of them.

usage: python3 tools/ps2/menu_go_shots.py --elf build/out1/SRB2.ELF --name RUN [--hw] [--args '-width 640 -height 480'] [--warp 1] [--scale 1] [--sheet OUT.png]
           MENU[:ITEM] [MENU[:ITEM] ...]
MENU is the number in the table of menu_crawl.py (the list of m_menu.c M_PS2MenuList). For each one the engine command ps2_menugo brings the menu up with the cursor on
ITEM (-menuseq SPACING,M:I,...: one menu every SPACING displayed frames) and a -vidshot m<N> takes the picture just before the next one (displayed frames,
so the pace does not depend on the game speed, a screen wipe or a slow game tic). The pictures go to build/runs/RUN/ and, with --sheet, into one contact sheet (a grid with the menu numbers).
"""
import argparse
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--name', required=True)
    ap.add_argument('--hw', action='store_true')
    ap.add_argument('--args', default='')
    ap.add_argument('--warp', type=int, default=0)
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--cols', type=int, default=2)
    ap.add_argument('--sheet', default='')
    ap.add_argument('--strips', default='', help='OUT.png: the bottom 44 rows of every picture, enlarged x3, one under the other (the corners with the hints)')
    ap.add_argument('--step', type=int, default=30, help='display frames between two pictures')
    ap.add_argument('--extra-engine', default='')
    ap.add_argument('menus', nargs='+')
    a = ap.parse_args()
    shots, labels, items = [], [], []
    for k, spec in enumerate(a.menus):
        menu, _, item = spec.partition(':')
        items.append('%s:%s' % (menu, item or '0'))
        shots.append('m%d' % (a.step * k + a.step - 5))
        labels.append(spec)
    cmd = [sys.executable, str(ROOT / 'tools/ps2/netui_run.py'), '--name', a.name, '--elf', a.elf, '--timeout', str(150 + 20 * len(a.menus)), '--until', 'VIDSHOT COMPLETE', '--', '-skipintro']
    if a.hw:
        cmd += ['-renderer', 'Hardware', '-zreserve', '1536']
    if a.warp:
        cmd += ['-warp', str(a.warp)]
    cmd += a.args.split() + a.extra_engine.split()
    cmd += ['-menuseq', '%d,%s' % (a.step, ','.join(items)), '-vidshot', ','.join(shots)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    pics = []
    run = ROOT / 'build/runs' / a.name
    for sh, lab in zip(shots, labels):
        found = sorted(run.glob('vidshot-*-%s.ppm' % sh))
        if not found:
            print('no picture for', lab, file=sys.stderr)
            continue
        pics.append((lab, Image.open(found[0]).convert('RGB')))
        print(lab, found[0].name)
    if a.sheet and pics:
        w, h = pics[0][1].size
        cols = a.cols
        rows = (len(pics) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * (w + 4), rows * (h + 4)), (255, 255, 255))
        for i, (lab, im) in enumerate(pics):
            sheet.paste(im, ((i % cols) * (w + 4), (i // cols) * (h + 4)))
            ImageDraw.Draw(sheet).text(((i % cols) * (w + 4) + 3, (i // cols) * (h + 4) + 2), lab, fill=(255, 255, 0))
        if a.scale != 1:
            sheet = sheet.resize((int(sheet.width * a.scale), int(sheet.height * a.scale)), Image.NEAREST)
        sheet.save(a.sheet)
        print('sheet', a.sheet, sheet.size)
    if a.strips and pics:
        w, h = pics[0][1].size
        dup = max(1, min(w // 320, h // 200))
        oy = (h - 200 * dup) // 2
        top = oy + 156 * dup
        bot = min(h, oy + 200 * dup + 0)
        rows = [im.crop((0, top, w, bot)).resize((w * 3, (bot - top) * 3), Image.NEAREST) for _, im in pics]
        strip = Image.new('RGB', (w * 3, sum(r.height + 4 for r in rows)), (255, 255, 255))
        yy = 0
        for (lab, _), r in zip(pics, rows):
            strip.paste(r, (0, yy))
            ImageDraw.Draw(strip).text((4, yy + 3), lab, fill=(255, 255, 0))
            yy += r.height + 4
        strip.save(a.strips)
        print('strips', a.strips, strip.size)
    return 0


if __name__ == '__main__':
    sys.exit(main())
