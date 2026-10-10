-- OPT14: after a level change every line / subsector / sector / side / vertex a script reads must be valid (lt_maps found MAP02 as the second level different on PC and PS2)
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local keep = {}
local MAPS = {1, 2, 1, 2}
local mi, state = 1, 0
local function count(name, arr)
	local bad, first = 0, nil
	for i = 0, #arr - 1 do
		local ok, v = pcall(function() return arr[i].valid end)
		if not (ok and v == true) then bad = bad + 1 if not first then first = i end end
	end
	return name .. "=" .. #arr .. "/" .. bad .. "/" .. tostring(first)
end
addHook("ThinkFrame", function()
	if mi > #MAPS then return end
	if state == 0 and leveltime == 4 and gamemap == MAPS[mi] then
		state = 1
		P("map", gamemap, count("sectors", sectors), count("lines", lines), count("sides", sides), count("vertexes", vertexes), count("subsectors", subsectors))
		-- keep userdata of this level in a table, as lt_maps does (its index tables)
		keep = {}
		for i = 0, #sectors - 1 do keep[#keep + 1] = sectors[i] end
		for i = 0, #lines - 1 do keep[#keep + 1] = lines[i] end
		for i = 0, #sides - 1 do keep[#keep + 1] = sides[i] end
		for i = 0, #vertexes - 1 do keep[#keep + 1] = vertexes[i] end
		for i = 0, #subsectors - 1 do keep[#keep + 1] = subsectors[i] end
		P("kept", #keep)
		mi = mi + 1
		if mi > #MAPS then P("DONE_STALE") return end
		COM_BufInsertText(server, "map " .. MAPS[mi] .. " -force")
		state = 0
	end
end)
