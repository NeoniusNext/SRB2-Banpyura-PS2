-- OPT14 (PS2-LUA-2): thousands of live userdata. The PS2 bit set of "pointers that ever got a userdata" (valid_bloom, 16 384 bits) is rebuilt from the registry table once more than
-- a quarter of its bits were marked; with more live userdata than that, every new userdata rebuilt it (O(n) each, O(n^2) in all). The answers must be the PC's, and the time must stay linear.
-- Run on a level with many lines (MAP11 has ~25 000); the line "LT time" is not compared (a clock).
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local done = false
local stage = 0
local held = {}
local t0

addHook("ThinkFrame", function()
	if done or leveltime < 3 then return end
	if stage == 0 then
		stage = 1
		t0 = getTimeMicros()
		local n = #lines
		local cap = n < 12000 and n or 12000
		for i = 0, cap - 1 do held[i + 1] = lines[i] end
		print("LT time " .. tostring((getTimeMicros() - t0) / 1000) .. " ms for " .. cap .. " line userdata of " .. n)
		P("held", #held, held[1].valid, held[#held].valid, #held >= 4200)
		local s = 0
		for i = 1, #held do s = s + (held[i].dx >> 16) + (held[i].dy >> 16) end
		P("sum", s)
		-- the same objects again: found in the registry, no new userdata
		t0 = getTimeMicros()
		local same = 0
		for i = 0, cap - 1 do if lines[i] == held[i + 1] then same = same + 1 end end
		print("LT time2 " .. tostring((getTimeMicros() - t0) / 1000) .. " ms")
		P("same", same)
		-- mobjs: spawn and remove thousands of objects holding their userdata, the userdata of the removed ones must turn invalid
		local mos = {}
		local p = players[0].mo
		t0 = getTimeMicros()
		for i = 1, 3000 do mos[i] = P_SpawnMobj(p.x, p.y, p.z + 4096 * FRACUNIT, MT_THOK) mos[i].fuse = 1 end
		print("LT time3 " .. tostring((getTimeMicros() - t0) / 1000) .. " ms")
		P("spawned", #mos, mos[1].valid, mos[3000].valid)
		for i = 1, #mos, 2 do P_RemoveMobj(mos[i]) end
		local inv = 0
		for i = 1, #mos do if not mos[i].valid then inv = inv + 1 end end
		P("removed", inv)
		held.mos = mos
	elseif stage == 1 and leveltime >= 8 then
		stage = 2
		local inv = 0
		for i = 1, #held.mos do if not held.mos[i].valid then inv = inv + 1 end end
		P("all_gone", inv, #held.mos)
		-- the line userdata are still valid; sectors / sides / vertexes too
		local ok = 0
		for i = 1, #held do if held[i].valid then ok = ok + 1 end end
		P("lines_valid", ok, #held)
		P("DONE_UDMANY")
		done = true
	end
end)
