-- Lua equivalence (OPT13 IQ-6): mobj hooks registered for ONE type and kind only (lt_hooks.lua registers every kind for every type, so the "no hook of this kind for this type" path is never taken there).
-- The engine of the PS2 port asks a per-type, per-kind mask before it enters the hook machinery (LUA_MobjHookWanted); the PC build asks mobj_hook_available inside it. Both must call the same hooks
-- for the same objects: the calls and a digest of their arguments are counted per (hook, type) and printed every 100 tics of a demo.
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
		elseif ta == "userdata" then
			local ok, x = pcall(function() return a.valid end)
			if ok and x then
				local o, ty = pcall(function() return a.type end)
				if o and type(ty) == "number" then mix(name, ty) end
				local o2, px = pcall(function() return a.x end)
				if o2 and type(px) == "number" then mix(name, (px >> 16) % 65536) end
				local o3, pz = pcall(function() return a.z end)
				if o3 and type(pz) == "number" then mix(name, (pz >> 16) % 65536) end
			else mix(name, 99) end
		else mix(name, 11) end
	end
end
local function hook(name, tag, ...)
	local extra = {...}
	local ok, err = pcall(addHook, name, function(...) digest(name .. ":" .. tag, ...) end, unpack(extra))
	if not ok then P("addhook_err", name, tag, (tostring(err):gsub("^.-:%d+: ", ""))) end
end
-- one kind for one type (a decoration at rest takes the quick path of the PS2 engine unless a MobjThinker hook is registered for it)
hook("MobjThinker", "flower1", MT_GFZFLOWER1)
hook("MobjThinker", "crawla", MT_BLUECRAWLA)
hook("MobjThinker", "player", MT_PLAYER)
hook("MobjCollide", "ring", MT_RING)
hook("MobjCollide", "crawla", MT_BLUECRAWLA)
hook("MobjLineCollide", "player", MT_PLAYER)
hook("MobjLineCollide", "crawla", MT_BLUECRAWLA)
hook("MobjMoveCollide", "player", MT_PLAYER)
hook("MobjFuse", "thok", MT_THOK)
hook("TouchSpecial", "ring", MT_RING)
hook("MobjSpawn", "crawla", MT_BLUECRAWLA)
hook("MobjSpawn", "flower2", MT_GFZFLOWER2)
hook("MobjRemoved", "ring", MT_RING)
hook("MobjDeath", "crawla", MT_BLUECRAWLA)
hook("MobjDamage", "player", MT_PLAYER)
hook("ShouldDamage", "crawla", MT_BLUECRAWLA)
hook("MobjMoveBlocked", "player", MT_PLAYER)
-- one kind for every type (row MT_NULL), nothing else generic
hook("MobjFuse", "all")
hook("BossThinker", "all")

local nextprint, finished = 100, false
local function summary()
	local names = {}
	for k in pairs(cnt) do names[#names+1] = k end
	table.sort(names)
	local parts = {}
	for _, k in ipairs(names) do parts[#parts+1] = k .. "=" .. cnt[k] .. "/" .. (acc[k] or 0) end
	return table.concat(parts, " ")
end
addHook("ThinkFrame", function()
	if not finished and leveltime == nextprint then
		local n, h = 0, 17
		for mo in mobjs.iterate() do
			n = n + 1
			h = (h * 31 + (mo.x >> 16) + (mo.y >> 16) * 3 + (mo.z >> 16) * 7 + mo.type * 11 + mo.health) % 1000003
		end
		P("tic", leveltime, "mobjs", n, h)
		P("hooks", summary())
		nextprint = nextprint + 100
		if leveltime >= 700 then finished = true P("DONE_HOOKS3") end -- (nothing is printed after the DONE line: the PC build runs on a little before the harness stops it)
	end
end)
P("registered", "ok")
