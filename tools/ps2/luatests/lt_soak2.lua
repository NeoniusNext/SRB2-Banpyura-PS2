-- OPT14: the first minutes of lt_soak, one line per event: where do the thinker counts of the PC and the PS2 start to differ? (spawn, remove, RNG state at a few tics)
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
local MT_B = freeslot("MT_LMSOAK")
local S_B = freeslot("S_LMSOAK")
states[S_B] = {sprite = SPR_THOK, frame = A|FF_FULLBRIGHT, tics = 4, nextstate = S_B}
mobjinfo[MT_B] = {doomednum = -1, spawnstate = S_B, spawnhealth = 1, seestate = S_NULL, seesound = sfx_None, reactiontime = 0, attacksound = sfx_None, painstate = S_NULL, painchance = 0, painsound = sfx_None,
	meleestate = S_NULL, missilestate = S_NULL, deathstate = S_NULL, xdeathstate = S_NULL, deathsound = sfx_None, speed = 0, radius = 8*FRACUNIT, height = 16*FRACUNIT,
	dispoffset = 0, mass = 1, damage = 0, activesound = sfx_None, flags = MF_NOGRAVITY|MF_NOBLOCKMAP|MF_NOCLIP|MF_SCENERY, raisestate = S_NULL}
local spawned, removed, thinks, nprint = 0, 0, 0, 0
local perTic = {}
addHook("MobjSpawn", function(mo) spawned = spawned + 1 mo.lm_born = leveltime end, MT_B)
addHook("MobjThinker", function(mo)
	thinks = thinks + 1
	perTic[leveltime] = (perTic[leveltime] or 0) + 1
	mo.momz = FRACUNIT / 2
	if leveltime - mo.lm_born > 20 + mo.health then
		if nprint < 60 then nprint = nprint + 1 P("rm", mo.lm_born, leveltime, mo.health, mo.z >> 16) end
		P_RemoveMobj(mo)
	end
end, MT_B)
addHook("MobjRemoved", function(mo) removed = removed + 1 end, MT_B)
local last = -1
addHook("ThinkFrame", function()
	local p = players[0]
	if not (p and p.mo and p.mo.valid) then return end
	local t = leveltime
	if t == last then P("DUPTIC", t) end
	last = t
	if t % 3 == 0 and t < 300 then
		local o = P_SpawnMobj(p.mo.x + rnd(-100, 100) * FRACUNIT, p.mo.y + rnd(-100, 100) * FRACUNIT, p.mo.z + 40 * FRACUNIT, MT_B)
		if o then o.health = rnd(0, 20) end
	end
	if t % 25 == 0 then P("tic", t, spawned, removed, thinks, perTic[t] or 0, rnd(0, 60000)) end
	if t == 300 then P("DONE_SOAK2") end
end)
