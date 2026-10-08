-- OPT11-MODEL: variant of tools/ps2/mdlscene.lua (load it BEFORE): the characters of the base game with different skin colours and animations, frozen in the same pose.
-- fx_pair.py --addon tools/ps2/mdlset_players.lua,tools/ps2/mdlscene.lua
rawset(_G, "MDL_SET", {"PLAY", "PLAY", "PLAY", "PLAY", "PLAY", "PLAY", "PLAY", "PLAY", "PLAY", "PLAY"})
local skins = {"sonic", "tails", "knuckles", "amy", "fang", "metalsonic", "sonic", "tails", "knuckles", "amy"}
local colors = {SKINCOLOR_BLUE, SKINCOLOR_ORANGE, SKINCOLOR_RED, SKINCOLOR_ROSY, SKINCOLOR_GREY, SKINCOLOR_COBALT, SKINCOLOR_GREEN, SKINCOLOR_PURPLE, SKINCOLOR_YELLOW, SKINCOLOR_SAPPHIRE}
local anims = {SPR2_STND, SPR2_WALK, SPR2_RUN_, SPR2_ROLL, SPR2_FLY_, SPR2_STND, SPR2_WALK, SPR2_RUN_, SPR2_ROLL, SPR2_STND}
rawset(_G, "MDL_FX", function(mo, n)
	local i = n + 1
	mo.skin = skins[i]
	mo.color = colors[i]
	mo.sprite = SPR_PLAY
	mo.sprite2 = anims[i]
	mo.frame = (n % 4)
end)
