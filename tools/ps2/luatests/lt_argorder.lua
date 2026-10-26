-- OPT14: C leaves the order of evaluation of function arguments unspecified; the Lua library functions that read two arguments inside one expression raise the error of the first
-- or of the second bad argument depending on the compiler (x86 GCC / MSVC: the last argument first; EE GCC: the first). The message must be the PC's.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function E(f, ...) local r = {pcall(f, ...)} if r[1] then table.remove(r, 1) for i = 1, #r do local v = r[i] r[i] = (type(v) == "userdata") and "userdata" or tostring(v) end return "ok:" .. table.concat(r, ",") end return "E:" .. (tostring(r[2]):gsub("^.-:%d+: ", "")) end
local bad = {}
for _, fn in ipairs({"FixedMul", "FixedMod", "FixedHypot", "FixedDiv", "FixedRem", "FixedSqrt", "R_PointToAngle2", "R_PointToDist2", "R_PointToDist", "P_AproxDistance", "P_ClosestPointOnLine", "FixedLerp", "FixedAngle", "AngleFixed", "TeleportMove"}) do
	local f = rawget(_G, fn)
	if f then
		P(fn, "nil,nil", E(f, nil, nil), "|", "{},nil", E(f, {}, nil), "|", "nil,{}", E(f, nil, {}), "|", "1,nil", E(f, 1, nil), "|", "nil,1", E(f, nil, 1), "|", "str,{}", E(f, "x", {}))
	else
		P(fn, "missing")
	end
end
for _, fn in ipairs({"R_PointToAngle2", "P_AproxDistance"}) do
	local f = rawget(_G, fn)
	if f then P(fn .. "4", E(f, nil, nil, nil, nil), E(f, 1, 2, nil, {}), E(f, {}, nil, 1, 2)) end
end
P("getsecspecial", E(GETSECSPECIAL, nil, nil), E(GETSECSPECIAL, 1, nil), E(GETSECSPECIAL, nil, 1), E(GETSECSPECIAL, 1, 2))
P("DONE_ARGORDER")
