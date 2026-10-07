-- OPT11-FX: a fixture of render features, the same objects on the PC engine and on the PS2 (tools/ps2/fx_pair.py --addon tools/ps2/fxscene.lua --map 1):
-- two rows of static scenery objects in front of the first player with every blend mode / translucency / colormap / render flag, and a HUD strip with
-- the HUD blend styles. Positions follow the player's start (angle), so the scene is the same everywhere.
local variants = {
	{"plain", function(mo) end},
	{"trans50", function(mo) mo.frame = mo.frame | FF_TRANS50 end},
	{"trans20", function(mo) mo.frame = mo.frame | FF_TRANS20 end},
	{"add", function(mo) mo.blendmode = AST_ADD; mo.frame = mo.frame | FF_TRANS30 end},
	{"sub", function(mo) mo.blendmode = AST_SUBTRACT; mo.frame = mo.frame | FF_TRANS30 end},
	{"revsub", function(mo) mo.blendmode = AST_REVERSESUBTRACT; mo.frame = mo.frame | FF_TRANS30 end},
	{"modulate", function(mo) mo.blendmode = AST_MODULATE; mo.frame = mo.frame | FF_TRANS30 end},
	{"colorized", function(mo) mo.colorized = true; mo.color = SKINCOLOR_RED end},
	{"fullbright", function(mo) mo.renderflags = $ | RF_FULLBRIGHT end},
	{"fulldark", function(mo) mo.renderflags = $ | RF_FULLDARK end},
	{"paper", function(mo) mo.renderflags = $ | RF_PAPERSPRITE; mo.angle = mo.angle + ANGLE_45 end},
	{"floorspr", function(mo) mo.renderflags = $ | RF_FLOORSPRITE; mo.angle = mo.angle + ANGLE_45 end},
	{"scale2", function(mo) mo.spritexscale = 2*FRACUNIT; mo.spriteyscale = FRACUNIT/2 end},
	{"hflip", function(mo) mo.renderflags = $ | RF_HORIZONTALFLIP end},
	{"roll", function(mo) mo.rollangle = ANGLE_45 end},
	{"vflip", function(mo) mo.renderflags = $ | RF_VERTICALFLIP end},
}

addHook("MapLoad", function()
	local p = players[0]
	if not (p and p.valid and p.mo and p.mo.valid) then return end
	local ang = p.mo.angle
	for i, v in ipairs(variants) do
		local row = (i - 1) / 8
		local col = (i - 1) % 8
		local dist = (240 + row * 220) * FRACUNIT
		local side = (col - 3) * 80 * FRACUNIT - 40 * FRACUNIT
		local x = p.mo.x + FixedMul(dist, cos(ang)) + FixedMul(side, cos(ang - ANGLE_90))
		local y = p.mo.y + FixedMul(dist, sin(ang)) + FixedMul(side, sin(ang - ANGLE_90))
		local mo = P_SpawnMobj(x, y, p.mo.z + 20 * FRACUNIT, MT_GFZFLOWER1)
		if mo and mo.valid then
			mo.flags = MF_NOGRAVITY | MF_NOBLOCKMAP | MF_NOCLIP | MF_SCENERY
			mo.angle = ang + ANGLE_180
			v[2](mo)
		end
	end
end)

hud.add(function(v, player)
	if not (leveltime > 5) then return end
	local flags = {0, V_ADD, V_SUBTRACT, V_REVERSESUBTRACT, V_MODULATE}
	for i, f in ipairs(flags) do
		local x = 8 + (i - 1) * 60
		v.drawFill(x, 150, 50, 14, 72 | (f == 0 and V_TRANSLUCENT or (f | V_20TRANS)))
		v.drawFill(x + 10, 154, 30, 14, 150 | (f == 0 and V_80TRANS or (f | V_50TRANS)))
	end
end, "game")
