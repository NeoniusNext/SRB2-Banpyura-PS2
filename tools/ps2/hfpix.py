"""OPT10-HF: print RGB values of a small window of the PC-OpenGL and PS2-HW pictures side by side (pixel-level look at an artifact).

usage: hfpix.py --pc a.png --hw b.ppm --box x0,y0,w,h
"""
import argparse

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pc', required=True)
    ap.add_argument('--hw', required=True)
    ap.add_argument('--box', required=True)
    a = ap.parse_args()
    x0, y0, w, h = [int(v) for v in a.box.split(',')]
    pc = Image.open(a.pc).convert('RGB')
    hw = Image.open(a.hw).convert('RGB')
    for y in range(y0, y0 + h):
        print('y=%3d PC: ' % y + ' '.join('%02x%02x%02x' % pc.getpixel((x, y)) for x in range(x0, x0 + w)))
        print('      HW: ' + ' '.join('%02x%02x%02x' % hw.getpixel((x, y)) for x in range(x0, x0 + w)))


if __name__ == '__main__':
    main()
