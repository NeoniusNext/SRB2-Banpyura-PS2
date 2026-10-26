-- OPT14: polyobjects seen from Lua on a map that has some (MAP02): line.polyobj, subsector.polyList, polyobjects.iterate, the fields of each object. lt_maps found a difference of the PC and the PS2
-- in the hash of the lines and subsectors of MAP02 (the only one of its first three maps with polyobjects).
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local done = false
addHook("ThinkFrame", function()
	if done or leveltime < 4 then return end
	done = true
	local pos = {}
	for po in polyobjects.iterate do pos[#pos + 1] = po end
	P("polyobjects", #pos)
	local pidx = {}
	for i, po in ipairs(pos) do pidx[po] = i end
	local function pid(po) return po and (pidx[po] and ("po#" .. pidx[po]) or ("?" .. type(po))) or "nil" end
	for i, po in ipairs(pos) do
		P("po", i, po.id, po.parent, #po.vertices, #po.lines, po.angle, po.damage, po.thrust, po.flags, po.translucency, po.triggertag)
	end
	local nl, shown = 0, 0
	for i = 0, #lines - 1 do
		local ok, po = pcall(function() return lines[i].polyobj end)
		if not ok then P("line", i, "ERR", po) elseif po then nl = nl + 1 if shown < 40 then shown = shown + 1 P("line", i, pid(po), type(po)) end end
	end
	P("lines with polyobj", nl)
	local ns, shown2 = 0, 0
	for i = 0, #subsectors - 1 do
		local ok, pl = pcall(function() return subsectors[i].polyList end)
		if not ok then P("ss", i, "ERR", pl) elseif pl then ns = ns + 1 if shown2 < 40 then shown2 = shown2 + 1 P("ss", i, pid(pl), type(pl)) end end
	end
	P("subsectors with polyList", ns)
	P("DONE_POLY")
end)
