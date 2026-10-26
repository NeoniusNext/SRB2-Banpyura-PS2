-- Lua equivalence: every hook of lua_hook.h is registered; the calls and a digest of their arguments are counted and printed every 100 tics of a demo (both builds play the same demo).
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local cnt, acc = {}, {}
local function mix(name, v) acc[name] = ((acc[name] or 0) * 31 + v) % 1000003 end
local function digest(name, ...)
	cnt[name] = (cnt[name] or 0) + 1
	for i = 1, select("#", ...) do
		local a = select(i, ...)
		local ta = type(a)
		if ta == "number" then mix(name, a % 65536)
		elseif ta == "boolean" then mix(name, a and 1 or 2)
		elseif ta == "string" then mix(name, #a)
		elseif ta == "userdata" then
			local ok, x = pcall(function() return a.valid end)
			if ok and x then
				local o, ty = pcall(function() return a.type end)
				if o and type(ty) == "number" then mix(name, ty) end
				local o2, px = pcall(function() return a.x end)
				if o2 and type(px) == "number" then mix(name, (px >> 16) % 65536) end
				local o3, pz = pcall(function() return a.z end)
				if o3 and type(pz) == "number" then mix(name, (pz >> 16) % 65536) end
				local o4, hp = pcall(function() return a.health end)
				if o4 and type(hp) == "number" then mix(name, hp % 65536) end
			elseif ok then mix(name, 77) else mix(name, 99) end
		elseif ta == "table" then mix(name, 55)
		else mix(name, 11) end
	end
end
local function hook(name, ...)
	local extra = {...}
	local ok, err = pcall(addHook, name, function(...) digest(name, ...) end, unpack(extra))
	if not ok then P("addhook_err", name, (tostring(err):gsub("^.-:%d+: ", ""))) end
end
-- Mobj hooks: generic (every type) and by type
local mobjhooks = {"MobjSpawn", "MobjCollide", "MobjLineCollide", "MobjMoveCollide", "TouchSpecial", "MobjFuse", "MobjThinker", "BossThinker", "ShouldDamage", "MobjDamage", "MobjDeath", "BossDeath", "MobjRemoved", "BotRespawn", "MobjMoveBlocked", "MapThingSpawn", "FollowMobj", "HurtMsg"}
for _, h in ipairs(mobjhooks) do
	hook(h) -- all types
	hook(h, MT_PLAYER)
	hook(h, MT_BLUECRAWLA)
	hook(h, MT_RING)
	hook(h, MT_FLINGRING)
	hook(h, MT_THOK)
end
local globalhooks = {"MapChange", "MapLoad", "PlayerJoin", "PreThinkFrame", "ThinkFrame", "PostThinkFrame", "JumpSpecial", "AbilitySpecial", "SpinSpecial", "JumpSpinSpecial", "BotTiccmd", "PlayerMsg", "PlayerSpawn", "ShieldSpawn", "ShieldSpecial", "PlayerCanDamage", "PlayerQuit", "IntermissionThinker", "TeamSwitch", "ViewpointSwitch", "SeenPlayer", "PlayerThink", "GameQuit", "PlayerCmd", "MusicChange", "PlayerHeight", "PlayerCanEnterSpinGaps", "AddonLoaded", "CameraThinker", "KeyDown", "KeyUp"}
for _, h in ipairs(globalhooks) do hook(h) end
hook("NetVars")
hook("BotAI", "sonic") hook("BotAI", "tails")
hook("LinedefExecute", "LQEXEC")
hook("ShouldJingleContinue", "LQJINGLE")
for _, bad in ipairs({"NoSuchHook", "mobjthinker"}) do
	local ok, err = pcall(addHook, bad, function() end)
	P("badhook", bad, ok, (tostring(err):gsub("^.-:%d+: ", "")))
end
-- the type argument is checked
local ok, err = pcall(addHook, "MobjThinker", function() end, 99999) P("badtype", ok, (tostring(err):gsub("^.-:%d+: ", "")))
local ft = freeslot("MT_LQHK") ok, err = pcall(addHook, "MobjThinker", function() end, ft) P("freetype", ft, ok)
ok, err = pcall(addHook, "MobjThinker", function() end, ft + 700) P("freetype2", ft + 700, ok)
ok, err = pcall(addHook, "ThinkFrame", "notafunction") P("notfn", ok, (tostring(err):gsub("^.-:%d+: ", "")))

-- world digest every 100 tics: all mobjs (count per type, positions), players
local nextprint = 100
local function summary()
	local names = {}
	for k in pairs(cnt) do names[#names+1] = k end
	table.sort(names)
	local parts = {}
	for _, k in ipairs(names) do parts[#parts+1] = k .. "=" .. cnt[k] .. "/" .. (acc[k] or 0) end
	return table.concat(parts, " ")
end
local function world()
	local n, h = 0, 17
	local types = {}
	for mo in mobjs.iterate() do
		n = n + 1
		h = (h * 31 + (mo.x >> 16) + (mo.y >> 16) * 3 + (mo.z >> 16) * 7 + mo.type * 11 + mo.health) % 1000003
		types[mo.type] = (types[mo.type] or 0) + 1
	end
	local tl = {} for ty, c in pairs(types) do tl[#tl+1] = ty .. ":" .. c end table.sort(tl)
	return n, h, table.concat(tl, ",")
end
addHook("ThinkFrame", function()
	if leveltime == nextprint then
		local n, h, tl = world()
		local p = players[0]
		P("tic", leveltime, "mobjs", n, h, tl)
		if p and p.mo then
			P("player", p.mo.x, p.mo.y, p.mo.z, p.mo.momx, p.mo.momy, p.mo.momz, p.rings, p.score, p.pflags, p.speed, p.mo.state == S_PLAY_STND)
		end
		P("hooks", summary())
		nextprint = nextprint + 100
		if leveltime >= 700 then P("DONE_HOOKS") end
	end
end)
P("registered", "ok")
