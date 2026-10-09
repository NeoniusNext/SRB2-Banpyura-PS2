-- Lua equivalence: HUD library. Values the library returns, patches (size, offsets), colormaps (as the cache index), string widths; every drawing function is
-- called with a wide range of arguments (the result of drawing is the engine's, here: the same calls must be accepted or refused on both sides).
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function E(f, ...) local r = {pcall(f, ...)} if r[1] then table.remove(r, 1) for i = 1, #r do r[i] = (type(r[i]) == "userdata") and "userdata" or tostring(r[i]) end return table.concat(r, ",") end return "E:" .. (tostring(r[2]):gsub("^.-:%d+: ", "")) end
local done = false
local frames = 0
local function patchinfo(v, name)
	local ex = v.patchExists(name)
	if not ex then return name .. ":none" end
	local p = v.cachePatch(name)
	return string.format("%s:%d,%d,%d,%d", name, p.width, p.height, p.leftoffset, p.topoffset)
end
hud.add(function(v, stplyr, cam)
	frames = frames + 1
	if done or leveltime < 4 then return end
	done = true
	P("hudapi", E(v.width), E(v.height), E(v.dupx), E(v.dupy), E(v.localTransFlag), E(v.userTransFlag))
	local names = {"STTNUM0", "STTRINGS", "STTPERIO", "STTMINUS", "STTPERCENT", "TNYFN065", "LTFNT065", "CRFNT065", "NTFNT065", "SONICLIF", "TAILSLIF", "KNUXLIF", "GFZ1OCEAN", "STTLIVE", "TTSONIC", "RINGS", "NOTAPATCH",
		"CHECKER", "DEFAULT", "HUDBACK", "CROSHAI1", "CURSOR", "SLIDEBAR", "M_SONIC", "LSSONIC", "SBOSLIFE", "TWRINGS", "CRFNT065", "STTRING"}
	for _, n in ipairs(names) do P("patch", E(patchinfo, v, n)) end
	local sp = {}
	for _, spr in ipairs({SPR_PLAY, SPR_THOK, SPR_RING, SPR_GFZD, SPR_CRAB, SPR_FISH, SPR_SPRK, SPR_NULL}) do
		sp[#sp+1] = E(function()
			local p, flip = v.getSpritePatch(spr, 0, 0, 0)
			if not p then return "nil" end
			return p.width .. "," .. p.height .. "," .. p.leftoffset .. "," .. p.topoffset .. "," .. tostring(flip)
		end)
	end
	P("sprpatch", table.concat(sp, " | "))
	P("sprpatch2", E(function() local p, f = v.getSpritePatch("RING", 0, 0, 0) return p and (p.width .. "," .. p.height) or "nil" end),
		E(function() local p, f = v.getSpritePatch("NOSUCH", 0, 0, 0) return p and "p" or "nil" end),
		E(function() local p, f = v.getSpritePatch(SPR_PLAY, 99, 0, 0) return p and "p" or "nil" end),
		E(function() local p, f = v.getSpritePatch(SPR_PLAY, 0, 9, 0) return p and "p" or "nil" end),
		E(function() local p, f = v.getSpritePatch(SPR_PLAY, 0, 0, 1) return p and (p.width .. "," .. p.height .. "," .. tostring(f)) or "nil" end))
	local s2 = {}
	for _, sk in ipairs({"sonic", "tails", "knuckles", "nosuch"}) do
		for _, an in ipairs({SPR2_STND, SPR2_WALK, SPR2_RUN, SPR2_SPIN, SPR2_LIFE}) do
			s2[#s2+1] = E(function() local p, f = v.getSprite2Patch(sk, an, false, A, 0) return p and (p.width .. "," .. p.height .. "," .. p.leftoffset .. "," .. tostring(f)) or "nil" end)
		end
	end
	P("spr2patch", table.concat(s2, " | "))
	local sw = {}
	for _, str in ipairs({"", "Hello", "HELLO WORLD", "A longer string with Mixed Case 123", "\130colored\128 text", "\195\169 high"}) do
		for _, fl in ipairs({0, V_ALLOWLOWERCASE, V_6WIDTHSPACE, V_ALLOWLOWERCASE|V_6WIDTHSPACE}) do
			for _, fo in ipairs({"normal", "thin", "small", "left", "center", "right", "fixed", "thin-fixed", "small-fixed", "small-right", "small-thin-right", "thin-right", "nosuch", "ooxx"}) do
				sw[#sw+1] = E(v.stringWidth, str, fl, fo)
			end
		end
	end
	P("stringwidth", #sw, table.concat(sw, ","))
	P("tagwidth", E(v.nameTagWidth, "SONIC"), E(v.nameTagWidth, ""), E(v.nameTagWidth, "Mixed Case"), E(v.levelTitleWidth, "GREENFLOWER"), E(v.levelTitleWidth, "ZONE"), E(v.levelTitleHeight, "GREENFLOWER"), E(v.levelTitleHeight, "x"))
	local cm = {}
	for _, c in ipairs({SKINCOLOR_NONE or 0, SKINCOLOR_RED, SKINCOLOR_BLUE, SKINCOLOR_GREEN, SKINCOLOR_SUPERGOLD1, SKINCOLOR_PURPLE}) do
		cm[#cm+1] = E(function() local m = v.getColormap(TC_DEFAULT, c) return type(m) == "userdata" and "cm" or tostring(m) end)
		cm[#cm+1] = E(function() local m = v.getColormap("sonic", c) return type(m) == "userdata" and "cm" or tostring(m) end)
		cm[#cm+1] = E(function() local m = v.getColormap(0, c) return type(m) == "userdata" and "cm" or tostring(m) end)
	end
	P("colormap", table.concat(cm, ","))
	P("strcmap", E(function() return type(v.getStringColormap(V_YELLOWMAP)) end), E(function() return type(v.getStringColormap(0)) end), E(function() return type(v.getStringColormap(V_PURPLEMAP | V_ALLOWLOWERCASE)) end))
	P("seccmap", E(function() return type(v.getSectorColormap(players[0].mo.subsector.sector, 0, 0, 0)) end))
	-- all drawing functions, many argument shapes (the engine records or draws them)
	local pt = v.cachePatch("STTRINGS") or v.cachePatch("STTNUM0")
	local calls = {
		function() v.draw(10, 10, pt) end, function() v.draw(10, 10, pt, V_SNAPTOLEFT) end, function() v.draw(10, 10, pt, V_SNAPTOTOP|V_50TRANS) end,
		function() v.draw(-10, 400, pt, V_HUDTRANS) end, function() v.draw(10, 10, pt, V_ADD, v.getColormap(TC_DEFAULT, SKINCOLOR_RED)) end,
		function() v.draw(10, 10, nil) end, function() v.draw("a", 10, pt) end, function() v.draw() end,
		function() v.drawScaled(10*FU, 10*FU, FU/2, pt) end, function() v.drawScaled(10*FU, 10*FU, 0, pt, V_30TRANS) end, function() v.drawScaled(10*FU, 10*FU, -FU, pt) end,
		function() v.drawScaled(10*FU, 10*FU, FU*100, pt) end, function() v.drawStretched(10*FU, 10*FU, FU, FU*2, pt) end, function() v.drawStretched(10*FU, 10*FU, 0, 0, pt) end,
		function() v.drawCropped(10*FU, 10*FU, FU, FU, pt, 0, 0, 0, 5*FU, 5*FU) end, function() v.drawCropped(10*FU, 10*FU, FU, FU, pt, 0, 0, 0, -5*FU, 5*FU) end,
		function() v.drawNum(100, 100, 12345) end, function() v.drawNum(100, 100, -5, V_YELLOWMAP) end, function() v.drawNum(100, 100, 2147483647) end,
		function() v.drawPaddedNum(100, 100, 12, 5) end, function() v.drawPaddedNum(100, 100, 123456, 3) end, function() v.drawPaddedNum(100, 100, 5, 0) end, function() v.drawPaddedNum(100, 100, 5, 99) end,
		function() v.drawFill(0, 0, 10, 10, 35) end, function() v.drawFill(-5, -5, 5000, 5000, 255|V_SNAPTOLEFT) end, function() v.drawFill(0, 0, 0, 0, 0) end, function() v.drawFill() end,
		function() v.drawString(10, 10, "Hello") end, function() v.drawString(10, 10, "Hello", V_YELLOWMAP|V_ALLOWLOWERCASE, "small") end, function() v.drawString(10, 10, "Hello", 0, "thin-right") end,
		function() v.drawString(10, 10, "Hello", 0, "fixed") end, function() v.drawString(10*FU, 10*FU, "Hello", 0, "fixed-center") end, function() v.drawString(10, 10, "Hello", 0, "nosuchalign") end,
		function() v.drawString(10, 10, string.rep("x", 600)) end, function() v.drawString(10, 10, "") end, function() v.drawString(10, 10, nil) end,
		function() v.drawNameTag(10, 10, "SONIC") end, function() v.drawNameTag(10, 10, "SONIC", V_ALLOWLOWERCASE, v.getColormap(TC_DEFAULT, SKINCOLOR_RED), SKINCOLOR_BLUE) end,
		function() v.drawScaledNameTag(10*FU, 10*FU, "SONIC", 0, FU/2) end, function() v.drawLevelTitle(10, 10, "GREENFLOWER", 0) end, function() v.drawLevelTitle(10, 10, "", 0) end,
		function() v.fadeScreen(0xFF00, 5) end, function() v.fadeScreen(0xFF00, 0) end, function() v.fadeScreen(0xFF00, 99) end, function() v.fadeScreen(0, 3) end,
	}
	local res = {}
	for i, f in ipairs(calls) do res[#res+1] = i .. "=" .. (E(f) == "" and "ok" or E(f)) end
	P("draws", table.concat(res, " "))
	P("hudenable", E(hud.enabled, "rings"), E(hud.enabled, "lives"), E(hud.enabled, "nosuch"), E(hud.disable, "rings"), E(hud.enabled, "rings"), E(hud.enable, "rings"), E(hud.enabled, "rings"))
	P("rand", E(function() return v.RandomRange(5, 5) end), E(function() return v.RandomKey(1) end), E(function() local a = v.RandomFixed() return a >= 0 and a < FU end))
	P("DONE_HUD")
end, "game")
-- other HUD kinds are accepted
for _, kind in ipairs({"scores", "title", "titlecard", "intermission", "continue", "playersetup", "nosuch"}) do
	P("hudadd", kind, E(hud.add, function() end, kind))
end
P("hudadd2", E(hud.add, 5, "game"), E(hud.add, function() end, 5))
