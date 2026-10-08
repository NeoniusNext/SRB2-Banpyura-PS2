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

// PS2-HW-251 (OPT11 round 3, FX3): PS2HWD_QuadHidden for a quad that is a parallelogram P0, P0 + R, P0 + R + U, P0 + U (a sprite, a drop shadow: the corners of both are
// affine images of a rectangle). The clip coordinates of the corners are C0 + (0 | dR) + (0 | dU) from the rows of the transform (cs), 4 x 3 products for the three
// differences instead of the 4 x 4 x 3 of xform_vertex, and the division only for the corners that are looked at. The answer is that of PS2HWD_QuadHidden (the same tests in
// the same coordinates: a corner near the eye = not hidden, all corners outside one plane = hidden, a box of more than 8 pixels = not hidden, a box without a pixel centre =
// hidden) up to the rounding of the floats, which cannot change a picture: the box test is widened by 0.1 pixel, the snapping of the GS is 1/16.
int PS2HWD_ParaHidden(const ps2cull_t *cs, const float *p0, const float *r, const float *u)
{
	float c[4][4], w2 = cs->w2; // [corner][x, y, z, w]
	float hx, hy, ox, oy, x0 = 1.0e30f, x1 = -1.0e30f, y0 = 1.0e30f, y1 = -1.0e30f;
	int k, and_ = 0x3F;

	for (k = 0; k < 4; k++)
	{
		const float *a = cs->r[k];
		const float b = a[0] * p0[0] + a[1] * p0[1] + a[2] * p0[2] + a[3];
		const float dr = a[0] * r[0] + a[1] * r[1] + a[2] * r[2];
		const float du = a[0] * u[0] + a[1] * u[1] + a[2] * u[2];

		c[0][k] = b;
		c[1][k] = b + dr;
		c[3][k] = b + du;
		c[2][k] = b + dr + du;
	}
	hx = H.vpw * 0.5f;
	hy = H.vph * 0.5f;
	ox = H.vpx + hx - 0.5f;
	oy = H.vpy + hy - 0.5f;
	for (k = 0; k < 4; k++)
	{
		const float x = c[k][0], y = c[k][1], z = c[k][2], w = c[k][3];
		float iw, sx, sy;

		if (w < w2)
			return 0; // near the eye (or behind it): the clipper decides
		and_ &= (x < -w ? 1 : 0) | (x > w ? 2 : 0) | (y < -w ? 4 : 0) | (y > w ? 8 : 0) | (z < -w ? 16 : 0) | (z > w ? 32 : 0);
		iw = 1.0f / w;
		sx = ox + x * iw * hx;
		sy = oy - y * iw * hy;
		x0 = sx < x0 ? sx : x0;
		x1 = sx > x1 ? sx : x1;
		y0 = sy < y0 ? sy : y0;
		y1 = sy > y1 ? sy : y1;
	}
	if (and_)
		return 1; // wholly outside the view volume
	if (x1 - x0 > 8.0f || y1 - y0 > 8.0f)
		return 0; // big: it covers a centre for sure
	return quad_no_centre(x0, x1, y0, y1) ? 1 : 0;
}
