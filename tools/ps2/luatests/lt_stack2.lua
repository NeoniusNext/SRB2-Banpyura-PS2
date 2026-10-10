-- OPT14: recursion through the ENGINE: a hook calls an engine function that calls the same hook again (P_DamageMobj from MobjDamage, P_KillMobj from MobjDeath, P_RemoveMobj/MobjRemoved,
-- P_SpawnMobj from MobjSpawn). Each level holds the engine frames plus the Lua call; the PC stops at LUAI_MAXCCALLS (200 levels, "C stack overflow"). On the EE the main thread stack is 384 KiB:
-- the run must end with the same lines and the same depths, and "stackused=" of ZSTAT (-zstack -zquit N) tells the margin. LIMIT=N (the script's own bound) lets the test measure a depth.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local MT_X = freeslot("MT_LMREC")
local S_X = freeslot("S_LMREC")
states[S_X] = {sprite = SPR_THOK, frame = A, tics = -1}
mobjinfo[MT_X] = {doomednum = -1, spawnstate = S_X, spawnhealth = 1000000, seestate = S_NULL, seesound = sfx_None, reactiontime = 0, attacksound = sfx_None, painstate = S_NULL, painchance = 0,
	painsound = sfx_None, meleestate = S_NULL, missilestate = S_NULL, deathstate = S_NULL, xdeathstate = S_NULL, deathsound = sfx_None, speed = 0, radius = 16*FRACUNIT, height = 32*FRACUNIT,
	dispoffset = 0, mass = 100, damage = 0, activesound = sfx_None, flags = MF_SHOOTABLE|MF_NOGRAVITY, raisestate = S_NULL}

local LM_LIMIT = 100000 -- (tools/ps2/lua_suite.sh: sed makes copies with other bounds)
local mode, depth, maxdepth, errs, limit = nil, 0, 0, {}, 100000
local function msg(e) return (tostring(e):gsub("^.-:%d+: ", "")) end

addHook("MobjDamage", function(target, inflictor, source, dmg, dmgtype)
	if mode ~= "damage" then return end
	depth = depth + 1
	if depth > maxdepth then maxdepth = depth end
	if depth < limit then P_DamageMobj(target, nil, nil, 1) end
	depth = depth - 1
end, MT_X)

addHook("MobjDeath", function(mo)
	if mode ~= "kill" then return end
	depth = depth + 1
	if depth > maxdepth then maxdepth = depth end
	if depth < limit then
		local n = P_SpawnMobj(mo.x, mo.y, mo.z, MT_X)
		n.health = 1
		P_KillMobj(n)
	end
	depth = depth - 1
end, MT_X)

addHook("MobjSpawn", function(mo)
	if mode ~= "spawn" then return end
	depth = depth + 1
	if depth > maxdepth then maxdepth = depth end
	if depth < limit then
		local n = P_SpawnMobj(mo.x, mo.y, mo.z, MT_X)
		P_RemoveMobj(n)
	end
	depth = depth - 1
end, MT_X)

addHook("MobjRemoved", function(mo)
	if mode ~= "remove" then return end
	depth = depth + 1
	if depth > maxdepth then maxdepth = depth end
	if depth < limit then
		local n = P_SpawnMobj(mo.x, mo.y, mo.z, MT_X)
		P_RemoveMobj(n)
	end
	depth = depth - 1
end, MT_X)

-- thinker recursion: a thinker hook that makes an object think by changing its state (P_SetMobjStateNF runs actions, not hooks), skip

local steps = {"damage", "kill", "spawn", "remove"}
local si = 0
local done = false
addHook("ThinkFrame", function()
	if done or leveltime < 6 then return end
	local p = players[0]
	if not (p and p.mo and p.mo.valid) then return end
	si = si + 1
	local m = steps[si]
	if not m then
		P("DONE_STACK2")
		done = true
		return
	end
	mode, depth, maxdepth = m, 0, 0
	limit = LM_LIMIT
	local x = P_SpawnMobj(p.mo.x + 200 * FRACUNIT, p.mo.y, p.mo.z, MT_X)
	local ok, e
	if m == "damage" then ok, e = pcall(P_DamageMobj, x, nil, nil, 1)
	elseif m == "kill" then x.health = 1 ok, e = pcall(P_KillMobj, x)
	elseif m == "spawn" then ok, e = true, nil
	else ok, e = pcall(P_RemoveMobj, x) end
	P(m, ok, e and msg(e) or "-", maxdepth, depth)
	mode = nil
end)
