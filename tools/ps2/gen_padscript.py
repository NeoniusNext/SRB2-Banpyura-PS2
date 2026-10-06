"""Pad script generator for the network / splitscreen tests (-padscript, src/ps2/i_joy.c).

usage: python tools/ps2/gen_padscript.py PORT START END [--seed N] > script.txt
Walks the player forward with a repeating pattern of turns and jumps between poll START and END (poll = displayed frame): the stick is held
forward (ly=0) all the time, lx alternates between straight, right and left, Cross (jump) is pressed for 6 polls every ~50.
"""
import argparse
import random

ap = argparse.ArgumentParser()
ap.add_argument('port', type=int)
ap.add_argument('start', type=int)
ap.add_argument('end', type=int)
ap.add_argument('--seed', type=int, default=1)
a = ap.parse_args()
r = random.Random(a.seed * 7919 + a.port)
out = [f'{a.start}:{a.port}:ly=0']
t = a.start
nextjump = a.start + 20
while t < a.end:
    lx = r.choice((128, 128, 255, 0, 200, 60))
    out.append(f'{t}:{a.port}:lx={lx}')
    t2 = t + r.randint(25, 60)
    while nextjump < t2:
        out.append(f'{nextjump}:{a.port}:+cross')
        out.append(f'{nextjump + 6}:{a.port}:-cross')
        nextjump += r.randint(40, 70)
    t = t2
out.append(f'{a.end}:{a.port}:lx=128')
out.append(f'{a.end}:{a.port}:ly=128')
print(','.join(sorted(out, key=lambda s: (int(s.split(':')[0]), s))))
