-- OPT14 stand: an "objects" mod in the style of the community's. Custom object types and states (freeslot, mobjinfo[], states[]), hooks by type and generic, dozens of spawned objects
-- every few tics that seek, collide, split, expire and are removed, targets/tracers between mobjs, a mobjs.iterate() every tic, Lua fields stored in mobjs, player manipulation.
-- Both builds play the same demo (-playdemo / -timedemo of DEMO_001 on GFZ1): everything that is printed is a function of the game and the script, an event hash is order sensitive.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end

local MT_BOMB = freeslot("MT_LMBOMB")
local MT_SPARK = freeslot("MT_LMSPARK")
local MT_ORB = freeslot("MT_LMORB")
local S_B1, S_B2, S_B3 = freeslot("S_LMBOMB1", "S_LMBOMB2", "S_LMBOMB3")
local S_SP1, S_SP2 = freeslot("S_LMSPARK1", "S_LMSPARK2")
local S_O1 = freeslot("S_LMORB1")
local sfx_bm = freeslot("sfx_lmboom")
P("slots", MT_BOMB, MT_SPARK, MT_ORB, S_B1, S_B3, S_SP2, S_O1, sfx_bm)

states[S_B1] = {sprite = SPR_THOK, frame = A, tics = 3, nextstate = S_B2}
states[S_B2] = {sprite = SPR_THOK, frame = A|FF_FULLBRIGHT, tics = 3, nextstate = S_B3}
states[S_B3] = {sprite = SPR_THOK, frame = A|FF_TRANS30, tics = 3, nextstate = S_B1}
states[S_SP1] = {sprite = SPR_SPRK, frame = A|FF_FULLBRIGHT, tics = 2, nextstate = S_SP2}
states[S_SP2] = {sprite = SPR_SPRK, frame = B|FF_FULLBRIGHT|FF_TRANS50, tics = 2, nextstate = S_NULL}
states[S_O1] = {sprite = SPR_RING, frame = A|FF_ANIMATE, tics = -1, var1 = 23, var2 = 2}
mobjinfo[MT_BOMB] = {
	doomednum = -1, spawnstate = S_B1, spawnhealth = 3, seestate = S_NULL, seesound = sfx_None, reactiontime = 8, attacksound = sfx_None, painstate = S_NULL, painchance = 0, painsound = sfx_None,
	meleestate = S_NULL, missilestate = S_NULL, deathstate = S_NULL, xdeathstate = S_NULL, deathsound = sfx_bm, speed = 8*FRACUNIT, radius = 16*FRACUNIT, height = 32*FRACUNIT,
	dispoffset = 0, mass = 100, damage = 0, activesound = sfx_None, flags = MF_SHOOTABLE|MF_NOGRAVITY|MF_NOCLIPHEIGHT, raisestate = S_NULL
}
mobjinfo[MT_SPARK] = {
	doomednum = -1, spawnstate = S_SP1, spawnhealth = 1, seestate = S_NULL, seesound = sfx_None, reactiontime = 0, attacksound = sfx_None, painstate = S_NULL, painchance = 0, painsound = sfx_None,
	meleestate = S_NULL, missilestate = S_NULL, deathstate = S_NULL, xdeathstate = S_NULL, deathsound = sfx_None, speed = 0, radius = 4*FRACUNIT, height = 8*FRACUNIT,
	dispoffset = 1, mass = 1, damage = 0, activesound = sfx_None, flags = MF_NOBLOCKMAP|MF_NOCLIP|MF_NOCLIPHEIGHT|MF_NOGRAVITY|MF_SCENERY, raisestate = S_NULL
}
mobjinfo[MT_ORB] = {
	doomednum = -1, spawnstate = S_O1, spawnhealth = 1, seestate = S_NULL, seesound = sfx_None, reactiontime = 0, attacksound = sfx_None, painstate = S_NULL, painchance = 0, painsound = sfx_None,
	meleestate = S_NULL, missilestate = S_NULL, deathstate = S_NULL, xdeathstate = S_NULL, deathsound = sfx_None, speed = 0, radius = 12*FRACUNIT, height = 24*FRACUNIT,
	dispoffset = 0, mass = 1, damage = 0, activesound = sfx_None, flags = MF_SPECIAL|MF_NOGRAVITY, raisestate = S_NULL
}
P("info", mobjinfo[MT_BOMB].spawnhealth, mobjinfo[MT_BOMB].radius, mobjinfo[MT_SPARK].flags == (MF_NOBLOCKMAP|MF_NOCLIP|MF_NOCLIPHEIGHT|MF_NOGRAVITY|MF_SCENERY), states[S_B2].tics, states[S_O1].var1)

-- an order sensitive digest of everything that happened
local ev, evn, cnt = 17, 0, {}
local function E(tag, ...)
	local h = ev
	for i = 1, select("#", ...) do
		local v = select(i, ...)
		if type(v) == "number" then h = (h * 31 + (v % 1000003)) % 1000003
		elseif type(v) == "boolean" then h = (h * 31 + (v and 1 or 2)) % 1000003 end
	end
	ev = (h * 31 + tag) % 1000003
	evn = evn + 1
	cnt[tag] = (cnt[tag] or 0) + 1
end
local TAG = {spawn = 1, think = 2, collide = 3, damage = 4, death = 5, removed = 6, fuse = 7, touch = 8, genspawn = 9, playerthink = 10, thok = 11}

local nbombs, maxbombs = 0, 0
local bombs = {}    -- Lua table of userdata, some stale after removal

addHook("MobjSpawn", function(mo)
	nbombs = nbombs + 1
	if nbombs > maxbombs then maxbombs = nbombs end
	mo.lm_age = 0          -- a Lua field stored in the mobj
	mo.lm_id = nbombs
	bombs[#bombs + 1] = mo
	E(TAG.spawn, mo.x >> 16, mo.y >> 16, mo.z >> 16, mo.health)
end, MT_BOMB)

addHook("MobjThinker", function(mo)
	if not mo.valid then return end
	mo.lm_age = mo.lm_age + 1
	local t = mo.target
	if t and t.valid then
		-- circle around the target
		local ang = mo.angle + ANG10 * (1 + (mo.lm_id % 3))
		mo.angle = ang
		local r = 64*FRACUNIT + (mo.lm_id % 5) * 12*FRACUNIT
		local tx = t.x + FixedMul(cos(ang), r)
		local ty = t.y + FixedMul(sin(ang), r)
		mo.momx = (tx - mo.x) / 4
		mo.momy = (ty - mo.y) / 4
		mo.momz = (t.z + 24*FRACUNIT + (mo.lm_id % 4) * 8*FRACUNIT - mo.z) / 6
	end
	if mo.lm_age % 9 == 0 then
		local sp = P_SpawnMobjFromMobj(mo, 0, 0, 8*FRACUNIT, MT_SPARK)
		if sp and sp.valid then
			sp.tracer = mo
			sp.scale = FRACUNIT * 2 / 3
			sp.color = SKINCOLOR_SKY + (mo.lm_id % 8)
			sp.renderflags = RF_FULLBRIGHT
			sp.spritexscale = FRACUNIT + (mo.lm_id % 3) * (FRACUNIT/4)
			sp.spriteyoffset = (mo.lm_age % 4) * FRACUNIT
		end
	end
	if mo.lm_age == 60 then mo.fuse = 7 end
	E(TAG.think, mo.x >> 16, mo.y >> 16, mo.z >> 16, mo.lm_age, mo.tics, mo.fuse)
end, MT_BOMB)

addHook("MobjFuse", function(mo)
	E(TAG.fuse, mo.lm_id or 0, mo.health)
	P_KillMobj(mo)    -- MobjDeath below
	return true
end, MT_BOMB)

addHook("MobjDeath", function(mo, inflictor, source, dmgtype)
	E(TAG.death, mo.lm_id or 0, dmgtype or 0, (inflictor and inflictor.valid) and inflictor.type or -1)
	for i = 1, 4 do
		local sp = P_SpawnMobjFromMobj(mo, P_RandomRange(-20, 20)*FRACUNIT, P_RandomRange(-20, 20)*FRACUNIT, 0, MT_SPARK)
		if sp and sp.valid then
			sp.momx = P_RandomRange(-6, 6) * FRACUNIT
			sp.momy = P_RandomRange(-6, 6) * FRACUNIT
			sp.momz = P_RandomRange(0, 8) * FRACUNIT
			sp.fuse = 10 + i
		end
	end
	S_StartSound(mo, sfx_bm)
	P_RemoveMobj(mo)
	return true
end, MT_BOMB)

addHook("MobjRemoved", function(mo)
	nbombs = nbombs - 1
	E(TAG.removed, mo.lm_id or 0, mo.lm_age or 0)
end, MT_BOMB)

addHook("MobjDamage", function(target, inflictor, source, dmg, dmgtype)
	E(TAG.damage, target.lm_id or 0, dmg, dmgtype)
	return nil
end, MT_BOMB)

addHook("MobjCollide", function(mo, other)
	E(TAG.collide, mo.lm_id or 0, other.type, other.z >> 16)
	return nil
end, MT_BOMB)

addHook("MobjThinker", function(mo)
	-- sparks follow their tracer a bit then fall
	if mo.tracer and mo.tracer.valid then
		mo.momz = mo.momz + (mo.tracer.z - mo.z) / 32
	end
	E(TAG.think + 20, mo.fuse, mo.tics)
end, MT_SPARK)

addHook("TouchSpecial", function(special, toucher)
	E(TAG.touch, toucher.type, special.type)
	return false
end, MT_ORB)

-- the generic hooks: every object of the level
local generic = 0
addHook("MobjSpawn", function(mo)
	generic = generic + 1
	E(TAG.genspawn, mo.type)
end)
local removedgen = 0
addHook("MobjRemoved", function(mo) removedgen = removedgen + 1 end)

-- players: spawn a ring of bombs, change the player's fields
local lastspawn = 0
addHook("PlayerThink", function(player)
	if not (player.mo and player.mo.valid) then return end
	E(TAG.playerthink, player.mo.x >> 16, player.mo.y >> 16, player.mo.z >> 16, player.mo.momx >> 8, player.pflags & 0xFFFF)
	if leveltime >= 10 and leveltime % 12 == 0 and nbombs < 24 then
		local b = P_SpawnMobjFromMobj(player.mo, 0, 0, 40*FRACUNIT, MT_BOMB)
		if b and b.valid then
			b.target = player.mo
			b.angle = leveltime * ANG1 * 7
			b.scale = FRACUNIT + (leveltime % 4) * (FRACUNIT/8)
			b.destscale = FRACUNIT
			b.scalespeed = FRACUNIT/32
			b.color = SKINCOLOR_RED + (leveltime % 12)
			b.colorized = (leveltime % 2 == 0)
			b.blendmode = AST_ADD
			b.alpha = FRACUNIT - (leveltime % 3) * (FRACUNIT/5)
			lastspawn = leveltime
		end
	end
	if leveltime == 200 then
		-- orbs in a ring: pickup
		for i = 0, 7 do
			local o = P_SpawnMobjFromMobj(player.mo, FixedMul(cos(i * ANGLE_45), 80*FRACUNIT), FixedMul(sin(i * ANGLE_45), 80*FRACUNIT), 16*FRACUNIT, MT_ORB)
			if o then o.extravalue1 = i end
		end
	end
end)

-- a scan of every object each tic: counts per type, mobjs with a Lua field, tracers and targets valid
local scans, lastsum = 0, ""
addHook("ThinkFrame", function()
	local n, withtgt, withtrace, lf = 0, 0, 0, 0
	local types = {}
	for mo in mobjs.iterate() do
		n = n + 1
		types[mo.type] = (types[mo.type] or 0) + 1
		if mo.target and mo.target.valid then withtgt = withtgt + 1 end
		if mo.tracer and mo.tracer.valid then withtrace = withtrace + 1 end
		if mo.lm_age then lf = lf + 1 end
	end
	scans = scans + 1
	E(30, n, withtgt, withtrace, lf)
	-- the stale references in the Lua table: valid or not, never an error when asked for .valid
	local alive, dead = 0, 0
	for i = #bombs, 1, -1 do
		local b = bombs[i]
		if b.valid then alive = alive + 1 else dead = dead + 1 table.remove(bombs, i) end
	end
	if leveltime % 70 == 0 and leveltime > 0 then
		local tl = {}
		for ty, c in pairs(types) do if ty == MT_BOMB or ty == MT_SPARK or ty == MT_ORB then tl[#tl+1] = (ty == MT_BOMB and "B" or ty == MT_SPARK and "S" or "O") .. c end end
		table.sort(tl)
		P("tic", leveltime, "n", n, "bombs", nbombs, maxbombs, alive, "tgt", withtgt, "trace", withtrace, "lf", lf, table.concat(tl, ","), "ev", ev, evn, generic, removedgen)
		local cl = {}
		for k, v in pairs(cnt) do cl[#cl+1] = k .. "=" .. v end
		table.sort(cl)
		P("cnt", table.concat(cl, " "))
	end
	if leveltime == 630 then P("DONE_LMOBJ") end
end)
P("registered")
