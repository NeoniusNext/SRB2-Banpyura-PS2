-- OPT14: a soak. A mod that keeps working for minutes: objects are spawned and die all the time (hooks by type, Lua fields in mobjs), strings and tables are made and dropped, userdata of
-- sectors / lines / mobjs are touched, a HUD draws every frame, and the level changes every LEVELTICS tics through a list of maps (the Lua state, the userdata registry and the zone must come
-- back to the same size after each change). Every LEVELTICS tics "memfree" is run (the PS2 log shows the Lua heap line), the LQ lines are the script's own counters (equal on both builds).
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
-- (a game that is not a demo starts with a random seed: the script's own generator keeps both builds on the same numbers)
local rseed = 12345
local function rnd(a, b)
	rseed = (rseed * 75 + 74) % 65537
	return a + rseed % (b - a + 1)
end
local LEVELTICS = 1500
local MAPS = {1, 2, 4, 1, 5, 2, 1, 4}
local mi = 1
local MT_B = freeslot("MT_LMSOAK")
local S_B = freeslot("S_LMSOAK")
states[S_B] = {sprite = SPR_THOK, frame = A|FF_FULLBRIGHT, tics = 4, nextstate = S_B}
mobjinfo[MT_B] = {doomednum = -1, spawnstate = S_B, spawnhealth = 1, seestate = S_NULL, seesound = sfx_None, reactiontime = 0, attacksound = sfx_None, painstate = S_NULL, painchance = 0, painsound = sfx_None,
	meleestate = S_NULL, missilestate = S_NULL, deathstate = S_NULL, xdeathstate = S_NULL, deathsound = sfx_None, speed = 0, radius = 8*FRACUNIT, height = 16*FRACUNIT,
	dispoffset = 0, mass = 1, damage = 0, activesound = sfx_None, flags = MF_NOGRAVITY|MF_NOBLOCKMAP|MF_NOCLIP|MF_SCENERY, raisestate = S_NULL}
local spawned, removed, thinks = 0, 0, 0
local kept = {}          -- a table that survives the level change and holds stale userdata after it
local keepsec = {}
local lastlevel = -1
local changing = false

addHook("MobjSpawn", function(mo) spawned = spawned + 1 mo.lm_born = leveltime mo.lm_data = {leveltime, spawned} end, MT_B)
addHook("MobjThinker", function(mo)
	thinks = thinks + 1
	mo.momz = FRACUNIT / 2
	if leveltime - mo.lm_born > 20 + mo.health then P_RemoveMobj(mo) end
end, MT_B)
addHook("MobjRemoved", function(mo) removed = removed + 1 end, MT_B)

local function garbage(n)
	local t = {}
	for i = 1, n do
		t[#t + 1] = {i, tostring(i), ("x"):rep(i % 40), {i * 2}}
	end
	local s = 0
	for _, e in ipairs(t) do s = s + e[1] + #e[2] + #e[3] + e[4][1] end
	return s
end

addHook("MapLoad", function(m)
	-- the userdata kept from the level before must be dead now
	local dead, alive = 0, 0
	for i, u in ipairs(kept) do local ok, v = pcall(function() return u.valid end) if ok and v then alive = alive + 1 else dead = dead + 1 end end
	for i, u in ipairs(keepsec) do local ok, v = pcall(function() return u.valid end) if ok and v then alive = alive + 1 else dead = dead + 1 end end
	P("maploaded", m, "stale", dead, alive, #sectors > 0)
	kept, keepsec = {}, {}
	lastlevel = leveltime
	changing = false
end)

local g = 0
addHook("ThinkFrame", function()
	local p = players[0]
	if not (p and p.mo and p.mo.valid) then return end
	p.lives = 9 -- (the idle player of a boss map would end the game and the run would go on in the title demos)
	p.powers[pw_invulnerability] = 3
	local t = leveltime
	if t % 3 == 0 and t < LEVELTICS - 60 then
		local o = P_SpawnMobj(p.mo.x + rnd(-100, 100) * FRACUNIT, p.mo.y + rnd(-100, 100) * FRACUNIT, p.mo.z + 40 * FRACUNIT, MT_B)
		if o then o.health = rnd(0, 20) end
	end
	g = g + garbage(20 + t % 30)
	if t % 5 == 0 then
		for i = 0, 9 do keepsec[#keepsec + 1] = sectors[(t + i * 7) % #sectors] end
		for i = 0, 9 do kept[#kept + 1] = lines[(t + i * 11) % #lines] end
		if #kept > 600 then table.remove(kept, 1) table.remove(keepsec, 1) end
	end
	if t == LEVELTICS - 5 then
		COM_BufInsertText(server, "memfree")
		P("counts", gamemap, spawned, removed, thinks, g)
	end
	if t >= LEVELTICS and not changing then
		changing = true
		mi = mi + 1
		if mi > #MAPS then
			P("DONE_SOAK", mi - 1, spawned, removed, thinks)
			return
		end
		COM_BufInsertText(server, "map " .. MAPS[mi] .. " -force")
	end
end)
hud.add(function(v)
	v.drawString(4, 4, "soak " .. leveltime, V_ALLOWLOWERCASE, "small")
	v.drawFill(4, 14, 30, 4, 35)
end, "game")
