// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_front.inc
/// \brief OPT12 HWFRONT (PS2-HW-400..439): the engine side of the hardware renderer, structural work. Included by hw_main.c (PS2 profile only).
///
/// -hwfr N (hwr_fr_off): a set bit switches one OPT12 front path OFF (A/B on one ELF).

#ifdef PS2_PROFILE

// -hwfr N: a set bit switches one OPT12 front path OFF (A/B on one ELF)
#define HWFR_NOCULL 0x1 // PS2-HW-400: every cull before building (walls, planes, translucent planes) off
#define HWFR_NOSEGCULL 0x2 // PS2-HW-400: walls not culled
#define HWFR_NOPLANECULL 0x4 // PS2-HW-400: planes (floors, ceilings, 3D floors) not culled

unsigned int hwr_fr_off; // -hwfr: OPT12 front paths switched off (see the HWFR_NO* bits)
static UINT32 hwr_fr_dbg_lo = 1, hwr_fr_dbg_hi; // -hwfrdbg LO HI: the segs and planes the culls leave out in the frames LO..HI (the numbers of the HWPH lines) are printed (HWFRC lines)

static void HWR_FrInit(void)
{
	if (M_CheckParm("-hwfr") && M_IsNextParm())
		hwr_fr_off = (unsigned int)strtoul(M_GetNextParm(), NULL, 0);
	if (M_CheckParm("-hwfrdbg") && M_IsNextParm())
	{
		hwr_fr_dbg_lo = (UINT32)atoi(M_GetNextParm());
		hwr_fr_dbg_hi = M_IsNextParm() ? (UINT32)atoi(M_GetNextParm()) : hwr_fr_dbg_lo;
	}
}

// ---- PS2-HW-400: nothing that cannot reach the screen is built -----------------------------------------------------------------------------------------------
// The BSP walk hands the driver every wall and plane of the subsectors its angle clipper lets through. The clipper works with gld_FrustumAngle, a "gross hack that barely works"
// (hw_clip.c: about 88 degrees to either side of the view direction for the 90 degree view), and knows nothing of the heights: the driver then threw half of the polygons of a
// frame away (D4: 58 % of the planes, 40 % of the walls; D1 20 %). A wall or a plane is not built when all of its corners lie outside one side plane of the view volume (or
// behind the near plane): the corners of a polygon made of them lie there too, and a convex polygon outside one plane of the volume has no pixel on the screen. The test is
// exact (a plane of the clip volume of the driver's own matrix, with a margin of one map unit), the walk, the clipper and the sprites are not touched.
typedef struct { float a, b, c, d; } fr_plane_t; // distance (map units, > 0 outside) of the point (x, height, y): a * x + b * height + c * y + d

static struct
{
	fr_plane_t p[5]; // right, left, top, bottom, near
	int on;
} frv;

#define FR_MARGIN 1.0f
#define FR_GATE() (frv.on && !(hwr_fr_off & HWFR_NOCULL))

static struct { unsigned int ptest, phit, stest, shit; } frs; // planes and segs looked at / not built (per window, HWPROF62)

// the volume of the view the driver has now (HWR_FrViewSetup is called before the BSP walk of every view, after the transform was handed to the driver)
static void HWR_FrViewSetup(void)
{
	const ps2cull_t *cs;
	int k;

	frv.on = 0;
	cs = PS2HWD_CullSetup();
	if (!cs->valid)
		return;
	for (k = 0; k < 5; k++)
	{
		// c[row] = r[row][0] * X + r[row][1] * Y + r[row][2] * Z + r[row][3] (X, Y, Z: x, height, y of FOutVector); outside the volume: x > w, x < -w, y > w, y < -w, z < -w
		static const signed char row[5] = {0, 0, 1, 1, 2}, sgn[5] = {-1, 1, -1, 1, 1}; // (the plane is  sg * (r[row] + r[3] * sgn))  with sg = +1 for the "greater than w" sides and -1 for the others
		const float sg = (k == 0 || k == 2) ? 1.0f : -1.0f;
		float n[4], len;
		int j;

		for (j = 0; j < 4; j++)
			n[j] = sg * (cs->r[row[k]][j] + (float)sgn[k] * cs->r[3][j]);
		len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (!(len > 1e-9f))
			return;
		len = 1.0f / len;
		frv.p[k].a = n[0] * len;
		frv.p[k].b = n[1] * len;
		frv.p[k].c = n[2] * len;
		frv.p[k].d = n[3] * len;
	}
	frv.on = 1;
}

// -hwpolyhash 3 (hw_batching.c): the polygons wholly outside the view volume (strictly, no margin) are left out of the hash: two runs of the same frames whose polygons differ only by what the
// culls above do not build give the same hash (the test of PS2-HW-400)
int HWR_FrPolyOutside(const FOutVector *v, unsigned int n)
{
	int k;

	if (!frv.on)
		return 0;
	for (k = 0; k < 5; k++)
	{
		const fr_plane_t *pl = &frv.p[k];
		unsigned int i;

		for (i = 0; i < n; i++)
			if (pl->a * v[i].x + pl->b * v[i].y + pl->c * v[i].z + pl->d <= 0.0f)
				goto inside;
		return 1;
inside:;
	}
	return 0;
}

extern UINT32 HWR_PHFrameNo(void);
#define FR_DBG() (hwr_fr_dbg_hi && HWR_PHFrameNo() >= hwr_fr_dbg_lo && HWR_PHFrameNo() <= hwr_fr_dbg_hi)

static inline void HWR_FrViewEnd(void)
{
	frv.on = 0;
}

// All the vertices of a plane polygon (the points pts[0..n), polyvertex_t, at the height h) are outside one side of the volume
static inline int HWR_FrPlaneHidden(const polyvertex_t *pts, int n, float h)
{
	int k;

	for (k = 0; k < 5; k++)
	{
		const fr_plane_t *pl = &frv.p[k];
		const float d0 = pl->b * h + pl->d;
		int i;

		for (i = 0; i < n; i++)
			if (pl->a * pts[i].x + pl->c * pts[i].y + d0 <= FR_MARGIN)
				goto inside;
		return 1;
inside:;
	}
	return 0;
}

// The four corners of the slab between the points (x1, y1), (x2, y2) of the map and the heights lo and hi are outside one side of the volume
static inline int HWR_FrSlabHidden(float x1, float y1, float x2, float y2, float lo, float hi)
{
	int k;

	for (k = 0; k < 5; k++)
	{
		const fr_plane_t *pl = &frv.p[k];
		const float p1 = pl->a * x1 + pl->c * y1 + pl->d, p2 = pl->a * x2 + pl->c * y2 + pl->d, bl = pl->b * lo, bh = pl->b * hi;

		if (p1 + bl <= FR_MARGIN || p1 + bh <= FR_MARGIN || p2 + bl <= FR_MARGIN || p2 + bh <= FR_MARGIN)
			continue;
		return 1;
	}
	return 0;
}

#ifdef PS2_HWDETAIL
// ---- census (--hwdetail builds): what the BSP walk hands to the driver, against the exact view volume -------------------------------------------------------
// src: 1 a wall (HWR_ProcessSeg), 2 a floor/ceiling (HWR_RenderPlane), 0 the rest (sky walls, FOF planes are 2), 3 a sprite batch polygon
enum { FRC_OTHER, FRC_WALL, FRC_PLANE, FRC_SPRITE, FRC_NSRC };
int hwr_fr_src;
unsigned int hwr_fr_subpolys, hwr_fr_subvis; // polygons of the subsector being walked (walls, planes) and those of them that can be seen
static struct
{
	unsigned int poly[FRC_NSRC], out[FRC_NSRC], outx[FRC_NSRC], outy[FRC_NSRC], outn[FRC_NSRC], outfar[FRC_NSRC], verts[FRC_NSRC];
	unsigned int segs, seg_polys, seg_vis, seg_allout, seg_nopoly;
	unsigned int planes, plane_polys, plane_vis, plane_allout, plane_nopoly;
	unsigned int subs, sub_vis, sub_allout, sub_nopoly;
	unsigned int cur_polys, cur_vis;
	unsigned int seg_cyc_vis, seg_cyc_out, seg_cyc_nopoly, plane_cyc_vis, plane_cyc_out, plane_cyc_nopoly;
	unsigned int addline_out_x; // segs that passed the clipper and the back face test but lie wholly outside a side plane (by their end points in plan view, at any height)
} frc;

void HWR_FrCensusPoly(const FOutVector *v, unsigned int n)
{
	const ps2cull_t *cs = PS2HWD_CullSetup();
	unsigned int i, and_ = 0x3F, src = (unsigned int)hwr_fr_src;

	if (!cs->valid)
		return;
	if (hwr_sprite_batch)
		src = FRC_SPRITE;
	for (i = 0; i < n; i++)
	{
		const float X = v[i].x, Y = v[i].y, Z = v[i].z;
		const float cx = cs->r[0][0] * X + cs->r[0][1] * Y + cs->r[0][2] * Z + cs->r[0][3];
		const float cy = cs->r[1][0] * X + cs->r[1][1] * Y + cs->r[1][2] * Z + cs->r[1][3];
		const float cz = cs->r[2][0] * X + cs->r[2][1] * Y + cs->r[2][2] * Z + cs->r[2][3];
		const float cw = cs->r[3][0] * X + cs->r[3][1] * Y + cs->r[3][2] * Z + cs->r[3][3];
		unsigned int oc = 0;

		if (cx > cw) oc |= 1;
		if (cx < -cw) oc |= 2;
		if (cy > cw) oc |= 4;
		if (cy < -cw) oc |= 8;
		if (cz < -cw) oc |= 16;
		if (cz > cw) oc |= 32;
		and_ &= oc;
	}
	frc.poly[src]++;
	frc.verts[src] += n;
	if (src != FRC_SPRITE)
	{
		frc.cur_polys++;
		hwr_fr_subpolys++;
	}
	if (and_)
	{
		frc.out[src]++;
		if (and_ & 3) frc.outx[src]++;
		if (and_ & 12) frc.outy[src]++;
		if (and_ & 16) frc.outn[src]++;
		if (and_ & 32) frc.outfar[src]++;
	}
	else if (src != FRC_SPRITE)
	{
		frc.cur_vis++;
		hwr_fr_subvis++;
	}
}

// the cycles of the next walls/planes/subsectors are split by whether anything of them can be seen
static inline void HWR_FrCensusBegin(void)
{
	frc.cur_polys = frc.cur_vis = 0;
}

static inline void HWR_FrCensusSeg(unsigned int cyc)
{
	frc.segs++;
	if (!frc.cur_polys) { frc.seg_nopoly++; frc.seg_cyc_nopoly += cyc; }
	else
	{
		frc.seg_polys++;
		if (frc.cur_vis) { frc.seg_vis++; frc.seg_cyc_vis += cyc; }
		else { frc.seg_allout++; frc.seg_cyc_out += cyc; }
	}
}

static inline void HWR_FrCensusPlane(unsigned int cyc)
{
	frc.planes++;
	if (!frc.cur_polys) { frc.plane_nopoly++; frc.plane_cyc_nopoly += cyc; }
	else
	{
		frc.plane_polys++;
		if (frc.cur_vis) { frc.plane_vis++; frc.plane_cyc_vis += cyc; }
		else { frc.plane_allout++; frc.plane_cyc_out += cyc; }
	}
}

static inline void HWR_FrCensusSub(void)
{
	frc.subs++;
	if (!hwr_fr_subpolys) frc.sub_nopoly++;
	else if (hwr_fr_subvis) frc.sub_vis++;
	else frc.sub_allout++;
}

void HWR_FrProf(unsigned int frames)
{
	static const char *const nm[FRC_NSRC] = {"other", "wall", "plane", "sprite"};
	int i;

	if (!frames)
		frames = 1;
	I_OutputMsg("HWPROF62 cull: planes tested=%u culled=%u | segs tested=%u culled=%u\n", frs.ptest / frames, frs.phit / frames, frs.stest / frames, frs.shit / frames);
	memset(&frs, 0, sizeof frs);
	for (i = 0; i < FRC_NSRC; i++)
		I_OutputMsg("HWPROF60 %s: polys=%u verts=%u out=%u (x=%u y=%u near=%u far=%u)\n", nm[i], frc.poly[i] / frames, frc.verts[i] / frames, frc.out[i] / frames, frc.outx[i] / frames, frc.outy[i] / frames, frc.outn[i] / frames, frc.outfar[i] / frames);
	I_OutputMsg("HWPROF61 segs=%u (nopoly=%u visible=%u allout=%u) cyc: nopoly=%u visible=%u allout=%u | planes=%u (nopoly=%u visible=%u allout=%u) cyc: nopoly=%u visible=%u allout=%u | subs=%u (nopoly=%u visible=%u allout=%u)\n",
		frc.segs / frames, frc.seg_nopoly / frames, frc.seg_vis / frames, frc.seg_allout / frames, frc.seg_cyc_nopoly / frames, frc.seg_cyc_vis / frames, frc.seg_cyc_out / frames,
		frc.planes / frames, frc.plane_nopoly / frames, frc.plane_vis / frames, frc.plane_allout / frames, frc.plane_cyc_nopoly / frames, frc.plane_cyc_vis / frames, frc.plane_cyc_out / frames,
		frc.subs / frames, frc.sub_nopoly / frames, frc.sub_vis / frames, frc.sub_allout / frames);
	memset(&frc, 0, sizeof frc);
}
#else
void HWR_FrProf(unsigned int frames)
{
	if (!frames)
		frames = 1;
	I_OutputMsg("HWPROF62 cull: planes tested=%u culled=%u | segs tested=%u culled=%u\n", frs.ptest / frames, frs.phit / frames, frs.stest / frames, frs.shit / frames);
	memset(&frs, 0, sizeof frs);
}
#endif // PS2_HWDETAIL

#endif // PS2_PROFILE
