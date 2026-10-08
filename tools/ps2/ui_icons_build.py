"""PS2-333 (OPT11 NETUI): build src/ps2/ps2_uiicons_data.inc - the pad button icons as Doom patches in the game palette.

usage: python3 tools/ps2/ui_icons_build.py [--sheet assets/ps2ui/pad_buttons_sheet.jpg] [--srb2 /opt/srb2-assets/srb2.pk3]
                                           [--out src/ps2/ps2_uiicons_data.inc] [--preview DIR] [--zoom 6] [--reference]

The icons are drawn at their final size by tools/ps2/ui_icons_pixel.py (odd grids, shapes that are symmetric by construction, explicit pixel masks for the
symbols and a 3x5 font for the letters): they are exactly symmetric and crisp. The sheet (assets/ps2ui/pad_buttons_sheet.jpg, "By: Max_the_Viking") is cut by
tools/ps2/ui_icons.py and reduced to the same size as a REFERENCE: it gives the shapes, the proportions and the colours that the drawings follow, and every icon
is compared with its reference (overlap of the silhouettes, "IoU" column). The build FAILS when an icon is not symmetric where it was designed to be
(EXPECT: axes of the silhouette and of the whole picture; mirror_h = left/right, mirror_v = top/bottom, in differing pixels).
--reference writes the reduced-sheet icons instead (soft, lopsided: what the first version of this feature looked like).
Output: the C data (included by src/ps2/ps2_uiicons.c) and, with --preview, a contact sheet of every icon (icons-contact.png, enlarged) and the icons at their real size
(icons-real-size.png).
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ui_icons as ref_lib  # noqa: E402  (the sheet: cutting and the reduced reference)
import ui_icons_pixel  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# the symmetry every icon must have: (axes of the silhouette, axes of the whole picture); 'h' = left/right mirror, 'v' = top/bottom mirror
# The letters of the sticks and L1.. and the direction arrows of the sticks make the picture asymmetric by design: only the silhouette is checked there.
EXPECT = {}
for _n in ('SELECT', 'SQUARE', 'CIRCLE', 'CROSS'):
    EXPECT[_n] = ('hv', 'hv')
EXPECT['TRIANGLE'] = ('h', 'h')
EXPECT['START'] = ('v', 'v')
for _n in ('L1', 'R1'):
    EXPECT[_n] = ('hv', '')  # the letters are not symmetric, the button is
for _n in ('L2', 'R2'):
    EXPECT[_n] = ('h', '')
for _n in ('LSTICK', 'RSTICK', 'L3', 'R3', 'LSTICK_ROT', 'RSTICK_ROT'):
    EXPECT[_n] = ('hv', '')
for _p in ('LSTICK', 'RSTICK'):
    for _s in ('DOWN', 'LEFT', 'RIGHT', 'UP', 'ALL', 'UD', 'LR'):
        EXPECT[_p + '_' + _s] = ('hv', '')
for _n in ('DPAD_LR', 'DPAD_ALL', 'DPAD_UD'):
    EXPECT[_n] = ('hv', 'hv')
for _n in ('DPAD_UP', 'DPAD_DOWN'):
    EXPECT[_n] = ('hv', 'h')
for _n in ('DPAD_LEFT', 'DPAD_RIGHT'):
    EXPECT[_n] = ('hv', 'v')
for _n in ('ARROW_UP', 'ARROW_DOWN'):
    EXPECT[_n] = ('h', 'h')
for _n in ('ARROW_LEFT', 'ARROW_RIGHT'):
    EXPECT[_n] = ('v', 'v')


# the small set (build_small): the same kind of expectation
EXPECT_SMALL = {'CROSS': ('hv', 'hv'), 'CIRCLE': ('hv', 'hv'), 'SQUARE': ('hv', 'hv'), 'TRIANGLE': ('hv', 'h'), 'START': ('hv', 'v'), 'SELECT': ('hv', 'hv'),
                'DPAD_UP': ('hv', 'h'), 'DPAD_DOWN': ('hv', 'h'), 'DPAD_LEFT': ('hv', 'v'), 'DPAD_RIGHT': ('hv', 'v'), 'DPAD_UD': ('hv', 'hv'), 'DPAD_LR': ('hv', 'hv')}
for _n in ('L1', 'R1', 'L2', 'R2', 'L3', 'R3', 'LSTICK', 'RSTICK'):
    EXPECT_SMALL[_n] = ('hv', '')  # the letters are not symmetric, the button is


def canvas_to_idx(cv):
    idx = np.full((cv.h, cv.w), -1, dtype=np.int32)
    for y in range(cv.h):
        for x in range(cv.w):
            ch = cv.p[y][x]
            if ch != '.':
                idx[y, x] = ui_icons_pixel.COLORS[ch]
    return idx


def mirror_diff(arr, axis):
    flipped = arr[:, ::-1] if axis == 'h' else arr[::-1, :]
    return int((arr != flipped).sum())


def render(idx, pal, zoom):
    h, w = idx.shape
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if idx[y, x] >= 0:
                px[x, y] = tuple(int(v) for v in pal[idx[y, x]]) + (255,)
    return im.resize((w * zoom, h * zoom), Image.NEAREST)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sheet', default=str(ROOT / 'assets/ps2ui/pad_buttons_sheet.jpg'))
    ap.add_argument('--srb2', default='/opt/srb2-assets/srb2.pk3')
    ap.add_argument('--out', default=str(ROOT / 'src/ps2/ps2_uiicons_data.inc'))
    ap.add_argument('--preview', default='')
    ap.add_argument('--zoom', type=int, default=6)
    ap.add_argument('--reference', action='store_true')
    a = ap.parse_args()

    rgb = np.asarray(Image.open(a.sheet).convert('RGB')).astype(np.int32)
    pal = ref_lib.load_palette(a.srb2)
    rows = ref_lib.find_icons(rgb)
    ref = {}
    for r, names in enumerate(ref_lib.LAYOUT):
        if len(rows[r]) != len(names):
            sys.exit('row %d: found %d icons, expected %d' % (r, len(rows[r]), len(names)))
        for name, b in zip(names, rows[r]):
            ref[name] = b
    canv = ui_icons_pixel.build()
    order = [n for names in ref_lib.LAYOUT for n in names]
    missing = [n for n in order if n not in canv]
    if missing:
        sys.exit('no pixel icon for %s' % missing)

    result = []
    bad = 0
    print('%-18s %-6s %-11s %-11s %5s  %s' % ('icon', 'size', 'silhouette', 'picture', 'IoU', 'bytes'))
    for name in order:
        cv = canv[name]
        idx = canvas_to_idx(cv)
        opaque = idx >= 0
        sil = {ax: mirror_diff(opaque.astype(np.int32), ax) for ax in 'hv'}
        pic = {ax: mirror_diff(idx, ax) for ax in 'hv'}
        want_sil, want_pic = EXPECT.get(name, ('', ''))
        flaws = ['silhouette %s: %d px differ' % (ax, sil[ax]) for ax in want_sil if sil[ax]] + ['picture %s: %d px differ' % (ax, pic[ax]) for ax in want_pic if pic[ax]]
        y0, x0, y1, x1, comp = ref[name]
        rfidx = ref_lib.reduce_icon(rgb, comp, (y0, x0, y1, x1), (cv.w, cv.h), pal)
        ropq = rfidx >= 0
        iou = float((opaque & ropq).sum()) / max(1, int((opaque | ropq).sum()))
        final = rfidx if a.reference else idx
        data = ref_lib.to_patch(final)
        result.append((name, (cv.w, cv.h), data, final))
        print('BTN_%-14s %2dx%-3d  h%-2d v%-2d      h%-2d v%-2d    %5.2f  %4d  %s' % (name, cv.w, cv.h, sil['h'], sil['v'], pic['h'], pic['v'], iou, len(data), '; '.join(flaws)))
        bad += bool(flaws)
    if bad and not a.reference:
        sys.exit('%d icon(s) are not symmetric as designed' % bad)

    small = ui_icons_pixel.build_small()
    sresult = []
    sbad = 0
    print()
    print('%-18s %-6s %-11s %-11s  %s' % ('small icon', 'size', 'silhouette', 'picture', 'bytes'))
    for name in order:
        if name not in small:
            continue
        cv = small[name]
        idx = canvas_to_idx(cv)
        opaque = idx >= 0
        sil = {ax: mirror_diff(opaque.astype(np.int32), ax) for ax in 'hv'}
        pic = {ax: mirror_diff(idx, ax) for ax in 'hv'}
        want_sil, want_pic = EXPECT_SMALL[name]
        flaws = ['silhouette %s: %d px differ' % (ax, sil[ax]) for ax in want_sil if sil[ax]] + ['picture %s: %d px differ' % (ax, pic[ax]) for ax in want_pic if pic[ax]]
        data = ref_lib.to_patch(idx)
        sresult.append((name, (cv.w, cv.h), data, idx))
        print('S_%-16s %2dx%-3d  h%-2d v%-2d      h%-2d v%-2d    %4d  %s' % (name, cv.w, cv.h, sil['h'], sil['v'], pic['h'], pic['v'], len(data), '; '.join(flaws)))
        sbad += bool(flaws)
    if sbad:
        sys.exit('%d small icon(s) are not symmetric as designed' % sbad)
    if set(small) - set(n for n, *_ in sresult):
        sys.exit('small icons not in the layout: %s' % sorted(set(small) - set(n for n, *_ in sresult)))

    with open(a.out, 'w', newline='\n') as f:
        f.write('// Generated by tools/ps2/ui_icons_build.py (PS2-333) from tools/ps2/ui_icons_pixel.py - do not edit.\n')
        f.write('// PlayStation pad button icons after the sheet assets/ps2ui/pad_buttons_sheet.jpg, credit: "By: Max_the_Viking" (as printed on the sheet).\n')
        f.write('// Doom patches in the SRB2 palette (PLAYPAL), aligned to 4: the patch reader reads the column offsets as 32 bit words.\n')
        for name, size, data, idx in result:
            f.write('static const UINT8 uiicon_%s[%d] __attribute__((aligned(4))) = {%s};\n' % (name.lower(), len(data), ','.join(str(b) for b in data)))
        f.write('static const struct { const char *name; const UINT8 *data; UINT16 size; UINT8 w, h; } uiicons[] = {\n')
        for name, size, data, idx in result:
            f.write('\t{"BTN_%s", uiicon_%s, %d, %d, %d},\n' % (name, name.lower(), len(data), size[0], size[1]))
        f.write('};\n')
        f.write('// the small set (7 px tall, for the lines of text): tools/ps2/ui_icons_pixel.py build_small(); an icon without one has data NULL\n')
        for name, size, data, idx in sresult:
            f.write('static const UINT8 uiiconsmall_%s[%d] __attribute__((aligned(4))) = {%s};\n' % (name.lower(), len(data), ','.join(str(b) for b in data)))
        f.write('static const struct { const UINT8 *data; UINT16 size; UINT8 w, h; } uiicons_small[] = {\n')
        for name, size, data, idx in sresult:
            f.write('\t[PS2UI_%s] = {uiiconsmall_%s, %d, %d, %d},\n' % (name, name.lower(), len(data), size[0], size[1]))
        f.write('};\n')
    print('wrote %s: %d icons, %d bytes of patches + %d small icons, %d bytes' % (a.out, len(result), sum(len(r[2]) for r in result), len(sresult), sum(len(r[2]) for r in sresult)))

    if a.preview:
        from PIL import ImageDraw
        pv = Path(a.preview)
        pv.mkdir(parents=True, exist_ok=True)
        zoom, cols = a.zoom, 8
        cell = 17 * zoom
        nrows = (len(result) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * cell, nrows * (cell + 12)), (70, 100, 180))
        dr = ImageDraw.Draw(sheet)
        for n, (name, size, data, idx) in enumerate(result):
            big = render(idx, pal, zoom)
            gx, gy = (n % cols) * cell, (n // cols) * (cell + 12)
            sheet.paste(big, (gx + (cell - big.width) // 2, gy + (cell - big.height) // 2), big)
            dr.text((gx + 2, gy + cell), name, fill=(255, 255, 255))
        sheet.save(pv / 'icons-contact.png', optimize=True)
        real = Image.new('RGB', (cols * 24, nrows * 20), (0, 0, 120))
        for n, (name, size, data, idx) in enumerate(result):
            big = render(idx, pal, 1)
            real.paste(big, ((n % cols) * 24 + (24 - big.width) // 2, (n // cols) * 20 + (20 - big.height) // 2), big)
        real.save(pv / 'icons-real-size.png')
        print('preview', pv / 'icons-contact.png')
        zoom2, cols2 = 10, 9
        cell2 = 13 * zoom2
        nrows2 = (len(sresult) + cols2 - 1) // cols2
        sheet2 = Image.new('RGB', (cols2 * cell2, nrows2 * (cell2 + 14)), (70, 100, 180))
        dr2 = ImageDraw.Draw(sheet2)
        for n, (name, size, data, idx) in enumerate(sresult):
            big = render(idx, pal, zoom2)
            gx, gy = (n % cols2) * cell2, (n // cols2) * (cell2 + 14)
            sheet2.paste(big, (gx + (cell2 - big.width) // 2, gy + (cell2 - big.height) // 2), big)
            dr2.text((gx + 2, gy + cell2), 'S_' + name, fill=(255, 255, 255))
        sheet2.save(pv / 'icons-small-contact.png', optimize=True)
        real2 = Image.new('RGB', (cols2 * 14, nrows2 * 10), (0, 0, 120))
        for n, (name, size, data, idx) in enumerate(sresult):
            big = render(idx, pal, 1)
            real2.paste(big, ((n % cols2) * 14 + (14 - big.width) // 2, (n // cols2) * 10 + (10 - big.height) // 2), big)
        real2.save(pv / 'icons-small-real-size.png')
        print('preview', pv / 'icons-small-contact.png')


if __name__ == '__main__':
    main()
