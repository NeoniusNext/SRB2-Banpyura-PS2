-- OPT14 (PS2-LUA-4): the slab pool of the Lua heap under churn. Objects of every size class (strings of 0..700 bytes, tables of 0..40 fields, closures, coroutines) are made in rounds, a part
-- of them is kept for good, the rest dropped; after each round the script asks for a full collection (the PS2 then gives empty slabs back to the zone) and checks the content of every survivor.
-- A block that was given back while still in use, or a free list that points into a freed slab, shows up as a wrong checksum or a crash.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local seed = 987654321
local function rnd(n)
	seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
	return (seed >> 8) % n
end
local function mkstring(id, len)
	local b = {}
	for i = 1, len do b[i] = string.char(97 + (id + i) % 26) end
	return table.concat(b)
end
local function mkobj(id)
	local kind = id % 4
	if kind == 0 then
		local len = rnd(700)
		return {kind = 0, id = id, len = len, s = mkstring(id, len)}
	elseif kind == 1 then
		local n = rnd(40)
		local t = {kind = 1, id = id, n = n}
		for i = 1, n do t[i] = id * 31 + i end
		return t
	elseif kind == 2 then
		local a, b = id, rnd(1000)
		return {kind = 2, id = id, f = function(x) return x + a + b end, a = a, b = b}
	else
		local co = coroutine.wrap(function(x) local y = coroutine.yield(x + id) return y * 2 + id end)
		return {kind = 3, id = id, co = co, first = co(5)}
	end
end
local function check(o)
	if o.kind == 0 then return #o.s == o.len and o.s == mkstring(o.id, o.len)
	elseif o.kind == 1 then
		for i = 1, o.n do if o[i] ~= o.id * 31 + i then return false end end
		return #o == o.n
	elseif o.kind == 2 then return o.f(3) == 3 + o.a + o.b
	else return o.first == 5 + o.id and o.co(7) == 14 + o.id end
end
local keep, nextid, bad, kept_total = {}, 1, 0, 0
local sum = 0
for round = 1, 40 do
	local tmp = {}
	for i = 1, 1500 do
		local o = mkobj(nextid)
		nextid = nextid + 1
		if rnd(9) == 0 then keep[#keep + 1] = o else tmp[#tmp + 1] = o end
	end
	-- drop about half of the survivors, in the middle of the list
	if #keep > 400 then
		for i = #keep, 1, -1 do if rnd(2) == 0 then table.remove(keep, i) end end
	end
	tmp = nil
	collectgarbage()
	for _, o in ipairs(keep) do
		-- (kind 3 coroutines are finished by the check: make them again)
		if o.kind ~= 3 then if not check(o) then bad = bad + 1 end else sum = sum + 1 end
	end
	kept_total = kept_total + #keep
	if round % 8 == 0 then
		local kb = collectgarbage("count")
		P("round", round, #keep, bad, nextid, kb > 0)
	end
end
-- the second kind of churn: almost everything is dropped at once (whole slabs become free and go back), a few survive
local keep2, bad2 = {}, 0
for round = 1, 30 do
	local tmp = {}
	for i = 1, 4000 do
		local o = mkobj(nextid)
		nextid = nextid + 1
		if rnd(400) == 0 then keep2[#keep2 + 1] = o else tmp[#tmp + 1] = o end
	end
	tmp = nil
	collectgarbage()
	for _, o in ipairs(keep2) do if o.kind ~= 3 and not check(o) then bad2 = bad2 + 1 end end
	if round % 10 == 0 then P("round2", round, #keep2, bad2, nextid) end
	if round % 6 == 0 then COM_BufInsertText(server, "memfree") end
end
-- the coroutines at the end
local cobad = 0
for _, o in ipairs(keep) do if o.kind == 3 and not check(o) then cobad = cobad + 1 end end
P("final", #keep, bad, cobad, kept_total, nextid, #keep2, bad2)
COM_BufInsertText(server, "memfree")
P("DONE_POOLSTRESS")
