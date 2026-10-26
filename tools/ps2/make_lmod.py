"""OPT14: LMOD.pk3, a small content mod for the end-to-end comparison "demo + Lua mod + software renderer, frame by frame" between the PC build and the PS2 ELF
(tools/ps2/lua_frames.sh; docs/GATES/g1/opt14-LUA.md).

usage: make_lmod.py [out.pk3]       default build/opt14-addons/LMOD.pk3
What is in it: sprites of its own (Sprites/LMODA0..D0 .png with offsets, SPR_LMOD freeslot), a HUD graphic (Graphics/LMHUD.png), a SOC file that defines an object (MT_LMSOC), and a Lua script that
spawns a ring of objects around the player at tic 40 of the demo, moves them every tic (orbit, bob, scale, colour, translucency, fullbright, sprite x scale), spawns sparks, changes the light of
a sector, writes strings and patches on the HUD (v.draw, v.drawScaled, v.drawString, v.drawFill) every frame. Everything is a function of the game state, so a demo gives the same pictures on
both builds when the engines agree.
"""
import io
import struct
import sys
import zipfile
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]

LUA = r'''
-- LMOD: objects with sprites of the mod, a HUD, sector lights
local MT_ORB = freeslot("MT_LMORB")
local MT_SPK = freeslot("MT_LMSPK")
local S_ORB = freeslot("S_LMORB1")
local S_SPK = freeslot("S_LMSPK1")
local SPR_LM = freeslot("SPR_LMOD")
states[S_ORB] = {sprite = SPR_LM, frame = A|FF_ANIMATE|FF_FULLBRIGHT, tics = -1, var1 = 3, var2 = 5}
states[S_SPK] = {sprite = SPR_LM, frame = D|FF_TRANS40, tics = 30, nextstate = S_NULL}
mobjinfo[MT_ORB] = {doomednum = -1, spawnstate = S_ORB, spawnhealth = 1, radius = 12*FRACUNIT, height = 24*FRACUNIT, dispoffset = 2, mass = 100, flags = MF_NOGRAVITY|MF_NOBLOCKMAP|MF_NOCLIP|MF_NOCLIPHEIGHT|MF_SCENERY}
mobjinfo[MT_SPK] = {doomednum = -1, spawnstate = S_SPK, spawnhealth = 1, radius = 6*FRACUNIT, height = 12*FRACUNIT, dispoffset = 1, mass = 1, flags = MF_NOGRAVITY|MF_NOBLOCKMAP|MF_NOCLIP|MF_NOCLIPHEIGHT|MF_SCENERY}
local orbs = {}
local lightsec
addHook("ThinkFrame", function()
	local p = players[0]
	if not (p and p.mo and p.mo.valid) then return end
	local t = leveltime
	if t == 40 then
		for i = 0, 15 do
			local o = P_SpawnMobj(p.mo.x, p.mo.y, p.mo.z + 24*FRACUNIT, MT_ORB)
			o.lm_i = i
			o.color = SKINCOLOR_RED + (i % 16)
			o.scale = FRACUNIT * (4 + i % 5) / 6
			o.spritexscale = FRACUNIT + (i % 3) * FRACUNIT / 8
			o.renderflags = (i % 4 == 0) and RF_FULLBRIGHT or 0
			o.frame = (o.frame & ~FF_TRANSMASK) | (i % 3 == 0 and FF_TRANS30 or 0)
			orbs[#orbs + 1] = o
		end
		lightsec = p.mo.subsector.sector
	end
	if t > 40 then
		for i, o in ipairs(orbs) do
			if o.valid then
				local a = (t * 3 + o.lm_i * 22) * ANG1
				local r = 90*FRACUNIT + (o.lm_i % 4) * 12*FRACUNIT
				P_MoveOrigin(o, p.mo.x + FixedMul(cos(a), r), p.mo.y + FixedMul(sin(a), r), p.mo.z + 30*FRACUNIT + FixedMul(sin((t * 5 + o.lm_i * 40) * ANG1), 14*FRACUNIT))
				o.angle = a
				if t % 7 == o.lm_i % 7 then
					local s = P_SpawnMobj(o.x, o.y, o.z, MT_SPK)
					if s then s.color = o.color s.scale = o.scale / 2 end
				end
			end
		end
		if lightsec and lightsec.valid then lightsec.lightlevel = 120 + (t * 3) % 100 end
	end
end)
addHook("MobjThinker", function(mo)
	mo.momz = (mo.momz or 0) + 0
end, MT_SPK)

hud.add(function(v, player)
	if not (player and player.mo) then return end
	local bg = v.cachePatch("LMHUD")
	v.drawFill(2, 2, 88, 22, 31 | V_50TRANS)
	v.draw(4, 4, bg, V_SNAPTOLEFT|V_SNAPTOTOP)
	v.drawString(30, 5, "LMOD " .. leveltime, V_ALLOWLOWERCASE|V_YELLOWMAP|V_SNAPTOLEFT|V_SNAPTOTOP, "thin")
	v.drawString(30, 14, "orbs " .. #orbs, V_ALLOWLOWERCASE|V_GREENMAP|V_SNAPTOLEFT|V_SNAPTOTOP, "small")
	v.drawScaled(280*FRACUNIT, 30*FRACUNIT, FRACUNIT*3/2, bg, V_SNAPTORIGHT|V_SNAPTOTOP|V_30TRANS)
	for i = 0, 4 do v.drawFill(100 + i * 6, 190, 4, 4 + i * 2, 120 + i * 8) end
end, "game")
print("LQ lmod registered")
'''

SOC = '''Freeslot
S_LMSOC1
MT_LMSOC

Object MT_LMSOC
MapThingNum = 3333
SpawnState = S_LMSOC1
SpawnHealth = 1
Radius = 8*FRACUNIT
Height = 16*FRACUNIT
Flags = MF_NOGRAVITY|MF_SCENERY

State S_LMSOC1
SpriteName = THOK
SpriteFrame = 0
Duration = -1
Next = S_NULL
'''


def png(img, grab=None):
    b = io.BytesIO()
    img.save(b, 'PNG')
    data = b.getvalue()
    if grab:
        chunk = struct.pack('>ii', grab[0], grab[1])
        crc = zlib.crc32(b'grAb' + chunk) & 0xFFFFFFFF
        ins = struct.pack('>I', len(chunk)) + b'grAb' + chunk + struct.pack('>I', crc)
        data = data[:33] + ins + data[33:]
    return data


def sprite(letter, color):
    img = Image.new('RGBA', (32, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((2, 2, 29, 29), fill=color + (255,))
    d.ellipse((8, 6, 20, 18), fill=(255, 255, 255, 255))
    d.rectangle((14 + 'ABCD'.index(letter) * 3, 20, 17 + 'ABCD'.index(letter) * 3, 28), fill=(0, 0, 0, 255))
    return png(img, grab=(16, 16))


def build(out):
    out.parent.mkdir(parents=True, exist_ok=True)
    z = zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED)
    z.writestr('Lua/main.lua', LUA)
    z.writestr('SOC/lmod.soc', SOC)
    for i, c in enumerate([(200, 60, 60), (60, 200, 60), (60, 60, 200), (220, 200, 40)]):
        z.writestr('Sprites/LMOD%s0.png' % 'ABCD'[i], sprite('ABCD'[i], c))
    hud = Image.new('RGBA', (24, 18), (0, 0, 0, 0))
    d = ImageDraw.Draw(hud)
    d.rectangle((0, 0, 23, 17), fill=(30, 30, 120, 255))
    d.rectangle((2, 2, 21, 15), outline=(255, 255, 0, 255))
    z.writestr('Graphics/LMHUD.png', png(hud))
    z.close()


if __name__ == '__main__':
    build(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/opt14-addons/LMOD.pk3')
    print('ok')
