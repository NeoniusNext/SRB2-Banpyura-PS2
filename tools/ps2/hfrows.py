"""OPT10-HF: which colormap row turns a palette colour into another (palette rendering analysis).

usage: hfrows.py R G B  [R G B ...]     print, for the palette index of each colour, the colour of every colormap row 0..31 of the default COLORMAP
Reads PLAYPAL and COLORMAP from /opt/srb2-assets/srb2.pk3.
"""
import sys
import zipfile

z = zipfile.ZipFile('/opt/srb2-assets/srb2.pk3')
pal = z.read('PLAYPAL')[:768]
cm = z.read('COLORMAP')
col = [(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]) for i in range(256)]
args = [int(a, 16) if len(a) <= 2 else int(a) for a in sys.argv[1:]]
for k in range(0, len(args), 3):
    c = tuple(args[k:k + 3])
    idx = [i for i in range(256) if col[i] == c]
    print('colour', c, 'indices', idx)
    for i in idx[:1]:
        print('  rows:', ' '.join('%02x%02x%02x' % col[cm[r * 256 + i]] for r in range(32)))
