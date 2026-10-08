"""OPT11-MODEL: statistics of the MD3 models of the game (surfaces, frames, vertices, triangles, bytes in the engine's tinyframe form, texture size).

usage: md3_stats.py [MODELSDIR=/opt/srb2-assets-models] [--csv out.csv]
The engine (hw_md3load.c, tinyframe mode) keeps per frame 6 bytes (short x3) + 3 bytes (normal, char x3) per vertex, 2 bytes of index per triangle corner and 8 bytes of UV per vertex;
RAM of a loaded model = sum over surfaces of numVerts*(9*frames + 8) + 6*numTris (+ 3 Z_Malloc headers per frame; the loader also leaks one index array per frame).
"""
import argparse
import csv
import struct
import sys
from pathlib import Path

from PIL import Image


def parse(path):
    d = Path(path).read_bytes()
    ident, ver = struct.unpack_from('<4si', d, 0)
    if ident != b'IDP3':
        raise ValueError('not md3')
    name = d[8:72]
    flags, nframes, ntags, nsurf, nskins, ofs_frames, ofs_tags, ofs_surf, ofs_end = struct.unpack_from('<9i', d, 72)
    surfs = []
    p = ofs_surf
    for _ in range(nsurf):
        sid, sname = struct.unpack_from('<4s64s', d, p)
        sflags, sframes, sshaders, nverts, ntris, o_tri, o_sh, o_st, o_xyz, o_end = struct.unpack_from('<10i', d, p + 68)
        surfs.append(dict(name=sname.split(b'\0')[0].decode(), frames=sframes, verts=nverts, tris=ntris, shaders=sshaders,
                          o_tri=p + o_tri, o_st=p + o_st, o_xyz=p + o_xyz, o_sh=p + o_sh))
        p += o_end
    fn = [d[ofs_frames + i * 56 + 40: ofs_frames + i * 56 + 56].split(b'\0')[0].decode() for i in range(nframes)]
    return dict(data=d, frames=nframes, tags=ntags, surfs=surfs, framenames=fn, size=len(d))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', nargs='?', default='/opt/srb2-assets-models')
    ap.add_argument('--csv', default='')
    a = ap.parse_args()
    root = Path(a.dir) / 'models'
    rows = []
    for f in sorted(root.glob('*/*.md3')):
        m = parse(f)
        nv = sum(s['verts'] for s in m['surfs'])
        nt = sum(s['tris'] for s in m['surfs'])
        ram = sum(s['verts'] * (9 * s['frames'] + 8) + 6 * s['tris'] for s in m['surfs'])
        png = f.with_suffix('.png')
        tex = Image.open(png).size if png.exists() else None
        blend = f.with_name(f.stem + '_blend.png').exists()
        rows.append((f.relative_to(root).as_posix(), m['size'], len(m['surfs']), m['frames'], nv, nt, ram, tex, blend))
    rows.sort(key=lambda r: -r[1])
    print('%-24s %8s %4s %5s %6s %6s %8s %-10s %s' % ('model', 'bytes', 'surf', 'frm', 'verts', 'tris', 'ram', 'tex', 'blend'))
    for r in rows:
        print('%-24s %8d %4d %5d %6d %6d %8d %-10s %s' % (r[0], r[1], r[2], r[3], r[4], r[5], r[6], 'x'.join(map(str, r[7])) if r[7] else '-', 'Y' if r[8] else '-'))
    print('total models %d, bytes %d, ram %d, max ram %d' % (len(rows), sum(r[1] for r in rows), sum(r[6] for r in rows), max(r[6] for r in rows)))
    if a.csv:
        with open(a.csv, 'w', newline='') as fh:
            csv.writer(fh).writerows(rows)


if __name__ == '__main__':
    main()
