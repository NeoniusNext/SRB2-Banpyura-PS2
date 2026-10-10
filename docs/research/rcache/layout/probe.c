/* OPT13-RCACHE: one object with debug info for every hot engine structure, compiled with the EE compiler and the engine flags (layout.sh); gdb "ptype /o" lists offsets and holes. */
#include "doomdef.h"
#include "doomstat.h"
#include "p_mobj.h"
#include "r_defs.h"
#include "r_things.h"
#include "r_plane.h"
#include "r_bsp.h"
#include "r_draw.h"
#include "d_player.h"
#include "p_local.h"
#include "p_spec.h"
#include "w_wad.h"
#include "hardware/hw_defs.h"
#include "hardware/hw_glob.h"
#include "hardware/hw_data.h"
#include "hardware/hw_batching.h"
#include "hardware/hw_drv.h"

mobj_t g_mobj; precipmobj_t g_precip; sector_t g_sector; line_t g_line; side_t g_side; seg_t g_seg; subsector_t g_ss; node_t g_node;
vertex_t g_vertex; vissprite_t g_vis; drawseg_t g_drawseg; visplane_t g_visplane; player_t g_player; msecnode_t g_msec; ffloor_t g_ffloor;
mapthing_t g_mthing; thinker_t g_thinker; spriteframe_t g_sf; spritedef_t g_sd; state_t g_state; mobjinfo_t g_mi;
polyvertex_t g_pv; poly_t g_poly; extrasubsector_t g_ess; gl_vissprite_t g_glvis; FOutVector g_fov; FSurfaceInfo g_fsi; PolygonArrayEntry g_pae;
GLMipmap_t g_mip; GLPatch_t g_glp; patch_t g_patch; texture_t g_tex; lumpinfo_t g_lump; pslope_t g_slope;
