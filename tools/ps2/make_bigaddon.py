"""A large synthetic add-on for the loading measurements of OPT12-LOAD (PS2-LOAD-18): many Lua scripts, a big SOC file, PNG textures / sprites / graphics and sounds in one pk3.

usage: make_bigaddon.py [--out build/addons] [--scripts 100] [--sprites 200] [--seed 1]
Writes BIG.pk3 and BIG.expected.json (what the files hold: counts, sizes, CRC32 of the lumps).
The scripts look like a character / gametype mod: constants tables, helper functions (string and table work), freeslot()s, addHook()s of the common hooks, HUD drawing, console
variables and commands; each also builds a table of a few hundred entries when it runs so that loading is not only parsing. Nothing in it needs the base game to be present
except the usual libraries, and every script runs without errors on the PC build (checked by tools/ps2/lua_equiv.py before the numbers are taken).
"""
import argparse
import json
import random
import struct
import sys
import zipfile
import zlib
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def png(w, h, rnd, alpha=True):
    img = Image.new('RGBA', (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            inside = (x - w / 2) ** 2 + (y - h / 2) ** 2 < (min(w, h) / 2 - 1) ** 2
            if inside:
                px[x, y] = ((x * 5 + rnd.randrange(24)) & 255, (y * 5 + rnd.randrange(24)) & 255, ((x + y) * 3) & 255, 255)
            else:
                px[x, y] = (0, 0, 0, 0 if alpha else 255)
    import io
    b = io.BytesIO()
    img.save(b, 'PNG')
    return b.getvalue()


def lua_script(n, rnd):
    names = ['alpha', 'bravo', 'charlie', 'delta', 'echo', 'foxtrot', 'golf', 'hotel']
    tag = 'MOD%03d' % n
    lines = []
    w = lines.append
    w('-- %s: generated mod script %d' % (tag, n))
    w('local %s = {}' % tag)
    w('%s.name = "%s"' % (tag, tag))
    w('%s.version = %d' % (tag, 100 + n))
    w('local consts = {')
    for i in range(24):
        w('\t%s_%d = %d,' % (rnd.choice(names), i, rnd.randrange(-5000, 90000)))
    w('}')
    w('local function clamp(v, lo, hi)')
    w('\tif v < lo then return lo elseif v > hi then return hi end')
    w('\treturn v')
    w('end')
    for f in range(8):
        w('local function helper%d(a, b, c)' % f)
        w('\tlocal r = 0')
        w('\tfor i = 1, %d do' % rnd.randrange(3, 9))
        w('\t\tr = r + (a * i) %% %d - (b >> %d) + (c & 255)' % (rnd.randrange(7, 400), rnd.randrange(0, 6)))
        w('\t\tif r > %d then r = r - %d end' % (rnd.randrange(1000, 90000), rnd.randrange(1, 900)))
        w('\tend')
        w('\treturn clamp(r, -%d, %d)' % (rnd.randrange(1000, 60000), rnd.randrange(1000, 60000)))
        w('end')
    w('local function describe(t)')
    w('\tlocal parts = {}')
    w('\tfor k, v in pairs(t) do parts[#parts + 1] = string.format("%s=%d", k, v) end')
    w('\ttable.sort(parts)')
    w('\treturn table.concat(parts, ",")')
    w('end')
    w('%s.table = {}' % tag)
    w('for i = 1, %d do' % rnd.randrange(150, 320))
    w('\t%s.table[i] = { id = i, name = "entry" .. i, value = helper%d(i, %d, %d), tag = "%s" .. (i %% 7) }' % (tag, rnd.randrange(8), rnd.randrange(1, 90), rnd.randrange(1, 255), tag))
    w('end')
    w('%s.summary = describe(consts)' % tag)
    for s in range(2):
        w('freeslot("MT_%s_%d", "S_%s_%d", "sfx_%s%d")' % (tag, s, tag, s, tag.lower(), s))
    w('local count_%s = 0' % tag)
    w('addHook("MapLoad", function(map) count_%s = count_%s + 1 end)' % (tag, tag))
    w('addHook("MobjThinker", function(mo)')
    w('\tif not (mo and mo.valid) then return end')
    w('\tmo.extravalue1 = (mo.extravalue1 or 0) + 1')
    w('end, MT_%s_0)' % tag)
    w('addHook("PlayerThink", function(player)')
    w('\tif player.mo and player.mo.valid and leveltime %% %d == 0 then' % rnd.randrange(30, 200))
    w('\t\tplayer.%s_ticks = (player.%s_ticks or 0) + 1' % (tag.lower(), tag.lower()))
    w('\tend')
    w('end)')
    w('local cv_%s = CV_RegisterVar({name = "%s_opt", defaultvalue = "On", flags = 0, PossibleValue = CV_OnOff})' % (tag, tag.lower()))
    w('COM_AddCommand("%s_info", function(player)' % tag.lower())
    w('\tCONS_Printf(player, "%s " .. #%s.table .. " " .. %s.summary:sub(1, 20))' % (tag, tag, tag))
    w('end)')
    w('hud.add(function(v, player)')
    w('\tif not cv_%s.value then return end' % tag)
    w('\tv.drawString(%d, %d, "%s", V_ALLOWLOWERCASE|V_SNAPTOTOP, "small")' % (rnd.randrange(8, 200), rnd.randrange(8, 20), tag))
    w('end, "game")')
    for extra in range(rnd.randrange(6, 20)):
        w('function %s.func%d(x)' % (tag, extra))
        w('\tlocal acc = {}')
        w('\tfor i = 1, %d do acc[#acc + 1] = (x * i) %% %d end' % (rnd.randrange(4, 30), rnd.randrange(3, 99)))
        w('\treturn table.concat(acc, ":")')
        w('end')
    w('return %s' % tag)
    return '\n'.join(lines) + '\n'


def soc_file(n_objects, rnd):
    out = ['# generated SOC: %d objects with frames' % n_objects, 'Freeslot']
    out += ['SPR_BG%02d' % i for i in range(100)]
    for i in range(n_objects):
        out.append('MT_BIGSOC%d' % i)
        out.append('S_BIGSOC%d_A' % i)
        out.append('S_BIGSOC%d_B' % i)
    out.append('')
    for i in range(n_objects):
        out += ['Object MT_BIGSOC%d' % i, 'MapThingNum = %d' % (9000 + i), 'SpawnState = S_BIGSOC%d_A' % i, 'SpawnHealth = %d' % rnd.randrange(1, 40),
                'Radius = %d*FRACUNIT' % rnd.randrange(4, 40), 'Height = %d*FRACUNIT' % rnd.randrange(8, 80), 'Speed = %d*FRACUNIT/2' % rnd.randrange(1, 30),
                'Mass = %d' % rnd.randrange(10, 400), 'Flags = MF_NOGRAVITY|MF_SOLID', '',
                'Frame S_BIGSOC%d_A' % i, 'SpriteNumber = SPR_PLAY', 'SpriteSubNumber = %d' % rnd.randrange(0, 20), 'Duration = %d' % rnd.randrange(2, 40), 'Next = S_BIGSOC%d_B' % i, '',
                'Frame S_BIGSOC%d_B' % i, 'SpriteNumber = SPR_PLAY', 'SpriteSubNumber = %d' % rnd.randrange(0, 20), 'Duration = %d' % rnd.randrange(2, 40), 'Next = S_BIGSOC%d_A' % i, '']
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build/addons'))
    ap.add_argument('--scripts', type=int, default=100)
    ap.add_argument('--sprites', type=int, default=200)
    ap.add_argument('--textures', type=int, default=40)
    ap.add_argument('--graphics', type=int, default=80)
    ap.add_argument('--soc', type=int, default=150)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    entries = []  # (name, data)
    for i in range(a.scripts):
        entries.append(('Lua/MOD%03d.lua' % i, lua_script(i, rnd).encode()))
    entries.append(('SOC/BIGSOC.soc', soc_file(a.soc, rnd).encode()))
    for i in range(a.textures):
        entries.append(('Textures/BIGT%03d.png' % i, png(64, 64, rnd, alpha=False)))
    for i in range(a.graphics):
        entries.append(('Graphics/BIGG%03d.png' % i, png(40 + (i % 5) * 8, 40, rnd)))
    for i in range(a.sprites):
        # sprite lumps: four letters of sprite name + frame + rotation (BG00 .. BG99 / frames A..)
        entries.append(('Sprites/BG%02dA0.png' % (i % 100) if i < 100 else 'Sprites/BG%02dB0.png' % (i % 100), png(48, 48, rnd)))
    for i in range(30):
        pcm = bytes(rnd.randrange(256) for _ in range(6000))
        entries.append(('Sounds/DSBIG%02d' % i, struct.pack('<HHI', 3, 11025, len(pcm) + 32) + b'\x80' * 16 + pcm + b'\x80' * 16))
    path = out / 'BIG.pk3'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, data in entries:
            z.writestr(name, data)
    exp = {'file': path.name, 'size': path.stat().st_size, 'lumps': len(entries),
           'scripts': a.scripts, 'lua_bytes': sum(len(d) for n, d in entries if n.endswith('.lua')),
           'crc': {n: zlib.crc32(d) & 0xFFFFFFFF for n, d in entries}}
    (out / 'BIG.expected.json').write_text(json.dumps(exp, indent=1))
    print('wrote %s: %d bytes, %d lumps, %d Lua bytes' % (path, exp['size'], exp['lumps'], exp['lua_bytes']))


if __name__ == '__main__':
    sys.exit(main())
