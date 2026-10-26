-- OPT14: every binary and unary operator of the integer VM over a grid of edge operands (INT32 limits, small, negative, powers of two), hashed, and the edge values printed. Shift counts beyond 31 and
-- negative are undefined in C; the PC (x86 shl/sar mask the count to 5 bits) and the EE (sllv/srav the same) must agree. INT_MIN / -1 and INT_MIN % -1 trap on x86 and are left out.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local MIN = -2147483647 - 1 -- (the literal 2147483648 is read by strtol: 64 bit long on the PC, saturated on the EE and on Windows: lt_plat)
local vals = {0, 1, -1, 2, -2, 3, 5, 7, 8, 15, 16, 31, 32, 33, 63, 64, 100, 255, 256, 1000, 32767, 32768, 65535, 65536, 65537, 1073741823, 1073741824, 2147483646, 2147483647,
	-2147483647, MIN, -2147483646, -1073741824, -65536, -32768, -100, -31, -32, -33}
local function h(f)
	local acc, n = 17, 0
	for _, a in ipairs(vals) do
		for _, b in ipairs(vals) do
			local ok, r = pcall(f, a, b)
			local v = ok and r or 12345
			if type(r) == "boolean" then v = r and 1 or 0 end
			acc = (acc * 31 + (v & 0x7FFFFFFF) + (v < 0 and 7 or 0)) % 1000003
			n = n + 1
		end
	end
	return acc
end
local skip = function(a, b) return (a == MIN and b == -1) end
P("add", h(function(a, b) return a + b end))
P("sub", h(function(a, b) return a - b end))
P("mul", h(function(a, b) return a * b end))
P("div", h(function(a, b) if b == 0 or skip(a, b) then return 0 end return a / b end))
P("mod", h(function(a, b) if b == 0 or skip(a, b) then return 0 end return a % b end))
P("and", h(function(a, b) return a & b end))
P("or", h(function(a, b) return a | b end))
P("xor", h(function(a, b) return a ^^ b end))
P("shl", h(function(a, b) return a << b end))
P("shr", h(function(a, b) return a >> b end))
P("lt", h(function(a, b) return a < b end))
P("le", h(function(a, b) return a <= b end))
P("eq", h(function(a, b) return a == b end))
P("pow", h(function(a, b) if b < 0 or b > 40 or a > 100000 or a < -100000 then return 0 end return a ^ b end))
P("unm", h(function(a) return -a end))
P("bnot", h(function(a) return ~a end))
P("concat", h(function(a, b) return #(a .. "x" .. b) end))
P("fmt_d", h(function(a, b) return #string.format("%d %5d %-5d %05d", a, b, a, b) end))
P("fmt_x", h(function(a, b) return #string.format("%X %o %c", a & 0x7FFFFFFF, b & 0x7FFFFFFF, 65 + (a & 7)) end)) -- (%x of a negative number prints 16 digits on LP64 Linux, 8 on the EE and Windows: lt_plat)
P("tostr", h(function(a, b) return #(tostring(a) .. tostring(b)) end))
P("tonum", h(function(a, b) return tonumber(tostring(a)) == a and 1 or 0 end))
P("edges", 1 << 31, 1 << 32, 1 << 33, 1 << -1, 1 << 63, -1 >> 1, -1 >> 31, -1 >> 32, 4 >> 33, MIN >> 31, MIN << 1, 2147483647 + 1, MIN - 1, 65536 * 65536, 46341 * 46341,
	MIN * -1, -MIN, ~0, ~-1, 7 / 2, -7 / 2, 7 / -2, -7 / -2, 7 % 3, -7 % 3, 7 % -3, -7 % -3, 0 ^ 0, 2 ^ 30, 2 ^ 31, -2 ^ 31, 3 ^ 20, 10 ^ 9, 10 ^ 10, 0 ^ -1)
P("cmp_str", "a" < "b", "Z" < "a", "" < "a", "10" < "9", "abc" == "abc", "a" <= "a")
P("num_str", 10 .. "", "10" + 5, "0x10" + 0, "5" * "6", pcall(function() return "x" + 1 end))
P("DONE_OPS")
