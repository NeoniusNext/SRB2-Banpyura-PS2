"""PS2-333 (OPT11 NETUI): cut the PlayStation pad button sheet into small icons and convert them to Doom patches in the game palette.

usage: python3 tools/ps2/ui_icons.py [--sheet assets/ps2ui/pad_buttons_sheet.jpg] [--srb2 /opt/srb2-assets/srb2.pk3]
                                     [--out src/ps2/ps2_uiicons_data.inc] [--preview DIR] [--only NAME ...]

Input: assets/ps2ui/pad_buttons_sheet.jpg (1200x1200, white background, no alpha; "By: Max_the_Viking", see assets/ps2ui/README.txt).
 1. every icon is a connected component of the non-white pixels (threshold on the minimum channel: JPEG noise sits at 245..255); the captions of the
    sheet ("Classic PS Buttons", "PS4 Buttons", "By: Max_the_Viking") and the islands inside letters (the hole of an R) are dropped; the square and R2 touch
    on the sheet and are separated at the row boundary; the 43 icons are named by their place in the grid (LAYOUT below);
 2. the alpha of an icon is its silhouette with the holes filled (white letters and the white arrows of the last row stay opaque) and, at the
    edge, the darkness of the black outline (the art is black outlined: alpha = 1 - min channel / 255 there, colour black);
 3. the icon is reduced from the 1200 px original with a high-quality filter (Lanczos on premultiplied colour, a slight unsharp mask so that the letters
    L1/R2 and the symbols stay readable) to the size of SIZES (a face button is 13 px: about the height of a menu font line plus a little);
 4. alpha >= 50 % is opaque (a Doom patch has no partial alpha), each colour becomes the nearest colour of the game palette (PLAYPAL of srb2.pk3;
    index 255 is not used as a "transparent" index: transparency is the absence of a post), and the picture is written as a Doom patch
    (softwarepatch_t: header, column offsets, posts) - exactly what Patch_CreateFromDoomPatch() of the engine reads.
Output: src/ps2/ps2_uiicons_data.inc (C arrays, included by src/ps2/ps2_uiicons.c) and, with --preview, 8x enlarged PNGs of every icon and a contact
sheet (docs/GATES/g1/opt11-NETUI/icons-*.png) to look at the result.
"""
import argparse
import struct
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter
from scipy import ndimage as ndi

ROOT = Path(__file__).resolve().parents[2]

# the grid of the sheet, row by row (the order of the components inside a row is left to right)
LAYOUT = [
    ['SELECT', 'START'],  # "Classic PS Buttons"; the two PS4 strips (Share, Options) of the sheet are NOT used
    ['TRIANGLE', 'SQUARE', 'CIRCLE', 'CROSS'],
    ['L2', 'R2', 'L1', 'R1'],
    ['LSTICK', 'DPAD_DOWN', 'DPAD_LEFT', 'DPAD_RIGHT', 'DPAD_UP', 'DPAD_LR', 'DPAD_ALL', 'DPAD_UD'],
    ['RSTICK', 'RSTICK_DOWN', 'RSTICK_LEFT', 'RSTICK_RIGHT', 'RSTICK_UP', 'RSTICK_ALL', 'RSTICK_UD', 'RSTICK_LR'],
    ['L3', 'LSTICK_DOWN', 'LSTICK_LEFT', 'LSTICK_RIGHT', 'LSTICK_UP', 'LSTICK_ALL', 'LSTICK_UD', 'LSTICK_LR'],
    ['R3', 'LSTICK_ROT', 'RSTICK_ROT'],
    ['ARROW_LEFT', 'ARROW_RIGHT', 'ARROW_DOWN', 'ARROW_UP'],
]
ROW_Y = [0, 140, 302, 450, 600, 750, 900, 1075]  # first y of each row of LAYOUT (the rows of the sheet)

# target sizes in pixels (width, height) of the reduced icon; the sheet icons are ~145 px, so a face button is 1/11 of the original
SIZES = {
    'SELECT': (15, 9), 'START': (15, 10),
    'TRIANGLE': (13, 13), 'SQUARE': (13, 13), 'CIRCLE': (13, 13), 'CROSS': (13, 13),
    'L2': (14, 15), 'R2': (14, 15), 'L1': (16, 11), 'R1': (16, 11),
    'ARROW_LEFT': (9, 9), 'ARROW_RIGHT': (9, 9), 'ARROW_DOWN': (9, 9), 'ARROW_UP': (9, 9),
}
DEFAULT_SIZE = (14, 14)  # sticks, D-pads, L3/R3


def load_palette(pk3):
    with zipfile.ZipFile(pk3) as z:
        data = z.read('PLAYPAL')
    pal = np.frombuffer(data[:768], dtype=np.uint8).reshape(256, 3).astype(np.int32)
    return pal


def find_icons(rgb):
    """-> list of (row, x0, y0, x1, y1) boxes in layout order, and the full-resolution masks"""
    mn = rgb.min(axis=2)
    mask = mn < 235
    lab, n = ndi.label(mask, structure=np.ones((3, 3)))
    objs = ndi.find_objects(lab)
    comps = []
    for i, sl in enumerate(objs, 1):
        y0, y1, x0, x1 = sl[0].start, sl[0].stop, sl[1].start, sl[1].stop
        area = int((lab[sl] == i).sum())
        if area < 1000:
            continue  # letter holes (islands inside R, 2, P), caption letters and noise
        if y1 <= 36:
            continue  # "Classic PS Buttons" / "PS4 Buttons"
        if y0 >= 1125 and x0 >= 760:
            continue  # "By: Max_the_Viking"
        if x0 >= 380 and y1 <= 125:
            continue  # the "PS4 Buttons" (Share, Options): removed on request, never cut, converted or packed
        comps.append((y0, x0, y1, x1, i))
    # the square and R2 are one component (their outlines touch): cut it at the row boundary of the sheet
    out = []
    for y0, x0, y1, x1, i in comps:
        if y1 - y0 > 250:
            cut = 301  # the sparsest row between the two (2 dark pixels)
            lab2 = np.zeros(lab.shape, dtype=bool)
            lab2[y0:y1, x0:x1] = (lab[y0:y1, x0:x1] == i)
            top = lab2.copy()
            top[cut:] = False
            bot = lab2.copy()
            bot[:cut] = False
            for part in (top, bot):
                l3, n3 = ndi.label(part, structure=np.ones((3, 3)))
                sizes = ndi.sum(part, l3, range(1, n3 + 1))
                part = l3 == (1 + int(np.argmax(sizes)))  # the largest piece: the 2 pixels of the touching row go away
                ys, xs = np.nonzero(part)
                out.append((ys.min(), xs.min(), ys.max() + 1, xs.max() + 1, part))
        else:
            out.append((y0, x0, y1, x1, lab == i))
    boxes = []
    for y0, x0, y1, x1, comp in out:
        boxes.append((y0, x0, y1, x1, comp))
    # rows
    rows = [[] for _ in LAYOUT]
    for b in boxes:
        cy = (b[0] + b[2]) // 2
        r = max(k for k, v in enumerate(ROW_Y) if v <= b[0] + 2) if b[0] + 2 >= ROW_Y[0] else 0
        # the icons of row 2 that start at 150 and R2 (starts at 305) are told apart by ROW_Y; L1/R1 start at 333 (row 3)
        rows[r].append(b)
    for r in rows:
        r.sort(key=lambda b: b[1])
    return rows


# classes of the source pixels (the sheet is 4..5 flat colours with a black outline; JPEG blends them at the edges)
C_OUTLINE, C_FILL, C_WHITE, C_COLOR = range(4)
# weights of the vote when a target pixel covers several classes: thin bright strokes (the symbols, the letters) must survive the reduction to 13 px,
# so they win against the dark body from 40 % of coverage on
WEIGHT = {C_OUTLINE: 1.0, C_FILL: 0.9, C_WHITE: 1.25, C_COLOR: 1.7}


def classify(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx = rgb.max(axis=2)
    mn = rgb.min(axis=2)
    sat = mx - mn
    lum = (r + g + b) / 3.0
    cls = np.full(r.shape, C_FILL, dtype=np.int8)
    cls[lum < 32] = C_OUTLINE
    cls[(lum >= 160) & (sat <= 60)] = C_WHITE
    cls[(sat > 60) & (mn < 235)] = C_COLOR
    return cls


def reduce_icon(rgb, comp, box, size, pal):
    """-> (index array h x w, -1 = transparent). The reduction is done per class (area average of every class mask, then a weighted vote), which keeps
    the thin symbols and letters crisp where a filtered reduction of the colours would smear them into the dark body."""
    y0, x0, y1, x1 = box
    crop = rgb[y0:y1, x0:x1]
    sil = ndi.binary_fill_holes(comp[y0:y1, x0:x1])
    cls = classify(crop)
    tw, th = size

    def avg(mask):
        im = Image.fromarray((mask.astype(np.uint8) * 255), 'L')
        return np.asarray(im.resize((tw, th), Image.BOX)).astype(np.float32) / 255.0

    alpha = avg(sil)
    cov = np.stack([avg(sil & (cls == k)) for k in range(4)], axis=0)
    score = np.stack([cov[k] * WEIGHT[k] for k in range(4)], axis=0)
    pick = score.argmax(axis=0)
    # representative colour of each class in this icon (mean of the pixels), mapped to the nearest palette colour
    refs = {}
    for k in range(4):
        m = sil & (cls == k)
        if m.sum() > 20:
            refs[k] = crop[m].mean(axis=0)
    # black outline: pure black; the body: its own grey; white: pure white
    refs[C_OUTLINE] = np.array([0.0, 0.0, 0.0])
    if C_WHITE in refs:
        refs[C_WHITE] = np.array([255.0, 255.0, 255.0])
    mid = np.array([150.0, 150.0, 150.0])

    def nearest(col):
        d = ((pal - col) ** 2 * np.array([2.0, 4.0, 3.0])).sum(axis=1)
        return int(d.argmin())

    idx = np.full((th, tw), -1, dtype=np.int32)
    for y in range(th):
        for x in range(tw):
            if alpha[y, x] < 0.5:
                continue
            k = int(pick[y, x])
            # anti-aliasing: a body pixel that is a third white (the edge of a letter) becomes light grey; the same for the colour on the body
            if k == C_FILL and cov[C_WHITE][y, x] >= 0.3:
                idx[y, x] = nearest(mid)
            elif k == C_FILL and cov[C_COLOR][y, x] >= 0.3 and C_COLOR in refs:
                idx[y, x] = nearest((refs[C_COLOR] + refs[C_FILL]) / 2.0)
            elif k in refs:
                idx[y, x] = nearest(refs[k])
            else:
                idx[y, x] = nearest(refs.get(C_FILL, np.array([64.0, 64.0, 64.0])))
    # a closed black outline whatever the phase of the reduction: an opaque pixel next to a transparent one (or at the border) is outline
    black = nearest(refs[C_OUTLINE])
    opaque = idx >= 0
    edge = np.zeros_like(opaque)
    padded = np.pad(opaque, 1, constant_values=False)
    for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
        edge |= opaque & ~padded[1 + dy:1 + dy + th, 1 + dx:1 + dx + tw]
    idx[edge] = black
    return idx


def to_patch(idx):
    h, w = idx.shape
    # Doom patch: header, column offsets, posts (topdelta, length, pad, data, pad), 0xFF
    cols = []
    for x in range(w):
        out = bytearray()
        y = 0
        while y < h:
            if idx[y, x] < 0:
                y += 1
                continue
            y2 = y
            while y2 < h and idx[y2, x] >= 0:
                y2 += 1
            out += bytes([y, y2 - y, 0]) + bytes(int(v) for v in idx[y:y2, x]) + bytes([0])
            y = y2
        out += bytes([0xFF])
        cols.append(bytes(out))
    head = struct.pack('<hhhh', w, h, 0, 0)
    ofs = 8 + 4 * w
    table = b''
    for cdata in cols:
        table += struct.pack('<i', ofs)
        ofs += len(cdata)
    return head + table + b''.join(cols)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sheet', default=str(ROOT / 'assets/ps2ui/pad_buttons_sheet.jpg'))
    ap.add_argument('--srb2', default='/opt/srb2-assets/srb2.pk3')
    ap.add_argument('--out', default=str(ROOT / 'src/ps2/ps2_uiicons_data.inc'))
    ap.add_argument('--preview', default='')
    ap.add_argument('--only', nargs='*', default=[])
    a = ap.parse_args()

    rgb = np.asarray(Image.open(a.sheet).convert('RGB')).astype(np.int32)
    pal = load_palette(a.srb2)
    rows = find_icons(rgb)
    items = []
    for r, names in enumerate(LAYOUT):
        if len(rows[r]) != len(names):
            sys.exit('row %d: found %d icons, expected %d (%s)' % (r, len(rows[r]), len(names), [b[:4] for b in rows[r]]))
        for name, b in zip(names, rows[r]):
            items.append((name, b))
    result = []
    for name, (y0, x0, y1, x1, comp) in items:
        if a.only and name not in a.only:
            continue
        size = SIZES.get(name, DEFAULT_SIZE)
        idx = reduce_icon(rgb, comp, (y0, x0, y1, x1), size, pal)
        data = to_patch(idx)
        result.append((name, size, data, idx))
        print('BTN_%-14s %3dx%-3d  box %4d,%4d %4dx%-4d  patch %4d bytes' % (name, size[0], size[1], x0, y0, x1 - x0, y1 - y0, len(data)))

    with open(a.out, 'w', newline='\n') as f:
        f.write('// Generated by tools/ps2/ui_icons.py (PS2-333) from assets/ps2ui/pad_buttons_sheet.jpg - do not edit.\n')
        f.write('// PlayStation pad button icons, credit: "By: Max_the_Viking" (as printed on the sheet). Doom patches in the SRB2 palette (PLAYPAL).\n')
        f.write('// name, width, height, data. Aligned to 4: the patch reader reads the column offsets as 32 bit words.\n')
        for name, size, data, idx in result:
            f.write('static const UINT8 uiicon_%s[%d] __attribute__((aligned(4))) = {' % (name.lower(), len(data)))
            f.write(','.join(str(b) for b in data))
            f.write('};\n')
        f.write('static const struct { const char *name; const UINT8 *data; UINT16 size; UINT8 w, h; } uiicons[] = {\n')
        for name, size, data, idx in result:
            f.write('\t{"BTN_%s", uiicon_%s, %d, %d, %d},\n' % (name, name.lower(), len(data), size[0], size[1]))
        f.write('};\n')
    print('wrote %s: %d icons, %d bytes of patches' % (a.out, len(result), sum(len(r[2]) for r in result)))

    if a.preview:
        pv = Path(a.preview)
        pv.mkdir(parents=True, exist_ok=True)
        zoom = 8
        cell = 18 * zoom
        cols = 8
        sheet = Image.new('RGB', (cols * cell, ((len(result) + cols - 1) // cols) * (cell + 14)), (90, 120, 200))
        from PIL import ImageDraw
        dr = ImageDraw.Draw(sheet)
        for n, (name, size, data, idx) in enumerate(result):
            h, w = idx.shape
            im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            px = im.load()
            for y in range(h):
                for x in range(w):
                    if idx[y, x] >= 0:
                        px[x, y] = tuple(int(v) for v in pal[idx[y, x]]) + (255,)
            big = im.resize((w * zoom, h * zoom), Image.NEAREST)
            gx, gy = (n % cols) * cell, (n // cols) * (cell + 14)
            sheet.paste(big, (gx + (cell - w * zoom) // 2, gy + (cell - h * zoom) // 2), big)
            dr.text((gx + 2, gy + cell), name, fill=(255, 255, 255))
        sheet.save(pv / 'icons-contact.png')
        print('preview', pv / 'icons-contact.png')


if __name__ == '__main__':
    main()
