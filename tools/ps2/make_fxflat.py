"""OPT11-FX: a PWAD that replaces flats with a high contrast grid (to see the water ripple, seams and texel shifts on both renderers).

usage: make_fxflat.py OUT.wad [NAME:SIZE ...]    default DSWATER1:256 DSWATER2:256
Every flat: palette index 0 (white) on index 31 (dark) background, 2 texel wide lines every 32 texels (a mark in each cell corner so that the
direction of a shift can be seen), a diagonal of index 112 across the repeat (the wrap seam is visible).
"""
import struct
import sys


def flat(size):
    b = bytearray([31] * (size * size))
    for y in range(size):
        for x in range(size):
            if x % 32 < 2 or y % 32 < 2:
                b[y * size + x] = 0
            elif (x % 32 < 8 and y % 32 < 8):
                b[y * size + x] = 112
            elif abs(x - y) < 2 or abs((size - 1 - x) - y) < 1:
                b[y * size + x] = 176
    return bytes(b)


def main():
    out = sys.argv[1]
    specs = sys.argv[2:] or ['DSWATER1:256', 'DSWATER2:256']
    lumps = [(b'F_START', b'')]
    for s in specs:
        n, sz = s.split(':')
        lumps.append((n.encode().ljust(8, b'\0'), flat(int(sz))))
    lumps.append((b'F_END', b''))
    data = b''
    ents = []
    off = 12
    for n, d in lumps:
        ents.append((off, len(d), n))
        data += d
        off += len(d)
    hdr = b'PWAD' + struct.pack('<II', len(lumps), off)
    dirb = b''.join(struct.pack('<II8s', o, s, n.ljust(8, b'\0')) for o, s, n in ents)
    open(out, 'wb').write(hdr + data + dirb)
    print(out, len(hdr + data + dirb))


if __name__ == '__main__':
    main()
