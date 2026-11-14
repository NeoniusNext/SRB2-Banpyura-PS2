-- Integration fixture only: exercise all four cropped-patch lookup modes.
-- Load the same file in the frozen and candidate engines with -singletics.
hud.add(function(v, player)
	local patch = v.cachePatch("STTNUM0")
	local cmap = v.getColormap(TC_DEFAULT, SKINCOLOR_RED)
	local fu = FRACUNIT
	local w = (patch.width - 1)*fu
	local h = (patch.height - 1)*fu
	v.drawCropped(18*fu, 150*fu, 2*fu, 2*fu, patch, 0, nil, fu, fu, w, h)
	v.drawCropped(44*fu, 150*fu, 2*fu, 2*fu, patch, 0, cmap, fu, fu, w, h)
	v.drawCropped(70*fu, 150*fu, 2*fu, 2*fu, patch, V_50TRANS, nil, fu, fu, w, h)
	v.drawCropped(96*fu, 150*fu, 2*fu, 2*fu, patch, V_50TRANS, cmap, fu, fu, w, h)
	v.drawString(18, 183, "NEXT2 CROP CHECK", V_ALLOWLOWERCASE, "small")
end, "game")
print("NEXT2 CROPPED HUD READY")
