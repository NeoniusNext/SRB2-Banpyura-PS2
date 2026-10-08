// PS2 GS hardware renderer driver: what OPT11 round 2 (FX2) needs of the driver for the sprite paths (included by ps2_hwd.c after ps2_hw_plan.inc).

#include "ps2_hw_fx2.h"

int ps2hwd_fx2;

// PS2-HW-240: the rows of the clip transform and the lengths of the gradients of the four side planes, for the sphere test of the things (hw_main.c, HWR_AddSprites):
// the rest of the work of HWR_ProjectSprite (the frame and the rotation of the sprite, its offsets, the quad, PS2HWD_QuadHidden) is not done for a sprite that a
// sphere around it proves to be outside the view. Made again when the matrix changes (the view of the sky box, the main view: xf_ver).
const ps2cull_t *PS2HWD_CullSetup(void)
{
	static ps2cull_t cs;
	static u32 cs_ver = 0xFFFFFFFFu;
	static float cs_near;

	xf_update();
	if (cs_ver != xf_ver || cs_near != H.near_plane)
	{
		const float *m = H.mvp;
		int k;

		for (k = 0; k < 4; k++)
		{
			cs.r[k][0] = m[0 * 4 + k];
			cs.r[k][1] = m[1 * 4 + k];
			cs.r[k][2] = m[2 * 4 + k];
			cs.r[k][3] = m[3 * 4 + k];
		}
#define FX_GRAD(a, b, sg) sqrtf((cs.r[a][0] sg cs.r[b][0]) * (cs.r[a][0] sg cs.r[b][0]) + (cs.r[a][1] sg cs.r[b][1]) * (cs.r[a][1] sg cs.r[b][1]) + (cs.r[a][2] sg cs.r[b][2]) * (cs.r[a][2] sg cs.r[b][2]))
		cs.nxm = FX_GRAD(0, 3, -);
		cs.nxp = FX_GRAD(0, 3, +);
		cs.nym = FX_GRAD(1, 3, -);
		cs.nyp = FX_GRAD(1, 3, +);
		cs.nzp = FX_GRAD(2, 3, +);
#undef FX_GRAD
		cs.nw = sqrtf(cs.r[3][0] * cs.r[3][0] + cs.r[3][1] * cs.r[3][1] + cs.r[3][2] * cs.r[3][2]);
		cs.w2 = 2.0f * H.near_plane;
		cs_ver = xf_ver;
		cs_near = H.near_plane;
	}
	// (the conditions of PS2HWD_QuadHidden that make it answer "not hidden" without looking at the quad)
	cs.valid = H.up && H.vpw >= 1.0f && H.vph >= 1.0f && !(ps2hwd_dbg_flags & HWDBG_NOPLAN);
	return &cs;
}
