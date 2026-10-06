"""List every map WAD of a cooked ZONES.PAK with the sizes of its level lumps (and an estimate of the expanded level structures).

usage: python tools/ps2/map_inventory.py [--pak build/pak-a/ZONES.PAK] [--json OUT.json] [--md OUT.md] [--top N]
Reads the SRP2 index (docs/PACK_FORMAT.md), decodes each embedded WAD (LZ4) and prints, per map, the raw sizes of THINGS,
LINEDEFS, SIDEDEFS, VERTEXES, SEGS, SSECTORS, NODES, SECTORS, REJECT, BLOCKMAP and whether the map is UDMF (TEXTMAP present).
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

import lz4.block

LEVEL = ['THINGS', 'LINEDEFS', 'SIDEDEFS', 'VERTEXES', 'SEGS', 'SSECTORS', 'NODES', 'SECTORS', 'REJECT', 'BLOCKMAP', 'TEXTMAP', 'ZNODES', 'SCRIPTS']
# on-disk record sizes of the binary map format (SRB2 2.2): things 10, linedefs 14, sidedefs 30, vertexes 4, segs 12, ssectors 4, nodes 28, sectors 26
REC = {'THINGS': 10, 'LINEDEFS': 14, 'SIDEDEFS': 30, 'VERTEXES': 4, 'SEGS': 12, 'SSECTORS': 4, 'NODES': 28, 'SECTORS': 26}


def read_pack(path):
    f = open(path, 'rb')
    h = f.read(64)
    magic, ver, hs, fl, nl, to, po, ps, do, fs, bs = struct.unpack('<4s10I', h[:44])
    assert magic == b'SRP2'
    f.seek(to)
    t = f.read(nl * 24)
    f.seek(po)
    pool = f.read(ps)
    ents = []
    for i in range(nl):
        p, ds, sz, fn, ln, c = struct.unpack_from('<6I', t, i * 24)
        ents.append((pool[fn:pool.index(b'\0', fn)].decode(), p, ds, sz, c))
    return f, bs, ents


def decode(f, block, pos, ds, sz, codec):
    f.seek(pos)
    raw = f.read(ds)
    if codec == 0:
        return raw
    if sz <= block:
        return lz4.block.decompress(raw, uncompressed_size=sz)
    nb = (sz + block - 1) // block
    idx = struct.unpack_from(f'<{nb}I', raw)
    p = 4 * nb
    out = bytearray()
    for b in range(nb):
        cs = idx[b] & 0x7FFFFFFF
        bs = min(block, sz - b * block)
        blk = raw[p:p + cs]
        p += cs
        out += blk if idx[b] >> 31 else lz4.block.decompress(blk, uncompressed_size=bs)
    return bytes(out)


def parse_wad(data):
    magic, n, off = struct.unpack_from('<4sII', data, 0)
    lumps = []
    for i in range(n):
        pos, size, name = struct.unpack_from('<II8s', data, off + i * 16)
        lumps.append((name.split(b'\0')[0].decode('latin1'), pos, size))
    return lumps


def classify(name):
    m = re.search(r'MAP(\w\w)\.wad$', name, re.I)
    if not m:
        return None
    code = m.group(1).upper()
    if 'Singleplayer' in name:
        kind = 'SP'
    elif 'Capture' in name:
        kind = 'CTF'
    elif 'Match' in name:
        kind = 'Match'
    elif 'Titlemap' in name:
        kind = 'Title'
    else:
        kind = 'MP-special'
    return code, kind


def inventory(pak):
    f, block, ents = read_pack(pak)
    res = []
    for name, pos, ds, sz, c in ents:
        cl = classify(name)
        if not cl:
            continue
        data = decode(f, block, pos, ds, sz, c)
        lumps = parse_wad(data)
        d = {'map': cl[0], 'kind': cl[1], 'path': name, 'wad': sz, 'udmf': False}
        for ln, p, s in lumps:
            if ln in LEVEL:
                d[ln] = s
            if ln == 'TEXTMAP':
                d['udmf'] = True
        for k, r in REC.items():
            d['n_' + k.lower()] = d.get(k, 0) // r
        d['lumps'] = len(lumps)
        d['level_bytes'] = sum(d.get(k, 0) for k in LEVEL)
        res.append(d)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pak', default=str(Path(__file__).resolve().parents[2] / 'build/pak-a/ZONES.PAK'))
    ap.add_argument('--json', default='')
    ap.add_argument('--md', default='')
    ap.add_argument('--top', type=int, default=0)
    a = ap.parse_args()
    res = inventory(a.pak)
    res.sort(key=lambda d: -d['level_bytes'])
    cols = ['THINGS', 'LINEDEFS', 'SIDEDEFS', 'VERTEXES', 'SEGS', 'SSECTORS', 'NODES', 'SECTORS', 'REJECT', 'BLOCKMAP']
    hdr = '| # | map | kind | udmf | wad | ' + ' | '.join(c.lower() for c in cols) + ' | level lumps |'
    lines = [hdr, '|' + '---|' * (6 + len(cols))]
    for i, d in enumerate(res[:a.top or None]):
        lines.append(f"| {i + 1} | {d['map']} | {d['kind']} | {'yes' if d['udmf'] else 'no'} | {d['wad']} | " +
                     ' | '.join(str(d.get(c, 0)) for c in cols) + f" | {d['level_bytes']} |")
    txt = '\n'.join(lines)
    print(txt)
    print(f"maps: {len(res)}, udmf: {sum(1 for d in res if d['udmf'])}")
    if a.json:
        Path(a.json).write_text(json.dumps(res, indent=1))
    if a.md:
        Path(a.md).write_text(txt + '\n')


if __name__ == '__main__':
    sys.exit(main())
