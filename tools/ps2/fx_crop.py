"""OPT11-FX: zoomed crops of the same region of several pictures, side by side (PC reference | HW A | HW B ...), nearest neighbour.

usage: fx_crop.py OUT.png x,y,w,h SCALE pic1 [pic2 ...]   (png / ppm; a label is the file name)
"""
import sys

from PIL import Image, ImageDraw


def main():
    out, box, scale = sys.argv[1], [int(v) for v in sys.argv[2].split(',')], int(sys.argv[3])
    x, y, w, h = box
    tiles = []
    for p in sys.argv[4:]:
        im = Image.open(p).convert('RGB').crop((x, y, x + w, y + h)).resize((w * scale, h * scale), Image.NEAREST)
        tiles.append((p, im))
    top = 12
    canvas = Image.new('RGB', (sum(t[1].width for t in tiles) + 4 * (len(tiles) - 1), h * scale + top), (30, 30, 30))
    d = ImageDraw.Draw(canvas)
    cx = 0
    for p, im in tiles:
        canvas.paste(im, (cx, top))
        d.text((cx + 2, 0), p.split('/')[-2] if '/' in p else p, fill=(255, 255, 0))
        cx += im.width + 4
    canvas.save(out)
    print(out, canvas.size)


if __name__ == '__main__':
    main()
