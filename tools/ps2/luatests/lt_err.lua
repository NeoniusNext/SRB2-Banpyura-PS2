-- Lua equivalence: error messages and tracebacks. Each hook fails once with another kind of error; later failures of the same hook are not reported (hooksErrored).
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function lvl3() local t = nil return t.x end
local function lvl2() return lvl3() + 1 end
local function lvl1() return lvl2() end
local n = 0
addHook("PreThinkFrame", function() if leveltime == 3 then error("plain string error") end end)
addHook("ThinkFrame", function()
	if leveltime == 3 then lvl1() end
	if leveltime == 5 then P("DONE_ERR") end
end)
addHook("PostThinkFrame", function() if leveltime == 3 then error({code = 7}) end end)
addHook("PlayerThink", function(p) if leveltime == 3 then local f = nil f() end end)
addHook("MobjThinker", function(mo) if leveltime == 3 and not n then n = 1 end end, MT_PLAYER)
addHook("PlayerCmd", function(p, cmd) end)
addHook("MobjSpawn", function(mo) if leveltime == 4 and mo.type ~= MT_NULL then local x = mo.nosuchfield end end)
addHook("MapLoad", function() local s = "a" .. nil end)
-- errors with xpcall / pcall, message and traceback texts
local ok, e = xpcall(lvl1, function(m) return m end) P("xp1", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(error) P("err0", ok, e)
ok, e = pcall(error, "msg", 0) P("err1", ok, e)
ok, e = pcall(error, "msg", 1) P("err2", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(error, "msg", 2) P("err3", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(error, nil) P("errnil", ok, e)
ok, e = pcall(function() return #5 end) P("lenerr", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() local a = {} a[nil] = 1 end) P("nilidx", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() return ("x"):nosuch() end) P("nomethod", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() return mobjinfo[99999] end) P("mobjinfo", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() return states[99999] end) P("states", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() return P_SpawnMobj(0, 0, 0, 99999) end) P("spawn", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() return S_StartSound(nil, 99999) end) P("snd", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() return string.rep("x", -1) end) P("rep", ok, e)
ok, e = pcall(string.format, "%d", "x") P("fmt", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(string.format, "%q", 1) P("fmtq", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(table.concat, {1, {}, 3}) P("concat", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(setmetatable, {}, 5) P("setmt", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(select, 0) P("select", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() for i = 1, "x" do end end) P("forerr", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() local t = setmetatable({}, {__index = function(t, k) error("idx " .. k) end}) return t.foo end) P("mmerr", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() players[0].mo.nosuch = 1 end) P("setfield", ok, (tostring(e):gsub("^.-:%d+: ", "")))
ok, e = pcall(function() return players[99] end) P("player99", ok, e)
ok, e = pcall(function() return lines[999999] end) P("line999999", ok, e)
ok, e = pcall(function() return sectors[-1] end) P("sectorneg", ok, e)
P("traceback", type(debug) , type(debug and debug.traceback))
P("REGISTERED")
