"""PS2-333: the pad button icons as pixel art, drawn at their final size by rules (a shape, an outline, a symbol), so that they are exactly symmetric and crisp.

Reducing the 1200 px sheet to 13 px (tools/ps2/ui_icons.py, kept as the REFERENCE for size, colours and shapes) gives soft, slightly lopsided icons: the phase of the
reduction decides which edge pixel is black. Here every icon is built on an odd-sized grid with its centre on a pixel, from shapes that are symmetric by
construction, and the symbols are explicit pixel masks (a 3x5 font for the letters, 7x7 / 9x9 masks for the PlayStation symbols). ui_icons.py checks the
symmetry of every icon (mirror_h / mirror_v of the picture and of the silhouette) and compares each icon with the reduced sheet (silhouette overlap).

Codes of a canvas: '.' transparent, K black outline, B body (dark grey), W white, M light grey, G green, P pink, R red, U blue.
"""

# the game palette indices of the codes (PLAYPAL of srb2.pk3: 31 black, 23 grey 63, 0 white, 11 grey 159, 113 green 0,223,0, 180 pink 255,74,255, 35 red 255,0,0, 148 blue 115,115,255)
COLORS = {'K': 31, 'B': 23, 'W': 0, 'M': 11, 'G': 113, 'P': 180, 'R': 35, 'U': 148}

FONT = {  # 3x5 letters and digits
    'L': ['W..', 'W..', 'W..', 'W..', 'WWW'],
    'R': ['WW.', 'W.W', 'WW.', 'W.W', 'W.W'],
    '1': ['.W.', 'WW.', '.W.', '.W.', 'WWW'],
    '2': ['WWW', '..W', 'WWW', 'W..', 'WWW'],
    '3': ['WWW', '..W', 'WWW', '..W', 'WWW'],
}


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.p = [['.'] * w for _ in range(h)]

    def get(self, x, y):
        return self.p[y][x] if 0 <= x < self.w and 0 <= y < self.h else '.'

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.p[y][x] = c

    def mask_set(self, x0, y0, rows, mapping=None):
        """blit an ASCII mask: ' ' and '.' leave the pixel, anything else is painted (mapping translates the character)"""
        for dy, row in enumerate(rows):
            for dx, ch in enumerate(row):
                if ch not in ' .':
                    self.set(x0 + dx, y0 + dy, (mapping or {}).get(ch, ch))

    def text(self, x0, y0, s):
        x = x0
        for ch in s:
            self.mask_set(x, y0, FONT[ch])
            x += 4

    def rows(self):
        return [''.join(r) for r in self.p]


def shape_icon(w, h, inside, fill='B'):
    """a body from a predicate inside(x, y) with a one pixel black outline (a pixel of the shape that touches the outside by an edge)"""
    c = Canvas(w, h)
    for y in range(h):
        for x in range(w):
            if inside(x, y):
                c.set(x, y, fill)
    out = []
    for y in range(h):
        for x in range(w):
            if inside(x, y) and any(not inside(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                out.append((x, y))
    for x, y in out:
        c.set(x, y, 'K')
    return c


def disc(n=13):
    """a round button on an odd grid: the ring between radius 5.5 and 6.5 is the outline"""
    r = n // 2
    return shape_icon(n, n, lambda x, y: 0 <= x < n and 0 <= y < n and (x - r) ** 2 + (y - r) ** 2 <= (r + 0.5) ** 2)


def round_rect(w, h, cut_top, cut_bottom=None):
    """a rectangle with the corners cut by a staircase: cut = number of pixels cut along the edge (0: square)"""
    cb = cut_top if cut_bottom is None else cut_bottom

    def inside(x, y):
        if not (0 <= x < w and 0 <= y < h):
            return False
        dx = min(x, w - 1 - x)
        dy_t, dy_b = y, h - 1 - y
        if dy_t < cut_top and dx + dy_t < cut_top:
            return False
        if dy_b < cb and dx + dy_b < cb:
            return False
        return True
    return shape_icon(w, h, inside)


def dpad(n=13, arm=5):
    r = n // 2
    a = arm // 2

    def inside(x, y):
        if not (0 <= x < n and 0 <= y < n):
            return False
        vert = abs(x - r) <= a
        horz = abs(y - r) <= a
        if not (vert or horz):
            return False
        # the end of an arm loses its corner pixels
        if (y in (0, n - 1) or x in (0, n - 1)) and ((vert and abs(x - r) == a) or (horz and abs(y - r) == a)):
            return False
        return True
    return shape_icon(n, n, inside)


# --- symbols of the face buttons (centred on the pixel (6, 6) of a 13x13 disc) ---
CIRCLE_SYMBOL = [
    '..RRRRR..',
    '.RRRRRRR.',
    'RRR...RRR',
    'RR.....RR',
    'RR.....RR',
    'RR.....RR',
    'RRR...RRR',
    '.RRRRRRR.',
    '..RRRRR..',
]
CROSS_SYMBOL = [
    'UU...UU',
    'UUU.UUU',
    '.UUUUU.',
    '..UUU..',
    '.UUUUU.',
    'UUU.UUU',
    'UU...UU',
]
SQUARE_SYMBOL = [
    'PPPPPPP',
    'PPPPPPP',
    'PP...PP',
    'PP...PP',
    'PP...PP',
    'PPPPPPP',
    'PPPPPPP',
]
TRIANGLE_SYMBOL = [
    '....G....',
    '...GGG...',
    '...G.G...',
    '..GG.GG..',
    '..G...G..',
    '.GG...GG.',
    'GGGGGGGGG',
]

ARROW_UP = [
    '....K....',
    '...KWK...',
    '..KWWWK..',
    '.KWWWWWK.',
    'KKKKKKKKK',
]


def rot90_cw(rows):
    h, w = len(rows), len(rows[0])
    return [''.join(rows[h - 1 - y][x] for y in range(h)) for x in range(w)]


def rot90_ccw(rows):
    h, w = len(rows), len(rows[0])
    return [''.join(rows[y][w - 1 - x] for y in range(h)) for x in range(w)]


def flip_v(rows):
    return rows[::-1]


def arrow(direction):
    """a white arrow head with a black outline, 9x5 (up, down) or 5x9 (left, right); its axis of symmetry is the middle row / column of the canvas"""
    if direction == 'UP':
        rows = ARROW_UP
    elif direction == 'DOWN':
        rows = flip_v(ARROW_UP)
    elif direction == 'LEFT':
        rows = rot90_ccw(ARROW_UP)
    else:
        rows = rot90_cw(ARROW_UP)
    c = Canvas(len(rows[0]), len(rows))
    c.mask_set(0, 0, rows)
    return c


def sym_face(name):
    c = disc(13)
    sym = {'CIRCLE': (CIRCLE_SYMBOL, 2, 2), 'CROSS': (CROSS_SYMBOL, 3, 3), 'SQUARE': (SQUARE_SYMBOL, 3, 3), 'TRIANGLE': (TRIANGLE_SYMBOL, 2, 3)}[name]
    c.mask_set(sym[1], sym[2], sym[0])
    return c


def stick(letter, dirs='', text=None):
    c = disc(13)
    if text:
        c.text(3, 4, text)
    else:
        c.text(5, 4, letter)
    if 'U' in dirs:
        c.mask_set(5, 1, ['.W.', 'WWW'])
        c.set(6, 1, 'W')
    if 'D' in dirs:
        c.mask_set(5, 10, ['WWW', '.W.'])
    if 'L' in dirs:
        c.mask_set(1, 5, ['.W', 'WW', '.W'])
    if 'R' in dirs:
        c.mask_set(10, 5, ['W.', 'WW', 'W.'])
    return c


def dpad_icon(dirs):
    c = dpad(13, 5)
    if 'U' in dirs:
        c.mask_set(5, 1, ['.W.', 'WWW'])
    if 'D' in dirs:
        c.mask_set(5, 10, ['WWW', '.W.'])
    if 'L' in dirs:
        c.mask_set(1, 5, ['.W', 'WW', '.W'])
    if 'R' in dirs:
        c.mask_set(10, 5, ['W.', 'WW', 'W.'])
    return c


def rotation_stick(letter):
    """the stick turning counter-clockwise: a white ring round the letter, open at the upper left, ending in an arrow head that points down"""
    import math
    c = disc(13)
    c.text(5, 4, letter)
    for y in range(13):
        for x in range(13):
            dx, dy = x - 6, 6 - y
            d = math.hypot(dx, dy)
            ang = math.degrees(math.atan2(dy, dx)) % 360
            if 4.3 <= d <= 5.2 and not (118 <= ang <= 168):
                c.set(x, y, 'W')
    # the arrow head at the open end on the left side (it points down, the way the ring runs counter-clockwise)
    c.mask_set(1, 4, ['WWW', '.W.'])
    return c


def start_icon():
    h, w = 11, 11
    c = shape_icon(w, h, lambda x, y: 0 <= x < w and 0 <= y < h and x <= (w - 1) - 2 * abs(y - h // 2))
    return c


def build():
    """-> dict name -> Canvas; the names are those of tools/ps2/ui_icons.py LAYOUT"""
    icons = {}
    icons['SELECT'] = round_rect(15, 7, 2)
    icons['START'] = start_icon()
    for n in ('TRIANGLE', 'SQUARE', 'CIRCLE', 'CROSS'):
        icons[n] = sym_face(n)
    for k, txt in (('L1', 'L1'), ('R1', 'R1')):
        c = round_rect(15, 9, 2)
        c.text(4, 2, txt)
        icons[k] = c
    for k, txt in (('L2', 'L2'), ('R2', 'R2')):
        c = round_rect(15, 13, 4, 1)
        c.text(4, 4, txt)
        icons[k] = c
    icons['LSTICK'] = stick('L')
    icons['RSTICK'] = stick('R')
    for tag, d in (('DOWN', 'D'), ('LEFT', 'L'), ('RIGHT', 'R'), ('UP', 'U'), ('LR', 'LR'), ('ALL', 'UDLR'), ('UD', 'UD')):
        icons['DPAD_' + tag] = dpad_icon(d)
        icons['LSTICK_' + tag] = stick('L', d)
        icons['RSTICK_' + tag] = stick('R', d)
    icons['L3'] = stick('', '', 'L3')
    icons['R3'] = stick('', '', 'R3')
    icons['LSTICK_ROT'] = rotation_stick('L')
    icons['RSTICK_ROT'] = rotation_stick('R')
    for d in ('LEFT', 'RIGHT', 'DOWN', 'UP'):
        icons['ARROW_' + d] = arrow(d)
    return icons


# ---------------------------------------------------------------------------------------------------------------------------------------------------------
# The SMALL set: the same buttons for the lines of text (7 px tall: the cap height of the menu font; the lines of the menus are 8 px apart). Drawn by hand on odd grids
# (7x7, 9x5 .. 11x7) with the centre on a pixel; the 3x3 symbols of the face buttons are explicit masks. NOT a reduction of the large icons.
# ---------------------------------------------------------------------------------------------------------------------------------------------------------
SMALL_SYMBOLS = {
    'CROSS': ['U.U', '.U.', 'U.U'],
    'CIRCLE': ['.R.', 'R.R', '.R.'],
    'SQUARE': ['PPP', 'P.P', 'PPP'],
    'TRIANGLE': ['.G.', 'G.G', 'GGG'],
}
PLAY = ['W..', 'WW.', 'WWW', 'WW.', 'W..']  # the arrow of Start (3x5)


def plus7():
    """a plus with arms three pixels wide: the D-pad"""
    return shape_icon(7, 7, lambda x, y: 0 <= x < 7 and 0 <= y < 7 and (abs(x - 3) <= 1 or abs(y - 3) <= 1))


def dpad_small(dirs):
    c = plus7()
    if 'U' in dirs:
        c.mask_set(3, 1, ['W', 'W'])
    if 'D' in dirs:
        c.mask_set(3, 4, ['W', 'W'])
    if 'L' in dirs:
        c.mask_set(1, 3, ['WW'])
    if 'R' in dirs:
        c.mask_set(4, 3, ['WW'])
    return c


def build_small():
    """-> dict name -> Canvas (names of build(): only the icons that the text tokens and the key names use, plus the two sticks)"""
    icons = {}
    for n, sym in SMALL_SYMBOLS.items():
        c = disc(7)
        c.mask_set(2, 2, sym)
        icons[n] = c
    for n, cut in (('L1', 1), ('R1', 1), ('L2', 2), ('R2', 2)):
        c = round_rect(11, 7, cut)
        c.text(2, 1, n)
        icons[n] = c
    for n in ('L3', 'R3'):
        c = round_rect(11, 7, 2)
        c.text(2, 1, n)
        icons[n] = c
    for n in ('LSTICK', 'RSTICK'):
        c = disc(7)
        c.text(2, 1, n[0])
        icons[n] = c
    c = round_rect(9, 7, 2)
    c.mask_set(3, 1, PLAY)
    icons['START'] = c
    c = round_rect(9, 5, 2)
    c.mask_set(3, 2, ['WWW'])
    icons['SELECT'] = c
    for tag, d in (('UP', 'U'), ('DOWN', 'D'), ('LEFT', 'L'), ('RIGHT', 'R'), ('UD', 'UD'), ('LR', 'LR')):
        icons['DPAD_' + tag] = dpad_small(d)
    return icons
