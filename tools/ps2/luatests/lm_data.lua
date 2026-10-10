-- OPT14 stand: a "library" style mod. Data structures of 10k+ elements, string work, closures and metatables, coroutines, garbage pressure, a big table kept for the whole run.
-- Everything printed is a function of the script alone (no clocks, no addresses), the PC build and the PS2 ELF must print the same.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end

-- a tiny deterministic generator (the engine's P_Random is another test)
local seed = 12345
local function rnd(n)
	seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
	return (seed >> 8) % n
end

local function digest(t)
	local h = 17
	for i = 1, #t do
		local v = t[i]
		if type(v) == "number" then h = (h * 31 + v) % 1000003
		elseif type(v) == "string" then
			for j = 1, #v do h = (h * 31 + v:byte(j)) % 1000003 end
		end
	end
	return h
end

-- 1. big arrays and sorting
local N = 12000
local big = {}
for i = 1, N do big[i] = rnd(1000000) end
P("big", #big, digest(big))
table.sort(big)
P("sorted", big[1], big[N/2], big[N], digest(big))
local ok = true
for i = 2, N do if big[i-1] > big[i] then ok = false end end
P("sorted_ok", ok)
table.sort(big, function(a, b) return a > b end)
P("sorted_desc", big[1], big[N], digest(big))
-- sort of records by two keys
local recs = {}
for i = 1, 3000 do recs[i] = {id = i, a = rnd(50), b = rnd(1000), name = "n" .. rnd(9999)} end
table.sort(recs, function(x, y) if x.a ~= y.a then return x.a < y.a end if x.b ~= y.b then return x.b < y.b end return x.id < y.id end)
local rh = 0
for i = 1, #recs do rh = (rh * 31 + recs[i].id) % 1000003 end
P("recs", recs[1].id, recs[3000].id, rh)

-- 2. hash tables with many keys: string keys, number keys, traversal order is not compared, the sums are
local ht = {}
for i = 1, 8000 do ht["key" .. i] = i end
local cnt, sum = 0, 0
for k, v in pairs(ht) do cnt = cnt + 1 sum = sum + v end
P("hash", cnt, sum)
for i = 1, 8000, 2 do ht["key" .. i] = nil end
cnt, sum = 0, 0
for k, v in pairs(ht) do cnt = cnt + 1 sum = sum + v end
P("hash_half", cnt, sum)
local sparse = {}
for i = 1, 5000 do sparse[i * 7919] = i end
cnt, sum = 0, 0
for k, v in pairs(sparse) do cnt = cnt + 1 sum = sum + k % 1000 end
P("sparse", cnt, sum)
local nk = {}
for i = -2000, 2000 do nk[i] = i * 2 end
sum = 0
for i = -2000, 2000 do sum = sum + nk[i] end
P("negkeys", sum)

-- 3. strings
local words = {}
for i = 1, 2000 do
	local w = {}
	for j = 1, 3 + rnd(6) do w[#w+1] = string.char(97 + rnd(26)) end
	words[i] = table.concat(w)
end
local text = table.concat(words, " ")
P("text", #text, digest({text}))
local wc, longest = 0, ""
for w in text:gmatch("%a+") do wc = wc + 1 if #w > #longest then longest = w end end
P("gmatch", wc, #longest)
local up = text:upper()
P("upper", up:sub(1, 20), up:sub(-20))
local rep = text:gsub("a", "AA")
P("gsub", #rep)
local rep2, n2 = text:gsub("(%a)(%a)", "%2%1")
P("gsub_cap", #rep2, n2, rep2:sub(1, 30))
local rep3 = text:gsub("%a+", function(w) return #w % 2 == 0 and w:reverse() or w end)
P("gsub_fn", #rep3, digest({rep3}))
local rep4 = text:gsub("%a+", {abc = "X", ab = "Y"})
P("gsub_tbl", #rep4)
P("find", text:find("zz", 1, true), text:find("q%a+x"), text:find("^%a+"))
P("format", string.format("%d|%5d|%-5d|%05d|%x|%X|%c|%s|%10s|%-10s|%q", 42, 42, 42, 42, 255, 255, 65, "str", "right", "left", "quo\"te"))
P("format2", string.format("%3d%%", 50), string.format("%s %s %s", 1, "t", "n"), string.format("%.3s", "abcdef"))
P("rep", ("ab"):rep(5), ("ab"):rep(3, "-"), #("x"):rep(10000))
P("byte", ("hello"):byte(1, -1))
P("char", string.char(72, 105, 33))
P("len", #"", #"abc", ("x"):len())
P("lower_sub", ("Hello World"):lower(), ("Hello World"):sub(-5, -2), ("Hello"):sub(2, 100), ("Hello"):sub(0))
-- string buffer by repeated concatenation (the quadratic way), then table.concat
local s = ""
for i = 1, 600 do s = s .. i .. "," end
P("concat_loop", #s, digest({s}))
local parts = {}
for i = 1, 5000 do parts[i] = tostring(i) end
local joined = table.concat(parts, ";")
P("concat_tbl", #joined, joined:sub(1, 20))
-- split
local function split(str, sep)
	local out = {}
	for piece in (str .. sep):gmatch("(.-)" .. sep:gsub("%p", "%%%0")) do out[#out+1] = piece end
	return out
end
local sp = split(joined, ";")
P("split", #sp, sp[1], sp[#sp])
-- number to string conversions
P("tostring", tostring(0), tostring(-1), tostring(2147483647), tostring(-2147483647), tostring(12345678))
P("tonumber", tonumber("123"), tonumber("-77"), tonumber("0x1F"), tonumber("12abc"), tonumber("  5  "), tonumber("ff", 16), tonumber("777", 8), tonumber("z", 36))
-- base64 in pure Lua (a typical mod helper)
local b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function enc(data)
	local out = {}
	for i = 1, #data, 3 do
		local a, b, c = data:byte(i, i + 2)
		local n = (a << 16) | ((b or 0) << 8) | (c or 0)
		out[#out+1] = b64:sub((n >> 18) + 1, (n >> 18) + 1) .. b64:sub(((n >> 12) & 63) + 1, ((n >> 12) & 63) + 1)
			.. (b and b64:sub(((n >> 6) & 63) + 1, ((n >> 6) & 63) + 1) or "=") .. (c and b64:sub((n & 63) + 1, (n & 63) + 1) or "=")
	end
	return table.concat(out)
end
local e = enc(text:sub(1, 3000))
P("b64", #e, e:sub(1, 24), e:sub(-8))
-- a checksum like crc32 with bit operators
local function crc(str)
	local c = 0xFFFFFFFF
	for i = 1, #str do
		c = c ^^ str:byte(i)
		for _ = 1, 8 do
			if c & 1 == 1 then c = (c >> 1) ^^ 0xEDB88320 else c = c >> 1 end
		end
	end
	return c
end
P("crc", crc("123456789"), crc(text:sub(1, 2000)))

-- 4. closures and metatables
local function counter()
	local c = 0
	return function() c = c + 1 return c end
end
local cs = {}
for i = 1, 3000 do cs[i] = counter() end
local tot = 0
for r = 1, 3 do for i = 1, #cs do tot = tot + cs[i]() end end
P("closures", tot)
local Vec = {}
Vec.__index = Vec
Vec.__add = function(a, b) return setmetatable({x = a.x + b.x, y = a.y + b.y}, Vec) end
Vec.__eq = function(a, b) return a.x == b.x and a.y == b.y end
Vec.__lt = function(a, b) return a.x < b.x end
Vec.__le = function(a, b) return a.x <= b.x end
Vec.__tostring = function(a) return "(" .. a.x .. "," .. a.y .. ")" end
Vec.__len = function(a) return 2 end
Vec.__call = function(self, k) return self[k] end
Vec.__concat = function(a, b) return tostring(a) .. tostring(b) end
Vec.__unm = function(a) return setmetatable({x = -a.x, y = -a.y}, Vec) end
function Vec.new(x, y) return setmetatable({x = x, y = y}, Vec) end
local acc = Vec.new(0, 0)
for i = 1, 4000 do acc = acc + Vec.new(i, -i) end
P("vec", tostring(acc), #acc, acc("x"), tostring(-acc), tostring(acc) == (acc .. "") , Vec.new(1, 2) == Vec.new(1, 2), Vec.new(1, 2) < Vec.new(2, 0))
local prox = setmetatable({}, {__index = function(t, k) return k * 2 end, __newindex = function(t, k, v) rawset(t, k, v + 1) end})
prox[5] = 10
P("proxy", prox[5], prox[7], rawget(prox, 7))
-- inheritance chain
local A = {} A.__index = A function A.hello() return "A" end function A.base() return "base" end
local B = setmetatable({}, A) B.__index = B function B.hello() return "B" end
local C = setmetatable({}, B) C.__index = C
local obj = setmetatable({}, C)
P("inherit", obj.hello(), obj.base(), getmetatable(obj) == C)
-- weak tables
local weak = setmetatable({}, {__mode = "v"})
for i = 1, 200 do weak[i] = {i} end
collectgarbage()
local alive = 0
for i = 1, 200 do if weak[i] then alive = alive + 1 end end
P("weak", alive)
local weakk = setmetatable({}, {__mode = "k"})
do local key = {} weakk[key] = 1 end
collectgarbage()
local wk = 0
for _ in pairs(weakk) do wk = wk + 1 end
P("weakk", wk)

-- 5. coroutines
local function gen(n)
	return coroutine.wrap(function() for i = 1, n do coroutine.yield(i * i) end end)
end
local gs = 0
for v in gen(500) do gs = gs + v end
P("coro_gen", gs)
local cos = {}
for i = 1, 100 do
	cos[i] = coroutine.create(function(a) local b = coroutine.yield(a + 1) local c = coroutine.yield(b * 2) return a + b + c end)
end
local cr = 0
for i = 1, 100 do
	local _, x = coroutine.resume(cos[i], i)
	local _, y = coroutine.resume(cos[i], x)
	local _, z = coroutine.resume(cos[i], y)
	cr = cr + z
end
P("coro_many", cr, coroutine.status(cos[1]), coroutine.resume(cos[1]))
local errco = coroutine.create(function() error("boom") end)
P("coro_err", coroutine.resume(errco))
P("coro_status", coroutine.status(errco))
-- yield across pcall
local pc = coroutine.wrap(function() local ok, v = pcall(function() return coroutine.yield(1) + 1 end) coroutine.yield(v) return "x" end)
P("coro_pcall", pc(), pc(41))

-- 6. error handling
local function thrower(kind)
	if kind == 1 then error("plain") elseif kind == 2 then error({code = 7}) elseif kind == 3 then error("lvl2", 2) elseif kind == 4 then local x = nil return x.y elseif kind == 5 then return 1 + {} elseif kind == 6 then return #5 elseif kind == 7 then return nil .. "x" end
end
for k = 1, 7 do
	local ok, err = pcall(thrower, k)
	P("pcall", k, ok, type(err) == "table" and err.code or (tostring(err):gsub("^.-:%d+: ", "")))
end
P("xpcall", xpcall(thrower, function(m) return "H:" .. tostring(m):gsub("^.-:%d+: ", "") end, 1))
P("assert", pcall(assert, false, "assert msg"), pcall(assert, nil), select("#", assert(1, 2, 3)))
P("select", select("#"), select("#", nil, nil), select(-1, 1, 2, 3), select(2, "a", "b", "c"))
P("unpack", unpack({1, 2, 3}), unpack({1, 2, 3}, 2), unpack({}, 1, 2))
P("next", next({}), next({10}), type(next))
P("rawequal", rawequal("a", "a"), rawequal({}, {}), rawlen and 1 or 0)
P("type", type(nil), type(1), type("s"), type({}), type(print), type(coroutine.create(function() end)))
P("getn", table.getn and table.getn({1, 2, 3}) or "nogetn", #{1, 2, nil, 4})
local tinsert = {}
for i = 1, 100 do table.insert(tinsert, i) end
table.insert(tinsert, 1, 0)
table.remove(tinsert, 50)
P("tinsert", #tinsert, tinsert[1], tinsert[50], tinsert[100], table.remove(tinsert), #tinsert)
P("maxn", table.maxn and table.maxn({1, 2, [10] = 3}) or "nomaxn")

-- 7. recursion and algorithms
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
P("fib", fib(18))
local function ack(m, n) if m == 0 then return n + 1 elseif n == 0 then return ack(m - 1, 1) else return ack(m - 1, ack(m, n - 1)) end end
P("ack", ack(2, 3))
local function queens(n)
	local cols, d1, d2, count = {}, {}, {}, 0
	local function place(r)
		if r > n then count = count + 1 return end
		for c = 1, n do
			if not cols[c] and not d1[r + c] and not d2[r - c + n] then
				cols[c], d1[r + c], d2[r - c + n] = true, true, true
				place(r + 1)
				cols[c], d1[r + c], d2[r - c + n] = nil, nil, nil
			end
		end
	end
	place(1)
	return count
end
P("queens", queens(6))
local sieve, primes = {}, 0
for i = 2, 30000 do
	if not sieve[i] then
		primes = primes + 1
		for j = i * i, 30000, i do sieve[j] = true end
	end
end
P("sieve", primes)
-- deep recursion until the Lua stack overflow error (message and survival)
local depth = 0
local function dive() depth = depth + 1 return 1 + dive() end
local okd, errd = pcall(dive)
P("overflow", okd, (tostring(errd):gsub("^.-:%d+: ", "")), depth > 1000)
-- sum to a depth that works
local function sumto(n) if n == 0 then return 0 end return n + sumto(n - 1) end
P("sumto", sumto(150))
local function tail(n, a) if n == 0 then return a end return tail(n - 1, a + 1) end
P("tail", tail(50000, 0))

-- 8. a long-lived big structure kept to the end (the heap stays large), then garbage pressure on top of it
local keep = {}
for i = 1, 4000 do keep[i] = {id = i, name = "item" .. i, tags = {i, i + 1, i + 2}, pos = {x = i, y = -i}} end
P("keep", #keep, keep[4000].name, keep[17].tags[3])
local garbage_sum = 0
for round = 1, 30 do
	local g = {}
	for i = 1, 1000 do g[i] = {i, tostring(i), {round}} end
	garbage_sum = garbage_sum + #g + g[1000][3][1]
end
P("garbage", garbage_sum)
collectgarbage()
P("keep_after_gc", #keep, keep[4000].name, keep[17].tags[3], keep[2000].pos.y)
P("count_ok", collectgarbage("count") > 100)
P("gc_steps", collectgarbage("step") ~= nil, collectgarbage("stop"), collectgarbage("restart"))

-- 9. globals: a mod that leaks a global gets the engine's error
local okg, errg = pcall(function() lm_global_leak = 1 end)
P("global", okg, (tostring(errg):gsub("^.-:%d+: ", "")))
local okg2, errg2 = pcall(function() return lm_undefined_global end)
P("global_read", okg2, errg2)
rawset(_G, "lm_ok_global", 5)
P("global_rawset", lm_ok_global)

P("DONE_LMDATA")
