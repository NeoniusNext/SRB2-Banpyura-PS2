-- Lua equivalence: console, input, colour, tag, blockmap, http (pure helpers), skin and banpyura libraries, user variables and commands.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function E(f, ...) local r = {pcall(f, ...)} if r[1] then table.remove(r, 1) for i = 1, #r do r[i] = (type(r[i]) == "userdata") and "userdata" or tostring(r[i]) end return table.concat(r, ",") end return "E:" .. (tostring(r[2]):gsub("^.-:%d+: ", "")) end
-- console variables and commands
local cv = CV_RegisterVar({name = "lq_var", defaultvalue = "5", flags = CV_NETVAR|CV_SHOWMODIF, PossibleValue = CV_Unsigned})
local cv2 = CV_RegisterVar({name = "lq_choice", defaultvalue = "Two", flags = CV_NETVAR, PossibleValue = {One = 1, Two = 2, Three = 3}})
local cv3 = CV_RegisterVar({name = "lq_range", defaultvalue = "10", flags = 0, PossibleValue = {MIN = -20, MAX = 50}})
local cv4 = CV_RegisterVar({name = "lq_float", defaultvalue = "1.5", flags = CV_FLOAT, PossibleValue = {MIN = 0, MAX = 5*FRACUNIT}})
local cv5 = CV_RegisterVar({name = "lq_onoff", defaultvalue = "On", flags = 0, PossibleValue = CV_OnOff})
P("cv_default", cv.value, cv.string, cv.name, cv.defaultvalue, cv2.value, cv2.string, cv3.value, cv4.value, cv4.string, cv5.value, cv5.string)
P("cv_find", E(function() return CV_FindVar("lq_var").value end), E(function() return CV_FindVar("nosuchvar") end), E(function() return CV_FindVar("gravity").value end), E(function() return CV_FindVar("timelimit").string end))
CV_StealthSet(cv, 99) P("cv_stealth", cv.value, cv.string)
CV_Set(cv3, 33) P("cv_set", cv3.value) CV_Set(cv3, 9999) P("cv_set_big", cv3.value) CV_Set(cv3, -9999) P("cv_set_small", cv3.value)
CV_Set(cv2, "Three") P("cv_set_str", cv2.value, cv2.string) CV_Set(cv2, 9) P("cv_set_bad", cv2.value, cv2.string)
CV_Set(cv5, "Off") P("cv_onoff", cv5.value, cv5.string) CV_Set(cv5, "yes") P("cv_onoff2", cv5.value, cv5.string)
CV_Set(cv4, 3*FRACUNIT/2 + 1234) P("cv_float", cv4.value, cv4.string)
CV_AddValue(cv3, 7) P("cv_add", cv3.value) CV_AddValue(cv3, -500) P("cv_add_neg", cv3.value) CV_AddValue(cv2, 1) P("cv_add_choice", cv2.value, cv2.string)
P("cv_dup", E(CV_RegisterVar, {name = "lq_var", defaultvalue = "1", flags = 0}))
P("cv_bad", E(CV_RegisterVar, {name = "lq bad name", defaultvalue = "1"}), E(CV_RegisterVar, {defaultvalue = "1"}), E(CV_RegisterVar, 5))
local calls = {}
COM_AddCommand("lq_cmd", function(player, ...) calls[#calls+1] = select("#", ...) .. ":" .. table.concat({...}, "|") end)
P("command_added", E(COM_AddCommand, "lq_cmd", function() end), E(COM_AddCommand, "lq_cmd2", 5))
-- input
P("input", E(input.keyNumToName, 97), E(input.keyNumToName, KEY_ENTER), E(input.keyNumToName, 0), E(input.keyNumToName, 300), E(input.keyNameToNum, "a"), E(input.keyNameToNum, "enter"), E(input.keyNameToNum, "nosuchkey"),
	E(input.keyNumPrintable, 97), E(input.keyNumPrintable, KEY_ENTER), E(input.shiftKeyNum, 97), E(input.shiftKeyNum, 49), E(input.shiftKeyNum, 91), E(input.getMouseGrab), E(input.gameControlDown, GC_JUMP), E(input.gameControl2Down, GC_SPIN), E(input.joyAxis, JA_TURN), E(input.joy2Axis, JA_MOVE))
P("input2", E(input.gameControlToKeyNum, GC_JUMP), E(input.gameControl2ToKeyNum, GC_JUMP), E(input.gameControlToKeyNum, 9999))
-- colour
P("color", E(color.rgbToPalette, 255, 0, 0), E(color.rgbToPalette, 0, 255, 0), E(color.rgbToPalette, 12, 34, 56), E(color.rgbToPalette, 255, 255, 255), E(color.rgbToPalette, 0, 0, 0), E(color.rgbToPalette, 300, -5, 7), E(color.rgbToPalette, 128, 128, 128, 255))
P("color2", E(color.paletteToRgb, 0), E(color.paletteToRgb, 35), E(color.paletteToRgb, 255), E(color.paletteToRgb, 256), E(color.hexToRgb, "ff8000"), E(color.hexToRgb, "#ff8000"), E(color.hexToRgb, "f80"), E(color.hexToRgb, "ff800080"), E(color.hexToRgb, "zz"))
P("color3", E(color.rgbToHex, 255, 128, 0), E(color.rgbToHex, 1, 2, 3, 4), E(color.hslToRgb, 0, 255, 128), E(color.hslToRgb, 128, 255, 128), E(color.hslToRgb, 200, 100, 50), E(color.rgbToHsl, 255, 0, 0), E(color.rgbToHsl, 12, 200, 99), E(color.rgbToHsl, 100, 100, 100))
P("color4", E(color.packRgb, 1, 2, 3), E(color.packRgba, 1, 2, 3, 4), E(color.unpackRgb, 0x010203), E(color.unpackRgb, 0xFF804020), E(color.packRgb, 300, 2, 3))
local cs = {} for r = 0, 255, 51 do for g = 0, 255, 51 do for b = 0, 255, 51 do cs[#cs+1] = color.rgbToPalette(r, g, b) end end end P("color_grid", #cs, table.concat(cs, ","))
-- http: only the pure helpers
if rawget(_G, "http") then
	P("http", E(http.encodeURL, "a b&c=d/é"), E(http.decodeURL, "a%20b%26c"), E(http.encodeBase64, "Hello, World!"), E(http.decodeBase64, "SGVsbG8sIFdvcmxkIQ=="), E(http.buildQuery, {a = "1"}), E(http.parseQuery, "a=1&b=2"), E(http.decodeBase64, "!!!"))
end
-- banpyura
P("banpyura", E(Banpyura.SpriteShadow_SetAngle, ANGLE_90), E(Banpyura.SpriteShadow_SetAngle), E(Banpyura.SpriteShadow_SetAngle, "x"))
-- metatables
P("meta", type(userdataMetatable("mobj_t")), type(userdataMetatable("player_t")), E(registerMetatable, 5))
local done = false
addHook("ThinkFrame", function()
	if leveltime == 42 then P("commands", #calls, table.concat(calls, " ; ")) P("cv_after", cv.value, cv.string) P("DONE_LIBS") end
	if done or leveltime ~= 40 then return end
	done = true
	COM_BufInsertText(players[0], "lq_cmd a b c\n")
	COM_BufInsertText(players[0], "lq_cmd \"quoted arg\" 12 -3\n")
	COM_BufInsertText(players[0], "lq_cmd\n")
	COM_BufAddText(players[0], "lq_var 77\n")
	CONS_Printf(players[0], "LQ cons printf 1 " .. 5)
	CONS_Printf(players[0], "LQ cons printf 2 %d")
	P("cv_after", cv.value, cv.string)
	-- tags
	local n, names = 0, {}
	for i = 0, #sectors - 1 do if sectors[i].tag ~= 0 then n = n + 1 if n <= 10 then names[#names+1] = i .. ":" .. sectors[i].tag end end end
	P("sectags", n, table.concat(names, ","))
	local t1 = 0 for s in sectors.tagged(1) do t1 = t1 + 1 end
	local t2 = 0 for l in lines.tagged(1) do t2 = t2 + 1 end
	local t3 = 0 for m in mapthings.tagged(1) do t3 = t3 + 1 end
	P("tagged", t1, t2, t3, E(function() local c = 0 for s in sectors.tagged(9999) do c = c + 1 end return c end))
	local sec = players[0].mo.subsector.sector
	local tl = sec.taglist
	P("taglist", #tl, E(function() return tl[1] end), E(function() return tl[5] end), E(function() local c = 0 for t in tl.iterate() do c = c + 1 end return c end), E(function() return tl.find(0) end))
	E(function() tl:add(3) tl:add(8) end) P("tl_add", #tl, E(function() return tl[#tl - 1] end), sec.tag)
	E(function() tl:remove(3) end) P("tl_rem", #tl)
	-- blockmap search
	local mo = players[0].mo
	local found = {}
	searchBlockmap("objects", function(refmobj, found_mobj) found[#found+1] = found_mobj.type end, mo, mo.x - 500*FRACUNIT, mo.x + 500*FRACUNIT, mo.y - 500*FRACUNIT, mo.y + 500*FRACUNIT)
	table.sort(found) P("blockmap_obj", #found, table.concat(found, ","))
	local lf = {} searchBlockmap("lines", function(refmobj, line) lf[#lf+1] = line.special end, mo, mo.x - 300*FRACUNIT, mo.x + 300*FRACUNIT, mo.y - 300*FRACUNIT, mo.y + 300*FRACUNIT)
	table.sort(lf) P("blockmap_lines", #lf, table.concat(lf, ","))
	local pf = {} searchBlockmap("polyobjs", function(refmobj, po) pf[#pf+1] = 1 end, mo) P("blockmap_poly", #pf)
	P("blockmap_bad", E(searchBlockmap, "bad", function() end, mo), E(searchBlockmap, "objects", 5, mo))
	-- thinkers
	local tc = 0 local tt = {}
	for th in thinkers.iterate and thinkers.iterate() or function() return nil end do tc = tc + 1 end
	P("thinkers", tc)
end)
