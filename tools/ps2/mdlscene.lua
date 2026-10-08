-- OPT11-MODEL: a fixture of 3D models, the same objects on the PC engine and on the PS2 (tools/ps2/fx_pair.py --addon tools/ps2/mdlscene.lua --map 1 --tree <models dir>).
-- Frozen (MF_NOTHINK) scenery objects of mobj types whose sprite has a model in models.dat, in a grid in front of the first player; every object keeps the
-- frame and the tics it had at spawn, so both renderers draw the same pose. The set is chosen by the console variable-less convention below:
--   MDL_SET (a global set by the first line of a variant file) or the default: the first 15 types found, in sprite-name order.
-- Output (console log): "MDLSCENE spr=NAME mt=N frame=F" for every spawned object.
local want = rawget(_G, "MDL_SET") or {"POSS", "SPOS", "BUZZ", "CRAB", "RING", "SPRB", "SPRY", "PTER", "BMCE", "BGAR", "FISH", "SKIM", "PNTY", "JETB", "EGGM"}
local MDL_ROWS_PER = 5

local function find_type(spr)
	local sp = _G["SPR_" .. spr]
	if sp == nil then return nil end
	for mt = 0, 799 do
		local info = mobjinfo[mt]
		if info and info.spawnstate and info.spawnstate ~= S_NULL and states[info.spawnstate].sprite == sp then
			return mt
		end
	end
	return nil
end

addHook("MapLoad", function()
	local p = players[0]
	if not (p and p.valid and p.mo and p.mo.valid) then return end
	local ang = p.mo.angle
	local n = 0
	for i, name in ipairs(want) do
		local mt = find_type(name)
		if mt then
			local row = n / MDL_ROWS_PER
			local col = n % MDL_ROWS_PER
			local dist = (170 + row * 60) * FRACUNIT
			local side = (col * 50 - 100) * FRACUNIT
			local x = p.mo.x + FixedMul(dist, cos(ang)) + FixedMul(side, cos(ang - ANGLE_90))
			local y = p.mo.y + FixedMul(dist, sin(ang)) + FixedMul(side, sin(ang - ANGLE_90))
			local mo = P_SpawnMobj(x, y, p.mo.z + (8 + row * 42) * FRACUNIT, mt) -- higher rows hang higher: every row shows over the one in front
			if mo and mo.valid then
				mo.flags = MF_NOTHINK | MF_NOGRAVITY | MF_NOBLOCKMAP | MF_NOCLIP | MF_NOCLIPHEIGHT | MF_SCENERY
				mo.angle = ang + ANGLE_180 + (n % 3) * ANGLE_45
				local fx = rawget(_G, "MDL_FX")
				if fx then fx(mo, n) end
				print(string.format("MDLSCENE spr=%s mt=%d frame=%d", name, mt, mo.frame & FF_FRAMEMASK))
				n = n + 1
			end
		end
	end
end)
