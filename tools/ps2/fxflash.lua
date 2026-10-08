-- OPT11-FX: palette flashes at fixed level times (the same on the PC engine and on the PS2): tools/ps2/fx_pair.py --addon tools/ps2/fxflash.lua --shot 'k150,k210,k270,k330' --all
addHook("ThinkFrame", function()
	local p = players[0]
	if not (p and p.valid) then return end
	if leveltime == 140 then P_FlashPal(p, PAL_WHITE, 40)
	elseif leveltime == 200 then P_FlashPal(p, PAL_NUKE, 40)
	elseif leveltime == 260 then P_FlashPal(p, PAL_INVERT, 40)
	end
end)
