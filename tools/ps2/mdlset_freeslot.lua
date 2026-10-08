-- OPT11-MODEL: sprites added by Lua (freeslot) while gr_models is on: the model tables of the engine (md2_models, md2_playermodels) are sized for them; the scene of the models must
-- look as without this file (compare the pictures).   --addon tools/ps2/mdlset_freeslot.lua,tools/ps2/mdlscene.lua
for i = 1, 60 do
	freeslot("SPR_MZ" .. string.format("%02d", i))
end
