"""Independent UDMF reference for tools/ps2/ftest_check.py udmf (OPT7-F, PS2-102).

Parses the TEXTMAP lump of a map WAD with Python only (no engine code) and computes what the engine's -ftest-level hook
(src/ps2/ps2_ftest.c: PS2FTest_Level) must print for the same level: counts and the CRC32 of the canonical little-endian
records of vertices, sectors, lines, sides, things (and, separately, of the flat/texture names).
The conversions copy what src/p_setup.c ParseTextmap*Parameter does: atol() for integers, atof() rounded to float and
FloatToFixed (float32 * FRACUNIT, truncated) for fixed values, UDMF defaults of P_LoadTextmap.
usage: udmf_ref.py MAP.wad [...]     prints the expectation as JSON
"""
import json
import re
import struct
import sys
import zlib


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def atof(s):
    m = re.match(r'\s*[-+]?(\d+\.?\d*([eE][-+]?\d+)?|\.\d+)', s)
    return float(m.group(0)) if m else 0.0


def atol(s):
    m = re.match(r'\s*[-+]?\d+', s)
    return int(m.group(0)) if m else 0


def fixed(s):  # FLOAT_TO_FIXED(atof(s))
    return wrap(int(f32(atof(s)) * 65536.0))


def wrap(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def short(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def wad_lumps(data):
    ident, n, off = struct.unpack('<4sII', data[:12])
    out = {}
    for i in range(n):
        pos, size, nm = struct.unpack('<II8s', data[off + 16 * i:off + 16 * i + 16])
        out[nm.rstrip(b'\0').decode('latin1')] = data[pos:pos + size]
    return out


def parse_textmap(text):
    """-> {'thing': [dict], ...}; values stay strings (strings without quotes), later keys of a block win like the engine's sequential parse."""
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    blocks = {'thing': [], 'vertex': [], 'linedef': [], 'sidedef': [], 'sector': []}
    pos = 0
    for m in re.finditer(r'(\w+)\s*\{(.*?)\}', text, re.S):
        kind = m.group(1).lower()
        if kind not in blocks:
            continue
        items = []  # ordered (key, value): the engine applies them in order (id/moreids matter)
        for km in re.finditer(r'(\w+)\s*=\s*("[^"]*"|[^;]*);', m.group(2)):
            v = km.group(2).strip()
            if v.startswith('"'):
                v = v[1:-1]
            items.append((km.group(1).lower(), v))
        blocks[kind].append(items)
    return blocks


def i32(v):
    return struct.pack('<i', wrap(v))


def name8(n):
    b = n.upper().encode('latin1')[:8]
    return b + b'\0' * (8 - len(b))


def crc(chunks):
    c = 0
    for ch in chunks:
        c = zlib.crc32(ch, c)
    return c & 0xFFFFFFFF


def extra_vertices(znodes):
    """The vertices an extended node lump (XNOD/XGLN/XGL2/XGL3) adds after the TEXTMAP ones: [(x, y)] fixed."""
    if znodes[:4] not in (b'XNOD', b'XGLN', b'XGL2', b'XGL3'):
        raise SystemExit('nodes %r: only uncompressed extended nodes are checked' % znodes[:4])
    org, new = struct.unpack('<II', znodes[4:12])
    return [struct.unpack('<ii', znodes[12 + 8 * i:20 + 8 * i]) for i in range(new)], org


def expect(textmap_bytes, znodes=None):
    b = parse_textmap(textmap_bytes.decode('latin1'))
    cv, cs, cl, cd, ct, cn = [], [], [], [], [], []
    for it in b['vertex']:
        x = y = None
        for k, v in it:
            if k == 'x': x = fixed(v)
            elif k == 'y': y = fixed(v)
        cv += [i32(x), i32(y)]
    if znodes is not None:  # the node builder's own vertices follow the map's (P_LoadExtendedNodes)
        extra, org = extra_vertices(znodes)
        assert org == len(b['vertex']), (org, len(b['vertex']))
        for x, y in extra:
            cv += [i32(x), i32(y)]
    # flats found in order (P_AddLevelFlat): the first one is levelflats[0] (the default flat of a sector that names none)
    flats = []

    def addflat(n):
        n = n.upper()[:8]
        if n not in flats:
            flats.append(n)
        return n

    secs = []
    for it in b['sector']:
        s = {'fh': 0, 'ch': 0, 'light': 255, 'tag': 0, 'fp': None, 'cp': None}
        for k, v in it:
            if k == 'heightfloor': s['fh'] = atol(v) << 16
            elif k == 'heightceiling': s['ch'] = atol(v) << 16
            elif k == 'texturefloor': s['fp'] = addflat(v)
            elif k == 'textureceiling': s['cp'] = addflat(v)
            elif k == 'lightlevel': s['light'] = atol(v)
            elif k == 'id': s['tag'] = atol(v)
        secs.append(s)
    first = flats[0] if flats else ''
    nvert = len(b['vertex']) + (len(extra) if znodes is not None else 0)
    for s in secs:
        cs += [i32(s['fh']), i32(s['ch']), i32(s['light']), i32(0), i32(s['tag'])]
        cn += [name8(s['fp'] if s['fp'] is not None else first), name8(s['cp'] if s['cp'] is not None else first)]
    for it in b['linedef']:
        l = {'v1': 0, 'v2': 0, 'special': 0, 'args': [0] * 5, 'tag': 0, 'f': -1, 'b': -1}
        for k, v in it:
            if k == 'id': l['tag'] = atol(v)
            elif k == 'special': l['special'] = atol(v)
            elif k == 'v1': l['v1'] = atol(v)
            elif k == 'v2': l['v2'] = atol(v)
            elif k == 'sidefront': l['f'] = atol(v)
            elif k == 'sideback': l['b'] = atol(v)
            elif k.startswith('arg') and not k.startswith('argstring') and len(k) > 3 and k[3:].isdigit() and int(k[3:]) < 5:
                l['args'][int(k[3:])] = atol(v)
        cl += [i32(l['v1']), i32(l['v2']), i32(l['special'])] + [i32(a) for a in l['args']] + [i32(l['tag']), i32(l['f']), i32(l['b'])]
    for it in b['sidedef']:
        d = {'tx': 0, 'ty': 0, 'sector': 0, 'top': '-', 'mid': '-', 'bot': '-'}
        for z in ('ox_t', 'ox_m', 'ox_b', 'oy_t', 'oy_m', 'oy_b'):
            d[z] = 0
        for z in ('sx_t', 'sx_m', 'sx_b', 'sy_t', 'sy_m', 'sy_b'):
            d[z] = 65536
        for k, v in it:
            if k == 'offsetx': d['tx'] = atol(v) << 16
            elif k == 'offsety': d['ty'] = atol(v) << 16
            elif k in ('offsetx_top', 'offsetx_mid', 'offsetx_bottom'): d['ox_' + k[8]] = atol(v) << 16
            elif k in ('offsety_top', 'offsety_mid', 'offsety_bottom'): d['oy_' + k[8]] = atol(v) << 16
            elif k in ('scalex_top', 'scalex_mid', 'scalex_bottom'): d['sx_' + k[7]] = fixed(v)
            elif k in ('scaley_top', 'scaley_mid', 'scaley_bottom'): d['sy_' + k[7]] = fixed(v)
            elif k == 'sector': d['sector'] = atol(v)
            elif k == 'texturetop': d['top'] = v
            elif k == 'texturemiddle': d['mid'] = v
            elif k == 'texturebottom': d['bot'] = v
        cd += [i32(x) for x in (d['tx'], d['ty'], d['ox_t'], d['ox_m'], d['ox_b'], d['oy_t'], d['oy_m'], d['oy_b'],
                                d['sx_t'], d['sx_m'], d['sx_b'], d['sy_t'], d['sy_m'], d['sy_b'], d['sector'])]
        cn += [name8(d['top']), name8(d['mid']), name8(d['bot'])]
    for it in b['thing']:
        t = {'x': 0, 'y': 0, 'z': 0, 'angle': 0, 'type': 0}
        for k, v in it:
            if k == 'x': t['x'] = short(atol(v))
            elif k == 'y': t['y'] = short(atol(v))
            elif k == 'height': t['z'] = short(atol(v))
            elif k == 'angle': t['angle'] = short(atol(v))
            elif k == 'type': t['type'] = atol(v) & 0xFFFF
        ct += [i32(t[z]) for z in ('x', 'y', 'z', 'angle', 'type')]
    return {'counts': [nvert, len(b['sector']), len(b['linedef']), len(b['sidedef']), len(b['thing'])],
            'crc': ['%08x' % crc(x) for x in (cv, cs, cl, cd, ct)], 'names': '%08x' % crc(cn)}


def expect_wad(data):
    lumps = wad_lumps(data)
    return expect(lumps['TEXTMAP'], lumps.get('ZNODES'))


if __name__ == '__main__':
    out = {}
    for p in sys.argv[1:]:
        out[p] = expect_wad(open(p, 'rb').read())
    print(json.dumps(out, indent=1))
