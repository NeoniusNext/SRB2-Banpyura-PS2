"""PS2-339 (OPT11 NETUI): the totals of the crawl tables of tools/ps2/menu_crawl.py (build/crawl-<label>.md) as one Markdown table, and the menus that hide a group.

usage: python3 tools/ps2/menu_crawl_summary.py [DIR] [--ref LABEL]
DIR holds crawl-<label>.md (default build). Labels: sw-320x200 sw-320x256-pal sw-640x480 sw-640x512-pal hw-ntsc hw-pal.
"""
import re
import sys
from pathlib import Path

MODES = [('sw-320x200', 'Software 320x200 (NTSC)'), ('sw-320x256-pal', 'Software 320x256 (PAL)'), ('sw-640x480', 'Software 640x480'),
         ('sw-640x512-pal', 'Software 640x512 (PAL)'), ('hw-ntsc', 'Hardware (NTSC)'), ('hw-pal', 'Hardware (PAL)')]


def main():
    d = Path(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else Path('build')
    print('| Режим | состояний (меню x пункт) | групп в углу одной строкой | на ряд выше | стопкой | только иконки | сдвинуты | не нарисованы (места нет) | перекрытий |')
    print('|---|--:|--:|--:|--:|--:|--:|--:|--:|')
    for k, t in MODES:
        f = d / ('crawl-%s.md' % k)
        if not f.exists():
            continue
        m = re.search(r'\| \| \*\*all\*\* \| (\d+) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \| \*\*(\d+)/(\d+)\*\* \|', f.read_text())
        g = m.groups()
        ov = ('%s/%s' % (g[7], g[8])) if k.startswith('sw') else 'пикселей нет (раскладка та же)'
        print('| %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (t, g[0], g[1], g[2], g[3], g[4], g[5], g[6], ov))
    print()
    for k in ('sw-320x200', 'sw-640x480'):
        f = d / ('crawl-%s.md' % k)
        if not f.exists():
            continue
        hid = []
        for line in f.read_text().split('\n'):
            if re.match(r'\| \d+ \|', line):
                c = [x.strip() for x in line.strip().strip('|').split('|')]
                if int(c[8]) > 0:
                    hid.append('%s (%s)' % (c[1], c[8]))
        print('%s, группы не нарисованы (число групп): %s' % (k, ', '.join(hid)))


if __name__ == '__main__':
    main()
