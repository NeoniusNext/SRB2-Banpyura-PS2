local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function fields(tbl, names)
	local out = {}
	for _, n in ipairs(names) do local ok, v = pcall(function() return tbl[n] end) out[#out+1] = n .. "=" .. (ok and (type(v) == "userdata" and "ud" or tostring(v)) or "E") end
	return table.concat(out, " ")
end
local function state(n) return fields(states[n], {"sprite", "frame", "tics", "var1", "var2", "nextstate", "action"}) end
local mi = {"doomednum", "spawnstate", "spawnhealth", "seestate", "seesound", "reactiontime", "attacksound", "painstate", "painchance", "painsound", "meleestate", "missilestate", "deathstate", "xdeathstate", "deathsound", "speed", "radius", "height", "dispoffset", "mass", "damage", "activesound", "flags", "raisestate"}
P("mt1", fields(mobjinfo[MT_LQSOC], mi))
P("mt2", fields(mobjinfo[MT_LQSOC2], mi))
P("s1", state(S_LQSOC1))
P("s2", state(S_LQSOC2))
P("sfx", pcall(function() return sfx_lqsoc end), pcall(function() return fields(S_sfx[sfx_lqsoc], {"singular", "priority", "flags", "pitch", "volume", "name"}) end))
P("color", fields(skincolors[SKINCOLOR_LQSOC], {"name", "ramp", "invcolor", "invshade", "chatcolor", "accessible"}))
local mh = mapheaderinfo[98]
P("level", mh and fields(mh, {"lvlttl", "weather", "skynum", "actnum", "nozone"}) or "nil")
P("DONE_SOCREAD")
