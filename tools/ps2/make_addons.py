"""Test add-ons for the content systems that PS2_PROFILE used to cut out (OPT6-F, docs/GATES/g1/opt6-F.md).

usage: make_addons.py [--out build/opt6-f/addons] [which...]      which = zip lua udmf lim skin sum (default: zip lua)
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


LIM_LUA = r"""
-- OPT8-F limits test (PS2-104): far more free slots than the old PS2 profile had (32 mobj types / 256 states / 256 sounds / 32 colours / 8 sprite2).
-- Every number printed is the PC number; tools/ps2/ftest_check.py same compares this log with the PC build's.
local function out(s) print("FTLUA "..s) end
out("lim loaded")

-- userdata made before the tables grew (PS2Limits_Grow moves them): they must still reach the live element
local early_st, early_mi, early_col, early_sfx = states[S_PLAY_STND], mobjinfo[MT_PLAYER], skincolors[SKINCOLOR_RED], sfxinfo[sfx_jump]
local old_st, old_mi, old_col, old_sfx = early_st.tics, early_mi.radius, early_col.accessible, early_sfx.priority
-- a colour renamed to the name of another colour: "WARNING: skincolor_t field 'name' ('GREEN') is a duplicate of another skincolor's name." on the PC (Doom II: Colors.lua)
out("cbn "..R_GetColorByName("GREEN").." "..R_GetColorByName("Green").." "..R_GetColorByName("RED").." "..R_GetColorByName("Red").." "..R_GetColorByName("Silver").." "..R_GetColorByName("NIGHT"))
local dupc = skincolors[1]
local dupold = dupc.name
dupc.name = "GREEN"
out("dupname "..dupc.name)
dupc.name = "NIGHT"
out("dupname "..dupc.name)
dupc.name = dupold
out("dupname "..dupc.name)
freeslot("SKINCOLOR_ZFDUP")
skincolors[SKINCOLOR_ZFDUP] = {name="WHITE", ramp={0x50,0x50,0x51,0x51,0x52,0x52,0x53,0x53,0x54,0x54,0x55,0x55,0x56,0x56,0x57,0x57}, invcolor=SKINCOLOR_WHITE, invshade=7, chatcolor=V_GRAYMAP, accessible=true}
out("dupnew "..skincolors[SKINCOLOR_ZFDUP].name.." "..R_GetColorByName("WHITE").." "..SKINCOLOR_ZFDUP)
-- files a script writes and reads (luafiles/ of the home directory: the PS2 home is the folder of the ELF / a memory card)
do
	local f = io.openlocal("zftest.txt", "w")
	if f then f:write("lua file line 1\nline 2 "..FRACUNIT.."\n") f:close() end
	local g = io.openlocal("zftest.txt", "r")
	local text = g and g:read("*a") or "NOFILE"
	if g then g:close() end
	out("io "..#text.." "..text:gsub("\n", "|"))
end
-- a mobj hook that exists before the growth: the table of hook lists moves with the object type table (their owners must follow, SL_DOOMII crashed on it)
local pcount, mcount = 0, 0
addHook("MobjThinker", function(mo) pcount = pcount + 1 end, MT_PLAYER)
addHook("MobjThinker", function(mo) pcount = pcount + 1 end, MT_BLUECRAWLA)

local S, M, P, C, F, P2 = {}, {}, {}, {}, {}, {}
for i = 1, 300 do S[i] = freeslot(string.format("S_ZF%03d", i)) end
for i = 1, 40 do M[i] = freeslot(string.format("MT_ZF%02d", i)) end
for i = 1, 40 do P[i] = freeslot(string.format("SPR_Z%03d", i)) end
for i = 1, 40 do C[i] = freeslot(string.format("SKINCOLOR_ZF%02d", i)) end
for i = 1, 300 do F[i] = freeslot(string.format("sfx_zf%03d", i)) end
for i = 1, 20 do P2[i] = freeslot(string.format("SPR2_Z%03d", i)) end
freeslot("SPR_ZFLONGSPRITE")
out("states "..S[1].." "..S[300])
out("mobjs "..M[1].." "..M[40])
out("sprites "..P[1].." "..P[40])
out("colors "..C[1].." "..C[40])
out("sounds "..F[1].." "..F[300])
out("sprite2 "..P2[1].." "..P2[20])

for i = 1, 300 do
	states[S[i]].sprite = SPR_RING
	states[S[i]].frame = A
	states[S[i]].tics = i
	states[S[i]].nextstate = S[i % 300 + 1]
end
for i = 1, 40 do
	mobjinfo[M[i]].spawnstate = S[i]
	mobjinfo[M[i]].radius = (10 + i)*FRACUNIT
	mobjinfo[M[i]].height = 30*FRACUNIT
	mobjinfo[M[i]].flags = MF_NOGRAVITY
	skincolors[C[i]].name = string.format("Zf%02d", i)
	skincolors[C[i]].accessible = true
end
local ssum, tsum = 0, 0
for i = 1, 300 do ssum = ssum + states[S[i]].tics + states[S[i]].nextstate end
for i = 1, 40 do tsum = tsum + mobjinfo[M[i]].radius + mobjinfo[M[i]].spawnstate end
out("check "..ssum.." "..tsum.." "..skincolors[C[40]].name.." "..tostring(skincolors[C[40]].accessible))
out("count "..#states.." "..#mobjinfo.." "..#skincolors)
early_st.tics = 77
early_mi.radius = 21*FRACUNIT
early_sfx.priority = 99
early_col.accessible = false
out("remap "..states[S_PLAY_STND].tics.." "..mobjinfo[MT_PLAYER].radius.." "..sfxinfo[sfx_jump].priority.." "..tostring(skincolors[SKINCOLOR_RED].accessible))
early_st.tics, early_mi.radius, early_sfx.priority, early_col.accessible = old_st, old_mi, old_sfx, old_col
out("restored "..states[S_PLAY_STND].tics.." "..mobjinfo[MT_PLAYER].radius.." "..sfxinfo[sfx_jump].priority.." "..tostring(skincolors[SKINCOLOR_RED].accessible))

local gt = G_AddGametype({name = "ZF Test", identifier = "ZFTEST", typeoflevel = TOL_COOP, rules = 0, rankingtype = 0,
	intermissiontype = 0, defaultpointlimit = 0, defaulttimelimit = 0, description = "Test gametype", headerleftcolor = 1, headerrightcolor = 2})
addHook("MobjThinker", function(mo) mcount = mcount + 1 end, M[40])
addHook("MobjThinker", function(mo) pcount = pcount + 1000 end, MT_PLAYER)
out("gametype "..tostring(gt).." "..tostring(GT_ZFTEST ~= nil))

local frames, spawned = 0, nil
addHook("ThinkFrame", function()
	if gamestate ~= GS_LEVEL then return end
	frames = frames + 1
	if frames == 3 then
		local mo = players[0] and players[0].mo
		local z = skins["ztest"]
		out("ztest "..(z and z.soundsid[SKSJUMP] or -1))
		out("live "..tostring(mo and mo.type).." "..tostring(mo and mo.state).." "..tostring(mo and mo.radius).." "..mobjinfo[MT_PLAYER].spawnstate.." "..states[S_PLAY_STND].tics)
		if mo then
			spawned = P_SpawnMobj(mo.x + 100*FRACUNIT, mo.y, mo.z, M[40])
			out("spawn "..spawned.type.." "..spawned.state.." "..spawned.radius.." "..spawned.tics)
		end
	end
	if frames == 12 then
		local mo = players[0] and players[0].mo
		out("hooks "..pcount.." "..mcount)
		out("later "..tostring(mo and mo.state).." "..tostring(spawned and spawned.state).." "..tostring(spawned and spawned.tics))
		out("done")
	end
	if frames == 14 then
		COM_BufInsertText(server, "quit")
	end
end)
"""


def make_lim(out):
    # a SOC with 100 more states, 20 objects, a sprite, a sound and a colour: the number expressions go to the real LUA_EvalMath
    lines = ['# OPT8-F limits SOC (PS2-104)', 'Freeslot']
    for i in range(1, 101):
        lines.append('S_ZSOC%03d' % i)
    for i in range(1, 21):
        lines.append('MT_ZSOC%02d' % i)
    lines += ['SPR_ZSOC', 'sfx_zsoc1', 'SKINCOLOR_ZSOC1', 'SPR2_ZSOC', '']
    for i in range(1, 21):
        lines += ['Object MT_ZSOC%02d' % i, 'MapThingNum = %d' % (3100 + i), 'SpawnState = S_ZSOC%03d' % i, 'Radius = %d*FRACUNIT' % (8 + i),
                  'Height = 24*FRACUNIT', 'Flags = MF_NOGRAVITY|MF_SCENERY', '']
    for i in range(1, 101):
        lines += ['State S_ZSOC%03d' % i, 'SpriteName = RING', 'SpriteFrame = A', 'Duration = %d' % (i + 1), 'Next = S_ZSOC%03d' % (i % 100 + 1), '']
    path = out / 'ZF.pk3'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('SOC/ZFSOC.soc', '\r\n'.join(lines) + '\r\n')
        z.writestr('Lua/ZFTEST.lua', LIM_LUA.replace(chr(10), chr(13) + chr(10)))
        from PIL import Image
        import io
        for frame in (3, 100, 130):  # long sprite names: frames up to MAXFRAMENUM-1 = 255 (a short name has frames 0..63)
            img = Image.new('RGBA', (16, 16))
            img.putdata([((i * frame) & 255, 90, 200, 255 if i % 3 else 0) for i in range(256)])
            b = io.BytesIO()
            img.save(b, 'PNG')
            z.writestr('LongSprites/ZFLONGSPRITE/%d.png' % frame, b.getvalue())
    print('wrote', path, path.stat().st_size, 'bytes')


SKIN_LUA = r"""
-- OPT8-F add-on test (PS2-103): a skin (a copy of Tails with its own name, colour, sounds), new sprites, a sound and music, seen from Lua.
local function out(s) print("FTLUA "..s) end
out("skin loaded")
freeslot("sfx_ztsnd") -- (a SOC freeslot of a sound is upper case: not reachable as sfx_ztsnd from Lua, on the PC either)
local frames = 0
addHook("ThinkFrame", function()
	if gamestate ~= GS_LEVEL then return end
	frames = frames + 1
	if frames == 4 then
		local sk = skins["ztest"]
		out("skin "..tostring(sk ~= nil))
		if sk then
			out("skin sounds "..sk.soundsid[SKSJUMP].." "..sk.soundsid[SKSSKID])
			out("skin name "..sk.name.." "..sk.realname.." "..sk.hudname.." speed "..sk.normalspeed.." ability "..sk.ability.." startcolor "..sk.starttranscolor)
			local sum = 0
			for i = 1, 40 do -- (index 0 of the compat table is the userdata of the table itself: a quirk of LUA_PushUserdata's pointer cache)
				sum = sum + sk.sprites[i].numframes * (i + 1)
			end
			out("skin sprites "..sk.sprites[SPR2_WAIT].numframes.." "..sk.sprites[SPR2_WALK].numframes.." "..sk.sprites[SPR2_RUN].numframes.." "..sk.sprites[SPR2_FLY].numframes.." sum "..sum)
		end
		out("numskins "..#skins)
		local sfxid = sfx_ztsnd
		out("sfx "..tostring(sfxid ~= nil and sfxid > 0))
		out("sprite "..tostring(SPR_ZTSP ~= nil))
	end
	if frames == 8 then
		S_ChangeMusic("ZTMUS", true, players[0])
		out("music "..tostring(S_MusicName() ~= nil and S_MusicName() or "none"))
		S_StartSound(nil, sfx_ztsnd)
		out("done")
	end
	if frames == 14 then
		COM_BufInsertText(server, "quit")
	end
end)
"""

SKIN_SOC = """# OPT8-F add-on test: a sprite, a sound, an object
Freeslot
SPR_ZTSP
MT_ZTOBJ
S_ZTOBJ1

Object MT_ZTOBJ
MapThingNum = 3200
SpawnState = S_ZTOBJ1
Radius = 20*FRACUNIT
Height = 30*FRACUNIT
Flags = MF_NOGRAVITY|MF_SCENERY

State S_ZTOBJ1
SpriteName = ZTSP
SpriteFrame = A
Duration = -1
"""


def wav_bytes(freq=11025, ms=300):
    import io
    import math
    import wave
    n = freq * ms // 1000
    b = io.BytesIO()
    with wave.open(b, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(freq)
        w.writeframes(bytes(int(128 + 100 * math.sin(i * 2 * math.pi * 440 / freq)) for i in range(n)))
    return b.getvalue()


def make_skin(out, assets='D:/Ai-Project3/SRB2-PS2-Port/srb2-assets'):
    """ZS.pk3: skin "ztest" = Tails' sprites under the folder 9_ZTest, a new sprite (PNG), a sound lump (WAV) and an OGG music lump (a small one of music.pk3)."""
    from PIL import Image
    path = out / 'ZS.pk3'
    skin_def = '\n'.join(['name = ztest', 'realname = ZTest', 'hudname = ZTEST', 'startcolor = 96', 'prefcolor = Red', 'supercolor = Red', 'ability = CA_FLY',
                          'actionspd = 100', 'normalspeed = 40', 'thrustfactor = 5', 'accelstart = 96', 'acceleration = 40', 'sfx_jump = ZTJMP', ''])
    with zipfile.ZipFile(Path(assets) / 'characters.pk3') as zc, zipfile.ZipFile(Path(assets) / 'music.pk3') as zm, zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('9_ZTest/1_Info/S_SKIN', skin_def.replace('\n', '\r\n'))
        for n in zc.namelist():
            if n.startswith('2_Tails/2_Sprites/') and not n.endswith('/'):
                z.writestr('9_ZTest/2_Sprites/' + n.split('/')[-1], zc.read(n))
        img = Image.new('RGBA', (24, 24))
        img.putdata([((i * 9) & 255, 40, 220, 255 if i % 5 else 0) for i in range(24 * 24)])
        import io
        b = io.BytesIO()
        img.save(b, 'PNG')
        z.writestr('Sprites/ZTSPA0.png', b.getvalue())
        z.writestr('Sounds/DSZTJMP', wav_bytes(11025, 250))
        z.writestr('Sounds/DSZTSND', wav_bytes(11025, 200))
        ogg = sorted((i for i in zm.infolist() if i.filename.split('/')[-1].startswith('O_') and i.file_size > 0), key=lambda i: i.file_size)[0]
        z.writestr('Music/O_ZTMUS', zm.read(ogg.filename))
        z.writestr('SOC/ZTSOC.soc', SKIN_SOC.replace('\n', '\r\n'))
        z.writestr('Lua/ZTSKIN.lua', SKIN_LUA.replace('\n', '\r\n'))
    print('wrote', path, path.stat().st_size, 'bytes, music from', ogg.filename, ogg.file_size)


SUM_LUA = r"""
-- OPT9-F summary (PS2-103): what the game looks like after the add-ons it was started with, as printed by the Lua API only, so that the PC game
-- and the PS2 port print the same lines when they load the same files (ftest_check.py same compares the FTLUA lines). Load it LAST (-file ZSUM.pk3).
local function out(s) print("FTLUA "..s) end
local frames = 0
local function hash(h, v) return (h * 31 + (v & 0xFFFFFFFF)) & 0xFFFFFFFF end
local function strhash(h, s) for i = 1, #s do h = hash(h, s:byte(i)) end return h end
out("sum loaded")
addHook("ThinkFrame", function()
	if gamestate ~= GS_LEVEL then return end
	frames = frames + 1
	if frames ~= 3 then return end
	out("skins "..#skins)
	for i = 0, #skins - 1 do
		local sk = skins[i]
		local h = 0
		for k = 1, 40 do
			local def = sk.sprites[k]
			if def then h = hash(h, def.numframes) end
		end
		out(string.format("skin %d %s real=%s hud=%s speed=%d run=%d thrust=%d accel=%d/%d ability=%d/%d jump=%d flags=%d color=%d/%d/%d spr=%08x",
			i, sk.name, sk.realname, sk.hudname, sk.normalspeed, sk.runspeed, sk.thrustfactor, sk.accelstart, sk.acceleration, sk.ability, sk.ability2,
			sk.jumpfactor, sk.flags, sk.starttranscolor, sk.prefcolor, sk.supercolor, h))
	end
	-- object types / states / colours that have a name, hashed field by field (the free slots an add-on defined, in order)
	local nmo, hmo = 0, 0
	for i = 0, #mobjinfo - 1 do
		local mi = mobjinfo[i]
		if mi.doomednum ~= -1 or mi.spawnstate ~= 0 or mi.radius ~= 0 then
			nmo = nmo + 1
			hmo = hash(hash(hash(hash(hmo, i), mi.doomednum), mi.spawnstate), mi.radius)
			hmo = hash(hash(hash(hmo, mi.height), mi.speed), mi.spawnhealth)
		end
	end
	out("mobjinfo used "..nmo.." hash "..string.format("%08x", hmo))
	local nst, hst = 0, 0
	for i = 0, #states - 1 do
		local st = states[i]
		if st.sprite ~= 0 or st.tics ~= 0 or st.nextstate ~= 0 then
			nst = nst + 1
			hst = hash(hash(hash(hash(hst, i), st.sprite), st.frame), st.tics)
			hst = hash(hst, st.nextstate)
		end
	end
	out("states used "..nst.." hash "..string.format("%08x", hst))
	local ncol, hcol = 0, 0
	for i = 1, #skincolors - 1 do
		local c = skincolors[i]
		if c.name and c.name ~= "" then
			ncol = ncol + 1
			hcol = strhash(hash(hcol, i), c.name)
			hcol = hash(hcol, c.accessible and 1 or 0)
		end
	end
	out("colors used "..ncol.." hash "..string.format("%08x", hcol))
	local nsf, hsf = 0, 0
	for i = 1, #sfxinfo - 1 do
		local sf = sfxinfo[i]
		if sf.name and sf.name ~= "" and sf.name:sub(1, 3) ~= "fre" and sf.name:sub(1, 2) ~= "fr" then
			nsf = nsf + 1
			hsf = strhash(hash(hsf, i), sf.name)
			hsf = hash(hsf, sf.priority)
		end
	end
	out("sfx named "..nsf.." hash "..string.format("%08x", hsf))
	out("map "..tostring(gamemap).." type "..tostring(gametype).." tol "..tostring(maptol).." players "..tostring(#players))
	-- the level structures the loader built (the first 3000 of each kind are hashed: the Lua loop has to stay short on a PS2)
	local function level_hash(count, fn)
		local h = 0
		for i = 0, (count < 3000 and count or 3000) - 1 do h = fn(h, i) end
		return h
	end
	out(string.format("sectors %d %08x", #sectors, level_hash(#sectors, function(h, i)
		local s = sectors[i]
		return hash(hash(hash(hash(h, s.floorheight), s.ceilingheight), s.lightlevel), s.special)
	end)))
	out(string.format("lines %d %08x", #lines, level_hash(#lines, function(h, i)
		local l = lines[i]
		return hash(hash(hash(hash(hash(h, l.special), l.flags), l.v1.x), l.v1.y), l.v2.x)
	end)))
	out(string.format("sides %d vertexes %d", #sides, #vertexes))
	out(string.format("things %d %08x", #mapthings, level_hash(#mapthings, function(h, i)
		local t = mapthings[i]
		return hash(hash(hash(hash(h, t.x), t.y), t.type), t.angle)
	end)))
end)
"""


def make_sum(out):
    path = out / 'ZSUM.pk3'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('Lua/ZSUM.lua', SUM_LUA.replace(chr(10), chr(13) + chr(10)))
    print('wrote', path, path.stat().st_size, 'bytes')


DEMO_LUA = r"""
-- OPT9-F demo/save test (PS2-103): the player runs forward and jumps (PlayerCmd, so the input is in the demo), the position is printed every 10 tics; quits after 100 tics.
local function out(s) print("FTLUA "..s) end
out("demo loaded")
addHook("PlayerCmd", function(p, cmd)
	if p ~= players[0] then return end
	cmd.forwardmove = 50
	cmd.sidemove = (leveltime % 70) < 20 and 20 or 0
	if leveltime % 45 == 10 then cmd.buttons = cmd.buttons | BT_JUMP end
end)
local done = false
addHook("ThinkFrame", function()
	if gamestate ~= GS_LEVEL or done then return end
	local mo = players[0] and players[0].mo
	if not mo then return end
	if leveltime % 10 == 0 and leveltime > 0 and leveltime <= 100 then
		out(string.format("dpos %d %d %d %d %d %d", leveltime, mo.x, mo.y, mo.z, mo.momx, mo.momy))
	end
	if leveltime >= 100 then
		done = true
		out("ddone "..tostring(demo and demo.playback).." "..tostring(demo and demo.recording))
		COM_BufInsertText(server, "quit")
	end
end)
"""


def make_demo(out):
    path = out / 'ZD.pk3'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('Lua/ZDEMO.lua', DEMO_LUA.replace(chr(10), chr(13) + chr(10)))
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
    if 'lim' in which:
        make_lim(out)
    if 'skin' in which:
        make_skin(out)
    if 'sum' in which:
        make_sum(out)
    if 'demo' in which:
        make_demo(out)


if __name__ == '__main__':
    sys.exit(main())
