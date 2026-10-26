-- OPT14: the worst C recursion a script can start (string.gsub callbacks, 200 levels, the recursion that holds the most C stack per level) from one chosen hook: LM_CTX is replaced by the test
-- driver (sed) with thinker / hud / playerthink / maploadhook / mapchange / chat / netvars / addonloaded / intermission / prethink / hudscores.
-- ZSTAT stackused (-zstack -zquit N) of that run is the deepest the 384 KiB main stack went in that context.
local LM_CTX = "thinker"
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function msg(e) return (tostring(e):gsub("[%w_/%.%-:]*lt_stack3%.lua:%d+: ", "")):sub(1, 60) end
local done = false
local function worst()
	if done then return end
	done = true
	local cnt = 0
	local function f(s)
		cnt = cnt + 1
		return (s:gsub(".", f))
	end
	local ok, e = pcall(f, "ab")
	P("gsub", LM_CTX, ok, msg(e), cnt)
	cnt = 0
	local function cmp(a, b)
		cnt = cnt + 1
		table.sort({3, 2, 1}, cmp)
		return a < b
	end
	ok, e = pcall(table.sort, {3, 2, 1}, cmp)
	P("sort", LM_CTX, ok, msg(e), cnt)
	cnt = 0
	local function ff(n)
		cnt = cnt + 1
		return string.format("%s%d", (("x"):rep(3)):gsub("x", function() return ff(n + 1) end), n)
	end
	ok, e = pcall(ff, 1)
	P("format", LM_CTX, ok, msg(e), cnt)
	P("DONE_STACK3")
end

if LM_CTX == "thinker" then
	addHook("MobjThinker", function(mo) if leveltime >= 5 then worst() end end, MT_PLAYER)
elseif LM_CTX == "playerthink" then
	addHook("PlayerThink", function(p) if leveltime >= 5 then worst() end end)
elseif LM_CTX == "prethink" then
	addHook("PreThinkFrame", function() if leveltime >= 5 then worst() end end)
elseif LM_CTX == "hud" then
	hud.add(function(v) if leveltime >= 5 then worst() end end, "game")
elseif LM_CTX == "hudscores" then
	hud.add(function(v) if leveltime >= 5 then worst() end end, "scores")
elseif LM_CTX == "maploadhook" then
	addHook("MapLoad", function() worst() end)
elseif LM_CTX == "mapchange" then
	addHook("MapChange", function() worst() end)
elseif LM_CTX == "chat" then
	addHook("PlayerMsg", function() worst() return false end)
	addHook("ThinkFrame", function() if leveltime == 6 then COM_BufInsertText(server, "say hello") end end)
elseif LM_CTX == "netvars" then
	addHook("NetVars", function(n) worst() end)
	addHook("ThinkFrame", function() if leveltime == 6 and not done then worst() end end) -- (no savegame is written in single player: the chain is run from the thinker too)
elseif LM_CTX == "addonloaded" then
	addHook("AddonLoaded", function() worst() end)
elseif LM_CTX == "intermission" then
	addHook("IntermissionThinker", function() worst() end)
	addHook("ThinkFrame", function() if leveltime == 6 then G_ExitLevel() end end)
elseif LM_CTX == "main" then
	worst()
end
P("registered", LM_CTX)
