-- OPT14 stand: a HUD mod. Patches are cached once (v.cachePatch / v.getSpritePatch / v.getSprite2Patch) and kept in tables for the rest of the run, across a level change; ~150 draw calls
-- per frame (patches with every flag, scaled, stretched, cropped, strings in every font, numbers, fills, fades); hooks for every HUD type; hud.enable/disable of the default items.
-- The picture is not compared (the renderers differ); what is compared: values the library returns, whether any call raised an error (counted, the message is printed once), which hooks ran.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end

local kept = {}      -- patches held for the whole run
local sprites = {}   -- sprite patches
local errcount, errfirst = 0, nil
local errset = {}
local calls = {}     -- hook name -> {frames, draws}
local stage = {}     -- things done once per level
local drawn = 0

local function try(name, f, ...)
	local ok, err = pcall(f, ...)
	if not ok then
		errcount = 1 -- (a frame count is not comparable between the machines: only which errors happened)
		errset[name .. ": " .. tostring(err):gsub("^.-:%d+: ", "")] = true
		errfirst = errfirst or name
	end
	drawn = drawn + 1
	return ok, err
end

local fontlist = {"thin", "small", "left", "center", "right", "fixed", "thin-fixed", "small-fixed", "small-right", "small-thin-right", "thin-right"}
local widthfonts = {"thin", "small", "fixed"}
local patchnames = {"STTNUM0", "STTNUM5", "STTRINGS", "STTMINUS", "TNYFN065", "TNYFN090", "LTFNT065", "CRFNT065", "NTFNT065", "SONICLIF", "TAILSLIF", "KNUXLIF", "STTLIVE", "CROSHAI1", "CURSOR",
	"SLIDEBAR", "M_SONIC", "LSSONIC", "SBOSLIFE", "STCFN065", "STCFN097", "RINGA0", "THOKA0", "SPRKA0", "FISHA1", "CRABA1", "NOSUCHPATCH", "GFZFLOWR", "TTSONIC", "TTCREDIT"}

local function cachealls(v)
	for _, n in ipairs(patchnames) do
		if v.patchExists(n) then kept[n] = v.cachePatch(n) end
	end
	for _, spr in ipairs({SPR_PLAY, SPR_THOK, SPR_RING, SPR_SPRK, SPR_FISH, SPR_CRAB, SPR_BUZZ, SPR_GFZD}) do
		local p, flip = v.getSpritePatch(spr, 0, 0, 0)
		if p then sprites[#sprites + 1] = {p, flip, spr} end
	end
	for _, sk in ipairs({"sonic", "tails", "knuckles"}) do
		for _, an in ipairs({SPR2_STND, SPR2_WALK, SPR2_RUN, SPR2_SPIN, SPR2_LIFE, SPR2_SIGN}) do
			local p, flip = v.getSprite2Patch(sk, an, false, A, 0)
			if p then sprites[#sprites + 1] = {p, flip, sk .. an} end
		end
	end
end

local function kinfo()
	local names = {}
	for n in pairs(kept) do names[#names + 1] = n end
	table.sort(names)
	local parts = {}
	for _, n in ipairs(names) do local p = kept[n] parts[#parts + 1] = n .. ":" .. p.width .. "x" .. p.height .. "@" .. p.leftoffset .. "," .. p.topoffset end
	return table.concat(parts, " ")
end

local function drawall(v, tag)
	local c = calls[tag] or {frames = 0}
	calls[tag] = c
	c.frames = c.frames + 1
	if c.frames == 1 and next(kept) == nil then cachealls(v) end
	local w, h = v.width(), v.height()
	-- fills
	for i = 0, 15 do try("fill", v.drawFill, 4 + i * 18, 4, 16, 6, i * 16 + 3) end
	try("fill", v.drawFill, 0, 0, 2, 2, 31 | V_50TRANS)
	-- patches: all kept ones, with flags
	local x, y = 8, 16
	local flags = {0, V_FLIP, V_30TRANS, V_50TRANS, V_70TRANS, V_ADD, V_SUBTRACT, V_REVERSESUBTRACT, V_MODULATE, V_SNAPTOLEFT, V_SNAPTOTOP, V_HUDTRANS, V_PERPLAYER}
	local fi = 0
	for name, p in pairs(kept) do
		fi = fi + 1
		try("draw", v.draw, x, y, p, flags[(fi % #flags) + 1])
		try("drawscaled", v.drawScaled, (x + 4) * FRACUNIT, (y + 2) * FRACUNIT, FRACUNIT / 2 + (fi % 4) * FRACUNIT / 4, p, flags[(fi * 3 % #flags) + 1])
		x = x + 20
		if x > w - 24 then x = 8 y = y + 16 end
		if y > h - 40 then y = 16 end
	end
	for i, s in ipairs(sprites) do
		local p, flip = s[1], s[2]
		try("sprite", v.draw, 20 + (i % 12) * 24, h - 40 - (i / 12) * 4, p, flip and V_FLIP or 0, v.getColormap(TC_DEFAULT, SKINCOLOR_RED + i % 20))
		try("spritescaled", v.drawScaled, (30 + i * 6) * FRACUNIT, (h - 60) * FRACUNIT, FRACUNIT * 3 / 2, p, 0, v.getColormap("sonic", SKINCOLOR_BLUE + i % 10))
	end
	-- stretched, cropped
	local p0 = kept.RINGA0 or kept.STTNUM0
	if p0 then
		try("stretched", v.drawStretched, 10 * FRACUNIT, 100 * FRACUNIT, 3 * FRACUNIT, 2 * FRACUNIT, p0, 0)
		try("cropped", v.drawCropped, 60 * FRACUNIT, 100 * FRACUNIT, FRACUNIT, FRACUNIT, p0, 0, nil, 0, 0, 8 * FRACUNIT, 8 * FRACUNIT)
	end
	-- strings in every font and with flags
	for i, f in ipairs(fontlist) do
		try("string", v.drawString, 8 + (i % 3) * 100, 120 + (i / 3) * 10, "Mod text " .. i .. " ABC xyz", V_ALLOWLOWERCASE | (i % 2 == 0 and V_YELLOWMAP or V_GREENMAP), f)
		try("stringw", v.stringWidth, "Mod text " .. i, 0, widthfonts[i % 3 + 1])
	end
	try("name", v.drawNameTag, 200, 20, "SONIC", 0, SKINCOLOR_BLUE)
	try("title", v.drawLevelTitle, 160, 60, "GREENFLOWER", 0)
	try("kart", v.drawKartString or v.drawString, 8, 90, "12345", 0)
	-- numbers
	try("num", v.drawNum, 300, 20, c.frames, 0)
	try("padnum", v.drawPaddedNum, 300, 30, 42, 5, 0)
	-- fades
	try("fade", v.fadeScreen, 0xFF00, 3)
	c.draws = (c.draws or 0) + 1
	return w, h
end

local function sizes(v)
	return v.width(), v.height(), v.dupx(), v.dupy()
end

local sums = {}
local function hook(tag)
	return function(v, stplyr, cam)
		local w, h = drawall(v, tag)
		sums[tag] = {w, h}
	end
end
hud.add(hook("game"), "game")
hud.add(hook("scores"), "scores")
hud.add(hook("title"), "title")
hud.add(hook("titlecard"), "titlecard")
hud.add(hook("intermission"), "intermission")
hud.add(hook("continue"), "continue")

-- the default items on and off
local hudnames = {"stagetitle", "textspectator", "score", "time", "rings", "lives", "weaponrings", "powerstones", "nightslink", "nightsdrill", "nightsrings", "nightsscore", "nightstime", "nightsrecords", "rankings", "coopemeralds", "tokens", "tabemblems", "intermissiontally", "intermissionmessages", "intermissionemblems"}
local function enabledlist()
	local t = {}
	for _, n in ipairs(hudnames) do
		local ok, e = pcall(hud.enabled, n)
		t[#t + 1] = n .. "=" .. (ok and tostring(e) or "E")
	end
	return table.concat(t, ",")
end
local function errs()
	local l = {}
	for k in pairs(errset) do l[#l + 1] = k end
	table.sort(l)
	return #l .. ":" .. table.concat(l, "|")
end
P("hudenabled", enabledlist())
for i, n in ipairs(hudnames) do if i % 2 == 0 then pcall(hud.disable, n) end end
P("hudenabled2", enabledlist())
for i, n in ipairs(hudnames) do if i % 2 == 0 then pcall(hud.enable, n) end end
P("hudenabled3", enabledlist())
P("hudbad", pcall(hud.enable, "nosuchitem"))

local done = false
local reported = {}
addHook("ThinkFrame", function()
	if leveltime == 40 and not reported[1] then
		reported[1] = true
		local c = calls.game
		P("t40", c and c.frames > 0, next(kept) ~= nil, errs())
		P("kept", kinfo())
		local spi = {}
		for i, s in ipairs(sprites) do spi[#spi + 1] = s[3] .. ":" .. s[1].width .. "x" .. s[1].height .. "," .. tostring(s[2]) end
		P("sprites", #sprites, table.concat(spi, " "))
	end
	-- a level change with the patches still in the tables
	if leveltime == 80 and not reported[2] then
		reported[2] = true
		P("mapchange", gamemap, errs())
		COM_BufInsertText(server, "map 2 -force")
	end
	if gamemap == 2 and leveltime == 60 and not reported[3] then
		reported[3] = true
		local nv = 0
		for n, p in pairs(kept) do local ok, w = pcall(function() return p.width end) if ok then nv = nv + 1 end end
		P("after_map", gamemap, nv, errs(), calls.game.frames > 40, calls.titlecard and calls.titlecard.frames > 0)
		-- exit the level: intermission HUD
		G_ExitLevel()
	end
	if calls.intermission and calls.intermission.frames > 20 and not reported[4] then
		reported[4] = true
		P("intermission", calls.intermission.frames > 20, errs(), enabledlist())
		P("hooks", calls.game ~= nil, calls.titlecard ~= nil, calls.intermission ~= nil, calls.scores ~= nil, calls.title ~= nil)
		P("DONE_LMHUD")
	end
end)
P("registered")
