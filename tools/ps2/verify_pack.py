"""Verify SRP2 packs against the pk3 archives they were cooked from (independent reader, shares no code with cook.py).

usage: verify_pack.py [--src DIR] [--pak DIR]
For every pack: header/table sanity, entry count and ORDER and names equal the zip central directory, the SHA-256 of EVERY
decoded lump equals the SHA-256 of the lump read from the pk3, name/hash/longname derived like ResGetLumpsZip,
alignment rules, no overlaps. Exit code 0 only if there are 0 discrepancies.

PS2-20: a pack with a <PACK>.pics.json sidecar has its PNG lumps replaced by cooked pictures (tools/ps2/strip_pics.py).
For those entries the pk3 side must hash to the sidecar's PNG hash, the pack side to the cooked hash, the lump must be a
cooked picture, its Doom patch must equal what the independent pure-Python model of the original PNG decoder makes of the
pk3's PNG (pixels, transparency, offsets), and no PNG lump may remain anywhere in the pack.
"""
import argparse
import hashlib
import struct
import sys
import zipfile
import zlib
from pathlib import Path

import json

import lz4.block

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import strip_pics  # noqa: E402
PAIRS = [('srb2.pk3', 'SRB2.PAK'), ('zones.pk3', 'ZONES.PAK'), ('characters.pk3', 'CHARS.PAK'), ('music.pk3', 'MUSIC.PAK')]


def fletcher(data):
    """Index checksum of SRP2 v2 (also written by cook.py and computed by src/w_pack.c): u32 words, a += w; b += a; a ^ rotl(b, 16)."""
    if len(data) % 4:
        data = data + bytes(4 - len(data) % 4)
    a = b = 0
    for w in struct.unpack(f'<{len(data) // 4}I', data):
        a = (a + w) & 0xFFFFFFFF
        b = (b + a) & 0xFFFFFFFF
    return a ^ (((b << 16) | (b >> 16)) & 0xFFFFFFFF)


def cstr(pool, off):
    end = pool.index(b'\0', off)
    return pool[off:end]


def derive(full):
    # ResGetLumpsZip: trimname after the last '/', dotpos = last '.' of trimname (or the end), name = first min(8, ..) chars
    slash = full.rfind(b'/')
    trim = full[slash + 1:] if slash >= 0 else full
    dot = trim.rfind(b'.')
    length = dot if dot >= 0 else len(trim)
    longname = trim[:length]
    name = longname[:8]
    x = 5381
    for ch in name:
        x = ((x * 33) & 0xFFFFFFFF) ^ (ch | 0x20 if 65 <= ch <= 90 else ch)
    return name, x, longname


def decode(f, pos, disksize, size, codec, block):
    f.seek(pos)
    raw = f.read(disksize)
    if len(raw) != disksize:
        raise ValueError('short read')
    if codec == 0:
        if disksize != size:
            raise ValueError('raw lump with disksize != size')
        return raw
    if codec != 1:
        raise ValueError(f'unknown codec {codec}')
    if size <= block:
        return lz4.block.decompress(raw, uncompressed_size=size)
    nb = (size + block - 1) // block
    idx = struct.unpack_from(f'<{nb}I', raw)
    p = 4 * nb
    out = bytearray()
    for b in range(nb):
        cs = idx[b] & 0x7FFFFFFF
        bs = min(block, size - b * block)
        blk = raw[p:p + cs]
        p += cs
        out += blk if idx[b] >> 31 else lz4.block.decompress(blk, uncompressed_size=bs)
        if (idx[b] >> 31) and cs != bs:
            raise ValueError('raw block of wrong size')
    if p != len(raw):
        raise ValueError('block sizes do not add up to disksize')
    return bytes(out)


def verify(pk3, pak, model=None, require_cooked=False, log=print):
    bad = []
    sidecar = Path(str(pak) + '.pics.json')
    entries = json.loads(sidecar.read_text())['entries'] if sidecar.exists() else []
    pics = {e['index']: e for e in entries}
    cooked_mode = require_cooked or sidecar.exists()

    def err(msg):
        bad.append(msg)
        if len(bad) <= 20:
            log('    ' + msg)

    zf = zipfile.ZipFile(pk3)
    infos = zf.infolist()
    if len(pics) != len(entries):
        err('duplicate picture indices in sidecar')
    for i in pics:
        if not isinstance(i, int) or not 0 <= i < len(infos):
            err(f'sidecar picture index outside archive: {i!r}')
    f = open(pak, 'rb')
    hdr = f.read(64)
    magic, version, hsize, flags, n, table_off, pool_off, pool_size, data_off, file_size, block, rsv = struct.unpack('<4s11I', hdr[:48])
    if magic != b'SRP2' or version not in (1, 2) or hsize != 64 or block != 65536:
        err(f'bad header {magic} {version} {hsize} {block}')
        return len(bad), 0, {}
    if file_size != Path(pak).stat().st_size:
        err('file_size field != actual size')
    if flags & ~(1 | (2 if version >= 2 else 0)) or hdr[44:] != bytes(20):
        err('unknown flags or nonzero reserved header bytes')
    if any(off % 2048 for off in (table_off, pool_off, data_off, file_size)):
        err('header regions/end are not sector aligned')
    if not (64 <= table_off and table_off + n * 24 <= pool_off and pool_off + pool_size <= data_off <= file_size):
        err('header/table/pool/data regions overlap or leave the file')
    head_off = head_bytes = crc_off = 0
    chk = None
    if version >= 2:
        # v2 extension at byte 64: extsize, head_off, head_bytes, crc_off, four index checksums, zero padding
        ext = f.read(64)
        extsize, head_off, head_bytes, crc_off, *chk = struct.unpack('<8I', ext[:32])
        if extsize != 64 or ext[32:] != bytes(32):
            err('bad v2 header extension')
        if not (flags & 2):
            err('v2 pack without the head-table flag')
        head_end = head_off + n * head_bytes
        crc_end = crc_off + 4 * n
        if head_bytes != 16 or head_off % 2048 or crc_off % 2048 or head_off < pool_off + pool_size or crc_off < head_end or crc_end > data_off:
            err('v2 head/CRC table regions are misplaced')
    f.seek(64 if version < 2 else 128)
    if n != len(infos):
        err(f'entry count {n} != zip {len(infos)}')
    f.seek(table_off)
    table = [struct.unpack('<6I', f.read(24)) for _ in range(n)]
    f.seek(pool_off)
    pool = f.read(pool_size)
    if not pool or pool[-1] != 0:
        err('pool not NUL terminated')
    prev_end = data_off
    stats = {0: 0, 1: 0}
    heads = crcs = b''
    if version >= 2:
        f.seek(head_off)
        heads = f.read(n * head_bytes)
        f.seek(crc_off)
        crcs = f.read(4 * n)
        raw_table = b''.join(struct.pack('<6I', *e) for e in table)
        for name_, sect, want in (('table', raw_table, chk[0]), ('string pool', pool, chk[1]), ('head table', heads, chk[2]), ('CRC table', crcs, chk[3])):
            if fletcher(sect) != want:
                err(f'v2 index checksum of the {name_} does not match')
    placed = {}   # v2: position -> (disksize, size, codec) of the first entry that uses it (identical lumps may share a payload)
    for i, (zi, (pos, disksize, size, fo, lo, codec)) in enumerate(zip(infos, table)):
        full = cstr(pool, fo)
        if full.decode('ascii') != zi.filename:
            err(f'[{i}] name {full!r} != zip {zi.filename!r}')
        name, h, longname = derive(full)
        if cstr(pool, lo) != longname:
            err(f'[{i}] longname {cstr(pool, lo)!r} != derived {longname!r}')
        if size != zi.file_size and i not in pics:
            err(f'[{i}] size {size} != zip {zi.file_size}')
        if zi.is_dir() and not (size == 0 and full.endswith(b'/')):
            err(f'[{i}] directory not stored as an empty entry')
        if size == 0:
            if pos != 0 or disksize != 0 or codec != 0:
                err(f'[{i}] empty lump with nonzero position/disksize/codec')
            data = b''
        else:
            a = 2048 if size >= block else 64
            if pos % a:
                err(f'[{i}] position {pos} not aligned to {a}')
            if version >= 2:
                if pos < data_off:
                    err(f'[{i}] payload starts in the index area ({pos} < {data_off})')
                if pos in placed:
                    if placed[pos] != (disksize, size, codec):
                        err(f'[{i}] shares a payload position with a different lump')
                else:
                    placed[pos] = (disksize, size, codec)
            elif pos < prev_end:
                err(f'[{i}] overlaps previous data ({pos} < {prev_end})')
            if pos + disksize > file_size:
                err(f'[{i}] data beyond end of file')
            prev_end = pos + disksize
            try:
                data = decode(f, pos, disksize, size, codec, block)
            except Exception as e:
                err(f'[{i}] {full!r}: decode failed: {e}')
                continue
        if len(data) != size:
            err(f'[{i}] decoded {len(data)} bytes, expected {size}')
        if version >= 2:
            if heads[i * head_bytes:(i + 1) * head_bytes] != data[:head_bytes].ljust(head_bytes, b'\0'):
                err(f'[{i}] {full!r}: head table entry differs from the first bytes of the lump')
            if struct.unpack_from('<I', crcs, 4 * i)[0] != (zlib.crc32(data) & 0xFFFFFFFF):
                err(f'[{i}] {full!r}: CRC table entry differs from the CRC32 of the lump')
        ref = b'' if zi.is_dir() else zf.read(zi)
        if i in pics:
            e = pics[i]
            if ref[:8] != strip_pics.PNG_SIG or e['name'] != zi.filename or e['png_size'] != len(ref):
                err(f'[{i}] sidecar does not identify this original PNG entry')
            if hashlib.sha256(ref).hexdigest() != e['png_sha256']:
                err(f'[{i}] {full!r}: pk3 PNG differs from the sidecar record')
            if hashlib.sha256(data).hexdigest() != e['cooked_sha256'] or len(data) != e['cooked_size']:
                err(f'[{i}] {full!r}: pack lump differs from the sidecar record')
            if zlib.crc32(ref) != e['png_crc32'] or zlib.crc32(data) != e['cooked_crc32']:
                err(f'[{i}] sidecar CRC mismatch')
            if not strip_pics.is_cooked(data):
                err(f'[{i}] {full!r}: not a cooked picture')
            elif model is not None:
                try:
                    want = model.convert(ref)
                    got = strip_pics.decode_doom_patch(data[8:])
                    if got[:4] != (e['width'], e['height'], e['leftoffset'], e['topoffset']):
                        err(f'[{i}] sidecar dimensions/offsets mismatch')
                    if want != got:
                        err(f'[{i}] {full!r}: cooked picture differs from the Python model of the original decoder')
                except Exception as ex:
                    err(f'[{i}] {full!r}: cooked picture check failed: {ex}')
            stats[codec] = stats.get(codec, 0) + 1
            continue
        if cooked_mode and data[:8] == strip_pics.PNG_SIG:
            err(f'[{i}] {full!r}: PNG lump left in a cooked pack')
        if cooked_mode and ref[:8] == strip_pics.PNG_SIG:
            err(f'[{i}] {full!r}: missing PNG conversion record')
        if hashlib.sha256(data).digest() != hashlib.sha256(ref).digest():
            err(f'[{i}] {full!r}: sha256 mismatch')
        stats[codec] = stats.get(codec, 0) + 1
    if version >= 2:
        prev = None   # distinct payloads must not overlap each other
        for q in sorted(placed):
            if prev is not None and q < prev[0] + prev[1]:
                err(f'payload at {q} overlaps the payload at {prev[0]}')
            prev = (q, placed[q][0])
    f.close()
    zf.close()
    return len(bad), n, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=str(ROOT / 'srb2-assets'))
    ap.add_argument('--pak', default=str(ROOT / 'build/pak'))
    ap.add_argument('--require-cooked', action='store_true', help='reject raw PNGs and missing conversion records in every pack')
    ap.add_argument('--log', type=Path, help='save verification output and before/after SHA256 of all inputs')
    a = ap.parse_args()
    messages = []

    def log(message):
        print(message, flush=True)
        messages.append(message)
        if a.log:
            a.log.parent.mkdir(parents=True, exist_ok=True)
            a.log.write_text('\n'.join(messages) + '\n', encoding='utf-8')

    inputs = [Path(root) / name for pk3, pak in PAIRS for root, name in [(a.src, pk3), (a.pak, pak)]]
    inputs += [Path(a.pak) / (pak + '.pics.json') for _pk3, pak in PAIRS if (Path(a.pak) / (pak + '.pics.json')).exists()]
    def sha(path):
        with path.open('rb') as f:
            return hashlib.file_digest(f, 'sha256').hexdigest()
    before = {p: sha(p) for p in inputs}
    for p, digest in before.items():
        log(f'INPUT {p} {p.stat().st_size} bytes SHA256 {digest}')
    total = 0
    model = None
    if any((Path(a.pak) / (pak + '.pics.json')).exists() for _pk3, pak in PAIRS):
        with zipfile.ZipFile(Path(a.src) / 'srb2.pk3') as z:
            model = strip_pics.PyOracle(strip_pics.read_palette(z.read('PLAYPAL')))   # shared: the memo is order dependent
    for pk3, pak in PAIRS:
        log(f'{pak} vs {pk3} ...')
        nbad, n, stats = verify(Path(a.src) / pk3, Path(a.pak) / pak, model, a.require_cooked, log)
        total += nbad
        log(f'  {n} lumps compared (sha256 of every decoded lump), raw {stats.get(0, 0)}, lz4 {stats.get(1, 0)}: '
              f'{nbad} discrepancies')
    changed = sum(sha(p) != digest for p, digest in before.items())
    total += changed
    log(f'Input preservation: {len(inputs)} packs/archives/sidecars hashed before and after, {changed} changed')
    log(f'TOTAL discrepancies: {total}')
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
