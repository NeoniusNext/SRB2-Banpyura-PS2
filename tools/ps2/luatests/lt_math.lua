-- Lua equivalence: fixed point math, trigonometry, tables of the engine (FINEACON.DAT on the PS2), easing, colour, text helpers.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local FU = FRACUNIT
local edge = {0, 1, -1, 2, -2, 7, FU/2, FU, -FU, FU+1, 3*FU, -3*FU, 100*FU, 255*FU, 1000*FU, 32767*FU, 2147483647, -2147483647, -2147483647-1, 12345678, -87654321, 46340*FU, 65535, 65536, 0x7fff0000, 0x12345678, FU*FU}
local function hash(fn, a, b)
	local h = 17
	for i = 1, #a do
		if b then
			for j = 1, #b do
				local ok, r = pcall(fn, a[i], b[j])
				h = (h * 31 + (ok and r or 7777)) % 4294967296 - 0
			end
		else
			local ok, r = pcall(fn, a[i])
			h = (h * 31 + (ok and r or 7777)) % 4294967296
		end
	end
	return h
end
-- numbers have to stay inside 32 bits: fold the hash back
local function fold(h) return h end
P("consts", FRACUNIT, FRACBITS, ANGLE_90, ANGLE_180, ANGLE_270, ANGLE_45, ANG1, ANG2, ANG10, ANG15, ANG20, ANG30, ANG60, ANGLE_11hh, ANGLE_22h, ANGLE_112h, ANGLE_157h)

local fns = {"FixedMul", "FixedDiv", "FixedRem", "FixedHypot"}
for _, n in ipairs(fns) do
	local f = _G[n]
	local row = {}
	for i = 1, #edge do for j = 1, #edge do
		local skip = (n == "FixedRem") and (edge[j] == 0 or (edge[j] == -1 and edge[i] == -2147483647-1)) -- C: SIGFPE on x86 (and a break on the EE)
		if n == "FixedHypot" and (edge[i] == -2147483647-1 or edge[j] == -2147483647-1) then skip = true end -- upstream: R_PointToDist2 reads tantoangle[] at a negative index for 0x80000000 (what is in front of the table differs per build)
		local ok, r = false, nil
		if not skip then ok, r = pcall(f, edge[i], edge[j]) end
		row[#row+1] = ok and r or "E"
	end end
	local h = 0 for i, v in ipairs(row) do h = h * 31 + (tonumber(v) or 99) end
	P(n, #row, h, row[1], row[2], row[30], row[40], row[100], row[200], row[#row])
end
local f1 = {"FixedSqrt", "FixedInt", "FixedFloor", "FixedTrunc", "FixedCeil", "FixedRound", "FixedAngle", "AngleFixed", "InvAngle", "abs", "sin", "cos", "tan", "asin", "acos", "GetSecSpecial"}
for _, n in ipairs(f1) do
	local f = _G[n]
	local row = {}
	for i = 1, #edge do
		local ok, r = pcall(f, edge[i])
		row[#row+1] = ok and r or "E"
	end
	local h = 0 for i, v in ipairs(row) do h = h * 31 + (tonumber(v) or 99) end
	P(n, #row, h, table.concat({row[1], row[2], row[3], row[7], row[8], row[10], row[11], row[14], row[17]}, ","))
end
-- aliases
P("alias", fixmul(3*FU, FU/2), fixdiv(3*FU, 2*FU), fixint(5*FU+3), fixsqrt(4*FU), fixhypot(3*FU, 4*FU), fixfloor(-FU*3/2), fixceil(FU*3/2), fixround(FU*5/2), fixtrunc(-FU*5/2), fixrem(7*FU, 2*FU), fixangle(90*FU), anglefix(ANGLE_90))
P("minmax", min(1, 2), max(1, 2), min(-5, 3, 9), max(-5, 3, 9), abs(-5), abs(5), abs(-2147483647-1))
-- the full sine / cosine / tangent / arccos tables
local hs, hc, ht, ha, hb = 0, 0, 0, 0, 0
for a = 0, 8191 do
	local an = a * 524288
	hs = (hs * 31 + sin(an)) % 1000000007
	hc = (hc * 31 + cos(an)) % 1000000007
	ht = (ht * 31 + tan(an)) % 1000000007
end
for v = -FU, FU, 257 do
	ha = (ha * 31 + acos(v)) % 1000000007
	hb = (hb * 31 + asin(v)) % 1000000007
end
P("tables", hs, hc, ht, ha, hb)
P("trigsamples", sin(0), sin(ANGLE_90), sin(ANGLE_180), sin(ANGLE_270), cos(0), cos(ANGLE_90), cos(ANGLE_180), tan(ANGLE_45), tan(ANGLE_22h), sin(ANG1), cos(ANG30), sin(-ANG10))
P("arcsamples", acos(0), acos(FU), acos(-FU), acos(FU/2), acos(-FU/2), asin(0), asin(FU), asin(-FU), asin(FU/2), acos(FU+1), asin(-FU-1), acos(2*FU), asin(100*FU))
-- angles between points (BSP / table based), distances
local ang = {}
for _, p in ipairs({{FU,0},{0,FU},{-FU,0},{0,-FU},{FU,FU},{3*FU,-4*FU},{100*FU,1},{1,100*FU},{-12345*FU,6789*FU},{0,0}}) do
	ang[#ang+1] = R_PointToAngle2(0, 0, p[1], p[2])
	ang[#ang+1] = R_PointToDist2(0, 0, p[1], p[2])
	ang[#ang+1] = P_AproxDistance(p[1], p[2])
end
P("points", table.concat(ang, ","))
-- easing
local en = {"linear", "insine", "outsine", "inoutsine", "inquad", "outquad", "inoutquad", "incubic", "outcubic", "inoutcubic", "inquart", "outquart", "inoutquart", "inquint", "outquint", "inoutquint", "inexpo", "outexpo", "inoutexpo", "inback", "outback", "inoutback"}
for _, n in ipairs(en) do
	local f = ease[n]
	local h, c = 0, 0
	for t = -FU/2, 3*FU/2, 977 do h = (h * 31 + f(t)) % 1000000007 h = (h * 31 + f(t, 5*FU)) % 1000000007 h = (h * 31 + f(t, -3*FU, 9*FU)) % 1000000007 c = c + 1 end
	P("ease_" .. n, c, h, f(0), f(FU/2), f(FU), f(FU/3, 10*FU, 20*FU))
end
P("ease_param", ease.inback(FU/2, 0, FU, FU), ease.outback(FU/2, 0, FU, 3*FU), ease.inoutback(FU/4, 0, FU, 2*FU), ease.inback(FU/2, 0, FU, nil))
-- helpers
P("tofixed", tofixed("1.5"), tofixed("-2.25"), tofixed("0.001"), tofixed("100"), tofixed("3.14159"), tofixed(".5"), tofixed("1e2"), tofixed("abc"), tofixed("32767.99"), tofixed("0.00001526"))
P("tics", G_TicsToHours(123456), G_TicsToMinutes(123456, true), G_TicsToMinutes(123456, false), G_TicsToSeconds(123456), G_TicsToCentiseconds(123456), G_TicsToMilliseconds(123456), G_TicsToHours(0), G_TicsToMilliseconds(35), G_TicsToMilliseconds(34))
-- the tables the PS2 build keeps on the side
P("coloropp", ColorOpposite and select("#", ColorOpposite(5)) or "none", ColorOpposite and ColorOpposite(5) or "", ColorOpposite and ColorOpposite(15) or "")
-- string helpers of the engine
P("All7", All7Emeralds(0), All7Emeralds(127), All7Emeralds(63))
P("secspecial", GetSecSpecial(0, 1), GetSecSpecial(0x1234, 1), GetSecSpecial(0x1234, 2), GetSecSpecial(0x1234, 3), GetSecSpecial(0x1234, 4))
-- bulk: pseudo random operands (xorshift on 32-bit integers), results folded into hashes
local seed = 123456789
local function rnd() seed = seed ^^ (seed << 13) seed = seed ^^ ((seed >> 17) & 0x7FFF) seed = seed ^^ (seed << 5) return seed end
local hm, hd, hs2, hh, ht2, hr = 0, 0, 0, 0, 0, 0
for i = 1, 3000 do
	local a, b = rnd(), rnd()
	if i % 3 == 0 then a = a >> 10 b = b >> 12 elseif i % 3 == 1 then a = a >> 4 b = (b >> 20) + 1 end
	hm = (hm * 31 + FixedMul(a, b)) % 1000000007
	if b ~= 0 then hd = (hd * 31 + FixedDiv(a, b)) % 1000000007 end
	hh = (hh * 31 + FixedHypot(a >> 2, b >> 2)) % 1000000007
	if a >= 0 then hs2 = (hs2 * 31 + FixedSqrt(a)) % 1000000007 end
	ht2 = (ht2 * 31 + R_PointToAngle2(0, 0, a, b)) % 1000000007
	hr = (hr * 31 + FixedAngle(a) % 65521 + AngleFixed(b) % 65519) % 1000000007
end
P("bulk", hm, hd, hh, hs2, ht2, hr)
local ht = 0
for i = 1, 400 do
	local ip = (rnd() >> 8) % 40000 - 8000
	local fr = (rnd() >> 8) % 1000000
	local str = ip .. "." .. string.format("%06d", fr)
	if i % 7 == 0 then str = "-" .. str end
	if i % 11 == 0 then str = str:sub(1, #str - 3) end
	ht = (ht * 31 + (tofixed(str) or 7)) % 1000000007
end
P("tofixed_bulk", ht)
P("DONE_MATH")
