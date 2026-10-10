-- OPT14 stand: a "world" mod. Moves floors and ceilings of sectors with a wave, changes light levels, flats, offsets and scales, edits 3D floors (ffloors), textures and offsets of sides,
-- flags of lines, tags, then reads the result back through the engine (P_FloorzAtPos, R_PointInSubsector, P_CheckPosition, P_TryMove). A demo of GFZ1 plays on both builds; the sector
-- movements can change the course of the demo, but they do it identically when the engines agree.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end

local picked, base = {}, {}
local function hashworld()
	local h = 17
	for i = 0, #sectors - 1 do
		local s = sectors[i]
		h = (h * 31 + (s.floorheight >> 16) + (s.ceilingheight >> 16) * 3 + s.lightlevel * 7 + s.special * 11) % 1000003
	end
	return h
end

addHook("MapLoad", function(mapnum)
	picked, base = {}, {}
	local n = 0
	for i = 0, #sectors - 1 do
		local s = sectors[i]
		if #s.lines >= 4 and s.ceilingheight - s.floorheight >= 192*FRACUNIT and i % 3 == 0 then
			picked[#picked + 1] = s
			base[#base + 1] = {s.floorheight, s.ceilingheight, s.lightlevel, s.floorpic, s.ceilingpic}
			n = n + 1
			if n >= 24 then break end
		end
	end
	P("maploaded", mapnum, #sectors, #lines, #sides, #vertexes, #subsectors, #picked, hashworld())
end)

local step = 0
local function E(f, ...) local r = {pcall(f, ...)} if r[1] then table.remove(r, 1) for i = 1, #r do local v = r[i] r[i] = (type(v) == "userdata") and "userdata" or tostring(v) end return table.concat(r, ",") end return "E:" .. (tostring(r[2]):gsub("^.-:%d+: ", "")) end

addHook("ThinkFrame", function()
	if #picked == 0 then return end
	local t = leveltime
	-- 1. waves
	for i, s in ipairs(picked) do
		if s.valid then
			local b = base[i]
			local ph = (t * (3 + i % 4) + i * 17) % 360
			s.floorheight = b[1] + FixedMul(sin(ph * ANG1), 6*FRACUNIT)
			s.ceilingheight = b[2] - FixedMul(cos(ph * ANG1), 6*FRACUNIT)
			s.lightlevel = (b[3] + ph / 4) % 256
			if t % 53 == 0 then s.floorxoffset = s.floorxoffset + FRACUNIT * 4 end
			if t % 59 == 0 then s.ceilingyoffset = s.ceilingyoffset - FRACUNIT * 4 end
			if t == 100 then s.floorxscale = FRACUNIT * 2 s.flooryscale = FRACUNIT / 2 s.floorangle = ANGLE_45 end
			if t == 140 then s.ceilingpic = "F_SKY1" s.floorpic = picked[1].floorpic end
			if t == 160 then s.floorpic = b[4] s.ceilingpic = b[5] end
			if t == 200 then s.floorlightlevel = 40 s.floorlightabsolute = true end
			if t == 220 then s.crumblestate = 0 s.gravity = FRACUNIT / 2 s.friction = ORIG_FRICTION end
			if t == 240 then s.tag = 777 end
		end
	end
	-- 2. lines and sides of the first picked sector
	local s1 = picked[1]
	if s1 and s1.valid and #s1.lines > 0 then
		for j = 0, #s1.lines - 1 do
			local l = s1.lines[j]
			if l and l.valid then
				if t == 50 then P("lineflags", j, l.flags, l.special, l.alpha, l.tag, E(function() l.flags = 0 end)) end
				local sd = l.frontside
				if sd and sd.valid then
					sd.textureoffset = (t * 2 + j * 8) * FRACUNIT % (64*FRACUNIT)
					sd.rowoffset = (t + j * 4) * FRACUNIT % (32*FRACUNIT)
					if t == 120 then local tn = R_CheckTextureNumForName("GFZROCK") if tn < 0 then tn = sd.toptexture end sd.toptexture = tn sd.midtexture = tn sd.bottomtexture = tn P("tex", tn, R_TextureNameForNum(tn)) end
					if t == 130 then sd.repeatcnt = 2 end
				end
			end
		end
	end
	-- 3. 3D floors: every FOF of the level, change heights and alpha
	if t % 5 == 0 then
		local nf, hf = 0, 0
		for i = 0, #sectors - 1 do
			local s = sectors[i]
			if s.ffloors then
				for rover in s.ffloors() do
					nf = nf + 1
					hf = (hf * 31 + (rover.topheight >> 16) + (rover.bottomheight >> 16) * 3 + rover.alpha) % 1000003
					if t == 70 and rover.valid then rover.alpha = 128 end
					if t == 90 then rover.fofflags = rover.fofflags & ~FOF_FLOATBOB end
				end
			end
		end
		if t % 50 == 0 then P("fofs", t, nf, hf) end
	end
	-- 4. queries through the engine
	if t % 25 == 0 and t > 0 then
		local p = players[0]
		if p and p.mo and p.mo.valid then
			local mo = p.mo
			local ss = R_PointInSubsector(mo.x, mo.y)
			P("query", t, ss and ss.valid, ss and ss.sector.valid, P_FloorzAtPos(mo.x, mo.y, mo.z, mo.height) >> 16, P_CeilingzAtPos(mo.x, mo.y, mo.z, mo.height) >> 16,
				P_GetZAt and "zat" or "nozat")
			P("pos", t, mo.x >> 16, mo.y >> 16, mo.z >> 16, mo.floorz >> 16, mo.ceilingz >> 16, mo.subsector ~= nil)
			P("check", t, E(P_CheckPosition, mo, mo.x + 8*FRACUNIT, mo.y), E(P_TryMove, mo, mo.x, mo.y, true))
		end
		P("world", t, hashworld())
	end
	-- a throwaway spawn and teleport sequence
	if t == 180 and players[0].mo then
		local mo = players[0].mo
		local th = P_SpawnMobj(mo.x, mo.y + 64*FRACUNIT, mo.z, MT_THOK)
		P("tele", E(P_TeleportMove, th, mo.x + 32*FRACUNIT, mo.y, mo.z), E(P_SetOrigin, th, mo.x, mo.y - 64*FRACUNIT, mo.z), th.x >> 16, th.y >> 16)
		P_RemoveMobj(th)
	end
	-- polyobjects and slopes of the level are read, not written
	if t == 260 then
		P("polys", #polyobjects)
		P("DONE_LMWORLD")
	end
end)
P("registered")
