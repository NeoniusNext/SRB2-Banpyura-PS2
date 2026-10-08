"""PS2-340 / PS2-341 (OPT11 NETUI): the tables "screen -> icon overlaps 0/N" and "screen -> hints / duplicates 0/N" from the crawl tables of tools/ps2/menu_crawl.py.

usage: python3 tools/ps2/menu_crawl_icons.py [DIR] [--all]
DIR holds crawl-<label>.md (default build). Labels: sw-320x200 sw-320x256-pal sw-640x480 sw-640x512-pal hw-ntsc hw-pal. Without --all only the screens that have icons in their
text are listed (the others, with the totals, are in the "all" row).
The columns of the crawl table: # | menu | items | L | R | S | I | M | - | overlaps | icons in text | icon overlaps | hints | duplicates
"""
import re
import sys
from pathlib import Path

MODES = [('sw-320x200', 'SW 320x200'), ('sw-320x256-pal', 'SW 320x256 PAL'), ('sw-640x480', 'SW 640x480'), ('sw-640x512-pal', 'SW 640x512 PAL'), ('hw-ntsc', 'HW NTSC'), ('hw-pal', 'HW PAL')]


def load(d, label):
    rows = {}
    total = None
    f = d / ('crawl-%s.md' % label)
    if not f.exists():
        return rows, total
    for line in f.read_text().split('\n'):
        c = [x.strip().strip('*') for x in line.strip().strip('|').split('|')]
        if re.match(r'\| \d+ \|', line) and len(c) >= 14:
            rows[c[1]] = c
        elif line.startswith('| | **all**') and len(c) >= 14:
            total = c
    return rows, total


def num(s):
    return int(s.split('/')[0]) if s and s[0].isdigit() else 0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    d = Path(args[0]) if args else Path('build')
    show_all = '--all' in sys.argv
    data = {label: load(d, label) for label, _ in MODES}
    names = []
    for label, _ in MODES:
        for n in data[label][0]:
            if n not in names:
                names.append(n)
    print('Icon overlaps (icons that touch text, another icon or a line) / states checked; icons in the text of the states (SW 320x200); hints in the frame / frames with a duplicate')
    print()
    print('| screen | icons in text | ' + ' | '.join(t for _, t in MODES) + ' | hints | duplicates (all modes) |')
    print('|---|--:|' + '--:|' * len(MODES) + '--:|--:|')
    ref = data['sw-320x200'][0]
    for n in names:
        r = ref.get(n)
        if r is None:
            for label, _ in MODES:
                r = data[label][0].get(n)
                if r:
                    break
        if not show_all and not (r and num(r[10]) > 0):
            continue
        cells = []
        dups = []
        for label, _ in MODES:
            c = data[label][0].get(n)
            cells.append(c[11] if c else '')
            if c:
                dups.append(num(c[13]))
        print('| %s | %s | %s | %s | %s |' % (n, r[10] if r else '', ' | '.join(cells), r[12] if r else '', '%d/%s' % (sum(dups), r[2] if r else '?')))
    tot = []
    for label, _ in MODES:
        t = data[label][1]
        tot.append(t[11] if t else '')
    ts = data['sw-320x200'][1]
    dsum = sum(num(data[l][1][13]) for l, _ in MODES if data[l][1])
    print('| **all screens** | %s | %s | %s | %d dup frames |' % (ts[10] if ts else '', ' | '.join('**%s**' % x for x in tot), ts[12] if ts else '', dsum))
    print()
    print('Duplicates per mode: ' + ', '.join('%s %s' % (t, data[l][1][13] if data[l][1] else '-') for l, t in MODES))


if __name__ == '__main__':
    main()
