-- Lua equivalence: info tables (states, mobjinfo, sfxinfo, spriteinfo, skincolors), freeslots, constants and their limits.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function try(f, ...) local ok, r = pcall(f, ...) if ok then return r end local m = tostring(r):gsub("^.-:%d+: ", "") return "E:" .. m end

local S_FIRST, MT_FIRST, SPR_FIRST, SC_FIRST, SFX0 = freeslot("S_LQBASE"), freeslot("MT_LQBASE"), freeslot("SPR_LQB"), freeslot("SKINCOLOR_LQBASE"), freeslot("sfx_lqbase")
P("firsts", S_FIRST, MT_FIRST, SPR_FIRST, SC_FIRST, SFX0)
P("lens", #states, #mobjinfo, #sfxinfo, #spriteinfo, #sprnames, #skincolors, #skins, #spr2names)
-- reading entries of the tables, including indexes that no freeslot made yet but that exist on the PC (they are zero filled there)
for _, idx in ipairs({0, 1, 100, 2000, S_FIRST, S_FIRST + 10, S_FIRST + 400, S_FIRST + 4000, S_FIRST + 8000, 8191, 8192, 9000}) do
	P("state[" .. idx .. "]", try(function() local s = states[idx] return s.sprite .. "," .. s.frame .. "," .. s.tics .. "," .. s.nextstate end))
end
for _, idx in ipairs({0, 1, 50, MT_FIRST, MT_FIRST + 31, MT_FIRST + 32, MT_FIRST + 500, MT_FIRST + 1000, 2000, 2200}) do
	P("mobjinfo[" .. idx .. "]", try(function() local m = mobjinfo[idx] return m.doomednum .. "," .. m.spawnstate .. "," .. m.spawnhealth .. "," .. m.radius .. "," .. m.height .. "," .. m.flags end))
end
for _, idx in ipairs({0, 1, 100, SFX0, SFX0 + 100, SFX0 + 300, SFX0 + 1000, SFX0 + 1199, SFX0 + 1400, 1700}) do
	P("sfxinfo[" .. idx .. "]", try(function() local f = sfxinfo[idx] return f.singular .. "," .. f.priority .. "," .. f.flags .. "," .. f.caption end))
end
for _, idx in ipairs({0, 1, 100, SPR_FIRST, SPR_FIRST + 33, SPR_FIRST + 500, SPR_FIRST + 1000, 1500}) do
	P("spriteinfo[" .. idx .. "]", try(function() local f = spriteinfo[idx] return type(f) end))
	P("sprnames[" .. idx .. "]", try(function() return sprnames[idx] end))
end
for _, idx in ipairs({0, 1, 50, SC_FIRST, SC_FIRST + 40, SC_FIRST + 500, 1000, 1100}) do
	P("skincolors[" .. idx .. "]", try(function() local c = skincolors[idx] return c.name .. "," .. c.chatcolor .. "," .. c.ramp[0] .. "," .. c.invcolor end))
end
-- freeslot: allocation numbers, names -> constants, exhaustion
local r = {}
for i = 1, 40 do r[#r+1] = freeslot("MT_LQMOB" .. i) end
P("freeslot_mt", table.concat(r, ","))
r = {}
for i = 1, 300 do r[#r+1] = freeslot("S_LQSTATE" .. i) end
P("freeslot_s", r[1], r[2], r[100], r[300], #r)
P("consts", MT_LQMOB1, MT_LQMOB40, S_LQSTATE1, S_LQSTATE300, MT_FIRST, rawget(_G, "MT_LQMOB41"))
P("freeslot_multi", freeslot("SPR_LQS1", "SPR_LQS2", "S_LQMULTI", "MT_LQMULTI", "sfx_lqmulti", "SKINCOLOR_LQCOLOR", "SPR2_LQ00"))
P("sfxfree", freeslot("sfx_lqs1"), freeslot("sfx_lqs2"), sfx_lqs1, sfx_lqs2, sfx_lqmulti)
P("dup", freeslot("MT_LQMOB1"), freeslot("S_LQSTATE1"), freeslot("sfx_lqs1"))
-- the freed slots can be written and read back
mobjinfo[MT_LQMOB1].doomednum = 12345
mobjinfo[MT_LQMOB1].spawnstate = S_LQSTATE1
mobjinfo[MT_LQMOB1].spawnhealth = 77
mobjinfo[MT_LQMOB1].radius = 20*FRACUNIT
mobjinfo[MT_LQMOB1].flags = MF_SOLID|MF_SHOOTABLE
P("mobjinfo_rw", mobjinfo[MT_LQMOB1].doomednum, mobjinfo[MT_LQMOB1].spawnstate == S_LQSTATE1, mobjinfo[MT_LQMOB1].spawnhealth, mobjinfo[MT_LQMOB1].radius, mobjinfo[MT_LQMOB1].flags)
states[S_LQSTATE1].sprite = SPR_THOK states[S_LQSTATE1].frame = 3 states[S_LQSTATE1].tics = 9 states[S_LQSTATE1].nextstate = S_LQSTATE2
P("state_rw", states[S_LQSTATE1].sprite == SPR_THOK, states[S_LQSTATE1].frame, states[S_LQSTATE1].tics, states[S_LQSTATE1].nextstate == S_LQSTATE2)
P("state_err", try(function() states[S_LQSTATE1].sprite = 99999 end), try(function() states[S_LQSTATE1].nextstate = 9999999 end), try(function() mobjinfo[MT_LQMOB1].spawnstate = 99999 end), try(function() mobjinfo[MT_LQMOB1].seesound = 99999 end))
P("index_err", try(function() return states[-1] end), try(function() return states[99999] end), try(function() return mobjinfo[99999] end), try(function() return sfxinfo[99999] end), try(function() return sfxinfo[0] end), try(function() return spriteinfo[0] end), try(function() return spriteinfo[99999] end))
sfxinfo[sfx_lqs1].caption = "LQ CAPTION" sfxinfo[sfx_lqs1].priority = 33 sfxinfo[sfx_lqs1].singular = true
P("sfx_rw", sfxinfo[sfx_lqs1].caption, sfxinfo[sfx_lqs1].priority, sfxinfo[sfx_lqs1].singular)
skincolors[SKINCOLOR_LQCOLOR].name = "LQ Color" skincolors[SKINCOLOR_LQCOLOR].chatcolor = V_BLUEMAP skincolors[SKINCOLOR_LQCOLOR].ramp[3] = 77 skincolors[SKINCOLOR_LQCOLOR].accessible = true
P("color_rw", skincolors[SKINCOLOR_LQCOLOR].name, skincolors[SKINCOLOR_LQCOLOR].ramp[3], skincolors[SKINCOLOR_LQCOLOR].accessible, SKINCOLOR_LQCOLOR)
P("sprnames", sprnames[SPR_LQS1], sprnames[SPR_LQS2], sprnames[SPR_THOK], sprnames[SPR_PLAY], R_GetSpriteNumByName and 1 or 0)
P("lens_after", #states, #mobjinfo, #sfxinfo, #spriteinfo, #sprnames, #skincolors)
-- exhaustion: warnings are printed by the engine and compared as text as well
-- references held across the growth of the tables (PS2: the tables are moved when a script needs a slot past the small size; the userdata have to follow)
local hs, hm, hx, hi, hc = states[S_PLAY_STND], mobjinfo[MT_PLAYER], sfxinfo[sfx_jump], spriteinfo[SPR_PLAY], skincolors[SKINCOLOR_RED]
P("held_before", hs.tics, hm.speed, hx.priority, hc.name)
local last
for i = 1, 70 do last = freeslot("MT_LQXTRA" .. i) end
P("last_mt", last)
hs.tics = 77 hm.speed = 12345 hx.priority = 99 hc.name = "HeldRed"
P("held_after", states[S_PLAY_STND].tics, mobjinfo[MT_PLAYER].speed, sfxinfo[sfx_jump].priority, skincolors[SKINCOLOR_RED].name, hs.tics, hm.speed, hx.priority, hc.name)
states[S_PLAY_STND].tics = 5 mobjinfo[MT_PLAYER].speed = 6 sfxinfo[sfx_jump].priority = 7
P("held_back", hs.tics, hm.speed, hx.priority)
-- the action constants and hooks the info tables carry
P("actions", states[S_THOK].action ~= nil or "nil", try(function() return states[S_THOK].var1 end), try(function() return states[S_PLAY_STND].tics end))
P("mobjflags", MF_SOLID, MF_SHOOTABLE, MF_NOGRAVITY, MF_SPRING, MF_PAIN, MF2_OBJECTFLIP, MF_NOTHINK, MFE_UNDERWATER)
P("statecount_lookup", S_PLAY_STND, S_PLAY_WALK, MT_PLAYER, MT_THOK, MT_RING, MT_BLUECRAWLA, sfx_jump, sfx_pop, sfx_s3k33, SPR_PLAY, SPR_THOK, SPR2_STND)
P("DONE_INFO")
