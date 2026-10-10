-- Lua equivalence: freeslot() up to and beyond the limits of every kind, and what the slots of the last entries hold
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function exhaust(kind, prefix, extra)
	local last, first, okc, h = nil, nil, 0, 0
	for i = 1, extra do
		local ok, r = pcall(freeslot, kind .. "LQ" .. prefix .. i)
		if ok and r then
			okc = okc + 1
			last = r
			first = first or r
			h = (h * 31 + r) % 1000003
		else
			h = (h * 31 + 7) % 1000003
		end
	end
	P("slots", kind, extra, okc, first, last, h)
	return first, last
end
local f, l = exhaust("MT_", "M", 1030)
P("mt-last", l, mobjinfo[l].spawnhealth, mobjinfo[l].doomednum, #mobjinfo)
mobjinfo[l].spawnhealth = 77 P("mt-set", mobjinfo[l].spawnhealth)
P("mt-names", MT_LQM1 == f, MT_LQM1024 == l, pcall(function() return MT_LQM1025 end))
f, l = exhaust("S_", "S", 8200)
P("s-last", l, states[l].sprite, states[l].tics, #states)
states[l].tics = 9 P("s-set", states[l].tics, S_LQS8192 == l, pcall(function() return S_LQS8193 end))
f, l = exhaust("SPR_", "P", 1030)
P("spr-last", l, #sprnames or 0, SPR_LQP1024 == l)
f, l = exhaust("SPR2_", "Q", 1030)
P("spr2-last", l)
f, l = exhaust("SKINCOLOR_", "C", 1030)
P("color-last", l, skincolors[l] and skincolors[l].name)
f, l = exhaust("sfx_", "F", 1610)
P("sfx-last", l, S_sfx and S_sfx[l] and S_sfx[l].name or "none")
-- a multiple argument call returns one number per name
P("multi", freeslot("MT_LQMULTIA", "MT_LQMULTIB"))
P("DONE_SLOTS")
