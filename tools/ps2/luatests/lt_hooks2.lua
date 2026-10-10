-- Lua equivalence: the hooks a demo does not reach. A live single-player game (-warp), the script causes the events (damage, boss, shield, chat, exit, intermission, map change) and
-- every call of every hook is counted with a digest of its arguments; lines are printed per event. Both builds run the same tics from the same level start.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local cnt, acc = {}, {}
local function mix(name, v) acc[name] = ((acc[name] or 0) * 31 + v) % 1000003 end
local function digest(name, ...)
	cnt[name] = (cnt[name] or 0) + 1
	for i = 1, select("#", ...) do
		local a = select(i, ...)
		local ta = type(a)
		if ta == "number" then mix(name, a % 65536)
		elseif ta == "boolean" then mix(name, a and 1 or 2)
		elseif ta == "string" then mix(name, #a)
		elseif ta == "userdata" then
			local ok, x = pcall(function() return a.valid end)
			if ok and x then
				local o, ty = pcall(function() return a.type end)
				if o and type(ty) == "number" then mix(name, ty) end
				local o2, px = pcall(function() return a.x end)
				if o2 and type(px) == "number" then mix(name, (px >> 16) % 65536) end
				local o4, hp = pcall(function() return a.health end)
				if o4 and type(hp) == "number" then mix(name, hp % 65536) end
			else mix(name, 77) end
		else mix(name, 11) end
	end
end
local rets = {}  -- hook name -> value the hook returns (nil: none)
local function hook(name, ...)
	local extra = {...}
	local ok, err = pcall(addHook, name, function(...) digest(name, ...) if name == "SeenPlayer" then return nil end return rets[name] end, unpack(extra))
	if not ok then P("addhook_err", name, (tostring(err):gsub("^.-:%d+: ", ""))) end
end
for _, h in ipairs({"MobjSpawn", "MobjCollide", "MobjLineCollide", "MobjMoveCollide", "TouchSpecial", "MobjFuse", "BossThinker", "ShouldDamage", "MobjDamage", "MobjDeath", "BossDeath", "MobjRemoved", "BotRespawn", "MobjMoveBlocked", "MapThingSpawn", "FollowMobj", "HurtMsg"}) do
	hook(h) hook(h, MT_PLAYER) hook(h, MT_EGGMOBILE) hook(h, MT_EGGMOBILE2)
end
for _, h in ipairs({"MapChange", "MapLoad", "PlayerJoin", "JumpSpecial", "AbilitySpecial", "SpinSpecial", "JumpSpinSpecial", "BotTiccmd", "PlayerMsg", "PlayerSpawn", "ShieldSpawn", "ShieldSpecial", "PlayerCanDamage", "PlayerQuit",
	"IntermissionThinker", "TeamSwitch", "ViewpointSwitch", "SeenPlayer", "PlayerThink", "GameQuit", "PlayerCmd", "MusicChange", "PlayerHeight", "PlayerCanEnterSpinGaps", "AddonLoaded", "CameraThinker", "NetVars"}) do hook(h) end
hook("LinedefExecute", "LQEXEC") hook("ShouldJingleContinue", "LQJINGLE") hook("BotAI", "sonic")
local function summary()
	local names = {} for k in pairs(cnt) do names[#names+1] = k end table.sort(names)
	local parts = {} for _, k in ipairs(names) do if k ~= "PlayerCmd" then parts[#parts+1] = k .. "=" .. cnt[k] .. "/" .. (acc[k] or 0) end end -- PlayerCmd: one call per ticcmd built, which depends on the frame timing of the machine
	return table.concat(parts, " ")
end
local function E(f, ...) local r = {pcall(f, ...)} if r[1] then table.remove(r, 1) for i = 1, #r do r[i] = (type(r[i]) == "userdata") and "userdata" or tostring(r[i]) end return table.concat(r, ",") end return "E:" .. (tostring(r[2]):gsub("^.-:%d+: ", "")) end
local boss
local step = {}
local function at(t, fn) step[t] = fn end
local lastmap = -1
addHook("MapLoad", function(m) P("maploaded", m, leveltime, gamemap) end)
addHook("MapChange", function(m) P("mapchange", m, gamemap) end)
addHook("IntermissionThinker", function() if not step.inter then step.inter = true P("intermission", "start", cnt.IntermissionThinker) end end)
at(5, function() local p = players[0] P("shield1", E(P_SwitchShield, p, SH_WHIRLWIND), p.powers[pw_shield], E(P_SpawnShieldOrb, p)) end)
at(8, function() local p = players[0] P("shield2", E(P_SwitchShield, p, SH_ELEMENTAL), p.powers[pw_shield], summary()) end)
at(12, function() local p = players[0] P("damage1", E(P_DamageMobj, p.mo, nil, nil, 1), p.powers[pw_shield], p.mo.health, p.rings) end)
at(20, function() local p = players[0] boss = P_SpawnMobj(p.mo.x + 300*FRACUNIT, p.mo.y, p.mo.z, MT_EGGMOBILE) P("boss", boss and boss.valid, boss and boss.health, boss and boss.flags & MF_BOSS ~= 0) end)
at(21, function() P("bosshealth", boss and boss.valid and boss.health or -1) end)
at(22, function() if boss and boss.valid then P("bosskill", E(P_KillMobj, boss, nil, players[0].mo), boss.valid) end end)
at(50, function() local p = players[0] P("chat", E(COM_BufAddText, p, "say hello")) end)
at(55, function() local p = players[0] P("viewpoint", E(COM_BufAddText, p, "viewpoint 1")) end)
at(60, function() local p = players[0] P("kill", E(P_DamageMobj, p.mo, nil, nil, 1, DMG_INSTAKILL), p.mo.health, p.playerstate, p.lives) end)
at(70, function() local p = players[0] P("afterkill", p.playerstate, p.mo and p.mo.health or -1, p.lives, p.rings) end)
at(75, function() P("linedef", E(function() lines[0].special = 443 lines[0].tag = 4242 lines[0].stringargs[0] = "LQEXEC" return P_LinedefExecute(4242, players[0].mo) end)) end)
at(78, function() P("music", E(S_ChangeMusic, "LCZONE", true, players[0]), E(S_StopMusic, players[0])) end)
at(80, function() P("exit", E(G_ExitLevel)) end)
local ticker = 0
addHook("ThinkFrame", function()
	ticker = ticker + 1
	local fn = step[leveltime]
	if fn and not step["done" .. leveltime] then step["done" .. leveltime] = true fn() end
	if leveltime == 80 or (leveltime > 0 and leveltime % 25 == 0) then P("hooks", leveltime, summary()) end
end)
-- the intermission and the next map are driven by the game; stop after the next map is loaded and ticked
addHook("MapLoad", function(m) if m ~= 1 then P("nextmap", m, summary()) P("DONE_HOOKS2") end end)
P("registered", "ok")
