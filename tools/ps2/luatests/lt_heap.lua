-- OPT14: the size of the Lua heap as the zone sees it (the "Lua heap" line of `memfree`, PS2 only) against what Lua itself counts (collectgarbage("count")): after the load of the scripts, after a
-- full collection, after phases that make and drop many objects of different sizes. LT lines are for people (not compared), LQ lines are the script's own results.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function LT(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LT " .. table.concat(s, " "))
end
local function report(tag)
	collectgarbage()
	LT("heap", tag, collectgarbage("count"))
	COM_BufInsertText(server, "memfree")
end
local phase = 0
local junk
addHook("ThinkFrame", function()
	if leveltime == 3 and phase == 0 then
		phase = 1
		report("after_load")
	elseif leveltime == 5 and phase == 1 then
		phase = 2
		junk = {}
		for i = 1, 15000 do junk[i] = ("a"):rep(i % 24) .. i end
		report("small_strings_alive")
		junk = nil
		report("small_strings_dropped")
	elseif leveltime == 8 and phase == 2 then
		phase = 3
		junk = {}
		for i = 1, 6000 do junk[i] = ("b"):rep(80 + i % 100) .. i end
		report("medium_strings_alive")
		junk = nil
		report("medium_strings_dropped")
	elseif leveltime == 11 and phase == 3 then
		phase = 4
		junk = {}
		for i = 1, 1500 do junk[i] = ("c"):rep(300 + i % 200) .. i end
		report("large_strings_alive")
		junk = nil
		report("large_strings_dropped")
	elseif leveltime == 14 and phase == 4 then
		phase = 5
		junk = {}
		for i = 1, 12000 do junk[i] = {i, i + 1, i + 2} end
		report("small_tables_alive")
		junk = nil
		report("small_tables_dropped")
		junk = {}
		for i = 1, 3000 do junk[i] = ("d"):rep(40 + i % 20) end
		report("again_small_strings")
		P("DONE_HEAP")
	end
end)
