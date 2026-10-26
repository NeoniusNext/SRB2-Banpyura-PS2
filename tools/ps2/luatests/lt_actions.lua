-- OPT14: overriding the C actions of states with Lua functions (A_Look, A_Chase, A_Fall... : `function A_Chase(actor, var1, var2) ... end`), calling the hardcoded one from inside
-- (the "super" stack), two overrides of the same action (the second wraps the first), and actions set on states of the mod. A demo plays, the calls are counted with a digest.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local cnt, acc = {}, {}
local function note(name, actor, a, b)
	cnt[name] = (cnt[name] or 0) + 1
	local h = acc[name] or 17
	h = (h * 31 + (actor.type or 0) + ((actor.x >> 16) & 0xFFFF) * 3 + ((actor.y >> 16) & 0xFFFF) * 5 + (a or 0) * 7 + (b or 0) * 11) % 1000003
	acc[name] = h
end
-- first override: counts and calls the original
local orig_chase = A_Chase
function A_Chase(actor, var1, var2)
	note("chase1", actor, var1, var2)
	orig_chase(actor, var1, var2)
end
-- a second override of the same action: wraps the first
local first_chase = A_Chase
function A_Chase(actor, var1, var2)
	note("chase2", actor, var1, var2)
	first_chase(actor, var1, var2)
end
function A_Look(actor, var1, var2)
	note("look", actor, var1, var2)
	super(actor, var1, var2)
end
local S_LM = freeslot("S_LMACT1")
local S_LM2 = freeslot("S_LMACT2")
local MT_LM = freeslot("MT_LMACT")
states[S_LM] = {sprite = SPR_THOK, frame = A, tics = 2, nextstate = S_LM2, action = A_Look, var1 = 3, var2 = 4}
states[S_LM2] = {sprite = SPR_THOK, frame = A, tics = 2, nextstate = S_LM, action = A_Chase, var1 = 1, var2 = 2}
mobjinfo[MT_LM] = {doomednum = -1, spawnstate = S_LM, spawnhealth = 1, radius = 8*FRACUNIT, height = 16*FRACUNIT, mass = 1, flags = MF_NOGRAVITY|MF_NOBLOCKMAP|MF_SCENERY}
P("action_names", type(A_Look), type(A_Chase), type(A_Fall), type(A_SetObjectFlags), type(A_RandomState))
local done = false
addHook("ThinkFrame", function()
	if leveltime == 10 then
		for i = 1, 3 do
			local m = P_SpawnMobj(players[0].mo.x + i * 64 * FRACUNIT, players[0].mo.y, players[0].mo.z, MT_LM)
			m.angle = i * ANG30
		end
	end
	if leveltime % 200 == 0 and leveltime > 0 then
		local names = {}
		for k in pairs(cnt) do names[#names + 1] = k end
		table.sort(names)
		local parts = {}
		for _, k in ipairs(names) do parts[#parts + 1] = k .. "=" .. cnt[k] .. "/" .. acc[k] end
		P("tic", leveltime, table.concat(parts, " "))
		if leveltime >= 800 and not done then done = true P("DONE_ACTIONS") end
	end
end)
