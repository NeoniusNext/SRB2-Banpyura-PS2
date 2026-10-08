// OPT11 round 2 (FX2): what the engine side of the hardware renderer (hardware/hw_main.c) shares with the driver for the sprite paths.
// Only for the PS2 profile build.

#ifndef __PS2_HW_FX2_H__
#define __PS2_HW_FX2_H__

#ifdef PS2_PROFILE

// -hwfx N (ps2hwd_fx2): a set bit switches one FX2 path OFF (A/B on one ELF)
#define FX2_NOPRE 0x1 // PS2-HW-240: the sphere test of the things before HWR_ProjectSprite
#define FX2_PRECHECK 0x2 // PS2-HW-240 check mode: the test runs but rejects nothing; a thing it would have rejected that gets a vissprite is reported (HWC pre MISMATCH)
#define FX2_NOSORTKEY 0x4 // PS2-HW-241: the sort keys of the sprites are read from the mobjs (as before)
#define FX2_NOSHADOW 0x8 // PS2-HW-242: the drop shadows as before
#define FX2_NOSKY 0x10 // PS2-HW-244: the sky dome without the sphere test of its quads
#define FX2_NOINTERP 0x40 // PS2-HW-245: the interpolated state of a thing is made as before (the full R_InterpolateMobjState, again for the shadow and the aim rotation)
#define FX2_NOWATER 0x100 // PS2-HW-246: the sky box view plans the water ripple as the main view does
#define FX2_NOVIS 0x400 // PS2-HW-248: R_ThingVisible for every thing (as before)
// OPT11 round 3 (FX3), measurement only: bit 13 (0x2000) draws no sprites and no shadows (HWR_DrawSprites does nothing): the difference of two runs is what the drawing costs
#define FX3_NODRAW 0x2000
#define FX3_NOLEAN 0x10000 // PS2-HW-253 (FX3): the lean paths of the sprite batch (planner call, collect, sort) are off, as before
#define FX3_NOFILL 0x8000 // PS2-HW-252 (FX3): the texels of a patch are stored by the fast loop only when the width is a multiple of 4 (as before)
#define FX3_NOPARA 0x4000 // PS2-HW-251 (FX3): PS2HWD_QuadHidden (four transforms) for the quads of the sprites and the shadows, as before
#define FX2_PTRORDER 0x1000 // PS2-HW-250 (FX3): the order of the patches in the batches by the hash of their address, as before (A/B)

extern int ps2hwd_fx2;

// The view of the driver as a bound for a sphere: the four clip coordinates of a point (X, Y, Z in the coordinates of FOutVector: x, height, y) are
//     c[k] = r[k][0] * X + r[k][1] * Y + r[k][2] * Z + r[k][3]     k = 0 x, 1 y, 2 (z unused), 3 w
// and the sphere of radius R around it lies wholly outside a side plane of the view volume when, for instance, (x - w)(c) - nxm * R > 0 (nxm = the length of the
// gradient of x - w, i.e. the largest change of x - w over a distance R). w2 = twice the near plane: the quad test of the driver (PS2HWD_QuadHidden) calls a quad
// "not hidden" whenever one of its corners is nearer than that to the eye, so a sphere that reaches it is not rejected.
typedef struct
{
	float r[4][4];
	float nxm; // |grad(x - w)|   right plane: x > w
	float nxp; // |grad(x + w)|   left plane: x < -w
	float nym; // |grad(y - w)|   top
	float nyp; // |grad(y + w)|   bottom
	float nw; // |grad(w)|
	float nzp; // |grad(z + w)|   near plane: z < -w (what the sky dome tests as well: a sphere wholly behind it draws nothing)
	float w2;
	int valid; // 0: the driver is not up (or the plan is switched off): nothing is rejected
} ps2cull_t;

extern const ps2cull_t *PS2HWD_CullSetup(void);
// PS2-HW-251 (FX3): PS2HWD_QuadHidden of the parallelogram P0, P0 + R, P0 + R + U, P0 + U (p0, r, u: x, height, y of FOutVector); 1 = it cannot put a pixel on the screen
extern int PS2HWD_ParaHidden(const ps2cull_t *cs, const float *p0, const float *r, const float *u);

#endif
#endif
