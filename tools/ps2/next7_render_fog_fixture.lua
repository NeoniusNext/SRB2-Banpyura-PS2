-- Paired-engine rendering fixture: retains geometry and all collision flags.
-- A 1x1 Doom-patch texture selects the existing solid-color flat drawers.
local loaded = false
addHook("MapLoad", function()
	loaded = false
	local fog, trans, water, slopedfofs, slopefloors, slopeceilings = 0, 0, 0, 0, 0, 0
	for sector in sectors.iterate do
		if sector.f_slope then
			sector.floorpic = "NX7SOL"
			slopefloors = $ + 1
		end
		if sector.c_slope then
			sector.ceilingpic = "NX7SOL"
			slopeceilings = $ + 1
		end
		for rover in sector.ffloors() do
			if (rover.fofflags & FOF_RENDERPLANES) ~= 0 then
				if rover.t_slope or rover.b_slope then slopedfofs = $ + 1 end
				local mode = (fog + trans + water) % 3
				rover.toppic = "NX7SOL"
				rover.bottompic = "NX7SOL"
				rover.fofflags = $ & ~(FOF_FOG | FOF_TRANSLUCENT | FOF_SPLAT | FOF_RIPPLE)
				rover.blend = 0
				if mode == 0 then
					rover.fofflags = $ | FOF_FOG
					fog = $ + 1
				else
					rover.fofflags = $ | FOF_TRANSLUCENT
					rover.alpha = 128
					if mode == 1 then
						trans = $ + 1
					else
						rover.fofflags = $ | FOF_RIPPLE
						water = $ + 1
					end
				end
			end
		end
	end
	loaded = true
	print("NEXT7 FOG SOLID READY map="..gamemap.." fog="..fog.." trans="..trans.." water="..water.." slopedfofs="..slopedfofs.." slopefloors="..slopefloors.." slopeceilings="..slopeceilings)
end)
addHook("ThinkFrame", function()
	if loaded and (leveltime == 1 or leveltime == 20 or leveltime == 70 or leveltime == 140) then
		print("NEXT7 FOG SOLID TIC "..leveltime)
	end
end)
print("NEXT7 FOG SOLID FIXTURE LOADED")
