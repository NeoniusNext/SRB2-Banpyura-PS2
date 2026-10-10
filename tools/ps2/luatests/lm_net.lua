-- OPT14 stand, network: a mod whose state is synchronised through the NetVars hook (tables with shared and nested references, strings, booleans, numbers, mobj and player references,
-- Lua fields of mobjs), hooks that spawn and remove objects deterministically, a net cvar, a Lua console command and the chat hook.
-- Run on the server (PC dedicated) and on the clients (PS2 joining later, after the mod has built up state): both print "LQ ..." lines keyed by the game tic, the lines of one tic must be equal.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end

local MT_MARK = freeslot("MT_LMMARK")
local S_MARK = freeslot("S_LMMARK")
states[S_MARK] = {sprite = SPR_THOK, frame = A|FF_FULLBRIGHT, tics = -1}
mobjinfo[MT_MARK] = {
	doomednum = -1, spawnstate = S_MARK, spawnhealth = 1, seestate = S_NULL, seesound = sfx_None, reactiontime = 0, attacksound = sfx_None, painstate = S_NULL, painchance = 0, painsound = sfx_None,
	meleestate = S_NULL, missilestate = S_NULL, deathstate = S_NULL, xdeathstate = S_NULL, deathsound = sfx_None, speed = 0, radius = 12*FRACUNIT, height = 24*FRACUNIT,
	dispoffset = 0, mass = 100, damage = 0, activesound = sfx_None, flags = MF_NOGRAVITY|MF_NOBLOCKMAP, raisestate = S_NULL
}

-- the synchronised state
local S = {n = 0, nobj = 0, removed = 0, log = {}, names = {"alpha", "beta"}, flag = true, txt = "state", nested = {a = {b = {c = 1, d = {1, 2, 3}}}}, nums = {}, hits = {}}
S.shared = S.nested          -- the same table twice
S.self = S                   -- a cycle
for i = 1, 150 do S.nums[i] = i * i end
local ref, bigstr = nil, ("x"):rep(300)
S.bigstr = bigstr
local cvhits = 0

addHook("NetVars", function(net)
	S = net(S)
	ref = net(ref)
	cvhits = net(cvhits)
end)

local function hashtable(t, seen, depth)
	seen = seen or {}
	if seen[t] then return 7 end
	seen[t] = true
	local keys = {}
	for k in pairs(t) do keys[#keys + 1] = k end
	table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
	local h = 17
	for _, k in ipairs(keys) do
		local v = t[k]
		local tv = type(v)
		local hv
		if tv == "number" then hv = v % 100003
		elseif tv == "string" then hv = #v * 31 + v:byte(1) % 97
		elseif tv == "boolean" then hv = v and 1 or 2
		elseif tv == "table" then hv = hashtable(v, seen, depth)
		elseif tv == "userdata" then hv = 77
		else hv = 3 end
		h = (h * 31 + hv + #tostring(k)) % 1000003
	end
	return h
end

-- the marks: objects spawned by the script near the players, each with a Lua field
addHook("MobjSpawn", function(mo)
	mo.lm_tag = S.n
	mo.lm_name = "mark" .. S.n
end, MT_MARK)
addHook("MobjThinker", function(mo)
	mo.lm_seen = (mo.lm_seen or 0) + 1
	if mo.lm_seen % 16 == 0 then mo.z = mo.z + FRACUNIT end
	if mo.fuse == 0 then end
end, MT_MARK)
addHook("MobjRemoved", function(mo)
	S.nobj = S.nobj - 1
	S.removed = S.removed + 1
end, MT_MARK)

addHook("ThinkFrame", function()
	S.n = S.n + 1
	local p = players[0]
	if p and p.mo and p.mo.valid then
		if S.n % 20 == 0 and S.nobj < 12 then
			local x = p.mo.x + P_RandomRange(-200, 200) * FRACUNIT
			local y = p.mo.y + P_RandomRange(-200, 200) * FRACUNIT
			local m = P_SpawnMobj(x, y, p.mo.z + 32 * FRACUNIT, MT_MARK)
			m.fuse = 105 + P_RandomRange(0, 70)
			m.tracer = p.mo
			S.nobj = S.nobj + 1
			if S.n == 40 then ref = m S.hits.first = m.lm_tag end
		end
		S.hits[S.n % 5] = (S.hits[S.n % 5] or 0) + 1
	end
	if S.n % 35 == 0 then S.log[#S.log + 1] = S.n end
	if S.n % 100 == 0 then
		local n, h, ext = 0, 17, 0
		for mo in mobjs.iterate() do
			if mo.type == MT_MARK then
				n = n + 1
				h = (h * 31 + (mo.x >> 16) + (mo.y >> 16) * 3 + (mo.z >> 16) * 7 + mo.fuse) % 1000003
				ext = ext + (mo.lm_seen or 0) * 3 + (mo.lm_tag or 0)
			end
		end
		P("tic", S.n, "marks", n, h, ext, "state", hashtable(S), #S.log, S.nobj, S.removed, S.shared == S.nested, S.self == S, #S.nums, #S.bigstr)
		P("ref", S.n, ref ~= nil and ref.valid or false, ref and ref.valid and ref.type == MT_MARK, ref and ref.valid and ref.lm_name or "-", S.hits.first, cvhits)
	end
end)

-- a net variable, a command, the chat
local lmcv = CV_RegisterVar({name = "lm_scale", defaultvalue = "3", flags = CV_NETVAR|CV_CALL, PossibleValue = {MIN = 0, MAX = 100}, func = function(var)
	cvhits = cvhits + 1
	P("cv", "lm_scale", var.value, S.n > 0)
end})
COM_AddCommand("lm_cmd", function(player, a, b, c)
	P("cmd", "lm_cmd", a, b, c, S.n % 10000 >= 0)
	S.hits.cmd = (S.hits.cmd or 0) + 1
end)
addHook("PlayerMsg", function(source, msgtype, target, msg)
	if msg:sub(1, 3) == "!lm" then
		P("chat", msgtype, msg, S.n >= 0)
		S.hits.chat = (S.hits.chat or 0) + 1
	end
end)
addHook("PlayerJoin", function(pn) P("join", pn, S.n >= 0) end)

-- a HUD so that the HUD path runs on the client as well
hud.add(function(v, player)
	v.drawString(4, 4, "LM " .. S.n, V_ALLOWLOWERCASE | V_YELLOWMAP, "small")
	v.drawFill(4, 12, 40 + S.nobj * 4, 3, 35)
end)
P("registered", MT_MARK)
