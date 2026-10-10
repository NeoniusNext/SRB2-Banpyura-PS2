"""OPT14-CHAT: where does the chat keyboard stand in the pictures of the video-mode scenarios (net_specs14.py chat-modes-srv[-hw])?

usage: python3 tools/ps2/chat_osk_check.py RUNDIR [first_frame step]     (RUNDIR = build/opt14-chat/run/chat-modes-srv/srv)
For every picture vidshot-WxH-fN.ppm with N = first + k*step (900 + 300 k by default: the shot of each block with the keyboard up) the rows that are dark over the
width of the panel (the panel is 252 base px wide, dark: the black fill of palette 31) give its top and bottom; printed in base pixels (a base pixel = `dup` real pixels):
the top above the bottom edge of the picture (94 expected: 105..194 of a 200 px picture, the panel is 90 px high) and the air under the dark part (6 expected: the 1 px border
and 5 px of safe area). The keyboard is pinned to the bottom edge (V_SNAPTOBOTTOM), so both numbers must be the same in every video mode.
"""
import glob
import re
import sys
from pathlib import Path

from PIL import Image


def dark_rows(path, th=48):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    dup = max(1, min(w // 320, h // 200))
    px = im.load()
    rows = []
    for y in range(h):
        run = best = 0
        for x in range(w):
            r, g, b = px[x, y]
            if r < th and g < th and b < th:
                run += 1
                best = max(best, run)
            else:
                run = 0
        if best >= 230 * dup:
            rows.append(y)
    return w, h, dup, rows


def main():
    run = Path(sys.argv[1])
    first = int(sys.argv[2]) if len(sys.argv) > 2 else 900
    step = int(sys.argv[3]) if len(sys.argv) > 3 else 300
    bad = 0
    for f in sorted(glob.glob(str(run / 'vidshot-*-f[0-9]*.ppm'))):
        m = re.search(r'vidshot-(\d+)x(\d+)-f(\d+)', f)
        n = int(m.group(3))
        if n < first or (n - first) % step:
            continue
        w, h, dup, rows = dark_rows(f)
        if not rows:
            print(f'{w}x{h} f{n}: no panel found')
            bad += 1
            continue
        top, bot = min(rows), max(rows)
        above, air = (h - top) / dup, (h - 1 - bot) / dup
        ok = abs(above - 94) <= 1 and abs(air - 6) <= 1
        bad += not ok
        print(f'{w}x{h} dup={dup} f{n}: panel top {above:.0f} base px above the bottom edge, air under the dark part {air:.1f}  {"ok" if ok else "OFF"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
