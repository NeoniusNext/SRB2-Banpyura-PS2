"""Compare the FT_ lines of an engine log (PCSX2 boot.txt or a host run) with the expectation of make_addons.py.

usage: ftest_check.py zip BOOT.TXT build/opt6-f/addons/ZT.expected.json [--wad N] [--other BOOT2.TXT]
  zip: every lump of wad N (default 4) must have the size and CRC32 of its zip entry (all of them, none missing, none extra);
       --other: the FT_PATCH lines must be identical to those of another log (host profile vs PS2: same engine code on two CPUs)
usage: ftest_check.py udmf BOOT.TXT build/opt7-f/addons/UD.expected.json [--names 17,..]
  udmf: every "FT_LEVEL map ..." of the log (written by -ftest-level) must have the counts and checksums that udmf_ref.py computed from
        the TEXTMAP lump; the flat/texture name checksum (FT_LNAM) is compared only for the maps in --names (the others use textures
        of the add-on that the test pack does not carry)
usage: ftest_check.py same PS2_LOG PC_LOG [--skip tag1,tag2]
  same: the FTLUA lines of the two logs (engine log of the PS2 run, stdout of the PC game running the same add-on) must be identical, line by line;
        --skip names the line tags (second word, e.g. "live") that are left out of both
Exit code 0 when everything agrees; the differences are printed.
"""
import argparse
import json
import re
import sys


def lumps(text, wad):
    out = {}
    for m in re.finditer(r'^FT_LUMP (\d+) (\d+) (.*) (\d+) ([0-9a-f]{8})\s*$', text, re.M):
        if int(m.group(1)) == wad:
            out[m.group(3)] = (int(m.group(4)), m.group(5))
    return out


def patches(text):
    return {m.group(1): m.group(2) for m in re.finditer(r'^FT_PATCH (\S+) (.*?)\s*$', text, re.M)}


def lua_expect():
    import pathlib
    root = pathlib.Path(__file__).resolve().parents[2]
    sine = [int(x) for x in re.findall(r'-?\d+', (root / 'src/t_fsin.c').read_text().split('{', 1)[1].split('}')[0])]
    ang1 = 0x00B60B60  # ANG1: the engine's constant (ANGLE_45 / 45 rounded down)
    sin30 = sine[((ang1 * 30) & 0xFFFFFFFF) >> 19]
    cos60 = sine[(((ang1 * 60) & 0xFFFFFFFF) >> 19) + 2048]
    facon = [int(x) if x != 'ANGLE_MAX' else 4294967295 for x in re.findall(r'ANGLE_MAX|-?\d+', (root / 'src/t_facon.c').read_text().split('{', 1)[1].split('}')[0])]
    fa = lambda x: facon[x + 65536]
    return [
        r'FTLUA loaded',
        r'FTLUA fix 98304 98304 16384 \d+',
        r'FTLUA trig %d %d 536870912 5898240' % (sin30, cos60),
        r'FTLUA acos %d %d %d %d %d' % (fa(0), fa(32768), fa(-32768), fa(12345), (-fa(16384) + 0x40000000) & 0xFFFFFFFF),
        r'FTLUA int 3 -3 1024 2 13',
        r'FTLUA tbl 1,4,9,16,25,36,49,64,81,100 #10',
        r'FTLUA str 42 x 00007abababABCell120',
        r'FTLUA pcall false boom',
        r'FTLUA closure 3',
        r'FTLUA sort aa,mm,zz',
        r'FTLUA coroutine 11 15',
        r'FTLUA const 3 \d+ \d+ \d+ \d+ 35 16',
        r'FTLUA freeslot mt st',
        r'FTLUA info 1310720 -1',
        r'FTLUA done',
        r'FTLUA soc speed 98304 doomednum 3999 radius 1310720 height 1966080',
        r'FTLUA level 1 leveltime \d+ player mo',
        r'FTLUA spawn true type ok radius 1310720',
        r'FTLUA alive true health 1',
        r'FTLUA think36',
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kind', choices=['zip', 'lua', 'udmf', 'same'])
    ap.add_argument('--skip', default='')
    ap.add_argument('--names', default='17')
    ap.add_argument('log')
    ap.add_argument('expected', nargs='?', default='')
    ap.add_argument('--wad', type=int, default=4)
    ap.add_argument('--other', default='')
    a = ap.parse_args()
    if a.kind == 'same':
        skip = set(x for x in a.skip.split(',') if x)
        def ft(path):
            return [l.strip() for l in open(path, errors='replace') if l.startswith('FTLUA ') and l.split()[1:2] and l.split()[1] not in skip]
        mine, theirs = ft(a.log), ft(a.expected)
        bad = 0
        for n in range(max(len(mine), len(theirs))):
            x = mine[n] if n < len(mine) else '<missing>'
            y = theirs[n] if n < len(theirs) else '<missing>'
            if x != y:
                print('DIFF line', n + 1, '\n  ps2:', x, '\n  pc: ', y)
                bad += 1
        print(f'same: {len(mine)} lines here, {len(theirs)} in the PC log, {bad} different')
        print('OK' if not bad else 'FAILED')
        return 1 if bad else 0
    text = open(a.log, errors='replace').read()
    if a.kind == 'lua':
        lines = [l.strip() for l in text.splitlines() if l.startswith('FTLUA ')]
        bad = 0
        for pat in lua_expect():
            if not any(re.fullmatch(pat, l) for l in lines):
                print('MISSING', pat)
                bad += 1
        print(f'lua: {len(lua_expect())} expected lines, {len(lines)} FTLUA lines, {bad} missing')
        print('OK' if not bad else 'FAILED')
        return 1 if bad else 0
    if a.kind == 'udmf':
        exp = json.load(open(a.expected))
        seen = {}
        lines = text.splitlines()
        for n, l in enumerate(lines):
            m = re.match(r'FT_LEVEL (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)\s*$', l)
            if not m:
                continue
            crcs = re.match(r'FT_LCRC (\w{8}) (\w{8}) (\w{8}) (\w{8}) (\w{8})\s*$', lines[n + 1])
            nam = re.match(r'FT_LNAM (\w{8})', lines[n + 2])
            seen[m.group(1)] = (int(m.group(2)), [int(x) for x in m.groups()[2:]], list(crcs.groups()), nam.group(1))
        bad = 0
        for num, e in exp.items():
            g = seen.get(num)
            if g is None:
                print('MISSING map', num)
                bad += 1
                continue
            if g[0] != 1:
                print('map', num, 'was not loaded as UDMF')
                bad += 1
            if g[1] != e['counts']:
                print('DIFF map', num, 'counts engine', g[1], 'expected', e['counts'])
                bad += 1
            for i, nm in enumerate(['vertices', 'sectors', 'lines', 'sides', 'things']):
                if g[2][i] != e['crc'][i]:
                    print('DIFF map', num, nm, 'engine', g[2][i], 'expected', e['crc'][i])
                    bad += 1
            if num in a.names.split(',') and g[3] != e['names']:
                print('DIFF map', num, 'names engine', g[3], 'expected', e['names'])
                bad += 1
            print('map', num, 'counts', g[1], 'crc', ' '.join(g[2]), 'names', g[3], '(compared)' if num in a.names.split(',') else '(not compared)')
        print(f'udmf: {len(exp)} maps expected, {len(seen)} levels loaded, {bad} problems')
        print('OK' if not bad else 'FAILED')
        return 1 if bad else 0
    exp = json.load(open(a.expected))
    got = lumps(text, a.wad)
    bad = 0
    for name, e in exp['lumps'].items():
        g = got.get(name)
        if g is None:
            print('MISSING', name)
            bad += 1
        elif g != (e['size'], e['crc32']):
            print('DIFF', name, 'engine', g, 'expected', (e['size'], e['crc32']))
            bad += 1
    for name in got:
        if name not in exp['lumps'] and not name.endswith('/'):
            print('EXTRA', name)
            bad += 1
    print(f'lumps: {len(exp["lumps"])} expected, {len(got)} found, {bad} problems')
    if a.other:
        mine, other = patches(text), patches(open(a.other, errors='replace').read())
        for k in sorted(set(mine) | set(other)):
            if mine.get(k) != other.get(k):
                print('PATCH DIFF', k, mine.get(k), other.get(k))
                bad += 1
        print(f'patches: {len(mine)} here, {len(other)} in the other log, compared')
    print('OK' if not bad else 'FAILED')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
