"""Pad-script helper for menu/console tests (OPT10-X): python3 tools/ps2/padseq.py OUT.txt 250:start 330:down*3 400:cross ...
Writes the text of "-padscript file:OUT.txt" (src/ps2/i_joy.c: "poll:port:+btn" / "-btn" / "lx=N"; a poll is one displayed frame). Items are
FRAME:BUTTON[*COUNT[/STEP]][~HOLD]: BUTTON pressed at FRAME for HOLD polls (default 6); with *COUNT it is repeated COUNT times STEP polls apart (default 30).
Buttons act as the PS2 port maps them: start = open the menu / Enter on the title, cross = Enter, circle = Escape, triangle = the on-screen keyboard,
d-pad = up/down/left/right. Importable: padseq.script(items)."""
import re
import sys


def script(items, port=1):
    out = []
    for it in items:
        m = re.match(r'^(\d+):(\w+)(?:\*(\d+)(?:/(\d+))?)?(?:~(\d+))?$', it)
        if not m:
            raise SystemExit('bad pad item ' + it)
        frame, btn = int(m.group(1)), m.group(2)
        count = int(m.group(3) or 1)
        step = int(m.group(4) or 30)
        hold = int(m.group(5) or 6)
        for k in range(count):
            f = frame + k * step
            out += [(f, f'{f}:{port}:+{btn}'), (f + hold, f'{f + hold}:{port}:-{btn}')]
    return ','.join(s for _, s in sorted(out, key=lambda x: (x[0], x[1])))


if __name__ == '__main__':
    open(sys.argv[1], 'w').write(script(sys.argv[2:]))
