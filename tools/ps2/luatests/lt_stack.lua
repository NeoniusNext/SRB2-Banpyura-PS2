-- OPT14: the deepest C recursion a script can cause, started from inside an engine hook (so the C stack below it is the thinker/hook chain of the engine): nested pcall, metamethods,
-- gsub / sort callbacks, coroutines inside coroutines, error handlers. The limits (LUAI_MAXCCALLS) and the messages must be the PC's, and the EE stack (384 KiB) must survive:
-- "ZSTAT ... stackused=" of the run tells how much of it was used (a stack that overflows on the EE corrupts memory silently).
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function msg(e) local m = tostring(e):gsub("[%w_/%.%-:]*lt_stack%.lua:%d+: ", "") return m:sub(1, 80) end

local depth
local function nested_pcall(n)
	depth = n
	local ok, e = pcall(nested_pcall, n + 1)
	if ok then return e end
	return e
end
local function rec_meta()
	local count = 0
	local t
	t = setmetatable({}, {__index = function(tbl, k) count = count + 1 return t[k + 1] end})
	local ok, e = pcall(function() return t[1] end)
	return ok, msg(e), count
end
local function rec_gsub(n)
	local cnt = 0
	local function f(s)
		cnt = cnt + 1
		return (s:gsub(".", f))
	end
	local ok, e = pcall(f, "ab")
	return ok, msg(e), cnt
end
local function rec_sort()
	local cnt = 0
	local function cmp(a, b)
		cnt = cnt + 1
		table.sort({3, 2, 1}, cmp)
		return a < b
	end
	local ok, e = pcall(table.sort, {3, 2, 1}, cmp)
	return ok, msg(e), cnt
end
local function rec_coro()
	local cnt = 0
	local function spawn()
		cnt = cnt + 1
		local co = coroutine.wrap(spawn)
		return co()
	end
	local ok, e = pcall(spawn)
	return ok, msg(e), cnt
end
local function rec_xpcall()
	local cnt = 0
	local function h(m) cnt = cnt + 1 error("again") end
	local ok, e = xpcall(function() error("first") end, h)
	return ok, msg(e), cnt
end
local function rec_tostring()
	local cnt = 0
	local t = setmetatable({}, {__tostring = function(s) cnt = cnt + 1 return tostring(s) end})
	local ok, e = pcall(tostring, t)
	return ok, msg(e), cnt
end
local function rec_concat()
	local cnt = 0
	local t = setmetatable({}, {__concat = function(a, b) cnt = cnt + 1 return a .. b end})
	local ok, e = pcall(function() return t .. "x" end)
	return ok, msg(e), cnt
end
local function rec_lua()
	local d = 0
	local function f() d = d + 1 return f() + 1 end
	local ok, e = pcall(f)
	return ok, msg(e), d > 1000
end
local function rec_string_rep()
	local ok, e = pcall(string.rep, "x", 100000000)
	return ok, msg(e)
end
local done = false
local function run()
	if done then return end
	done = true
	depth = 0
	local ok, e = pcall(nested_pcall, 1)
	P("nested_pcall", ok, msg(e), depth)
	P("meta", rec_meta())
	P("gsub", rec_gsub())
	P("sort", rec_sort())
	P("coro", rec_coro())
	P("xpcall", rec_xpcall())
	P("tostring", rec_tostring())
	P("concat", rec_concat())
	P("lua", rec_lua())
	P("DONE_STACK")
end

-- from the main chunk, from a thinker (deep engine stack), from a HUD hook, from a net command-like hook
addHook("MobjThinker", function(mo)
	if leveltime >= 5 and not done then
		P("from", "thinker")
		run()
	end
end, MT_PLAYER)
