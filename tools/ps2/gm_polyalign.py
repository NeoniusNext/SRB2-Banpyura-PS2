#!/usr/bin/env python3
"""gm_polyalign.py RUN_A RUN_B [--runs DIR] [--show N]: -hwpolyhash streams of two runs aligned by game state (the view of the frame, v=), not by the frame number
(the frame counter differs by the number of frames the start-up console drew).  Per aligned frame: world hash (w=: walls/planes), whole hash (h=: with sprites)."""
import re
import sys
from pathlib import Path

args = sys.argv[1:]
runs = Path(__file__).resolve().parents[2] / 'build/runs'
show = 5
names = []
i = 0
while i < len(args):
    if args[i] == '--runs':
        runs = Path(args[i + 1]); i += 2
    elif args[i] == '--show':
        show = int(args[i + 1]); i += 2
    else:
        names.append(args[i]); i += 1


def load(n):
    out = []
    for line in (runs / n / 'boot.txt').read_text(errors='replace').splitlines():
        m = re.match(r'HWPH f=(\d+) n=(\d+) h=([0-9a-f]+)(?: o=([0-9a-f]+))?(?: w=(\d+):([0-9a-f]+))?(?: s=([0-9a-f]+) v=(\S+))?', line)
        if m and int(m.group(2)) > 0:
            out.append((int(m.group(1)), int(m.group(2)), m.group(3), m.group(5), m.group(6), m.group(7), m.group(8)))
    return out


a, b = load(names[0]), load(names[1])
# per view key a queue of frames, matched in order
from collections import defaultdict, deque
qb = defaultdict(deque)
for r in b:
    qb[r[6]].append(r)
matched = 0
wdiff = []
hdiff = []
unmatched = 0
for r in a:
    q = qb.get(r[6])
    if not q:
        unmatched += 1
        continue
    s = q.popleft()
    matched += 1
    if (r[3], r[4]) != (s[3], s[4]):
        wdiff.append((r, s))
    elif r[1] != s[1] or r[2] != s[2]:
        hdiff.append((r, s))
print('%s: %d drawn frames, %s: %d, aligned %d, unmatched in A %d; world (walls and planes) differ %d, sprites/whole differ %d' % (
    names[0], len(a), names[1], len(b), matched, unmatched, len(wdiff), len(hdiff)))
for r, s in wdiff[:show]:
    print('  world  f %d/%d  n %d/%d  w %s:%s / %s:%s' % (r[0], s[0], r[1], s[1], r[3], r[4], s[3], s[4]))
for r, s in hdiff[:show]:
    print('  whole  f %d/%d  n %d/%d  h %s / %s' % (r[0], s[0], r[1], s[1], r[2], s[2]))
