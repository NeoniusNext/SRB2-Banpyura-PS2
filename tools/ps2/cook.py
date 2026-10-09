"""Cook the SRB2 pk3 archives into SRP2 packs (format: docs/PACK_FORMAT.md).

usage: cook.py [--src DIR] [--out DIR] [--jobs N] [--only NAME ...] [--tool-dir DIR] [--keep-png] [--from-pak DIR] [--order FILE] [--version 1|2] [--no-dedup]
       cook.py --texc DUMP [--out DIR]      (OPT13 IZ, PS2-602: TEXC.PAK, the composite textures prebuilt by the host engine; docs/PACK_FORMAT.md)
  --src       directory with srb2.pk3 zones.pk3 characters.pk3 music.pk3 (default srb2-assets)
  --out       output directory (default build/pak): SRB2.PAK ZONES.PAK CHARS.PAK MUSIC.PAK (+ <PACK>.pics.json)
  --tool-dir  where the host picture tool is built (default build/strip-pic-tool)
  --keep-png  old behaviour: PNG lumps stay PNG (the profile engine has no decoder for them: only for comparison)
  --from-pak  PS2-LOAD-10: re-cook from the lumps of existing packs (SRP2 v1 or v2) instead of the pk3 files: the lump contents (incl. the cooked pictures,
              the <PACK>.pics.json sidecars are copied) and their numbering stay exactly as they are; used where the PNG tool (MSVC + libpng) is not available
  --order     PS2-LOAD-11: text file, one lump full name per line (first use order of a boot, tools/ps2/lump_order.py): these lumps are stored first, in this order,
              so that the start-up reads them as a few long sequential runs; all other lumps follow in directory order. Lump NUMBERS never change.
  --version   2 (default): head table, per-lump CRC32, index checksums, identical lumps stored once; 1: the old layout (comparison)

One pack per pk3, same entry order as the zip central directory (this is the order the engine's
ResGetLumpsZip walks, so wadnum / lumpnum / folder logic stay as they were). Lump data is stored raw or as
LZ4HC (64 KiB blocks for lumps > 64 KiB). Needs `pip install lz4`.

PNG lumps (15 in srb2.pk3, PS2-20): the PS2 engine has no libpng/zlib, so every PNG is converted here by the ORIGINAL
engine decoder (src/r_picformats.c with libpng, built with MSVC by tools/ps2/strip_pics.py, needs the vcpkg libpng) into a
"cooked picture" lump (marker + Doom patch with the exact pixels/transparency/offsets of Picture_PNGConvert; see
strip_pics.py). Conversion order = pk3 order (the engine's nearest-colour memo is order dependent; measured: not for
these 15). The converted entries are listed with the hashes of both forms in <PACK>.pics.json, which verify_pack.py,
test_pack_reader.py and strip_pics_test.py use. ZIP/zlib are used here on the host only.
Verify with tools/ps2/verify_pack.py.
"""
import argparse
import hashlib
import os
import struct
import sys
import time
import zipfile
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import json
import shutil
import zlib

import lz4.block

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import strip_pics  # noqa: E402  (PNG -> cooked picture, PS2-20)

PACKS = [('srb2.pk3', 'SRB2.PAK'), ('zones.pk3', 'ZONES.PAK'), ('characters.pk3', 'CHARS.PAK'), ('music.pk3', 'MUSIC.PAK')]

MAGIC = b'SRP2'
VERSION = 2
BLOCK = 65536          # decoded block size of big lumps
SECTOR = 2048
ALIGN_SMALL = 64
MIN_PACK_SIZE = 256    # smaller lumps are always raw
MAX_RATIO = 0.90       # LZ4 only if the stored form is <= 90% of the raw size
CM_RAW, CM_LZ4 = 0, 1
HEADER = struct.Struct('<4s11I16x')   # magic, version, header_size, flags, numlumps, table_off, pool_off, pool_size, data_off, file_size, block, 0
HEADEXT = struct.Struct('<8I32x')      # v2, at byte 64: extsize, head_off, head_bytes, crc_off, chk_table, chk_pool, chk_head, chk_crc
HEAD_BYTES = 16                       # v2: first bytes of every decoded lump (Doom patch header 8, PNG header 16)
FLAG_HEAD = 2                         # v2: head table + per-lump CRC32 table present
ENTRY = struct.Struct('<6I')          # position, disksize, size, fullname_off, longname_off, codec
FLAG_NONMUSIC = 1                     # W_VerifyNMUSlumps would say "has other lumps" (lump is "important")
assert HEADER.size == 64 and ENTRY.size == 24 and HEADEXT.size == 64


def align(v, a):
    return (v + a - 1) // a * a


# ---- the engine's view of the zip (ResGetLumpsZip), used to prove the order and to derive names -------------

def engine_walk(path):
    """Walk the central directory exactly like ResGetLumpsZip: returns [(fullname bytes, compression, csize, size)]."""
    data = Path(path).read_bytes()
    start = max(0, len(data) - (22 + 65536))
    p = data.find(b'PK\x05\x06', start)          # ResFindSignature: first match in the tail window
    if p < 0:
        raise SystemExit(f'{path}: missing central directory')
    (_sig, _disk, _cdisk, _de, entries, _cdsize, cdoff, _clen) = struct.unpack_from('<4sHHHHIIH', data, p)
    out = []
    q = cdoff
    for _ in range(entries):
        (sig, _v, _vn, _fl, comp, _mt, _md, _crc, csize, size, nlen, xlen, clen, _ds, _ai, _ae, _off) = struct.unpack_from('<4s6HIIIHHHHHII', data, q)
        if sig != b'PK\x01\x02':
            raise SystemExit(f'{path}: central directory is corrupt')
        name = data[q + 46:q + 46 + nlen]
        out.append((name, comp, csize, size))
        q += 46 + nlen + xlen + clen
    return out


def engine_names(full):
    """name[8], hash, longname exactly as ResGetLumpsZip computes them from the full name (bytes)."""
    i = full.rfind(b'/')
    trim = full[i + 1:] if i >= 0 else full
    d = trim.rfind(b'.')
    long_ = trim if d < 0 else trim[:d]           # dotpos - trimname characters of trimname
    name = long_[:8]
    h = 5381
    for c in name:
        if c == 0:
            break
        h = ((h * 33) & 0xFFFFFFFF) ^ (c + 32 if 65 <= c <= 90 else c)
    return name, h, long_


# W_VerifyNMUSlumps (music.pk3 may only hold these) -----------------------------------------------------
NMUS = [b'D_', b'O_', b'DS', b'ENDOOM', b'PLAYPAL', b'PAL', b'COLORMAP', b'CLM', b'TRANS', b'CONSBACK', b'SAVE',
        b'BLACXLVL', b'GAMEDONE', b'CONT', b'STNONEX', b'ULTIMATE', b'SLCT', b'LSSTATIC', b'BLANKLV', b'CRFNT',
        b'NTFNT', b'NTFNO', b'LTFNT', b'TTL', b'STCFN', b'TNYFN', b'STLIVE', b'CROSHAI', b'INTERSC', b'SPECTILE',
        b'STT', b'YB_', b'RESULT', b'RACE', b'SRB2BACK', b'M_', b'LT', b'HOMING', b'HOMITM', b'CHARFG', b'CHARBG',
        b'RECATK', b'RECCLOCK', b'NTSATK', b'NTSSONC', b'SLID', b'CONT', b'MINICAPS', b'BLUESTAT', b'BYELSTAT',
        b'ORNGSTAT', b'REDSTAT', b'YELSTAT', b'NBRACKET', b'NGHTLINK', b'NGT', b'NARROW', b'NREDAR', b'NSS', b'NBON',
        b'NRNG', b'NHUD', b'CAPS', b'DRILL', b'GRADE', b'MINUS5', b'NGRTIMER', b'MUSICDEF', b'SHADERS', b'SH_']
FOLDER_BLACKLIST = [b'Lua/', b'SOC/', b'Sprites/', b'LongSprites/', b'Textures/', b'Patches/', b'Flats/', b'Fades/']


def _strncasecmp0(a, b, n):
    """strncasecmp(a, b, n) == 0 for C strings (a may be NUL padded)."""
    a = a.split(b'\0', 1)[0]
    for k in range(n):
        ca = a[k] if k < len(a) else 0
        cb = b[k] if k < len(b) else 0
        la = ca + 32 if 65 <= ca <= 90 else ca
        lb = cb + 32 if 65 <= cb <= 90 else cb
        if la != lb:
            return False
        if la == 0:
            return True
    return True


def music_only(names):
    """True if W_VerifyPK3(checklist=NMUSlist, status=false) accepts every entry (W_VerifyNMUSlumps() == 1)."""
    for full in names:
        i = full.rfind(b'/')
        trim = full[i + 1:] if i >= 0 else full
        if not trim:
            continue                              # directories are ignored
        name, _, _ = engine_names(full)
        if not any(_strncasecmp0(name, e, len(e)) for e in NMUS):
            return False
        if any(_strncasecmp0(full, e, len(e)) for e in FOLDER_BLACKLIST):
            return False
    return True


# ---- compression ----------------------------------------------------------------------------------------------

def lz4_block(b):
    return lz4.block.compress(b, mode='high_compression', compression=12, store_size=False)


def lz4_lump(data):
    """Stored LZ4 form of a lump: one raw block if len <= BLOCK, else u32 index[nblocks] (bit31 = block stored raw) + blocks."""
    if len(data) <= BLOCK:
        return lz4_block(data)
    idx, body = [], []
    for i in range(0, len(data), BLOCK):
        b = data[i:i + BLOCK]
        c = lz4_block(b)
        if len(c) >= len(b):
            idx.append(len(b) | 0x80000000)
            body.append(b)
        else:
            idx.append(len(c))
            body.append(c)
    return struct.pack(f'<{len(idx)}I', *idx) + b''.join(body)


def fletcher(data):
    """The index checksum of SRP2 v2: little endian u32 words (the data zero padded to a multiple of 4), a += w; b += a (both mod 2^32),
    folded to one word: a ^ rotl(b, 16). Same function in src/w_pack.c (WPack_Check) and tools/ps2/verify_pack.py."""
    if len(data) % 4:
        data = data + bytes(4 - len(data) % 4)
    n = len(data) // 4
    words = struct.unpack(f'<{n}I', data)
    a = b = 0
    for w in words:
        a = (a + w) & 0xFFFFFFFF
        b = (b + a) & 0xFFFFFFFF
    return a ^ (((b << 16) | (b >> 16)) & 0xFFFFFFFF)


class PackReader:
    """Lazy reader of an existing SRP2 pack (v1 or v2): names of all lumps, decoded data of one lump on demand (mmap, nothing is read twice)."""

    def __init__(self, path):
        import mmap
        self.f = open(path, 'rb')
        self.data = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        d = self.data
        magic, version, hsize, self.flags, self.n, self.toff, poff, psize, doff, fsize, self.block = struct.unpack_from('<4s10I', d, 0)
        if magic != b'SRP2' or version not in (1, 2) or fsize != len(d):
            raise SystemExit(f'{path}: not an SRP2 v1/v2 pack')
        self.pool = d[poff:poff + psize]
        self.names = []
        for i in range(self.n):
            fo = ENTRY.unpack_from(d, self.toff + 24 * i)[3]
            self.names.append(self.pool[fo:self.pool.index(b'\0', fo)])

    def get(self, i):
        d = self.data
        pos, dsz, size, fo, lo, codec = ENTRY.unpack_from(d, self.toff + 24 * i)
        if size == 0:
            return b''
        raw = d[pos:pos + dsz]
        if codec == CM_RAW:
            return bytes(raw)
        if size <= self.block:
            return lz4.block.decompress(raw, uncompressed_size=size)
        nb = (size + self.block - 1) // self.block
        idx = struct.unpack_from(f'<{nb}I', raw)
        p = 4 * nb
        buf = bytearray()
        for k in range(nb):
            cs = idx[k] & 0x7FFFFFFF
            bs = min(self.block, size - k * self.block)
            blk = raw[p:p + cs]
            p += cs
            buf += blk if idx[k] >> 31 else lz4.block.decompress(blk, uncompressed_size=bs)
        return bytes(buf)


def encode(args):
    """Worker: read entry `i` of the source (a pk3: zipfile checks the CRC; or an existing pack), choose a codec.
    Returns (i, codec, payload, size, sha256, crc32, head). cooked: the cooked picture that replaces a PNG lump (sha256/size describe the stored lump), else None."""
    path, i, cooked = args
    global _zf
    if path.endswith('.PAK') or path.endswith('.pak'):
        if _zf is None or _zf[0] != path:
            _zf = (path, PackReader(path), None)
        data = _zf[1].get(i)
    else:
        if _zf is None or _zf[0] != path:
            z = zipfile.ZipFile(path)
            _zf = (path, z, z.infolist())
        zi = _zf[2][i]
        data = b'' if zi.is_dir() else _zf[1].read(zi)
    if cooked is not None:
        data = cooked
    sha = hashlib.sha256(data).digest()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    head = data[:HEAD_BYTES].ljust(HEAD_BYTES, b'\0')
    if len(data) < MIN_PACK_SIZE or data[:4] == b'OggS' or data[:4] == b'\x89PNG':
        return i, CM_RAW, data, len(data), sha, crc, head        # tiny, Ogg (music is kept byte for byte) or a PNG left in place (--keep-png)
    packed = lz4_lump(data)
    if len(packed) <= len(data) * MAX_RATIO:
        return i, CM_LZ4, packed, len(data), sha, crc, head
    return i, CM_RAW, data, len(data), sha, crc, head


_zf = None


# ---- pack writer ----------------------------------------------------------------------------------------------

def convert_pngs(srcdir, tooldir, only, log):
    """Converts every PNG lump of the pk3 in `only` (list of names, pk3 order) with the original engine decoder.
    Returns {(pk3 name, entry index): meta dict incl. 'cooked' bytes}."""
    found = []
    with zipfile.ZipFile(Path(srcdir) / 'srb2.pk3') as palette_archive:
        playpal = palette_archive.read('PLAYPAL')
    for pk3, _pak in PACKS:
        if only and pk3 not in only:
            continue
        with zipfile.ZipFile(Path(srcdir) / pk3) as z:
            for i, zi in enumerate(z.infolist()):
                if zi.file_size >= 8 and not zi.is_dir():
                    d = z.read(zi)
                    if d[:8] == strip_pics.PNG_SIG:
                        found.append((pk3, i, zi.filename, d))
    if not found:
        return {}
    tooldir = Path(tooldir)
    pngdir = tooldir / 'png'
    pngdir.mkdir(parents=True, exist_ok=True)
    files = []
    for k, (_pk3, _i, _name, d) in enumerate(found):
        p = pngdir / f'{k:03d}.png'
        p.write_bytes(d)
        files.append(p)
    (tooldir / 'PLAYPAL').write_bytes(playpal)
    exe = strip_pics.build_oracle(tooldir / 'oracle')
    log(f'converting {len(found)} PNG lumps with the original engine decoder ({exe.name})')
    strip_pics.run_tool(exe, tooldir / 'PLAYPAL', tooldir / 'oracle-out', files)
    out = {}
    for k, (pk3, i, name, d) in enumerate(found):
        w, h, left, top, pix = strip_pics.read_matrix(tooldir / 'oracle-out' / f'{k:03d}.matrix')
        cooked = strip_pics.cooked_from_matrix(w, h, left, top, pix)
        out[(pk3, i)] = dict(index=i, name=name, png_size=len(d), png_sha256=hashlib.sha256(d).hexdigest(),
                             png_crc32=zlib.crc32(d), cooked=cooked, cooked_size=len(cooked),
                             cooked_sha256=hashlib.sha256(cooked).hexdigest(), cooked_crc32=zlib.crc32(cooked),
                             width=w, height=h, leftoffset=left, topoffset=top)
    return out


def cook(src, dst, jobs, pics=None, order=None, version=VERSION, dedup=True):
    pics = pics or {}
    from_pack = str(src).endswith('.PAK')
    if from_pack:
        pr = PackReader(src)
        names = list(pr.names)
        n = len(names)
        src_size = Path(src).stat().st_size
        folders = sum(1 for nm in names if nm.endswith(b'/'))
    else:
        zf = zipfile.ZipFile(src)
        infos = zf.infolist()
        walk = engine_walk(src)
        # prove that infolist() order == the order ResGetLumpsZip walks, and that the engine can read every entry
        if len(walk) != len(infos):
            raise SystemExit(f'{src}: engine sees {len(walk)} entries, zipfile {len(infos)}')
        for k, ((name, comp, csize, size), zi) in enumerate(zip(walk, infos)):
            if b'\0' in name or b'\n' in name or not name.isascii():
                raise SystemExit(f'{src}: entry {k} {name!r}: NUL/newline/non-ASCII names are not supported')
            if name.decode('ascii') != zi.filename or size != zi.file_size or csize != zi.compress_size:
                raise SystemExit(f'{src}: entry {k} differs between engine walk and zipfile ({name!r} vs {zi.filename!r})')
            if comp not in (0, 8):
                raise SystemExit(f'{src}: entry {k} {name!r} uses compression {comp} (engine: unsupported)')
        zf.close()
        n = len(walk)
        names = [w[0] for w in walk]
        src_size = Path(src).stat().st_size
        folders = sum(1 for nm in names if nm.endswith(b'/'))
    if n > 0xFFFF:
        raise SystemExit(f'{src}: {n} entries do not fit in UINT16')

    t0 = time.time()
    with ProcessPoolExecutor(jobs) as ex:
        results = list(ex.map(encode, [(str(src), i, pics[i]['cooked'] if i in pics else None) for i in range(n)], chunksize=8))
    t_enc = time.time() - t0

    # string pool: fullname\0 [longname\0]; a longname equal to the tail of its fullname shares it
    pool = bytearray()
    seen = {}
    offs = []
    for full in names:
        _, _, long_ = engine_names(full)
        if full in seen:
            fo = seen[full]
        else:
            fo = seen[full] = len(pool)
            pool += full + b'\0'
        trim = full[full.rfind(b'/') + 1:]
        if long_ == trim:
            lo = fo + len(full) - len(trim)       # no extension: longname is the tail of fullname (shares its NUL)
        elif long_ in seen:
            lo = seen[long_]
        else:
            lo = seen[long_] = len(pool)
            pool += long_ + b'\0'
        offs.append((fo, lo))
    pool_size = len(pool)

    v2 = version >= 2
    table_off = SECTOR
    pool_off = align(table_off + ENTRY.size * n, SECTOR)
    head_off = align(pool_off + pool_size, SECTOR) if v2 else 0
    crc_off = align(head_off + HEAD_BYTES * n, SECTOR) if v2 else 0
    data_off = align(crc_off + 4 * n, SECTOR) if v2 else align(pool_off + pool_size, SECTOR)

    # storage order: the lumps of the order list first (in that order), then the rest in directory order
    storage = list(range(n))
    if order:
        index = {}
        for i, nm in enumerate(names):
            index.setdefault(nm.decode('ascii'), i)
        first = []
        taken = set()
        for nm in order:
            i = index.get(nm)
            if i is not None and i not in taken:
                first.append(i)
                taken.add(i)
        storage = first + [i for i in range(n) if i not in taken]

    entries = [None] * n
    pos = data_off
    blobs = []          # (position, payload)
    stored = {}         # (codec, size, sha256 of the stored bytes) -> position   (identical lumps are stored once)
    dedup_saved = 0
    for i in storage:
        (ri, codec, payload, size, sha, crc, head) = results[i]
        fo, lo = offs[i]
        if len(payload) == 0:
            entries[i] = ENTRY.pack(0, 0, 0, fo, lo, CM_RAW)
            continue
        key = (codec, size, hashlib.sha256(payload).digest())
        if v2 and dedup and key in stored:
            entries[i] = ENTRY.pack(stored[key], len(payload), size, fo, lo, codec)
            dedup_saved += len(payload)
            continue
        pos = align(pos, SECTOR if size >= BLOCK else ALIGN_SMALL)
        entries[i] = ENTRY.pack(pos, len(payload), size, fo, lo, codec)
        stored[key] = pos
        blobs.append((pos, payload))
        pos += len(payload)
    file_size = align(pos, SECTOR)
    flags = (0 if music_only(names) else FLAG_NONMUSIC) | (FLAG_HEAD if v2 else 0)

    table_bytes = b''.join(entries)
    head_bytes = b''.join(r[6] for r in results) if v2 else b''
    crc_bytes = b''.join(struct.pack('<I', r[5]) for r in results) if v2 else b''
    with open(dst, 'wb') as f:
        f.write(HEADER.pack(MAGIC, version, HEADER.size, flags, n, table_off, pool_off, pool_size, data_off, file_size, BLOCK, 0))
        if v2:
            f.write(HEADEXT.pack(HEADEXT.size, head_off, HEAD_BYTES, crc_off, fletcher(table_bytes), fletcher(bytes(pool)), fletcher(head_bytes), fletcher(crc_bytes)))
        f.seek(table_off)
        f.write(table_bytes)
        f.seek(pool_off)
        f.write(pool)
        if v2:
            f.seek(head_off)
            f.write(head_bytes)
            f.seek(crc_off)
            f.write(crc_bytes)
        for p, payload in blobs:
            f.seek(p)
            f.write(payload)
        f.truncate(file_size)

    sidecar = Path(str(dst) + '.pics.json')
    if pics:
        meta = [{k: v for k, v in m.items() if k != 'cooked'} for _i, m in sorted(pics.items())]
        sidecar.write_text(json.dumps(dict(
            note='PNG lumps replaced by cooked pictures (strip_pics.py); entry data in the pack = cooked picture',
            entries=meta), indent=1), encoding='utf-8')
    elif from_pack and Path(str(src) + '.pics.json').exists():
        shutil.copyfile(str(src) + '.pics.json', sidecar)   # the same lumps in the same order: the record of the cooked pictures still holds
    elif sidecar.exists():
        sidecar.unlink()  # --keep-png / new content must not retain an obsolete conversion record

    # statistics
    st = {}
    for (i, codec, payload, size, sha, crc, head) in results:
        s_ = st.setdefault(codec, [0, 0, 0])
        s_[0] += 1
        s_[1] += size
        s_[2] += len(payload)
    return dict(n=n, stats=st, file_size=file_size, pool=pool_size, flags=flags, t=t_enc, srcsize=src_size,
                png=sum(1 for r in results if r[2][:4] == b'\x89PNG'), cooked=len(pics), folders=folders, dedup_saved=dedup_saved,
                index_bytes=data_off)


# ---- TEXC.PAK (OPT13 IZ, PS2-602, R2): composite textures prebuilt by the host engine -------------------------------------------

TEXC_MAGIC = b'TXCD'
TEXC_INFO = b'TXC1'


def read_texc_dump(path):
    """The dump `SRB2 -texcdump FILE` writes (src/ps2/ps2_texc.c): u32 'TXCD', u32 version 1, then records u64 key, u32 texture number, u16 w, u16 h, u32 size, size bytes.
    Returns [(texnum, key, w, h, pixels)] in file order (texture number order)."""
    d = Path(path).read_bytes()
    if d[:4] != TEXC_MAGIC or struct.unpack_from('<I', d, 4)[0] != 1:
        raise SystemExit(f'{path}: not a texture dump (TXCD version 1)')
    out, p = [], 8
    while p < len(d):
        key, num, w, h, size = struct.unpack_from('<QIHHI', d, p)
        p += 20
        if size != w * h or p + size > len(d):
            raise SystemExit(f'{path}: damaged record at byte {p - 20}')
        out.append((num, key, w, h, d[p:p + size]))
        p += size
    return out


def _texc_encode(data):
    sha = hashlib.sha256(data).digest()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    head = data[:HEAD_BYTES].ljust(HEAD_BYTES, b'\0')
    if len(data) < MIN_PACK_SIZE:
        return CM_RAW, data, sha, crc, head
    packed = lz4_lump(data)
    if len(packed) <= len(data) * MAX_RATIO:
        return CM_LZ4, packed, sha, crc, head
    return CM_RAW, data, sha, crc, head


def cook_texc(dump, dst, jobs):
    """TEXC.PAK: an SRP2 v2 pack (same container and reader as the game packs) whose lumps are the prebuilt composites, named by the 16 hex digits of the key of the texture definition
    (PS2TexC_Key), in texture list order (a zone's textures lie together: the level prefetch reads a few long runs); lump 0 is TEXCINFO. Identical pixels are stored once."""
    recs = read_texc_dump(dump)
    seen, items = set(), []
    for num, key, w, h, pix in recs:
        if key in seen:
            continue
        seen.add(key)
        items.append((f'{key:016x}'.encode(), pix))
    info = TEXC_INFO + struct.pack('<III', 1, len(items), 0)
    names = [b'TEXCINFO'] + [nm for nm, _ in items]
    datas = [info] + [pix for _, pix in items]
    n = len(names)
    t0 = time.time()
    with ProcessPoolExecutor(jobs) as ex:
        results = list(ex.map(_texc_encode, datas, chunksize=16))
    t_enc = time.time() - t0
    pool, offs = bytearray(), []
    for full in names:
        fo = len(pool)
        pool += full + b'\0'
        offs.append((fo, fo))          # no extension, no folder: the long name is the full name
    pool_size = len(pool)
    table_off = SECTOR
    pool_off = align(table_off + ENTRY.size * n, SECTOR)
    head_off = align(pool_off + pool_size, SECTOR)
    crc_off = align(head_off + HEAD_BYTES * n, SECTOR)
    data_off = align(crc_off + 4 * n, SECTOR)
    entries, blobs, stored, pos, saved = [], [], {}, data_off, 0
    for i, (codec, payload, sha, crc, head) in enumerate(results):
        fo, lo = offs[i]
        size = len(datas[i])
        key = (codec, size, hashlib.sha256(payload).digest())
        if key in stored:
            entries.append(ENTRY.pack(stored[key], len(payload), size, fo, lo, codec))
            saved += len(payload)
            continue
        pos = align(pos, SECTOR if size >= BLOCK else ALIGN_SMALL)
        entries.append(ENTRY.pack(pos, len(payload), size, fo, lo, codec))
        stored[key] = pos
        blobs.append((pos, payload))
        pos += len(payload)
    file_size = align(pos, SECTOR)
    table_bytes = b''.join(entries)
    head_bytes = b''.join(r[4] for r in results)
    crc_bytes = b''.join(struct.pack('<I', r[3]) for r in results)
    with open(dst, 'wb') as f:
        f.write(HEADER.pack(MAGIC, 2, HEADER.size, FLAG_NONMUSIC | FLAG_HEAD, n, table_off, pool_off, pool_size, data_off, file_size, BLOCK, 0))
        f.write(HEADEXT.pack(HEADEXT.size, head_off, HEAD_BYTES, crc_off, fletcher(table_bytes), fletcher(bytes(pool)), fletcher(head_bytes), fletcher(crc_bytes)))
        f.seek(table_off)
        f.write(table_bytes)
        f.seek(pool_off)
        f.write(pool)
        f.seek(head_off)
        f.write(head_bytes)
        f.seek(crc_off)
        f.write(crc_bytes)
        for p, payload in blobs:
            f.seek(p)
            f.write(payload)
        f.truncate(file_size)
    raw = sum(len(d) for d in datas)
    return dict(n=n - 1, raw=raw, file_size=file_size, t=t_enc, saved=saved, dups=len(recs) - len(items))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=str(ROOT / 'srb2-assets'))
    ap.add_argument('--out', default=str(ROOT / 'build/pak'))
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--only', nargs='*', default=[], choices=[p for p, _ in PACKS], help='pk3 names to cook (default all four)')
    ap.add_argument('--tool-dir', default=str(ROOT / 'build/strip-pic-tool'))
    ap.add_argument('--keep-png', action='store_true')
    ap.add_argument('--from-pak', help='directory with existing SRB2.PAK ZONES.PAK CHARS.PAK MUSIC.PAK (v1 or v2): re-cook their lumps (no pk3, no PNG tool needed)')
    ap.add_argument('--order', type=Path, help='lump full names, one per line: stored first, in this order (the start-up read order)')
    ap.add_argument('--version', type=int, default=VERSION, choices=[1, 2])
    ap.add_argument('--no-dedup', action='store_true', help='v2: store identical lumps more than once')
    ap.add_argument('--log', type=Path, help='save cooker output')
    ap.add_argument('--texc', type=Path, metavar='DUMP', help='OPT13 IZ (PS2-602): make TEXC.PAK (prebuilt composite textures) in --out from the dump of the host engine (SRB2 -texcdump DUMP; tools/ps2/cook_texc.sh) and do nothing else')
    a = ap.parse_args()
    if a.jobs < 1:
        ap.error('--jobs must be positive')
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    if a.texc:
        r = cook_texc(a.texc, out / 'TEXC.PAK', a.jobs)
        print(f'TEXC.PAK: {r["n"]} composites ({r["dups"]} duplicate definitions), pixels {r["raw"]:,} B -> pack {r["file_size"]:,} B ({r["file_size"] / r["raw"] * 100:.1f}%), identical pixels stored once: {r["saved"]:,} B, encode {r["t"]:.1f}s')
        return 0
    messages = []

    def log(message):
        print(message, flush=True)
        messages.append(message)
        if a.log:
            a.log.parent.mkdir(parents=True, exist_ok=True)
            a.log.write_text('\n'.join(messages) + '\n', encoding='utf-8')
    order = None
    if a.order:
        order = [l.strip() for l in a.order.read_text().splitlines() if l.strip() and not l.startswith('#')]
        log(f'storage order list: {len(order)} lump names from {a.order}')
    tot = [0, 0, 0]
    codec_name = {CM_RAW: 'raw', CM_LZ4: 'lz4'}
    allpics = {} if (a.keep_png or a.from_pak) else convert_pngs(a.src, a.tool_dir, a.only, log)
    for pk3, pak in PACKS:
        if a.only and pk3 not in a.only:
            continue
        pics = {i: m for (p, i), m in allpics.items() if p == pk3}
        source = Path(a.from_pak) / pak if a.from_pak else Path(a.src) / pk3
        r = cook(source, out / pak, a.jobs, pics, order, a.version, not a.no_dedup)
        log(f'{source.name} -> {pak}: {r["n"]} entries ({r["folders"]} folders), source {r["srcsize"]:,} B, pack {r["file_size"]:,} B (index part {r["index_bytes"]:,} B), '
              f'pool {r["pool"]:,} B, nonmusic={r["flags"] & 1}, encode {r["t"]:.1f}s, PNG lumps left {r["png"]}, cooked pictures {r["cooked"]}, identical lumps stored once: {r["dedup_saved"]:,} B')
        for c, (cnt, raw, disk) in sorted(r['stats'].items()):
            log(f'    {codec_name[c]:4} {cnt:6} lumps  {raw:>13,} B -> {disk:>13,} B' + (f'  ({disk / raw * 100:.1f}%)' if raw else ''))
        tot[0] += r['srcsize']
        tot[1] += r['file_size']
    log(f'total: source {tot[0]:,} B -> packs {tot[1]:,} B')


if __name__ == '__main__':
    sys.exit(main())
