"""NETUI: crop window grabs / engine -vidshot pictures to the console picture and store them as JPEG under docs/GATES/g1/opt11-NETUI/.

usage: python3 tools/ps2/netui_pics.py OUTNAME=SRC [OUTNAME=SRC ...] [--out DIR] [--scale N]
 SRC is a PNG grab of the emulator window (build/runs/<run>/grabs/x.png: the picture is cropped out of the black window) or a PPM written by -vidshot.
 A window grab is cropped to the bounding box of the non-black part (the PCSX2 window is 800x600 with the 4:3 console picture inside).
"""
import argparse
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('items', nargs='+')
    ap.add_argument('--out', default=str(ROOT / 'docs/GATES/g1/opt11-NETUI'))
    ap.add_argument('--scale', type=float, default=0)
    ap.add_argument('--quality', type=int, default=80)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for it in a.items:
        name, _, src = it.partition('=')
        im = Image.open(src).convert('RGB')
        if str(src).endswith('.png') and im.size == (800, 600):
            # the PCSX2 window: crop the picture. A mostly black picture (a black net screen) is cropped by the known placement (0,25)-(641,505)
            im = im.crop((1, 25, 641, 505))
        if a.scale:
            im = im.resize((int(im.width * a.scale), int(im.height * a.scale)), Image.NEAREST)
        dst = out / (name if name.endswith('.jpg') else name + '.jpg')
        im.save(dst, quality=a.quality)
        print(dst, im.size)


if __name__ == '__main__':
    sys.exit(main())
