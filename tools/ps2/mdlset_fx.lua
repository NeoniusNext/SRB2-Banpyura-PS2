-- OPT11-MODEL: variant of tools/ps2/mdlscene.lua (load it BEFORE): one model (the Crawla) with every blend mode / translucency / render flag / colour / flip / scale / roll,
-- as tools/ps2/fxscene.lua does it for sprites.   fx_pair.py --addon tools/ps2/mdlset_fx.lua,tools/ps2/mdlscene.lua
rawset(_G, "MDL_SET", {"POSS", "POSS", "POSS", "POSS", "POSS", "POSS", "POSS", "POSS", "POSS", "POSS", "POSS", "POSS", "POSS", "POSS", "POSS"})
local variants = {
	function(mo) end,
	function(mo) mo.frame = mo.frame | FF_TRANS50 end,
	function(mo) mo.frame = mo.frame | FF_TRANS20 end,
	function(mo) mo.blendmode = AST_ADD; mo.frame = mo.frame | FF_TRANS30 end,
	function(mo) mo.blendmode = AST_SUBTRACT; mo.frame = mo.frame | FF_TRANS30 end,
	function(mo) mo.blendmode = AST_REVERSESUBTRACT; mo.frame = mo.frame | FF_TRANS30 end,
	function(mo) mo.colorized = true; mo.color = SKINCOLOR_RED end,
	function(mo) mo.color = SKINCOLOR_GREEN end,
	function(mo) mo.renderflags = mo.renderflags | RF_FULLBRIGHT end,
	function(mo) mo.renderflags = mo.renderflags | RF_FULLDARK end,
	function(mo) mo.renderflags = mo.renderflags | RF_HORIZONTALFLIP end,
	function(mo) mo.renderflags = mo.renderflags | RF_VERTICALFLIP end,
	function(mo) mo.spritexscale = 2 * FRACUNIT; mo.spriteyscale = FRACUNIT / 2 end,
	function(mo) mo.rollangle = ANGLE_45 end,
	function(mo) mo.scale = FRACUNIT * 2 end,
}
rawset(_G, "MDL_FX", function(mo, n) variants[n + 1](mo) end)
