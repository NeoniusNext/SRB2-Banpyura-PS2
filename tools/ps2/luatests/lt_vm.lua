-- OPT12-LOAD Lua equivalence: the language itself (integer VM of SRB2, blua), printed as "LQ ..." lines. PC build and PS2 must print the same.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end

-- arithmetic on 32-bit integers
local big = 2147483647
P("add", 1+2, big+1, -big-1, big*2, 7/2, -7/2, 7%3, -7%3, 7%-3, -7%-3)
P("mul", 46341*46341, 65536*65536, -65536*65536, 100000*100000)
P("pow", 2^10, 3^4, 2^31, 2^32, 10^9)
P("unm", -5, - -5, -(-big-1))
P("cmp", 1<2, 2<1, 1==1, 1~=1, "a"<"b", "abc"<"abd", "Z"<"a", ""<"a")
P("concat", 1 .. 2, "x" .. 3 .. "y", -5 .. "")
P("len", #"hello", #"", #{1,2,3}, #{n=1})
local ok, e = pcall(function() return 1/0 end) P("div0", ok, e)
ok, e = pcall(function() return 1%0 end) P("mod0", ok, e)
ok, e = pcall(function() return {} + 1 end) P("addtable", ok, e)
ok, e = pcall(function() return nil .. "x" end) P("concatnil", ok, e)
ok, e = pcall(function() local t = nil; return t.x end) P("indexnil", ok, e)
ok, e = pcall(function() return #nil end) P("lennil", ok, e)
ok, e = pcall(error, "msg") P("error", ok, e)
ok, e = pcall(error, {code=5}) P("errortable", ok, type(e), e.code)
ok, e = pcall(error) P("errornone", ok, e)
ok, e = pcall(function() error("lvl2", 2) end) P("errorlvl", ok, e)
P("tostring", tostring(nil), tostring(true), tostring(false), tostring(0), tostring(-1), tostring(big), tostring(-big-1))
P("tonumber", tonumber("12"), tonumber("-12"), tonumber("0x10"), tonumber("  7  "), tonumber("abc"), tonumber("12abc"), tonumber("10", 2), tonumber("ff", 16), tonumber("zz", 36), tonumber(""))
P("type", type(1), type("s"), type({}), type(print), type(nil), type(true))
P("select", select("#"), select("#", nil, nil), select(2, "a", "b", "c"), select(-1, "a", "b", "c"))
P("unpack", unpack({1,2,3}))
P("rawequal", rawequal("a", "a"), rawequal({}, {}), rawlen and rawlen({1,2}) or "norawlen")
P("next", next({}), next({10}))

-- strings
P("sub", ("hello"):sub(2,3), ("hello"):sub(-3), ("hello"):sub(0), ("hello"):sub(10), ("hello"):sub(2,-2), ("hello"):sub(-100, 100))
P("upper", ("Hello, World 123 \200"):upper(), ("Hello, World 123"):lower())
P("rep", ("ab"):rep(3), ("ab"):rep(0), ("ab"):rep(2, "-"))
P("rev", ("abc"):reverse(), ("abc"):byte(1,-1))
P("char", string.char(72, 105, 33))
P("find", ("hello world"):find("o w"), ("hello world"):find("o", 6), ("hello"):find("l+"), ("hello"):find("xyz"), ("a.b"):find(".", 1, true), ("a+b"):find("+", 1, true))
P("match", ("key=value"):match("(%w+)=(%w+)"), ("  trim  "):match("^%s*(.-)%s*$"), ("2024-10-08"):match("(%d+)-(%d+)-(%d+)"), ("abc"):match("(x*)"), ("abc"):match("()b()"))
P("gsub", ("hello world"):gsub("o", "0"), ("hello"):gsub("l", {l="L"}), ("hello"):gsub(".", function(c) return c:upper() end), ("abc"):gsub("%w", "%0%0"), ("abc def"):gsub("%s+", "_"), ("x"):gsub("x", "%%"))
local words = {} for w in ("one two  three"):gmatch("%a+") do words[#words+1] = w end P("gmatch", #words, table.concat(words, ","))
P("format", string.format("%d %5d %-5d| %05d %x %X %o %c %s %10s %-10s| %q %%", 42, 42, 42, 42, 255, 255, 8, 65, "str", "right", "left", "q\"uote"))
P("format2", string.format("%i", -7), string.format("%u", 5), string.format("%5.1s|", "abc"), string.format("%.3s", "abcdef"), string.format("%s %s", 1, tostring(true)), string.format("%d", "10"))
ok, e = pcall(string.format, "%d", "x") P("formaterr", ok, e)
ok, e = pcall(string.format, "%y", 1) P("formatbad", ok, e)
ok, e = pcall(string.rep, "x", -1) P("repneg", ok, e)
P("len2", ("abc"):len(), #"\0\0", ("a\0b"):byte(2), ("%d"):format(3))
P("cmpstr", ("a" == "a"), ("a" == "A"), ("\0a" < "\0b"))

-- tables
local t = {} for i = 1, 10 do t[i] = i * i end
P("insert", table.concat(t, ","))
table.insert(t, 5, 99) table.insert(t, 1000) P("insert2", table.concat(t, ","), #t)
P("remove", table.remove(t), table.remove(t, 1), table.remove(t, 3), #t, table.concat(t, ","))
local s = {5, 2, 9, 1, 5, 6, 0, -3, 12, 7, 7, 8} table.sort(s) P("sort", table.concat(s, ","))
table.sort(s, function(a, b) return a > b end) P("sortdesc", table.concat(s, ","))
local names = {"pear", "Apple", "fig", "banana", "cherry", "apple", "Fig"} table.sort(names) P("sortstr", table.concat(names, ","))
local recs = {} for i = 1, 20 do recs[i] = {k = (i * 7) % 5, i = i} end
table.sort(recs, function(a, b) if a.k ~= b.k then return a.k < b.k end return a.i < b.i end)
local o = {} for i, r in ipairs(recs) do o[#o+1] = r.k .. ":" .. r.i end P("sortrec", table.concat(o, " "))
P("maxn", table.maxn and table.maxn({1, 2, nil, 4}) or "nomaxn", #{1, 2, nil, 4} >= 2)
P("concat2", table.concat({}, ","), table.concat({1}, ","), table.concat({1, 2, 3}, ", ", 2, 3), table.concat({"a", "b"}, ""))
ok, e = pcall(table.concat, {1, {}, 3}) P("concaterr", ok, e)
local keys = {} for k in pairs({a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8}) do keys[#keys+1] = k end P("pairs_str", table.concat(keys, ","))
local nk = {} for k, v in pairs({[10]="a", [20]="b", [30]="c", [5]="d", [1]="e", [100]="f"}) do nk[#nk+1] = k .. "=" .. v end P("pairs_num", table.concat(nk, ","))
local mixed = {1, 2, 3, x=1, y=2} local mk = {} for k, v in pairs(mixed) do mk[#mk+1] = tostring(k) end P("pairs_mixed", table.concat(mk, ","))
local ik = {} for i, v in ipairs({10, 20, nil, 40}) do ik[#ik+1] = i end P("ipairs", table.concat(ik, ","))
local big_t = {} for i = 1, 100 do big_t["k" .. i] = i end local c = 0 local order = {} for k in pairs(big_t) do c = c + 1 if c <= 12 then order[#order+1] = k end end P("pairs_100", c, table.concat(order, ","))

-- metatables, closures, varargs, goto-free control flow
local mt = {__index = function(_, k) return k .. "!" end, __add = function(a, b) return "add" end, __tostring = function() return "OBJ" end, __len = function() return 42 end, __call = function(_, a) return a * 2 end, __concat = function() return "cc" end, __eq = function() return true end, __lt = function() return true end, __le = function() return false end, __unm = function() return "neg" end, __newindex = function(t, k, v) rawset(t, k, v * 2) end}
local obj = setmetatable({}, mt)
P("meta", obj.foo, obj + 1, tostring(obj), #obj, obj(21), obj .. "x", -obj)
obj.z = 4 P("newindex", rawget(obj, "z"))
P("getmeta", getmetatable(obj) == mt, getmetatable("x") ~= nil, getmetatable("x").__index == string)
local function counter() local n = 0 return function() n = n + 1 return n end end
local c1, c2 = counter(), counter() P("closure", c1(), c1(), c2(), c1())
local function va(...) local a, b = ... return select("#", ...), a, b, ... end P("vararg", va(), va(1), va(1, 2, 3))
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end P("fib", fib(20))
local sum = 0 for i = 1, 100 do if i % 3 == 0 then sum = sum + i elseif i % 5 == 0 then sum = sum - i end end P("loop", sum)
sum = 0 for i = 10, 1, -3 do sum = sum * 10 + i end P("forneg", sum)
sum = 0 local i = 0 repeat i = i + 1 sum = sum + i until i >= 10 P("repeat", sum)
local w = 0 while true do w = w + 1 if w > 5 then break end end P("while", w)
P("and_or", nil and 1, false or 2, 1 and 2, nil or false, not nil, not 0)
P("shortcircuit", (function() return false and error("no") end)(), (function() return true or error("no") end)())

-- environment / globals
ok, e = pcall(function() x_global_lq = 5 end) P("global", ok, e, rawget(_G, "x_global_lq"), _VERSION)
local names_g = {} for _, n in ipairs({"loadstring", "load", "setfenv", "getfenv", "dofile", "require", "rawlen", "unpack", "coroutine", "os", "io", "debug", "package", "bit32", "string", "table", "math", "collectgarbage", "gcinfo", "newproxy", "xpcall", "assert", "select", "module"}) do names_g[#names_g+1] = n .. "=" .. tostring(rawget(_G, n) ~= nil) end
P("globals", table.concat(names_g, " "))
local ls = rawget(_G, "loadstring") or rawget(_G, "load")
if ls then
	P("loadstring", ls("return 1+1")())
	local f, err = ls("x = = 1") P("syntax", f, err)
	f, err = ls("return 'unfinished") P("syntax2", f, err)
	f, err = ls("local 5 = 1") P("syntax3", f, err)
	-- identifiers with bytes >= 0x80 (the PC game runs the lexer under a code page where isalpha() accepts them)
	f, err = ls("local \195\177x = 3 return \195\177x") P("highbyte", f and f() or err)
	f, err = ls("return 0x10 + 010 + 1") P("numlex", f and f() or err)
	f, err = ls("return 1.5") P("numlex2", f and f() or err)
	f, err = ls("return 'a\\n\\t\\065\\x41'") P("strlex", f and f() or err)
	f, err = ls("--[[ block ]] return [[long\nstring]]") P("longstr", f and f() or err)
end

-- error objects with positions
ok, e = pcall(function() local a = {} a.b.c = 1 end) P("poserr", ok, e)
ok, e = pcall(function() undefined_fn_lq() end) P("callnil", ok, e)
ok, e = pcall(function() return ("x"):bad() end) P("badmethod", ok, e)
ok, e = pcall(function() return 1 < "x" end) P("cmperr", ok, e)
ok, e = pcall(function() return {} < {} end) P("cmptab", ok, e)
ok, e = pcall(string.sub) P("noarg", ok, e)
ok, e = pcall(setmetatable, 1, {}) P("setmeta", ok, e)
ok, e = xpcall(function() error("boom") end, function(m) return "handled:" .. m end) P("xpcall", ok, e)

-- coroutines
local co = coroutine.create(function(a, b) local c = coroutine.yield(a + b) local d, e2 = coroutine.yield(c * 2) return d + e2 end)
P("co1", coroutine.resume(co, 1, 2)) P("co2", coroutine.resume(co, 10)) P("co3", coroutine.resume(co, 3, 4)) P("co4", coroutine.resume(co)) P("costatus", coroutine.status(co))
local gen = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i) end end) P("cowrap", gen(), gen(), gen())

-- collectgarbage: only that it works
local cnt = collectgarbage("count") P("gc", type(cnt), cnt > 0)
collectgarbage() collectgarbage("collect") collectgarbage("step") P("gc2", "ok")
-- deep recursion limits behave the same (error, not a crash)
local function deep(n) return n == 0 and 0 or 1 + deep(n - 1) end
P("deep", pcall(deep, 100), (pcall(deep, 100000)))
local function so() return so() + 1 end
ok, e = pcall(so) P("stackoverflow", ok, type(e))
-- number literals and strings beyond 32 bits (lexer and tonumber): the PC reads them with a 64-bit long
P("lit32", 2147483647, 2147483648, -2147483648, 2147483649, 4294967295, 4294967296, 9999999999, 99999999999999999999)
P("lithex", 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000, 0x1FFFFFFFF, 0xDEADBEEF)
P("tonum32", tonumber("2147483647"), tonumber("2147483648"), tonumber("-2147483648"), tonumber("-2147483649"), tonumber("4294967295"), tonumber("4294967297"), tonumber("99999999999999999999"))
P("tonumhex", tonumber("0xFFFFFFFF"), tonumber("0x100000000"), tonumber("0x1FFFFFFFF"), tonumber("-0x10"), tonumber("0x"))
P("strarith", "10" + 5, "0x10" + 1, "2147483648" + 0, " 12 " * 2, pcall(function() return "abc" + 1 end))
P("wrap", 2147483647 + 1, -2147483647 - 2, 65536 * 65536, 46341 * 46341, -2147483647 - 1 - 1)
P("DONE_VM")
