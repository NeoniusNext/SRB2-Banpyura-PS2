"""OPT11-MODEL: cook the MD3 models of the game into MODELS.PAK (SRP2 pack, docs/PACK_FORMAT.md) for the PS2 engine.

usage: cook_models.py [--models DIR] [--srb2pk3 FILE] [--out build/pak/MODELS.PAK] [--verify]
  --models   directory with models/ (PLAY, OBJE, BOSS, ENMY ...) and models.dat   (default /opt/srb2-assets-models)
  --srb2pk3  srb2.pk3 (the first palette of its PLAYPAL is the palette the 8 bit textures are made for)   (default srb2-assets/srb2.pk3)

What the pack holds (lump full names are the paths models.dat uses, so the engine looks them up by the same strings):
  models.dat                  verbatim
  <PATH>.md3                  "SRMD" cooked model (layout below): the arrays of the engine's tinyframe form, ready to be used in place
  PLAYPAL                     the 768 bytes of palette 0 the textures were made for (the engine remaps them when its texture palette is another)
  <PATH>.png                  "SRMT" cooked texture: 8 bit palette indices (palette 0 of PLAYPAL), index 255 = hole (alpha < 128): 16 byte header + w * h bytes
  <PATH>_blend.png            "SRMB" cooked blend map (skin colour mask): sparse runs of (alpha, brightness, average)
The PS2 engine has no use for 32 bit model textures: a GS texture of 256 x 256 CT32 is 256 KB of VRAM (15 models overflow the 3.2 MB pool and the sky and
flats are rebuilt every frame), the same texture as 8 bit indices + a CLUT is 64 KB and is lit by the colormap rows exactly like walls and sprites.
The indices are what the OpenGL palette rendering shader makes of the texel (gr_paletterendering On): cell = round(v * 63 / 255) per channel, colour (4 * cell),
nearest palette colour (NearestPaletteColor of r_data.c, first of equals), see PC_cell() / nearest() below.

SRMD layout (little endian, every section 4 byte aligned; the loader uses the arrays in place):
  header 32 B : "SRMD", u32 version = 1, u32 total size, u16 numSurfaces, u16 numFrames (max over surfaces), u32 frameNamesOff, u32 surfaceTableOff, u32 flags, u32 0
  frame names : numFrames x 16 B (NUL terminated)
  surface table : numSurfaces x 48 B { u32 numVerts, numTris, numFrames, uvOff, idxOff, posOff, nrmOff, 5 x reserved }
  per surface : float uv[2 * numVerts]; u16 idx[3 * numTris] (padded to 4); s16 pos[numFrames][numVerts][3] = (x, z, 1 - y) as hw_md3load.c stores them
                (the engine draws them scaled by 1/64); s8 nrm[numFrames][numVerts] = (char)(n.z * 127), the up component of the normal
                (the only one the model lighting equation reads: dot(gl_Normal, (0, 1, 0)) with the engine's (x, z, y) normal order).
"""
import argparse
import ctypes
import hashlib
import math
import struct
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cook  # noqa: E402  (the pack constants and the LZ4 lump writer)

ROOT = Path(__file__).resolve().parents[2]
SRMD_VERSION = 1


# ---- palette and the OpenGL palette rendering quantizer ---------------------------------------------------------------------

def load_palette(pk3):
    with zipfile.ZipFile(pk3) as z:
        raw = z.read('PLAYPAL')
    return np.frombuffer(raw[:768], dtype=np.uint8).reshape(256, 3).astype(np.int32)


def PC_cell(v):
    """cell of the 64^3 lookup table a texel channel (0..255) falls in: floor(v / 255 * 63 + 0.5)  (GLSL: (texel * 63 + 0.5) / 64, NEAREST)"""
    return (v.astype(np.int64) * 126 + 255) // 510


def nearest(pal, rgb, exclude255=True):
    """index of the nearest palette colour for every row of rgb (N x 3), first of equals; index 255 (the hole key) is never chosen when exclude255"""
    n = 255 if exclude255 else 256
    d = ((rgb[:, None, :].astype(np.int64) - pal[None, :n, :]) ** 2).sum(axis=2)
    return d.argmin(axis=1).astype(np.uint8)


def quantize_rgba(pal, rgba):
    """RGBA (h x w x 4 uint8) -> (h x w uint8 indices: 255 = hole, nearest by the PC lookup, count of partial alpha texels)"""
    h, w, _ = rgba.shape
    flat = rgba.reshape(-1, 4)
    cell = PC_cell(flat[:, :3])
    key = (cell[:, 0] << 12) | (cell[:, 1] << 6) | cell[:, 2]
    uk, inv = np.unique(key, return_inverse=True)
    rgb = np.stack([(uk >> 12) & 63, (uk >> 6) & 63, uk & 63], axis=1) * 4
    idx = nearest(pal, rgb)[inv]
    alpha = flat[:, 3]
    out = np.where(alpha >= 128, idx, 255).astype(np.uint8)
    partial = int(((alpha > 0) & (alpha < 255)).sum())
    return out.reshape(h, w), partial


# ---- MD3 -> SRMD ------------------------------------------------------------------------------------------------------------

_libm = ctypes.CDLL('libm.so.6')
_libm.cosf.restype = ctypes.c_float
_libm.cosf.argtypes = [ctypes.c_float]


def up_normal_byte(n):
    """(char)(tempNormal[2] * 127) of hw_md3load.c: LatLngToNormal (float): lng = (n & 255) * (PI / 128); z = cosf(lng)"""
    lng = (n & 255)
    f32 = np.float32
    ang = f32(lng) * f32(f32(3.1415926535897932384626433832795) / f32(128.0))
    z = f32(_libm.cosf(float(ang)))
    v = f32(z * f32(127.0))
    return int(np.trunc(v))


_NRM_TABLE = [up_normal_byte(n) for n in range(256)]


def cook_md3(path):
    d = Path(path).read_bytes()
    ident, ver = struct.unpack_from('<4si', d, 0)
    if ident != b'IDP3' or ver != 15:
        raise ValueError(f'{path}: not an MD3 version 15')
    flags, nframes, ntags, nsurf, nskins, ofs_frames, ofs_tags, ofs_surf, ofs_end = struct.unpack_from('<9i', d, 72)
    names = []
    for i in range(nframes):
        nm = d[ofs_frames + i * 56 + 40: ofs_frames + i * 56 + 56]
        nm = nm.split(b'\0', 1)[0][:15]
        names.append(nm.ljust(16, b'\0'))
    surfs = []
    p = ofs_surf
    for _ in range(nsurf):
        sflags, sframes, sshaders, nverts, ntris, o_tri, o_sh, o_st, o_xyz, o_end = struct.unpack_from('<10i', d, p + 68)
        tri = np.frombuffer(d, dtype='<i4', count=ntris * 3, offset=p + o_tri).reshape(-1, 3)
        if nverts > 65535 or (tri.size and (tri.min() < 0 or tri.max() >= nverts)):
            raise ValueError(f'{path}: surface vertex indices out of range')
        st = np.frombuffer(d, dtype='<f4', count=nverts * 2, offset=p + o_st)
        xyz = np.frombuffer(d, dtype='<i2', count=sframes * nverts * 4, offset=p + o_xyz).reshape(sframes, nverts, 4)
        # engine order: x, z, then "1.0f - y" converted to short
        pos = np.empty((sframes, nverts, 3), dtype='<i2')
        pos[..., 0] = xyz[..., 0]
        pos[..., 1] = xyz[..., 2]
        pos[..., 2] = (1 - xyz[..., 1].astype(np.int32)).astype(np.int32).astype('<i2')
        nrm = np.array(_NRM_TABLE, dtype=np.int8)[(xyz[..., 3].astype(np.uint16) & 255)]
        surfs.append(dict(nv=nverts, nt=ntris, nf=sframes, uv=st.astype('<f4'), idx=tri.astype('<u2'), pos=pos, nrm=nrm))
        p += o_end
    # layout
    hdr_size = 32
    off = hdr_size
    names_off = off
    off += nframes * 16
    surf_tab = off
    off += len(surfs) * 48
    tab = []
    body = []
    for s in surfs:
        o_uv = off
        off += s['nv'] * 8
        o_idx = off
        sz = (s['nt'] * 6 + 3) & ~3
        off += sz
        o_pos = off
        off += s['nf'] * s['nv'] * 6
        off = (off + 3) & ~3
        o_nrm = off
        off += s['nf'] * s['nv']
        off = (off + 3) & ~3
        tab.append(struct.pack('<12I', s['nv'], s['nt'], s['nf'], o_uv, o_idx, o_pos, o_nrm, 0, 0, 0, 0, 0))
        body.append((o_uv, s['uv'].tobytes()))
        body.append((o_idx, s['idx'].tobytes()))
        body.append((o_pos, s['pos'].tobytes()))
        body.append((o_nrm, s['nrm'].tobytes()))
    total = off
    buf = bytearray(total)
    buf[0:hdr_size] = struct.pack('<4sIIHHIIII', b'SRMD', SRMD_VERSION, total, len(surfs), nframes, names_off, surf_tab, 1, 0)
    buf[names_off:names_off + nframes * 16] = b''.join(names)
    buf[surf_tab:surf_tab + len(surfs) * 48] = b''.join(tab)
    for o, b in body:
        buf[o:o + len(b)] = b
    return bytes(buf), dict(frames=nframes, surfaces=len(surfs), verts=sum(s['nv'] for s in surfs), tris=sum(s['nt'] for s in surfs))


# ---- PNG -> SRMT / SRMB -----------------------------------------------------------------------------------------------------

def cook_texture(png, pal):
    im = Image.open(png)
    rgba = np.asarray(im.convert('RGBA'))
    idx, partial = quantize_rgba(pal, rgba)
    h, w = idx.shape
    holes = int((idx == 255).sum())
    hdr = struct.pack('<4sIHHI', b'SRMT', 1, w, h, 1 if holes else 0)  # 16 bytes: magic, version, w, h, flags (bit 0: some texel is a hole)
    return hdr + idx.tobytes(), dict(w=w, h=h, holes=holes, partial=partial)


def cook_blend(png):
    """sparse runs: header "SRMB", u32 version, u16 w, u16 h, u32 nruns, u32 npixels; runs {u32 pos, u16 len, u16 0} then npixels x (alpha, lum, avg, 0)"""
    im = Image.open(png)
    a = np.asarray(im.convert('RGBA')).astype(np.int64)
    h, w, _ = a.shape
    al = a[..., 3].reshape(-1)
    r, g, b = a[..., 0].reshape(-1), a[..., 1].reshape(-1), a[..., 2].reshape(-1)
    # SETBRIGHTNESS of hw_md2.c, integer arithmetic
    lum = ((1063 * r) // 5000 + (3576 * g) // 5000 + (361 * b) // 5000).astype(np.int64)
    avg = (r + g + b) // 3
    nz = np.nonzero(al)[0]
    runs = []
    if nz.size:
        start = nz[0]
        prev = nz[0]
        for v in nz[1:]:
            if v != prev + 1:
                runs.append((int(start), int(prev - start + 1)))
                start = v
            prev = v
        runs.append((int(start), int(prev - start + 1)))
    px = np.stack([al[nz], lum[nz], avg[nz], np.zeros_like(nz)], axis=1).astype(np.uint8)
    out = bytearray(struct.pack('<4sIHHII', b'SRMB', 1, w, h, len(runs), int(nz.size)))
    for pos, ln in runs:
        out += struct.pack('<IHH', pos, ln, 0)
    out += px.tobytes()
    return bytes(out), dict(w=w, h=h, runs=len(runs), px=int(nz.size))


# ---- the pack ---------------------------------------------------------------------------------------------------------------

def write_pack(entries, dst):
    """entries: [(full name str, bytes)]. Same layout rules as cook.py (sector aligned index, string pool, payload alignment, LZ4 per 64 KiB)"""
    names = [e[0].encode('ascii') for e in entries]
    results = []
    for full, data in entries:
        if len(data) < cook.MIN_PACK_SIZE:
            results.append((cook.CM_RAW, data, len(data)))
            continue
        packed = cook.lz4_lump(data)
        if len(packed) <= len(data) * cook.MAX_RATIO:
            results.append((cook.CM_LZ4, packed, len(data)))
        else:
            results.append((cook.CM_RAW, data, len(data)))
    pool = bytearray()
    seen = {}
    offs = []
    for full in names:
        _, _, long_ = cook.engine_names(full)
        if full in seen:
            fo = seen[full]
        else:
            fo = seen[full] = len(pool)
            pool += full + b'\0'
        trim = full[full.rfind(b'/') + 1:]
        if long_ == trim:
            lo = fo + len(full) - len(trim)
        elif long_ in seen:
            lo = seen[long_]
        else:
            lo = seen[long_] = len(pool)
            pool += long_ + b'\0'
        offs.append((fo, lo))
    n = len(entries)
    table_off = cook.SECTOR
    pool_off = cook.align(table_off + cook.ENTRY.size * n, cook.SECTOR)
    data_off = cook.align(pool_off + len(pool), cook.SECTOR)
    pos = data_off
    ents, layout = [], []
    for (codec, payload, size), (fo, lo) in zip(results, offs):
        if not payload:
            ents.append(cook.ENTRY.pack(0, 0, 0, fo, lo, cook.CM_RAW))
            layout.append(None)
            continue
        pos = cook.align(pos, cook.SECTOR if size >= cook.BLOCK else cook.ALIGN_SMALL)
        ents.append(cook.ENTRY.pack(pos, len(payload), size, fo, lo, codec))
        layout.append(pos)
        pos += len(payload)
    file_size = cook.align(pos, cook.SECTOR)
    with open(dst, 'wb') as f:
        f.write(cook.HEADER.pack(cook.MAGIC, cook.VERSION, cook.HEADER.size, cook.FLAG_NONMUSIC, n, table_off, pool_off, len(pool), data_off, file_size, cook.BLOCK, 0))
        f.seek(table_off)
        f.write(b''.join(ents))
        f.seek(pool_off)
        f.write(pool)
        for (codec, payload, size), p in zip(results, layout):
            if p is not None:
                f.seek(p)
                f.write(payload)
        f.truncate(file_size)
    return file_size, results


def read_pack(path):
    """independent reader of a pack made by write_pack: {name: bytes}"""
    import lz4.block
    d = Path(path).read_bytes()
    (magic, ver, hs, flags, n, toff, poff, psz, doff, fsz, blk) = struct.unpack_from('<4s10I', d, 0)
    assert magic == b'SRP2' and ver == 1 and fsz == len(d)
    out = {}
    for i in range(n):
        pos, dsz, size, fo, lo, codec = struct.unpack_from('<6I', d, toff + i * 24)
        name = d[poff + fo: d.index(b'\0', poff + fo)].decode()
        raw = d[pos:pos + dsz]
        if codec == 1:
            if size <= cook.BLOCK:
                raw = lz4.block.decompress(raw, uncompressed_size=size)
            else:
                nb = 1 + (size - 1) // cook.BLOCK
                idx = struct.unpack_from(f'<{nb}I', raw, 0)
                q = nb * 4
                parts = []
                for b, e in enumerate(idx):
                    cs = e & 0x7FFFFFFF
                    bs = min(cook.BLOCK, size - b * cook.BLOCK)
                    parts.append(raw[q:q + cs] if e & 0x80000000 else lz4.block.decompress(raw[q:q + cs], uncompressed_size=bs))
                    q += cs
                raw = b''.join(parts)
        assert len(raw) == size, (name, len(raw), size)
        out[name] = raw
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--models', default='/opt/srb2-assets-models')
    ap.add_argument('--srb2pk3', default=str(ROOT / 'srb2-assets/srb2.pk3'))
    ap.add_argument('--out', default=str(ROOT / 'build/pak/MODELS.PAK'))
    ap.add_argument('--verify', action='store_true')
    a = ap.parse_args()
    mdir = Path(a.models)
    pal = load_palette(a.srb2pk3)
    dat = (mdir / 'models.dat').read_bytes()
    entries = [('models.dat', dat), ('PLAYPAL', pal.astype(np.uint8).tobytes())]
    wanted = []
    for line in dat.decode('ascii', 'replace').splitlines():
        t = line.split()
        if len(t) == 4:
            wanted.append(t[1])
    seen = set()
    stats = dict(models=0, tex=0, blend=0, miss=[], bytes_md3=0, bytes_cooked=0)
    for rel in wanted:
        if rel in seen:
            continue
        seen.add(rel)
        src = mdir / 'models' / rel
        if not src.exists():
            stats['miss'].append(rel)
            continue
        blob, info = cook_md3(src)
        entries.append((rel, blob))
        stats['models'] += 1
        stats['bytes_md3'] += src.stat().st_size
        stats['bytes_cooked'] += len(blob)
        stem = rel[:-4]
        png = mdir / 'models' / (stem + '.png')
        if png.exists():
            tex, ti = cook_texture(png, pal)
            entries.append((stem + '.png', tex))
            stats['tex'] += 1
        blend = mdir / 'models' / (stem + '_blend.png')
        if blend.exists():
            bl, bi = cook_blend(blend)
            entries.append((stem + '_blend.png', bl))
            stats['blend'] += 1
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    size, results = write_pack(entries, a.out)
    raw = sum(len(e[1]) for e in entries)
    print(f"MODELS.PAK: {len(entries)} lumps, {raw:,} B -> {size:,} B on disk; models {stats['models']} (md3 {stats['bytes_md3']:,} -> cooked {stats['bytes_cooked']:,}), "
          f"textures {stats['tex']}, blend maps {stats['blend']}, missing md3: {stats['miss']}")
    if a.verify:
        back = read_pack(a.out)
        assert len(back) == len({e[0] for e in entries})
        for full, data in entries:
            assert back[full] == data, full
        print('verify: every lump reads back identical')
    return 0


if __name__ == '__main__':
    sys.exit(main())
