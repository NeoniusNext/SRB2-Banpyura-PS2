-- OPT14: patches and colormaps a HUD script keeps in tables survive a change of renderer (Software <-> Hardware) and a level change. The script draws every frame; the LQ lines say whether the kept
-- userdata are still valid, whether any draw raised an error, and which renderer the engine reports.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local kept, spr, cmaps = {}, {}, {}
local errs, errset = 0, {}
local frames = 0
local function try(f, ...)
	local ok, e = pcall(f, ...)
	if not ok then errs = errs + 1 errset[(tostring(e):gsub("^.-:%d+: ", ""))] = true end
end
hud.add(function(v)
	frames = frames + 1
	if next(kept) == nil then
		for _, n in ipairs({"STTNUM0", "STTRINGS", "TNYFN065", "RINGA0", "CROSHAI1", "M_SONIC"}) do if v.patchExists(n) then kept[n] = v.cachePatch(n) end end
		for _, s in ipairs({SPR_PLAY, SPR_THOK, SPR_RING, SPR_FISH}) do local p = v.getSpritePatch(s, 0, 0, 0) if p then spr[#spr + 1] = p end end
		for c = SKINCOLOR_RED, SKINCOLOR_RED + 5 do cmaps[#cmaps + 1] = v.getColormap(TC_DEFAULT, c) end
	end
	local x = 4
	for n, p in pairs(kept) do try(v.draw, x, 20, p, 0) x = x + 24 end
	for i, p in ipairs(spr) do try(v.drawScaled, (40 + i * 30) * FRACUNIT, 100 * FRACUNIT, FRACUNIT, p, 0, cmaps[(i % #cmaps) + 1]) end
	try(v.drawString, 4, 4, "render " .. leveltime, V_ALLOWLOWERCASE, "small")
end, "game")
local function valid()
	local n, tot = 0, 0
	for _, p in pairs(kept) do tot = tot + 1 local ok, w = pcall(function() return p.width end) if ok and w > 0 then n = n + 1 end end
	for _, p in ipairs(spr) do tot = tot + 1 local ok, w = pcall(function() return p.width end) if ok and w > 0 then n = n + 1 end end
	return n, tot
end
local step = {[230] = "map 2 -force"} -- (the renderer cannot be changed from a script: the PS2 run changes it with -netcmd file:cmd.txt, see tools/ps2/lua_renderswitch.sh)
local rep = {[60] = true, [130] = true, [200] = true, [300] = true, [400] = true}
local last = -1
addHook("ThinkFrame", function()
	local t = leveltime
	if t == last then return end
	last = t
	if gamemap == 1 or t > 5 then
		if step[t] and gamemap == 1 then COM_BufInsertText(server, step[t]) end -- (once: the next map is another one)
		if rep[t] then
			local n, tot = valid()
			local es = {} for k in pairs(errset) do es[#es + 1] = k end table.sort(es)
			P("check", t, gamemap, n, tot, errs > 0, table.concat(es, "|"), frames > 20)
			if t == 400 then P("DONE_RENDERSWITCH") end
		end
	end
end)
