"""PS2-339 (OPT11 NETUI): the same menu with the button hints on and off, pixel by pixel (both renderers).

usage: python3 tools/ps2/menu_hint_diff.py --elf build/out1/SRB2.ELF --name RUN [--hw] [--args '-vidmode 9'] [--sheet OUT.png] [--scale 1] MENU[:ITEM] [MENU[:ITEM] ...]
For each menu one -menuseq series shows it with the cvar menuhints On, then Off (ps2_menugo MENU ITEM 1 / 0) and takes a -vidshot of each. The pictures differ where the hints are:
  * inside   = differing pixels inside the plates that the engine reported for that picture (MHCHECK lines of -menuhintscheck: [x,y,wxh] in the 320x200 picture),
  * outside  = differing pixels anywhere else. Zero means: nothing of the menu or of its background was touched, the hints drew only on their own plates;
               a moving background (title screen sky) shows up here as a count too, so it is reported separately: `bg` = differing pixels between two Off pictures is not taken, the
               count is simply the number.
The result is printed as a Markdown table; --sheet writes the On pictures with the plates outlined next to the difference masks.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]


def pause_pair(a):
    """the pause menu of the first level: On picture at f190 (then menuhints 0), Off picture at f230; the plates from the last MHCHECK line"""
    cmd = [sys.executable, str(ROOT / 'tools/ps2/netui_run.py'), '--name', a.name, '--elf', a.elf, '--timeout', '300', '--until', 'VIDSHOT COMPLETE', '--stall', 'VIDSHOT=120', '--',
           '-skipintro', '-warp', '1', '-menuhintscheck', '-vidcmd', 'con_hudlines~0']
    if a.hw:
        cmd += ['-renderer', 'Hardware', '-zreserve', '1536']
    cmd += a.args.split()
    cmd += ['-vidkeys', '150:esc', '-vidshot', 'f190=menuhints~0,f230']
    subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL)
    run = ROOT / 'build/runs' / a.name
    log = (run / 'boot.txt').read_text(errors='replace')
    mm = re.findall(r'MHCHECK [^\n]*?plates=\d+((?: \[-?\d+,-?\d+,\d+x\d+\])*)', log)
    pl = [tuple(map(int, p)) for p in re.findall(r'\[(-?\d+),(-?\d+),(\d+)x(\d+)\]', mm[-1])] if mm else []
    fon, foff = sorted(run.glob('vidshot-*-f190.ppm')), sorted(run.glob('vidshot-*-f230.ppm'))
    if not fon or not foff:
        print('no pictures')
        return 2
    on, off = Image.open(fon[0]).convert('RGB'), Image.open(foff[0]).convert('RGB')
    w, h = on.size
    dup = max(1, min(w // 320, h // 200))
    ox, oy = (w - 320 * dup) // 2, (h - 200 * dup) // 2
    inside = [[False] * w for _ in range(h)]
    for (x, y, pw, ph) in pl:
        for yy in range(max(0, y * dup + oy), min(h, (y + ph) * dup + oy)):
            for xx in range(max(0, x * dup + ox), min(w, (x + pw) * dup + ox)):
                inside[yy][xx] = True
    pa, pb = on.load(), off.load()
    n_in = n_out = 0
    mask = Image.new('RGB', (w, h), (0, 0, 0))
    mp = mask.load()
    for yy in range(h):
        for xx in range(w):
            if pa[xx, yy] != pb[xx, yy]:
                if inside[yy][xx]:
                    n_in += 1
                    mp[xx, yy] = (255, 255, 255)
                else:
                    n_out += 1
                    mp[xx, yy] = (255, 0, 0)
    print('| menu | picture | plates | differing px inside the plates | differing px elsewhere |')
    print('|---|---|--:|--:|--:|')
    print('| pause menu of the level | %dx%d | %d | %d | %d |' % (w, h, len(pl), n_in, n_out))
    if a.sheet:
        d = ImageDraw.Draw(on)
        for (x, y, pw, ph) in pl:
            d.rectangle([x * dup + ox, y * dup + oy, (x + pw) * dup + ox - 1, (y + ph) * dup + oy - 1], outline=(255, 255, 0))
        sheet = Image.new('RGB', (2 * (w + 4), h), (255, 255, 255))
        sheet.paste(on, (0, 0))
        sheet.paste(mask, (w + 4, 0))
        sheet.save(a.sheet)
        print('sheet', a.sheet, sheet.size)
    return 0 if n_out == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--name', required=True)
    ap.add_argument('--hw', action='store_true')
    ap.add_argument('--args', default='')
    ap.add_argument('--warp', type=int, default=0)
    ap.add_argument('--step', type=int, default=30)
    ap.add_argument('--sheet', default='')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--pause', action='store_true', help='the pause menu of a level (-warp 1, Esc): the game stands still, so the two pictures differ only where the hints are')
    ap.add_argument('menus', nargs='*')
    a = ap.parse_args()
    if a.pause:
        return pause_pair(a)
    items, shots, labels = [], [], []
    for k, spec in enumerate(a.menus):
        menu, _, item = spec.partition(':')
        for on in (1, 0):
            items.append('%s:%s:%d' % (menu, item or '0', on))
            shots.append('m%d' % (a.step * (len(items) - 1) + a.step - 5))
        labels.append(spec)
    cmd = [sys.executable, str(ROOT / 'tools/ps2/netui_run.py'), '--name', a.name, '--elf', a.elf, '--timeout', str(150 + 20 * len(items)), '--until', 'VIDSHOT COMPLETE', '--stall', 'VIDSHOT=90', '--', '-skipintro', '-menuhintscheck']
    if a.hw:
        cmd += ['-renderer', 'Hardware', '-zreserve', '1536']
    if a.warp:
        cmd += ['-warp', str(a.warp)]
    cmd += a.args.split()
    cmd += ['-menuseq', '%d,%s' % (a.step, ','.join(items)), '-vidshot', ','.join(shots)]
    subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL)
    run = ROOT / 'build/runs' / a.name
    log = (run / 'boot.txt').read_text(errors='replace')
    # the plates of each "hints on" state: the MHCHECK lines between the k-th MHGO line and the next one
    chunks = re.split(r'^MHGO ', log, flags=re.M)[1:]
    plates_of = []
    for ch in chunks:
        head = ch.split('\n', 1)[0]
        m = re.search(r'hints (\d)', head)
        pl = []
        if m and m.group(1) == '1':
            mm = re.findall(r'MHCHECK [^\n]*?plates=\d+((?: \[-?\d+,-?\d+,\d+x\d+\])*)', ch)
            if mm:
                pl = [tuple(map(int, p)) for p in re.findall(r'\[(-?\d+),(-?\d+),(\d+)x(\d+)\]', mm[-1])]
        plates_of.append(pl)
    rows = ['| menu | picture | plates | differing px inside the plates | differing px elsewhere |', '|---|---|--:|--:|--:|']
    sheet_imgs = []
    total_out = 0
    for k, lab in enumerate(labels):
        on_i, off_i = 2 * k, 2 * k + 1
        fon = sorted(run.glob('vidshot-*-%s.ppm' % shots[on_i]))
        foff = sorted(run.glob('vidshot-*-%s.ppm' % shots[off_i]))
        if not fon or not foff:
            rows.append('| %s | (no picture) | | | |' % lab)
            continue
        on = Image.open(fon[0]).convert('RGB')
        off = Image.open(foff[0]).convert('RGB')
        w, h = on.size
        dup = max(1, min(w // 320, h // 200))
        ox, oy = (w - 320 * dup) // 2, (h - 200 * dup) // 2
        inside = [[False] * w for _ in range(h)]
        pl = plates_of[on_i] if on_i < len(plates_of) else []
        for (x, y, pw, ph) in pl:
            for yy in range(max(0, y * dup + oy), min(h, (y + ph) * dup + oy)):
                for xx in range(max(0, x * dup + ox), min(w, (x + pw) * dup + ox)):
                    inside[yy][xx] = True
        pa, pb = on.load(), off.load()
        n_in = n_out = 0
        mask = Image.new('RGB', (w, h), (0, 0, 0))
        mp = mask.load()
        for yy in range(h):
            for xx in range(w):
                if pa[xx, yy] != pb[xx, yy]:
                    if inside[yy][xx]:
                        n_in += 1
                        mp[xx, yy] = (255, 255, 255)
                    else:
                        n_out += 1
                        mp[xx, yy] = (255, 0, 0)
        total_out += n_out
        rows.append('| %s | %dx%d | %d | %d | %d |' % (lab, w, h, len(pl), n_in, n_out))
        d = ImageDraw.Draw(on)
        for (x, y, pw, ph) in pl:
            d.rectangle([x * dup + ox, y * dup + oy, (x + pw) * dup + ox - 1, (y + ph) * dup + oy - 1], outline=(255, 255, 0))
        sheet_imgs.append((lab, on, mask))
    print('\n'.join(rows))
    if a.sheet and sheet_imgs:
        w, h = sheet_imgs[0][1].size
        sheet = Image.new('RGB', (2 * (w + 4), len(sheet_imgs) * (h + 4)), (255, 255, 255))
        for i, (lab, on, mask) in enumerate(sheet_imgs):
            sheet.paste(on, (0, i * (h + 4)))
            sheet.paste(mask, (w + 4, i * (h + 4)))
        if a.scale != 1:
            sheet = sheet.resize((int(sheet.width * a.scale), int(sheet.height * a.scale)), Image.NEAREST)
        sheet.save(a.sheet)
        print('sheet', a.sheet, sheet.size)
    return 0 if total_out == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
