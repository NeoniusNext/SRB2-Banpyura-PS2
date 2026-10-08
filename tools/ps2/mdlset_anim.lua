-- OPT11-MODEL: objects whose frames advance by themselves (rings, a Crawla's deterministic walk is not used: AI), to check gr_modelinterpolation between the tics.
-- fx_pair.py --addon tools/ps2/mdlset_anim.lua,tools/ps2/mdlscene.lua --shot 'k300,k301,k302,k303,k304,k305' --all   (the PC and the PS2 pictures of the same tic are compared)
rawset(_G, "MDL_ANIM", true)
rawset(_G, "MDL_SET", {"RING", "RING", "RING", "RING", "RING", "RING", "RING", "RING", "RING", "RING"})
