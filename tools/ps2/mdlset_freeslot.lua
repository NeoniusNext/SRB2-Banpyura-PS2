-- OPT11-MODEL: sprites added by Lua (freeslot) while gr_models is on: the model tables of the engine (md2_models, md2_playermodels) are made again and every model that was loaded
-- is given back first (HWR_GrowSpriteTables, PS2Models_FreeAll); the scene of the models must look as without this file.   --addon tools/ps2/mdlset_freeslot.lua,tools/ps2/mdlscene.lua
for i = 1, 60 do
	freeslot("SPR_MZ" .. string.format("%02d", i))
end
addHook("MapLoad", function()
	for i = 1, 60 do
		freeslot("SPR_MY" .. string.format("%02d", i)) -- more slots after the first level load: the tables are made again with models alive
	end
end)
