"""Where two P6 PPM pictures of the same size differ (OPT10-S): count, bounding box, a coarse 8x8 block histogram and the first few pixels.

usage: python tools/ps2/ppm_diff.py a.ppm b.ppm
"""
import sys


def read_ppm(path):
    d = open(path, 'rb').read()
    toks = []
    i = 0
    while len(toks) < 4:
        while d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b'#':
            while d[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        toks.append(d[i:j])
        i = j
    i += 1
    w, h = int(toks[1]), int(toks[2])
    return w, h, d[i:i + w * h * 3]


w, h, a = read_ppm(sys.argv[1])
w2, h2, b = read_ppm(sys.argv[2])
assert (w, h) == (w2, h2), 'size differs'
diff = [(i // 3 % w, i // 3 // w) for i in range(0, w * h * 3, 3) if a[i:i + 3] != b[i:i + 3]]
print(f'{len(diff)} of {w * h} pixels differ')
if diff:
    xs = [p[0] for p in diff]
    ys = [p[1] for p in diff]
    print(f'bbox x {min(xs)}..{max(xs)} y {min(ys)}..{max(ys)}')
    blocks = {}
    for x, y in diff:
        blocks[(x // 16, y // 16)] = blocks.get((x // 16, y // 16), 0) + 1
    print('16x16 blocks (bx,by): count ->', sorted(blocks.items(), key=lambda kv: -kv[1])[:12])
    print('first pixels:', diff[:8])
