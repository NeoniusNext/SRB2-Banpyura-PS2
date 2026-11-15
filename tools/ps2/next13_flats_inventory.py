#!/usr/bin/env python3
"""Count the canonical flat table from the actual SRP2 directory and source assets."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zipfile

from next13_flats_variant import ROOT, BASE

HEADER = struct.Struct('<4s11I16x')
ENTRY = struct.Struct('<6I')
PACKS = [('srb2.pk3', 'SRB2.PAK'), ('zones.pk3', 'ZONES.PAK'),
         ('characters.pk3', 'CHARS.PAK'), ('music.pk3', 'MUSIC.PAK')]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def count_definitions(data):
    pattern = r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/|[{}]|[^\s{},]+'
    tokens = [t for t in re.findall(pattern, data.decode('ascii')) if not t.startswith(('//', '/*'))]
    n = at = 0
    while at < len(tokens):
        if tokens[at].upper() not in ('TEXTURE', 'WALLTEXTURE'):
            raise RuntimeError('Unexpected top-level TEXTURES token ' + tokens[at])
        n += 1
        while at < len(tokens) and tokens[at] != '{':
            at += 1
        if at == len(tokens):
            raise RuntimeError('Missing texture definition body')
        depth = 1
        at += 1
        while at < len(tokens) and depth:
            depth += (tokens[at] == '{') - (tokens[at] == '}')
            at += 1
        if depth:
            raise RuntimeError('Unbalanced TEXTURES body')
    return n


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--assets', type=Path, default=Path('/workspace/.onboarding/srb2-assets'))
    p.add_argument('--packs', type=Path, default=ROOT / 'build/pak')
    p.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next13/flats-inventory-v1')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    records = []
    for asset_name, pack_name in PACKS:
        asset, pack = a.assets / asset_name, a.packs / pack_name
        raw = pack.read_bytes()
        h = HEADER.unpack_from(raw)
        if h[0] != b'SRP2' or h[1] != 2 or h[2] != 64 or h[9] != len(raw):
            raise RuntimeError('Unexpected canonical pack header: ' + str(pack))
        n, table_at, pool_at, pool_n = h[4:8]
        pool = raw[pool_at:pool_at + pool_n]
        def name(at):
            return pool[at:pool.index(b'\0', at)]
        entries = [ENTRY.unpack_from(raw, table_at + i * ENTRY.size) for i in range(n)]
        names = [name(e[3]) for e in entries]
        longnames = [name(e[4]) for e in entries]
        flats = sum(s.lower().startswith(b'flats/') and len(s) > 6 and bool(l) for s, l in zip(names, longnames))
        singles = sum(s.lower().startswith(b'textures/') and len(s) > 9 and bool(l) for s, l in zip(names, longnames))
        definitions = []
        with zipfile.ZipFile(asset) as z:
            if names != [s.encode('utf-8') for s in z.namelist()]:
                raise RuntimeError('Pack directory differs from canonical source asset: ' + pack_name)
            crc_at = struct.unpack_from('<8I32x', raw, 64)[3]
            for i, (s, longname) in enumerate(zip(names, longnames)):
                if longname[:8].upper() == b'TEXTURES':
                    full = s.decode('utf-8')
                    data = z.read(full)
                    # The exact original TEXTURES bytes were preserved by cooker.
                    crc = struct.unpack_from('<I', raw, crc_at + 4 * i)[0]
                    if crc != z.getinfo(full).CRC or len(data) != entries[i][2]:
                        raise RuntimeError('TEXTURES pack CRC/extent differs from source')
                    definitions.append({'lump': i, 'fullname': full, 'sha256': hashlib.sha256(data).hexdigest(), 'count': count_definitions(data)})
        total = flats + singles + sum(d['count'] for d in definitions)
        records.append({'asset': str(asset), 'asset_sha256': sha(asset), 'pack': str(pack),
                        'pack_sha256': sha(pack), 'lumps': n, 'flats': flats,
                        'single_patch': singles, 'definitions': definitions, 'textures': total})
    n = sum(r['textures'] for r in records)
    report = {'revision': '353cc94103aeb38cef87d26151c779302135d0e9',
        'method': 'Actual canonical SRP2 directories; same folder predicates and 8-byte lump names as R_CountTextures, TEXTURES source bodies verified by pack CRC+length.',
        'not_a_live_game_observation': True, 'inputs': records, 'numtextures': n,
        'EE_bytes': {'GLMipmap_t': 76, 'GLMapTexture_t': 84, 'original_flat_table': 84*n,
                     'compact_flat_table': 76*n, 'saved_flat_table': 8*n},
        'source_sha256': {s: sha(BASE / s) for s in ('src/r_textures.c', 'src/w_wad.c', 'src/hardware/hw_cache.c', 'src/hardware/hw_data.h')},
        'tool_sha256': sha(Path(__file__))}
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'numtextures': n, 'EE_flat_table_saved_bytes': 8*n, 'out': str(a.out)}))


if __name__ == '__main__':
    main()
