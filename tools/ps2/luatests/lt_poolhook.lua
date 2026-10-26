-- OPT14 (PS2-LUA-4): the memory of garbage that the collector freed is given back to the zone when the zone needs it. A level with ~6 MB free: 60 000 small tables are made and dropped, the
-- collector (steps only, no full collection by the script) frees them, then one 2 MB string is made (it needs about 4 MB while it is built). Without the reclaim hook of the pool the
-- zone has ~2 MB free and the PS2 ends with "Out of memory"; the PC has memory for all of it.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local done = false
addHook("ThinkFrame", function()
	if done or leveltime < 3 then return end
	done = true
	local junk = {}
	for i = 1, 60000 do junk[i] = {i, i, i} end
	local peak = collectgarbage("count")
	junk = nil
	for i = 1, 40 do collectgarbage("step", 100000) end -- (steps, not a full collection)
	COM_BufInsertText(server, "memfree")
	local after = collectgarbage("count")
	P("garbage", peak > 3000, after < peak / 2)
	local s = string.rep("x", 2000000)
	P("bigstring", #s)
	s = nil
	COM_BufInsertText(server, "memfree")
	P("DONE_POOLHOOK")
end)
