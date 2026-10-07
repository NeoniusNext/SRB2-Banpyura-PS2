"""OPT10-HF: compares the HWPROF windows of two runs (e.g. palette lighting on / off): mean EE cycles per frame of every field.

usage: hf_perfcmp.py runA/boot.txt runB/boot.txt [first_window]
The first windows (map load, texture warm-up) are skipped: default first_window = 1.
"""
import re
import sys


def load(path, first):
    acc = {}
    n = 0
    for line in open(path, errors='replace'):
        m = re.match(r'HWPROF win=(\d+) frames=(\d+) (.*)', line)
        if not m or int(m.group(1)) < first:
            continue
        frames = int(m.group(2))
        n += frames
        for k, v in re.findall(r'(\w+)=(\d+)', m.group(3)):
            acc[k] = acc.get(k, 0) + int(v)
        acc.setdefault('_frames', 0)
        acc['_frames'] += frames
    return acc, n


def main():
    a, b = sys.argv[1], sys.argv[2]
    first = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    ra, na = load(a, first)
    rb, nb = load(b, first)
    print('frames A %d B %d' % (na, nb))
    print('%-10s %12s %12s %8s' % ('field', 'A /frame', 'B /frame', 'B/A'))
    for k in ['wall', 'clear', 'bsp', 'batch', 'sprites', 'nodes', 'draw', 'tex', 'wait', 'polys', 'vout', 'qw', 'state', 'passes', 'bands', 'uploads']:
        if k in ra and k in rb and na and nb:
            va, vb = ra[k] / na, rb[k] / nb
            print('%-10s %12.0f %12.0f %8.3f' % (k, va, vb, vb / va if va else 0))


if __name__ == '__main__':
    main()
