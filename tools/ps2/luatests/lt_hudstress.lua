-- OPT14: a HUD that draws a lot of different graphics every frame: the first frame of every sprite of the game (several hundred patches, most of them big), at different scales and flags, for
-- 250 frames. In the hardware renderer every patch is a texture of the GS (4 MB of VRAM): the cache has to cope without an error, a hang or a picture that stops. The LQ lines are counters.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local patches = {}
local built = false
local frames, draws, errors = 0, 0, 0
local errmsg
hud.add(function(v)
	if leveltime < 3 then return end
	if not built then
		built = true
		for spr = 1, 700 do
			local ok, p = pcall(function() local p = v.getSpritePatch(spr, 0, 0, 0) return p end)
			if ok and p then patches[#patches + 1] = p end
		end
		P("patches", #patches > 200)
	end
	frames = frames + 1
	local n = #patches
	for i = 1, 90 do
		local p = patches[((frames * 7 + i * 13) % n) + 1]
		local ok, e = pcall(v.drawScaled, ((i * 37) % 300) * FRACUNIT, ((i * 23) % 180) * FRACUNIT, FRACUNIT / 4 + (i % 5) * FRACUNIT / 8, p, (i % 7 == 0) and V_50TRANS or 0)
		if ok then draws = draws + 1 else errors = errors + 1 errmsg = errmsg or tostring(e) end
	end
end, "game")
addHook("ThinkFrame", function()
	if leveltime == 300 then
		P("frames", frames > 100, draws > 5000, errors, errmsg)
		P("DONE_HUDSTRESS")
	end
end)
