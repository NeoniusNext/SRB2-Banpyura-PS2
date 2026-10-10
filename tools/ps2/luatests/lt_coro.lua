-- OPT14 (PS2-LUA-1): userdata made inside a coroutine. The PS2 build marks every pointer that gets a userdata in a bit set (valid_bloom) so that LUA_InvalidateUserdata can
-- skip the registry lookup for a block that never had one; the mark was made only when the pushing lua_State was the main one (L == gL), a coroutine is another lua_State.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end

local held = {}      -- userdata made in a coroutine
local heldmain = {}  -- the same kind of object, made in the main thread
local co
local done = false

local function spawn(tag)
	local p = players[0].mo
	return P_SpawnMobj(p.x + (tag * 16 << 16), p.y, p.z + (64 << 16), MT_THOK)
end

addHook("ThinkFrame", function()
	if done or not (players[0] and players[0].mo and players[0].mo.valid) then return end
	if leveltime == 3 then
		co = coroutine.wrap(function()
			for i = 1, 6 do
				local m = spawn(i)
				m.fuse = 40 + i -- the engine removes it by itself
				held[i] = m
				coroutine.yield(i)
			end
			-- a player object seen from inside the coroutine, an explicit removal
			local r = spawn(9)
			r.fuse = 500
			held[7] = r
			return "end"
		end)
		for i = 1, 7 do P("co", co()) end
		for i = 1, 7 do P("made", i, held[i].valid, held[i].type == MT_THOK) end
		for i = 1, 7 do
			local m = spawn(20 + i)
			m.fuse = 40 + i
			heldmain[i] = m
		end
		P("main made", #heldmain)
	elseif leveltime == 5 then
		-- explicit removal by the engine call, userdata made in a coroutine
		P_RemoveMobj(held[7])
		P("removed explicitly", held[7].valid, heldmain[1].valid)
		P_RemoveMobj(heldmain[1])
		P("removed explicitly main", heldmain[1].valid)
		-- reading a removed mobj must raise the same error on both
		local ok, err = pcall(function() return held[7].x end)
		P("read removed", ok, (tostring(err):gsub("^.-:%d+: ", "")))
	elseif leveltime == 40 then
		for i = 1, 6 do P("t40", i, held[i].valid, heldmain[i].valid) end
	elseif leveltime == 60 then
		for i = 1, 6 do
			P("t60", i, held[i].valid, heldmain[i].valid)
		end
		-- the memory of the removed objects is reused by new ones: an old reference must not turn into the new object
		local fresh = {}
		for i = 1, 12 do fresh[i] = spawn(30 + i) end
		for i = 1, 6 do
			local ok, err = pcall(function() return held[i].type end)
			P("reuse", i, ok, ok and held[i].valid or (tostring(err):gsub("^.-:%d+: ", "")))
		end
		P("DONE_CORO")
		done = true
	end
end)
