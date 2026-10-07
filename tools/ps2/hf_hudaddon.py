"""OPT10-HF: builds build/addons/HFHUD.pk3, a Lua HUD test add-on (v.drawFill / drawString / draw / drawScaled / drawNum with colour maps, translucency and flip flags).

usage: hf_hudaddon.py [out.pk3]
Run it on both sides with the same frame spec (the HUD is static):
  PC : tools/ps2/pcshot.py luahud --shots k60 --warp 1 --cmd con_hudlines~0 -- -file build/addons/HFHUD.pk3
  PS2: tools/ps2/hf_run.py luahud_hw --elf <full build ELF> --files build/addons/HFHUD.pk3 -- -skipintro -warp 1 -renderer Hardware -zreserve 3072 -hwfbh 200 -vidcmd con_hudlines~0 -vidshot k60
"""
import sys
import zipfile
from pathlib import Path

LUA = r'''
-- OPT10-HF Lua HUD test: everything the HUD library can draw, at fixed places
hud.add(function(v, player, cam)
	local ring = v.cachePatch("RINGA0")
	-- flat boxes: opaque, translucent, colour map
	v.drawFill(8, 8, 40, 14, 35)
	v.drawFill(52, 8, 40, 14, 152 | V_30TRANS)
	v.drawFill(96, 8, 40, 14, 73 | V_50TRANS)
	v.drawFill(140, 8, 40, 14, 120 | V_70TRANS)
	-- strings: colour maps, small/thin fonts, translucency
	v.drawString(8, 30, "LUA HUD TEST", V_YELLOWMAP)
	v.drawString(8, 40, "Lower case text", V_ALLOWLOWERCASE | V_GREENMAP)
	v.drawString(8, 50, "THIN FONT", V_ALLOWLOWERCASE | V_REDMAP, "thin")
	v.drawString(8, 60, "SMALL FONT", V_ALLOWLOWERCASE | V_BLUEMAP, "small")
	v.drawString(8, 70, "TRANSLUCENT", V_50TRANS | V_PURPLEMAP)
	v.drawString(312, 30, "RIGHT", V_ORANGEMAP, "right")
	v.drawString(160, 82, "CENTER", V_SKYMAP, "center")
	-- patches: plain, flipped, translucent, scaled, colour mapped
	v.draw(200, 20, ring, 0)
	v.draw(220, 20, ring, V_FLIP)
	v.draw(240, 20, ring, V_50TRANS)
	v.draw(260, 20, ring, 0, v.getColormap(TC_DEFAULT, SKINCOLOR_RED))
	v.draw(280, 20, ring, 0, v.getColormap(TC_DEFAULT, SKINCOLOR_BLUE))
	v.drawScaled(200*FRACUNIT, 50*FRACUNIT, FRACUNIT*2, ring, 0)
	v.drawScaled(240*FRACUNIT, 50*FRACUNIT, FRACUNIT*3/2, ring, V_30TRANS)
	-- numbers
	v.drawNum(120, 100, 12345, 0)
	v.drawPaddedNum(120, 120, 42, 6, 0)
	-- a box that spans the screen edges
	v.drawFill(0, 188, 320, 12, 31 | V_50TRANS)
end, "game")
'''

SOC = '''# HFHUD\nFreeslot("SPR_HFHUD")\n'''


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/addons/HFHUD.pk3')
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('Lua/HFHUD.lua', LUA.lstrip('\n'))
    print(out, out.stat().st_size)


if __name__ == '__main__':
    main()
