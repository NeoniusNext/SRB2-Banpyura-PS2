"""A synthetic UDMF test map with its own GL nodes (OPT10-X; the user's ZombieEscape2 maps of OPT8 are not on the Linux box).

usage: make_udmf_map.py [--out build/opt10-x/addons] [--map 99] [--cols 4] [--rows 3]
Writes UM.pk3 (Maps/MAP<nn>.wad = MAP<nn> marker + TEXTMAP + ZNODES (XGL3) + ENDMAP, and SOC/UMSOC.soc with a level header) and UM.expected.json (the
counts/CRCs tools/ps2/udmf_ref.py computes from the TEXTMAP, for tools/ps2/ftest_check.py udmf).
The map is a COLS x ROWS grid of 256-unit square rooms, each its own sector (own floor height, light, flat; sky ceiling), every room edge a linedef
(one-sided on the border, two-sided inside), so that every BSP leaf is one convex room and no mini-segs are needed. The BSP splits the longer
dimension at a column/row border (the partition line lies on linedefs). Things: player 1 start in room (0,0), rings, two crawlas, one more start.
The nodes are checked here by locating every room centre through the tree (the same walk as R_PointInSubsector) before anything is written.
"""
import argparse
import json
import struct
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import udmf_ref  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
CELL = 256
FLATS = ['GFZFLR01', 'GFZFLR02', 'GFZFLR03', 'GFZFLR04', 'GFZFLR05', 'GFZFLR06', 'GFZFLR07', 'GFZFLR08']
WALLS = ['GFZBRICK', 'GFZROCK2', 'GFZCRACK']


def build(cols, rows):
    nv = (cols + 1) * (rows + 1)
    vid = lambda i, j: j * (cols + 1) + i
    cell = lambda c, r: r * cols + c
    lines = []  # (v1, v2, front sector, back sector or None)
    for j in range(rows + 1):
        for i in range(cols):
            if j == 0:
                lines.append((vid(i + 1, 0), vid(i, 0), cell(i, 0), None))
            elif j == rows:
                lines.append((vid(i, j), vid(i + 1, j), cell(i, rows - 1), None))
            else:
                lines.append((vid(i, j), vid(i + 1, j), cell(i, j - 1), cell(i, j)))
    for i in range(cols + 1):
        for j in range(rows):
            if i == 0:
                lines.append((vid(0, j), vid(0, j + 1), cell(0, j), None))
            elif i == cols:
                lines.append((vid(cols, j + 1), vid(cols, j), cell(cols - 1, j), None))
            else:
                lines.append((vid(i, j), vid(i, j + 1), cell(i, j), cell(i - 1, j)))
    return nv, lines


def textmap(cols, rows, mapnum):
    nv, lines = build(cols, rows)
    out = ['// OPT10-X synthetic UDMF map: %dx%d rooms' % (cols, rows), 'namespace = "srb2";', '']
    for j in range(rows + 1):
        for i in range(cols + 1):
            out += ['vertex', '{', '\tx = %d.0;' % (i * CELL), '\ty = %d.0;' % (j * CELL), '}', '']
    for r in range(rows):
        for c in range(cols):
            s = r * cols + c
            out += ['sector', '{', '\theightfloor = %d;' % (((c + r * 2) % 4) * 32), '\theightceiling = 768;',
                    '\ttexturefloor = "%s";' % FLATS[s % len(FLATS)], '\ttextureceiling = "F_SKY1";', '\tlightlevel = %d;' % (160 + 12 * (s % 6)), '}', '']
    sides = []
    for li, (v1, v2, f, b) in enumerate(lines):
        two = b is not None
        out += ['linedef', '{', '\tv1 = %d;' % v1, '\tv2 = %d;' % v2, '\tsidefront = %d;' % len(sides)]
        sides.append((f, two))
        if two:
            out.append('\tsideback = %d;' % len(sides))
            sides.append((b, two))
            out.append('\ttwosided = true;')
        else:
            out.append('\tblocking = true;')
        out += ['}', '']
    for si, (sec, two) in enumerate(sides):
        out += ['sidedef', '{', '\tsector = %d;' % sec]
        if two:
            out += ['\ttexturetop = "%s";' % WALLS[0], '\ttexturebottom = "%s";' % WALLS[1]]
        else:
            out.append('\ttexturemiddle = "%s";' % WALLS[(si % 2) * 2])
        out += ['}', '']
    things = [(1, CELL // 2, CELL // 2, 0, 0)]  # player 1 start
    for r in range(rows):
        for c in range(cols):
            x, y = c * CELL + CELL // 2, r * CELL + CELL // 2
            if (c, r) != (0, 0):
                things.append((300, x, y, 0, 0))  # ring
    things += [(100, 2 * CELL + 64, CELL + 64, 90, 0), (100, 3 * CELL + 100, 100, 180, 0), (2, CELL + 64, CELL + 64, 0, 0)]
    for (t, x, y, a, z) in things:
        out += ['thing', '{', '\tx = %d.0;' % x, '\ty = %d.0;' % y, '\tangle = %d;' % a, '\ttype = %d;' % t, '\tskill2 = true;', '\tskill3 = true;', '\tskill4 = true;',
                '\tskill5 = true;', '\tskill1 = true;', '\tskill6 = true;', '}', '']
    return '\n'.join(out) + '\n', nv, lines


def znodes(cols, rows, nv, lines):
    cell = lambda c, r: r * cols + c
    edge = {}  # (a, b) -> (linedef, side)
    for li, (v1, v2, f, b) in enumerate(lines):
        edge[(v1, v2)] = (li, 0)
        if b is not None:
            edge[(v2, v1)] = (li, 1)
    vid = lambda i, j: j * (cols + 1) + i
    subs = []  # per cell: list of (vertex, linedef, side)
    for r in range(rows):
        for c in range(cols):
            loop = [vid(c, r), vid(c, r + 1), vid(c + 1, r + 1), vid(c + 1, r)]  # clockwise with y up: up the left edge, along the top, down the right, back along the bottom
            segs = []
            for k in range(4):
                a, b = loop[k], loop[(k + 1) % 4]
                li, side = edge[(a, b)]
                segs.append((a, li, side))
            subs.append(segs)
    # partners: the seg of the other room on the same linedef
    first = []
    n = 0
    for s in subs:
        first.append(n)
        n += len(s)
    where = {}  # (linedef, side) -> global seg index
    for si, s in enumerate(subs):
        for k, (a, li, side) in enumerate(s):
            where[(li, side)] = first[si] + k
    nodes = []

    def rec(c0, c1, r0, r1):
        """returns the child id (high bit = subsector) and the bounding box (top, bottom, left, right) in map units"""
        if c1 - c0 == 1 and r1 - r0 == 1:
            return 0x80000000 | cell(c0, r0), (r1 * CELL, r0 * CELL, c0 * CELL, c1 * CELL)
        if c1 - c0 >= r1 - r0:
            cm = (c0 + c1) // 2
            x, y, dx, dy = cm * CELL, r0 * CELL, 0, (r1 - r0) * CELL  # direction north: the right side (child 0) is the east
            right = rec(cm, c1, r0, r1)
            left = rec(c0, cm, r0, r1)
        else:
            rm = (r0 + r1) // 2
            x, y, dx, dy = c0 * CELL, rm * CELL, (c1 - c0) * CELL, 0  # direction east: the right side is the south
            right = rec(c0, c1, r0, rm)
            left = rec(c0, c1, rm, r1)
        nodes.append((x, y, dx, dy, right[1], left[1], right[0], left[0]))
        return len(nodes) - 1, (max(right[1][0], left[1][0]), min(right[1][1], left[1][1]), min(right[1][2], left[1][2]), max(right[1][3], left[1][3]))

    root, _ = rec(0, cols, 0, rows)
    assert root == len(nodes) - 1, 'the root must be the last node'
    b = bytearray(b'XGL3')
    b += struct.pack('<II', nv, 0)
    b += struct.pack('<I', len(subs))
    for s in subs:
        b += struct.pack('<I', len(s))
    b += struct.pack('<I', n)
    for si, s in enumerate(subs):
        for k, (a, li, side) in enumerate(s):
            partner = where.get((li, 1 - side), 0xFFFFFFFF)
            b += struct.pack('<IIIB', a, partner, li, side)
    b += struct.pack('<i', len(nodes))
    for (x, y, dx, dy, rb, lb, rc, lc) in nodes:
        b += struct.pack('<iiii', x << 16, y << 16, dx << 16, dy << 16)
        b += struct.pack('<8h', *rb, *lb)
        b += struct.pack('<II', rc, lc)
    return bytes(b), nodes


def locate(nodes, cols, x, y):
    """R_PointOnSide walk from the root: the subsector number of the point"""
    n = len(nodes) - 1
    while True:
        nx, ny, dx, dy, rb, lb, rc, lc = nodes[n]
        side = 0 if dy * (x - nx) - dx * (y - ny) > 0 else 1  # R_PointOnSide: front (right of the direction) = 0
        child = rc if side == 0 else lc
        if child & 0x80000000:
            return child & 0x7FFFFFFF
        n = child


def wad(lumps):
    body = bytearray()
    entries = []
    for name, data in lumps:
        entries.append((12 + len(body), len(data), name))
        body += data
    dirofs = 12 + len(body)
    d = b''.join(struct.pack('<II8s', o, s, n.encode().ljust(8, b'\0')) for o, s, n in entries)
    return b'PWAD' + struct.pack('<II', len(lumps), dirofs) + bytes(body) + d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build/opt10-x/addons'))
    ap.add_argument('--map', type=int, default=99)
    ap.add_argument('--cols', type=int, default=4)
    ap.add_argument('--rows', type=int, default=3)
    ap.add_argument('--name', default='UM', help='base name of the pk3 (UM)')
    ap.add_argument('--comment', action='store_true', help='PS2-LOAD-25: comments in the middle of the TEXTMAP (the one-pass block scan of the engine refuses such a text: this map takes the count pass + parse path and has to give the same level)')
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    tm, nv, lines = textmap(a.cols, a.rows, a.map)
    if a.comment:
        parts = tm.split('\n\n')
        mid = len(parts) // 2
        parts[mid] = '// a line comment between two blocks\n/* and a block\n comment */\n' + parts[mid]
        tm = '\n\n'.join(parts) + '\n// the end\n'
    zn, nodes = znodes(a.cols, a.rows, nv, lines)
    for r in range(a.rows):
        for c in range(a.cols):
            got = locate(nodes, a.cols, c * CELL + CELL // 2, r * CELL + CELL // 2)
            assert got == r * a.cols + c, (c, r, got)
    name = 'MAP%02d' % a.map
    data = wad([(name, b''), ('TEXTMAP', tm.encode()), ('ZNODES', zn), ('ENDMAP', b'')])
    soc = 'Level %d\nLevelName = UDMF Rooms\nTypeOfLevel = Race,Match,Coop\nAct = 1\nNoZone = 1\nSkyNum = 1\nMusic = NONE\nNextLevel = %d\n\n' % (a.map, 1)
    path = out / (a.name + '.pk3')
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('Maps/%s.wad' % name, data)
        z.writestr('SOC/UMSOC.soc', soc.replace('\n', '\r\n'))
    exp = {str(a.map): udmf_ref.expect_wad(data)}
    (out / (a.name + '.expected.json')).write_text(json.dumps(exp, indent=1))
    (out / ('%s_%s.wad' % (a.name, name))).write_bytes(data)
    print('wrote', path, path.stat().st_size, 'bytes;', a.cols * a.rows, 'rooms,', len(lines), 'linedefs,', nv, 'vertices,', len(nodes), 'nodes;', json.dumps(exp[str(a.map)]['counts']))


if __name__ == '__main__':
    main()
