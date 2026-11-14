-- Integration fixture, following fxscene.lua's static scenery placement.
-- The paired run driver supplies the same -zseed and -singletics to both ELFs.
-- NX5FA0/NX5FLAT are non-solid 63x45 Doom patches; NX5BASE is dense 63x45.
-- P_CreateFloorSpriteSlope and slope.o/xydirection/zdelta have audited Lua setters.
freeslot("SPR_NX5F")

local loaded = false

addHook("MapLoad", function()
	loaded = false
	local p = players[0]
	if not (p and p.valid and p.mo and p.mo.valid) then
		print("NEXT5 NPOT SPRITES FAILED no player on MapLoad")
		return
	end
	local ang = p.mo.angle
	local spawned, slopes = 0, 0
	for row = 0, 1 do
		for col = 0, 3 do
			local dist = (160 + row*120)*FRACUNIT
			local side = (col*72 - 108)*FRACUNIT
			local x = p.mo.x + FixedMul(dist, cos(ang)) + FixedMul(side, cos(ang - ANGLE_90))
			local y = p.mo.y + FixedMul(dist, sin(ang)) + FixedMul(side, sin(ang - ANGLE_90))
			local mo = P_SpawnMobj(x, y, p.mo.z + 24*FRACUNIT, MT_GFZFLOWER1)
			if mo and mo.valid then
				mo.flags = MF_NOGRAVITY | MF_NOBLOCKMAP | MF_NOCLIP | MF_SCENERY
				mo.sprite = SPR_NX5F
				mo.frame = (row == 1) and FF_TRANS50 or 0
				mo.tics = -1
				mo.scale = 5*FRACUNIT/4
				mo.destscale = mo.scale
				mo.angle = ang
				if col % 2 ~= 0 then mo.angle = $ + ANGLE_45 end
				mo.renderflags = RF_FLOORSPRITE | RF_SLOPESPLAT | RF_NOSPLATBILLBOARD | RF_FULLBRIGHT
				local slope = P_CreateFloorSpriteSlope(mo)
				if not (slope and slope.valid) then
					print("NEXT5 NPOT SPRITES FAILED no floor-sprite slope")
					return
				end
				slope.o = {x=mo.x, y=mo.y, z=mo.z}
				slope.xydirection = mo.angle
				slope.zdelta = FRACUNIT/4
				if col % 2 ~= 0 then slope.zdelta = -FRACUNIT/4 end
				slopes = $ + 1
				if col >= 2 then
					mo.colorized = true
					mo.color = SKINCOLOR_RED
				end
				spawned = $ + 1
			end
		end
	end
	if spawned ~= 8 or slopes ~= 8 then
		print("NEXT5 NPOT SPRITES FAILED expected=8 spawned="..spawned)
		return
	end

	-- FOF flags changed here affect rendering only; no heights or collision flags
	-- change. A stock map with no eligible FOFs logs zero rather than claiming
	-- that it covered either ordinary masked-flat drawer.
	local opaque, translucent, slopedfofs, slopefloors, slopeceilings = 0, 0, 0, 0, 0
	for sector in sectors.iterate do
		-- Retain existing geometry while giving already-sloped base planes an
		-- opaque NPOT texture. Counts do not assert camera visibility.
		if sector.f_slope then
			sector.floorpic = "NX5BASE"
			slopefloors = $ + 1
		end
		if sector.c_slope then
			sector.ceilingpic = "NX5BASE"
			slopeceilings = $ + 1
		end
		-- sector_get returns a factory closure; calling it supplies the iterator,
		-- initial ffloor state and nil control value to Lua's generic for loop.
		for rover in sector.ffloors() do
			if (rover.fofflags & FOF_RENDERPLANES) ~= 0 then
				if rover.t_slope or rover.b_slope then slopedfofs = $ + 1 end
				local count = opaque + translucent
				rover.fofflags = (rover.fofflags | FOF_SPLAT | FOF_TRANSLUCENT) & ~(FOF_FOG | FOF_RIPPLE)
				rover.toppic = "NX5FLAT"
				rover.bottompic = "NX5FLAT"
				rover.blend = 0
				if count % 2 == 0 then
					rover.alpha = 255
					opaque = $ + 1
				else
					rover.alpha = 128
					translucent = $ + 1
				end
			end
		end
	end
	loaded = true
	print("NEXT5 NPOT SPRITES READY map="..gamemap.." sprites="..spawned.." slopes="..slopes.." opaque=4 translucent=4 npot=63x45 fof_opaque="..opaque.." fof_translucent="..translucent.." fof_sloped="..slopedfofs.." slope_floors="..slopefloors.." slope_ceilings="..slopeceilings)
end)

addHook("ThinkFrame", function()
	if loaded and (leveltime == 1 or leveltime == 20 or leveltime == 70 or leveltime == 140) then
		print("NEXT5 NPOT SPRITES TIC "..leveltime)
	end
end)

print("NEXT5 NPOT SPRITES FIXTURE LOADED")
