"""Test add-ons for the content systems that PS2_PROFILE used to cut out (OPT6-F, docs/GATES/g1/opt6-F.md).

usage: make_addons.py [--out build/opt6-f/addons] [which...]      which = zip lua udmf (default: all that exist)
  zip    ZT.pk3     ZIP container (stored/deflate/extra fields/empty files/folders) and PNG pictures (RGBA, palette+tRNS,
                    grey, grAb offsets, 16 bit, big flat); expected.json lists every entry with its CRC32 and every PNG
                    with the decoded RGBA, which tools/ps2/ftest_check.py compares with the FT_ lines of the engine.
  lua    ZL.pk3     Lua script (maths, strings, tables, pcall, freeslots, hooks, HUD drawing, net vars) + a SOC whose number
                    expressions are not in the base-game table (they go to the real LUA_EvalMath); prints FTLUA lines.
  udmf   UD.pk3     three real UDMF maps (MAP17, MAP16, MAP99 of the user's ZombieEscape2 mod, namespace "srb2", ZNODES/XGLN nodes) as Maps/MAPnn.wad;
                    UD.expected.json holds what tools/ps2/udmf_ref.py (an independent TEXTMAP parser) says the level structures contain.
"""
import argparse
import json
import os
import random
import struct
import sys
import zipfile
import zlib
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def png_bytes(img, grab=None, **kw):
    import io
    b = io.BytesIO()
    img.save(b, 'PNG', **kw)
    data = b.getvalue()
    if grab:  # grAb chunk (left, top offset) right after IHDR, as SRB2 PNG sprites carry it
        chunk = struct.pack('>ii', grab[0], grab[1])
        crc = zlib.crc32(b'grAb' + chunk) & 0xFFFFFFFF
        ins = struct.pack('>I', len(chunk)) + b'grAb' + chunk + struct.pack('>I', crc)
        data = data[:33] + ins + data[33:]  # signature 8 + IHDR chunk 25
    return data


def make_zip(out):
    rnd = random.Random(20261005)
    entries = []  # (name, data, compress_type, extra)

    def add(name, data, ctype=zipfile.ZIP_DEFLATED, extra=b''):
        entries.append((name, data, ctype, extra))

    # plain lumps: sizes at the edges of the deflate/stream buffers, compressible and incompressible
    for i, size in enumerate([0, 1, 7, 100, 4095, 4096, 4097, 65535, 65536, 65537, 300000, 1500000]):
        rand = bytes(rnd.getrandbits(8) for _ in range(min(size, 70000))) * (size // 70000 + 1)
        add('Misc/ZTRND%02d' % i, rand[:size], zipfile.ZIP_DEFLATED if i % 2 == 0 else zipfile.ZIP_STORED)
        add('Misc/ZTTXT%02d' % i, (b'SRB2 PS2 test line %d\n' % i) * (size // 20 + 1), zipfile.ZIP_DEFLATED)
    add('Misc/ZTSTORED', bytes(range(256)) * 40, zipfile.ZIP_STORED)
    add('Misc/ZTEXTRA', b'extra field in the local header', zipfile.ZIP_DEFLATED, b'\x55\x54\x05\x00\x01\x00\x00\x00\x00')  # "UT" extended timestamp
    add('Misc/Sub Folder/ZTSPACE', b'name with spaces in the folder')
    add('Misc/ZTEMPTY', b'')

    # pictures
    pics = {}
    rgba = Image.new('RGBA', (32, 32))
    for y in range(32):
        for x in range(32):
            hole = (x - 16) ** 2 + (y - 16) ** 2 < 36 or (x < 4 and y < 4)
            rgba.putpixel((x, y), (x * 8, y * 8, (x + y) * 4 & 255, 0 if hole else 255))
    pics['ZTRGBA'] = (png_bytes(rgba), rgba)
    pal = Image.new('P', (16, 48))
    pal.putpalette([c for i in range(256) for c in (i, 255 - i, (i * 7) & 255)])
    pal.putdata([(x + y * 3) % 40 for y in range(48) for x in range(16)])
    pics['ZTPAL'] = (png_bytes(pal, transparency=0), pal.convert('RGBA'))  # index 0 transparent
    grey = Image.new('L', (24, 24))
    grey.putdata([(x * 10 + y * 3) & 255 for y in range(24) for x in range(24)])
    pics['ZTGRAY'] = (png_bytes(grey), grey.convert('RGBA'))
    grab = Image.new('RGBA', (20, 40))
    grab.putdata([((i * 13) & 255, (i * 5) & 255, 200, 255 if i % 7 else 0) for i in range(20 * 40)])
    pics['ZTGRAB'] = (png_bytes(grab, grab=(8, 30)), grab)
    big = Image.new('RGB', (128, 128))
    big.putdata([((x * 2) & 255, (y * 2) & 255, ((x ^ y) * 4) & 255) for y in range(128) for x in range(128)])
    pics['ZTBIG'] = (png_bytes(big), big.convert('RGBA'))
    i16 = Image.new('I;16', (16, 16))
    i16.putdata([(i * 257) & 0xFFFF for i in range(256)])
    pics['ZT16BIT'] = (png_bytes(i16), None)
    for name, (data, _) in pics.items():
        add('Graphics/%s.png' % name, data)

    path = out / 'ZT.pk3'
    expected = {'lumps': {}, 'pics': {}}
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr(zipfile.ZipInfo('Misc/'), b'')
        for name, data, ctype, extra in entries:
            zi = zipfile.ZipInfo(name)
            zi.compress_type = ctype
            zi.extra = extra
            z.writestr(zi, data)
    # expected values: engine lump name = file name without folders (and without a ".png" suffix, SRB2 keeps the extension in
    # fullname only); the check script matches on fullname
    with zipfile.ZipFile(path) as z:
        for zi in z.infolist():
            if zi.is_dir():
                continue
            data = z.read(zi)
            expected['lumps'][zi.filename] = {'size': len(data), 'crc32': '%08x' % (zlib.crc32(data) & 0xFFFFFFFF), 'method': zi.compress_type}
    for name, (data, img) in pics.items():
        if img is not None:
            expected['pics'][name] = {'size': img.size, 'rgba_crc32': '%08x' % (zlib.crc32(img.convert('RGBA').tobytes()) & 0xFFFFFFFF)}
    (out / 'ZT.expected.json').write_text(json.dumps(expected, indent=1))
    print('wrote', path, path.stat().st_size, 'bytes,', len(expected['lumps']), 'entries,', len(pics), 'pictures')


LUA_TEST = r"""
-- OPT6-F Lua test (PS2-101). Every line it prints starts with FTLUA; tools/ps2/ftest_check.py lua compares them with the PC build.
local function out(s) print("FTLUA "..s) end
out("loaded")

-- integer-only maths and fixed point
out("fix "..(FRACUNIT*3/2).." "..FixedMul(3*FRACUNIT, FRACUNIT/2).." "..FixedDiv(FRACUNIT, 4*FRACUNIT).." "..FixedHypot(3*FRACUNIT, 4*FRACUNIT))
out("trig "..sin(ANG1*30).." "..cos(ANG1*60).." "..FixedAngle(45*FRACUNIT).." "..AngleFixed(ANGLE_90))
out("acos "..acos(0).." "..acos(FRACUNIT/2).." "..acos(-FRACUNIT/2).." "..acos(12345).." "..asin(FRACUNIT/4))
out("int "..(7/2).." "..(-7/2).." "..(2^10).." "..(100%7).." "..(5*3-2))

-- strings, tables, pcall, closures
local tbl = {}
for i = 1, 10 do tbl[i] = i*i end
out("tbl "..table.concat(tbl, ",").." #"..#tbl)
out("str "..string.format("%d %s %05d", 42, "x", 7)..string.rep("ab", 3)..("abc"):upper()..("Hello"):sub(2,4)..tostring(("x"):byte()))
local ok, err = pcall(function() error("boom", 0) end)
out("pcall "..tostring(ok).." "..tostring(err))
local function counter() local n = 0 return function() n = n + 1 return n end end
local c = counter() c() c()
out("closure "..c())
local keys = {}
for k in pairs({zz = 1, aa = 2, mm = 3}) do keys[#keys+1] = k end
table.sort(keys)
out("sort "..table.concat(keys, ","))
local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1) return a + b end)
out("coroutine "..co(10).." "..co(5))

-- engine constants through the enum library
out("const "..MT_PLAYER.." "..S_PLAY_STND.." "..SPR_PLAY.." "..sfx_jump.." "..SKINCOLOR_RED.." "..TICRATE.." "..FRACBITS)

-- freeslots (new object, state, sprite, sound, colour)
freeslot("MT_FTTHING", "S_FTTHING1", "SPR_FTTH", "sfx_ftsnd", "SKINCOLOR_FTCOL")
out("freeslot "..(MT_FTTHING > MT_PLAYER and "mt" or "bad").." "..(S_FTTHING1 > S_PLAY_STND and "st" or "bad"))
mobjinfo[MT_FTTHING].spawnstate = S_FTTHING1
mobjinfo[MT_FTTHING].radius = 20*FRACUNIT
mobjinfo[MT_FTTHING].height = 30*FRACUNIT
mobjinfo[MT_FTTHING].flags = MF_NOGRAVITY
states[S_FTTHING1].sprite = SPR_RING
states[S_FTTHING1].frame = A
states[S_FTTHING1].tics = -1
out("info "..mobjinfo[MT_FTTHING].radius.." "..states[S_FTTHING1].tics)
-- hooks
local frames, spawned = 0, nil
addHook("ThinkFrame", function()
	if gamestate ~= GS_LEVEL then return end
	frames = frames + 1
	if frames == 3 then
		local si = MT_ZSTHING and mobjinfo[MT_ZSTHING]
		out("soc speed "..tostring(si and si.speed).." doomednum "..tostring(si and si.doomednum).." radius "..tostring(si and si.radius).." height "..tostring(si and si.height))
		local p = players[0]
		out("level "..tostring(gamemap).." leveltime "..leveltime.." player "..(p and p.mo and "mo" or "none"))
		if p and p.mo then
			spawned = P_SpawnMobj(p.mo.x + 100*FRACUNIT, p.mo.y, p.mo.z, MT_FTTHING)
			out("spawn "..tostring(spawned and spawned.valid).." type "..(spawned and spawned.type == MT_FTTHING and "ok" or "bad").." radius "..(spawned and spawned.radius or -1))
		end
	end
	if frames == 10 then
		out("alive "..tostring(spawned and spawned.valid).." health "..tostring(spawned and spawned.health))
	end
	if frames == 36 then
		out("think36")
		COM_BufInsertText(server, "quit")
	end
end)
addHook("MobjThinker", function(mo) mo.ftcount = (mo.ftcount or 0) + 1 end, MT_FTTHING)
addHook("PlayerThink", function(p) end)
addHook("NetVars", function(net) end)

-- HUD: a box and a string every frame (proves the draw list works the same in the software and the hardware renderer)
hud.add(function(v, p)
	v.drawFill(8, 8, 120, 14, 35)
	v.drawString(10, 11, "LUA HUD OK", V_YELLOWMAP, "left")
end, "game")
out("done")
"""

ZS_SOC = """# OPT6-F SOC test (PS2-101): the number expressions are not in the base game table, they go to the real LUA_EvalMath
Freeslot
MT_ZSTHING
S_ZSTHING1

Object MT_ZSTHING
MapThingNum = 3999
SpawnState = S_ZSTHING1
Speed = 3*FRACUNIT/2
Radius = 16*FRACUNIT+4*FRACUNIT
Height = (1+2)*FRACUNIT*10
Mass = 100
Flags = MF_NOGRAVITY|MF_SCENERY

State S_ZSTHING1
SpriteName = RING
SpriteFrame = A
Duration = -1
"""


def make_lua(out):
    path = out / 'ZL.pk3'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('SOC/ZLSOC.soc', ZS_SOC.replace(chr(10), chr(13) + chr(10)))
        z.writestr('Lua/ZLTEST.lua', LUA_TEST.replace(chr(10), chr(13) + chr(10)))
    print('wrote', path, path.stat().st_size, 'bytes')


ZE2_PK3 = 'D:/AI-projects/ZombieEscape2/.validation/ZombieEscape2.pk3'
UD_MAPS = [('Maps/17 - Grancolia/MAP17.wad', 17), ('Maps/16 - Spooky Flower/MAP16.wad', 16), ('Maps/_Title Screen/MAP99.wad', 99)]


def make_udmf(out, src=ZE2_PK3):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import udmf_ref
    path = out / 'UD.pk3'
    expected = {}
    with zipfile.ZipFile(src) as zs, zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, num in UD_MAPS:
            data = zs.read(name)
            z.writestr('Maps/MAP%02d.wad' % num, data)
            expected[str(num)] = udmf_ref.expect_wad(data)
    (out / 'UD.expected.json').write_text(json.dumps(expected, indent=1))
    print('wrote', path, path.stat().st_size, 'bytes,', len(expected), 'maps')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build/opt6-f/addons'))
    ap.add_argument('which', nargs='*')
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    which = a.which or ['zip', 'lua']
    if 'udmf' in which:
        make_udmf(out)
    if 'zip' in which:
        make_zip(out)
    if 'lua' in which:
        make_lua(out)


if __name__ == '__main__':
    sys.exit(main())
