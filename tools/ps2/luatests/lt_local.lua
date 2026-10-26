-- addfilelocal: a Lua file added locally (the console command) runs on both builds the same way; this script asks for it from a hook after the level started
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local asked = false
addHook("ThinkFrame", function()
	if not asked and leveltime == 5 then
		asked = true
		P("ask", leveltime)
		COM_BufAddText(players[0], "addfilelocal lt_local2.lua")
	end
	if leveltime == 40 then P("DONE_LOCAL") end
end)
addHook("AddonLoaded", function() P("addonloaded", leveltime) end)
