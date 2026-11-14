-- Existing stock stereo 44.1 kHz effects at the normal pitch select step 2.0.
-- Use in both reference/candidate runs; no engine or asset changes are made.
local played = 0
addHook("MapLoad", function() played = 0 end)
addHook("ThinkFrame", function()
	local sound, name
	if leveltime == 10 or leveltime == 80 then
		sound, name = sfx_oneup, "DSONEUP"
	elseif leveltime == 40 or leveltime == 110 then
		sound, name = sfx_bowl, "DSBOWL"
	else
		return
	end
	S_StartSound(nil, sound)
	played = $ + 1
	print("NEXT5 STEREO SFX event="..played.." lump="..name.." tic="..leveltime)
end)
print("NEXT5 STEREO SFX FIXTURE LOADED")
