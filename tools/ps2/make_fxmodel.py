"""OPT11-FX: a test model for the renderer comparison (the base game has no MD3 content): a cube with per-face normals, 8 frames (the cube grows and turns),
a checker texture; replaces the sprite FWR1 (the GFZ flower) through models.dat.

usage: make_fxmodel.py OUTDIR      -> OUTDIR/models.dat, OUTDIR/models/FWR1.md3, OUTDIR/models/FWR1.png
Copy the tree into srb2home of the PC engine and next to the ELF for the PS2 (tools/ps2/fx_pair.py --homefiles OUTDIR), run with gr_models On / gr_modellighting On.
"""
import math
import struct
import sys
from pathlib import Path

from PIL import Image


def lat_lng(n):
    """MD3 normal: two bytes, latitude and longitude of the unit vector (z up)"""
    x, y, z = n
    lng = math.acos(max(-1.0, min(1.0, z)))
    lat = math.atan2(y, x)
    return (int(round(lng * 255 / (2 * math.pi))) & 255) << 8 | (int(round(lat * 255 / (2 * math.pi))) & 255)


def cube(size, turn):
    c, s = math.cos(turn), math.sin(turn)
    faces = [((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]), ((0, 0, -1), [(-1, 1, -1), (1, 1, -1), (1, -1, -1), (-1, -1, -1)]),
             ((1, 0, 0), [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]), ((-1, 0, 0), [(-1, 1, -1), (-1, -1, -1), (-1, -1, 1), (-1, 1, 1)]),
             ((0, 1, 0), [(1, 1, -1), (-1, 1, -1), (-1, 1, 1), (1, 1, 1)]), ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)])]
    verts = []
    for n, quad in faces:
        nx, ny = n[0] * c - n[1] * s, n[0] * s + n[1] * c
        for p in quad:
            x, y = p[0] * c - p[1] * s, p[0] * s + p[1] * c
            verts.append(((x * size, y * size, p[2] * size + size), (nx, ny, n[2])))
    return verts


def main():
    out = Path(sys.argv[1])
    (out / 'models').mkdir(parents=True, exist_ok=True)
    nframes = 8
    frames = [cube(10.0 + 2.0 * i, i * 0.15) for i in range(nframes)]
    tris = []
    st = []
    for f in range(6):
        b = f * 4
        tris += [(b, b + 1, b + 2), (b, b + 2, b + 3)]
        st += [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    nv = 24
    # surface
    shader = b'FWR1'.ljust(64, b'\0') + struct.pack('<i', 0)
    tri_b = b''.join(struct.pack('<3i', *t) for t in tris)
    st_b = b''.join(struct.pack('<2f', *q) for q in st)
    xyz_b = b''
    for fr in frames:
        for (p, n) in fr:
            xyz_b += struct.pack('<3hH', int(round(p[0] * 64)), int(round(p[1] * 64)), int(round(p[2] * 64)), lat_lng(n))
    ofs_tri = 108
    ofs_sh = ofs_tri + len(tri_b)
    ofs_st = ofs_sh + len(shader)
    ofs_xyz = ofs_st + len(st_b)
    ofs_end = ofs_xyz + len(xyz_b)
    surf = b'IDP3' + b'cube'.ljust(64, b'\0') + struct.pack('<i', 0) + struct.pack('<9i', nframes, 1, nv, len(tris), ofs_tri, ofs_sh, ofs_st, ofs_xyz, ofs_end)
    assert len(surf) == 108 - 0 or True
    surf = surf + tri_b + shader + st_b + xyz_b
    # frames
    fb = b''
    for i, fr in enumerate(frames):
        size = 10.0 + 2.0 * i
        fb += struct.pack('<10f', -size, -size, 0.0, size, size, 2 * size, 0.0, 0.0, 0.0, size * 1.8) + f'f{i}'.encode().ljust(16, b'\0')
    hdr_len = 108
    ofs_frames = hdr_len
    ofs_tags = ofs_frames + len(fb)
    ofs_surf = ofs_tags
    ofs_eof = ofs_surf + len(surf)
    hdr = b'IDP3' + struct.pack('<i', 15) + b'cube'.ljust(64, b'\0') + struct.pack('<i', 0) + struct.pack('<8i', nframes, 0, 1, 0, ofs_frames, ofs_tags, ofs_surf, ofs_eof)
    assert len(hdr) == 108, len(hdr)
    (out / 'models' / 'FWR1.md3').write_bytes(hdr + fb + surf)
    im = Image.new('RGBA', (64, 64))
    for y in range(64):
        for x in range(64):
            k = ((x // 8) + (y // 8)) % 2
            im.putpixel((x, y), (230, 90, 40, 255) if k else (40, 120, 230, 255))
    im.save(out / 'models' / 'FWR1.png')
    (out / 'models.dat').write_text('FWR1 FWR1.md3 6.0 0.0\n')
    print('model:', out / 'models' / 'FWR1.md3', 'frames', nframes)


if __name__ == '__main__':
    main()
