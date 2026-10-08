"""OPT11-FX2: compare the HWHASH lines (-hwhash: a hash of the GIF packets of every frame) of two runs: equal hashes of a frame = the same stream = the same picture, bit for bit.

usage: fx_hashcmp.py A/boot.txt B/boot.txt
"""
import re
import sys


def load(path):
    d = {}
    for l in open(path, errors='replace'):
        m = re.match(r'HWHASH f=(\d+) h=([0-9a-f]+)', l)
        if m:
            d[int(m.group(1))] = m.group(2)
    return d


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    common = sorted(set(a) & set(b))
    diff = [f for f in common if a[f] != b[f]]
    print('frames A %d, B %d, common %d, differing %d' % (len(a), len(b), len(common), len(diff)))
    if diff:
        print('first differing frames:', diff[:20])


if __name__ == '__main__':
    main()
