-- Lua equivalence: calls into the engine (P_*, R_*, G_*, M_*) with fixed arguments inside a played demo; results printed as numbers / booleans / strings.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function norm(v)
	local t = type(v)
	if t == "userdata" then
		local ok, ty = pcall(function() return v.type end)
		local ok2, x = pcall(function() return v.x end)
		if ok and ok2 and type(ty) == "number" then return "mo(" .. ty .. "," .. (x >> 16) .. ")" end
		local ok3, fh = pcall(function() return v.floorheight end)
		if ok3 and type(fh) == "number" then return "sector(" .. fh .. ")" end
		local ok4, ln = pcall(function() return v.special end)
		if ok4 and type(ln) == "number" then return "obj(" .. ln .. ")" end
		return "ud"
	elseif t == "table" then return "table"
	elseif t == "function" then return "function" end
	return tostring(v)
end
local function T(name, ...)
	local f = rawget(_G, name)
	if not f then P(name, "MISSING") return end
	local r = {pcall(f, ...)}
	local out = {}
	for i = 2, #r do out[#out+1] = norm(r[i]) end
	if not r[1] then out = {"E:" .. (tostring(r[2]):gsub("^.-:%d+: ", ""))} end
	P(name, table.concat(out, ","))
end
local done = false
addHook("ThinkFrame", function()
	if done or leveltime ~= 80 then return end
	done = true
	local p = players[0]
	local mo = p.mo
	local x, y, z = mo.x, mo.y, mo.z
	local FU = FRACUNIT
	P("start", x, y, z, mo.momx, mo.momy, mo.momz, mo.state == S_PLAY_STND, mo.sprite, mo.frame, mo.radius, mo.height, mo.scale, mo.floorz, mo.ceilingz)
	-- pure geometry
	T("P_AproxDistance", 3*FU, 4*FU) T("P_AproxDistance", -100*FU, 77*FU) T("P_AproxDistance", 0, 0) T("P_AproxDistance", 2147483647, 5)
	T("R_PointToAngle2", 0, 0, 100*FU, 50*FU) T("R_PointToAngle2", x, y, x + 5*FU, y - 7*FU) T("R_PointToDist2", 0, 0, 300*FU, -400*FU)
	T("R_PointInSubsector", x, y) T("R_PointInSubsectorOrNil", 1000000*FU, 1000000*FU)
	T("R_Char2Frame", "A") T("R_Char2Frame", "z") T("R_Char2Frame", "5") T("R_Frame2Char", 0) T("R_Frame2Char", 27) T("R_Frame2Char", 40) T("R_Char2Frame", "")
	T("R_CheckTextureNumForName", "GFZROCK") T("R_CheckTextureNumForName", "NOSUCHTEX") T("R_TextureNumForName", "GFZROCK") T("R_TextureNumForName", "NOSUCHTEX") T("R_CheckTextureNameForNum", 5) T("R_TextureNameForNum", 5)
	T("R_GetColorByName", "Red") T("R_GetColorByName", "nosuch") T("R_GetSuperColorByName", "Gold") T("R_GetNameByColor", SKINCOLOR_RED) T("R_GetNameByColor", 999999)
	T("R_SkinUsable", 0, "sonic") T("R_SkinUsable", 0, "nosuch") T("R_SetPlayerSkin", p, "sonic")
	T("M_MapNumber", "MAP01") T("M_MapNumber", "MAPZZ") T("M_MapNumber", "01") T("M_GetColorAfter", SKINCOLOR_RED) T("M_GetColorBefore", SKINCOLOR_RED) T("M_MoveColorAfter", SKINCOLOR_RED, SKINCOLOR_BLUE)
	T("G_BuildMapName", 1) T("G_BuildMapName", 36) T("G_BuildMapName", 100) T("G_BuildMapName", 1035) T("G_BuildMapTitle", 1) T("G_BuildMapTitle", 11) T("G_FindMap", "Greenflower") T("G_FindMapByNameOrCode", "MAP01") T("G_FindMapByNameOrCode", "gfz")
	T("G_IsSpecialStage", 50) T("G_IsSpecialStage", 1) T("G_GametypeUsesLives") T("G_GametypeUsesCoopLives") T("G_GametypeUsesCoopStarposts") T("G_GametypeHasTeams") T("G_GametypeHasSpectators") T("G_RingSlingerGametype") T("G_PlatformGametype") T("G_CoopGametype") T("G_TagGametype") T("G_CompetitionGametype")
	T("G_EnoughPlayersFinished") T("G_TicsToHours", 999999) T("G_TicsToMinutes", 999999, true) T("G_TicsToSeconds", 999999) T("G_TicsToCentiseconds", 999999) T("G_TicsToMilliseconds", 999999)
	-- mobj queries
	T("P_GetMobjGravity", mo) T("P_MobjFlip", mo) T("P_IsObjectOnGround", mo) T("P_InQuicksand", mo) T("P_InSpaceSector", mo) T("P_InJumpFlipSector", p) T("P_IsObjectInGoop", mo) T("P_GetPlayerHeight", p) T("P_GetPlayerSpinHeight", p)
	T("P_GetPlayerControlDirection", p) T("P_PlayerInPain", p) T("P_IsLocalPlayer", p) T("P_PlayerCanDamage", p, mo) T("P_PlayerFullbright", p) T("P_GetJumpFlags", p) T("P_CanRunOnWater", mo) T("P_CheckDeathPitCollide", mo) T("P_CheckSolidLava", mo)
	T("P_PlayerShouldUseSpinHeight", p) T("P_PlayerCanEnterSpinGaps", p) T("P_SuperReady", p) T("P_GetClosestAxis", mo) T("P_InsideANonSolidFFloor", mo, mo.subsector.sector.ffloors and nil) T("P_MobjTouchingSectorSpecial", mo, 1, 1)
	T("P_PlayerTouchingSectorSpecial", p, 1, 1) T("P_PlayerTouchingSectorSpecialFlag", p, 1) T("P_MobjTouchingSectorSpecialFlag", mo, 1) T("P_ThingOnSpecial3DFloor", mo)
	local sec = mo.subsector.sector
	T("P_FloorzAtPos", x, y, z, mo.height) T("P_CeilingzAtPos", x, y, z, mo.height) T("P_GetSectorLightLevelAt", x, y, z) T("P_FindLowestFloorSurrounding", sec) T("P_FindHighestFloorSurrounding", sec) T("P_FindNextHighestFloor", sec, sec.floorheight)
	T("P_FindNextLowestFloor", sec, sec.floorheight) T("P_FindLowestCeilingSurrounding", sec) T("P_FindHighestCeilingSurrounding", sec) T("P_FindSpecialLineFromTag", 1, 1, -1) T("P_GetZAt", nil, x, y)
	T("P_CheckSight", mo, mo) T("P_PointOnLineSide", x, y, lines[0]) T("P_ClosestPointOnLine", x, y, lines[0]) T("P_ClosestPointOnLine", x, y, lines[5])
	T("P_CheckPosition", mo, x, y) T("P_CheckPosition", mo, x + 100*FU, y) T("P_CheckHoopPosition", mo, x, y, z, 50*FU) T("P_TryMove", mo, x, y, true)
	T("P_CheckMeleeRange", mo) T("P_CheckMissileRange", mo) T("P_LookForPlayers", mo, 0, false, 0) T("P_NewChaseDir", mo) T("P_SwitchWeather", 0)
	-- random numbers (the demo seeded the generator; both consume it the same way)
	local r = {} for i = 1, 20 do r[#r+1] = P_RandomByte() end P("randbytes", table.concat(r, ","))
	r = {} for i = 1, 10 do r[#r+1] = P_RandomFixed() end P("randfixed", table.concat(r, ","))
	r = {} for i = 1, 10 do r[#r+1] = P_RandomKey(100) end P("randkey", table.concat(r, ","))
	r = {} for i = 1, 10 do r[#r+1] = P_RandomRange(-5, 5) end P("randrange", table.concat(r, ","))
	r = {} for i = 1, 10 do r[#r+1] = P_SignedRandom() end P("randsigned", table.concat(r, ","))
	r = {} for i = 1, 10 do r[#r+1] = tostring(P_RandomChance(FU/3)) end P("randchance", table.concat(r, ","))
	T("P_RandomKey", 0) T("P_RandomKey", -3) T("P_RandomRange", 5, -5)
	-- spawning and state changes
	local m = P_SpawnMobj(x + 200*FU, y, z + 50*FU, MT_RING)
	P("spawn", norm(m), m.health, m.radius, m.height, m.flags, m.state == states[mobjinfo[MT_RING].spawnstate], m.floorz, m.ceilingz)
	T("P_SetMobjStateNF", m, S_RING) T("P_SetScale", m, FU * 2) P("scale", m.scale, m.radius, m.height) T("P_InstaThrust", m, ANGLE_90, 10*FU) P("thrust", m.momx, m.momy) T("P_Thrust", m, ANGLE_45, 5*FU) P("thrust2", m.momx, m.momy)
	T("P_SetObjectMomZ", m, 12*FU, false) P("momz", m.momz) T("P_TeleportMove", m, x + 300*FU, y + 20*FU, z) P("tp", m.x, m.y, m.z, m.floorz, m.ceilingz)
	T("P_XYMovement", m) T("P_ZMovement", m) P("moved", m.x, m.y, m.z, m.momx, m.momy, m.momz)
	T("P_RemoveMobj", m) P("removed", m.valid)
	T("P_SpawnMobj", x, y, z, 99999) T("P_SpawnMobj", x, y, z, MT_NULL) T("P_SpawnMobj", x, y, z, -1)
	local m2 = P_SpawnMobjFromMobj(mo, 10*FU, 20*FU, 30*FU, MT_THOK) P("spawnfrom", norm(m2), m2 and m2.x - x, m2 and m2.y - y, m2 and m2.z - z) P_RemoveMobj(m2)
	local m3 = P_SpawnMissile(mo, mo, MT_ARROW) P("missile", m3 ~= nil)
	T("P_DamageMobj", mo, nil, nil, 0, 0) P("hurt", p.rings, mo.health, p.powers[pw_invulnerability])
	T("P_GivePlayerRings", p, 5) P("rings", p.rings) T("P_GivePlayerLives", p, 2) P("lives", p.lives) T("P_AddPlayerScore", p, 100) P("score", p.score)
	T("P_ResetPlayer", p) T("P_DoJump", p, true) P("jump", mo.momz, p.pflags) T("P_MovePlayer", p) T("P_PlayerZMovement", mo)
	T("P_RadiusAttack", mo, mo, 100*FU, 0, false) T("P_Earthquake", mo, mo, 500*FU) T("P_FlashPal", p, 1, 10)
	T("P_ButteredSlope", mo) T("P_IsValidSprite2", mo, SPR2_STND) T("P_IsValidSprite2", mo, 9999) T("P_GetStateSprite2", S_PLAY_STND) T("P_GetStateSprite2", 99999) T("P_GetSprite2StateFrame", S_PLAY_STND) T("P_IsStateSprite2Super", S_PLAY_STND)
	T("P_GetSuperSprite2", SPR2_STND) T("P_GetSuperSprite2", 99999) T("P_SpawnLockOn", p, mo, S_LOCKON1)
	-- misc
	T("userdataType", mo) T("userdataType", p) T("userdataType", sec) T("userdataType", 5) T("userdataType", nil)
	T("tofixed", "1.5") T("IsPlayerAdmin", p) T("reserveLuabanks") T("S_SoundPlaying", mo, sfx_jump) T("S_IdPlaying", sfx_jump) T("S_MusicExists", "GFZ1") T("S_MusicExists", "NOSUCHMUSIC") T("S_MusicName")
	T("P_SetupLevelSky", 1) T("P_StartQuake", 5, 10, nil, nil) T("P_CreateFloorSpriteSlope", sec) T("P_PlayJingle", p, JT_1UP)
	-- player fields written
	p.rings = 99 p.score = 12345 p.lives = 3 p.pflags = p.pflags | PF_GODMODE p.powers[pw_shield] = SH_PITY p.powers[pw_underwater] = 100 p.thokitem = MT_THOK p.speed = 1234 mo.color = SKINCOLOR_BLUE mo.scale = FU
	P("pwrite", p.rings, p.score, p.lives, p.pflags & PF_GODMODE, p.powers[pw_shield], p.powers[pw_underwater], p.thokitem == MT_THOK, p.speed, mo.color, mo.scale)
	local ok, e = pcall(function() p.nosuch = 1 end) P("pbad", ok, (tostring(e):gsub("^.-:%d+: ", "")))
	ok, e = pcall(function() mo.nosuch = 1 end) P("mbad", ok, (tostring(e):gsub("^.-:%d+: ", "")))
	ok, e = pcall(function() mo.type = 99999 end) P("mtype", ok, (tostring(e):gsub("^.-:%d+: ", "")))
	ok, e = pcall(function() mo.type = MT_RING mo.type = MT_PLAYER end) P("mtype2", ok)
	ok, e = pcall(function() sec.floorheight = sec.floorheight + FU sec.lightlevel = 100 sec.tag = 7 end) P("secw", ok, sec.floorheight, sec.lightlevel, sec.tag)
	ok, e = pcall(function() lines[0].special = 5 lines[0].flags = lines[0].flags | 1 end) P("linew", ok, lines[0].special)
	ok, e = pcall(function() sides[0].textureoffset = 5*FU sides[0].toptexture = 10 sides[0].offsetx_top = 3*FU end) P("sidew", ok, sides[0].textureoffset, sides[0].toptexture, sides[0].offsetx_top, sides[0].offsetx_mid)
	P("DONE_API")
end)
