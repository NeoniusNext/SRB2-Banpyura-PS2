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
#define HWFR_NOSEGCULL 0x2 // PS2-HW-402: walls not culled (the azimuth arc of the seg against the arc of the view)
#define HWFR_NOPLANECULL 0x4 // PS2-HW-400: planes (floors, ceilings, 3D floors) not culled
#define HWFR_NOLEAFCULL 0x8 // PS2-HW-401: no test of the whole subsector (the box of its planes) before the planes are looked at one by one
#define HWFR_NOSPRBATCH 0x40 // PS2-HW-403: the sprite polygons of the sprite batch go through the general HWR_ProcessPolygon (hw_batching.c) again
#define HWFR_NOINLINEZ 0x80 // PS2-HW-404: the four P_GetSector*ZAt calls of HWR_Subsector for a sector without slopes again
#define HWFR_NOSPRUNSORT 0x800 // PS2-HW-408: the opaque sprites are sorted far to near again (HWR_SortVisSprites)

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
hwr_fbox_t hwr_fbox[5]; // OPT13 IR
int hwr_fbox_on;

#define FR_MARGIN 1.0f
#define FR_GATE() (frv.on && !(hwr_fr_off & HWFR_NOCULL))

static struct { unsigned int ptest, phit, stest, shit, ltest, lhit, aout, aexempt; } frs; // planes and segs looked at / not built (per window, HWPROF62)

static void HWR_FrArc(void);

// the volume of the view the driver has now (HWR_FrViewSetup is called before the BSP walk of every view, after the transform was handed to the driver)
static void HWR_FrViewSetup(void)
{
	const ps2cull_t *cs;
	int k;

	frv.on = 0;
	hwr_fbox_on = 0;
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
	HWR_FrArc();
	// OPT13 IR (hw_pbatch.inc, HWR_PBBoxOut): the same planes for the integer test of the box of a stored polygon. The order is that of the test: the planes that say "outside" most often first.
	// The coefficients are rounded to 1/16384, a box coordinate is a whole number of map units of at most 32767: every product is at most 2^29 and the sum 1.6e9 (no overflow), and the rounding of
	// the three coefficients moves the distance by at most 3.1 units: the margin of the test is the margin of the floats (1) and 4 more.
	{
		static const signed char ord[5] = {0, 1, 2, 3, 4}; // right, left, top, bottom, near (in a run of the demos the side planes say "outside" far more often than the others)
		int j;

		for (j = 0; j < 5; j++)
		{
			const fr_plane_t *pl = &frv.p[(int)ord[j]];
			float nd = -(pl->d - FR_MARGIN - 4.0f) * 16384.0f;

			hwr_fbox[j].a = (INT32)(pl->a * 16384.0f + (pl->a < 0.0f ? -0.5f : 0.5f));
			hwr_fbox[j].b = (INT32)(pl->b * 16384.0f + (pl->b < 0.0f ? -0.5f : 0.5f));
			hwr_fbox[j].c = (INT32)(pl->c * 16384.0f + (pl->c < 0.0f ? -0.5f : 0.5f));
			if (nd > 2.0e9f) nd = 2.0e9f;
			if (nd < -2.0e9f) nd = -2.0e9f;
			hwr_fbox[j].nd = (INT32)nd;
			hwr_fbox[j].ox = pl->a > 0.0f ? 0u : 1u; // x: the min when the plane's x is positive (the most inside corner)
			hwr_fbox[j].oy = pl->b > 0.0f ? 2u : 3u;
			hwr_fbox[j].oz = pl->c > 0.0f ? 4u : 5u;
		}
		hwr_fbox_on = 1;
	}
}

// -hwpolyhash 3 (hw_batching.c): the polygons wholly outside the view volume (strictly, no margin) are left out of the hash: two runs of the same frames whose polygons differ only by what the
// culls above do not build give the same hash (the test of PS2-HW-400)
int HWR_FrPolyOutside(const FOutVector *v, unsigned int n)
{
	// exact: the polygon is clipped by the five planes (Sutherland-Hodgman) and nothing is left of it. (A polygon with some corners behind the near plane and some beside the side planes is wholly
	// outside the volume but no single plane has all its corners: the outcode test of the driver draws it, clips it away.)
	float a[160][3], b[160][3];
	unsigned int cnt = n, k, i;
	float (*in)[3] = a, (*out)[3] = b;

	if (!frv.on)
		return 0;
	if (n > 64)
		return 0;
	for (i = 0; i < n; i++)
	{
		a[i][0] = v[i].x;
		a[i][1] = v[i].y;
		a[i][2] = v[i].z;
	}
	for (k = 0; k < 5 && cnt; k++)
	{
		const fr_plane_t *pl = &frv.p[k];
		unsigned int m = 0;

		for (i = 0; i < cnt; i++)
		{
			const float *p = in[i], *q = in[(i + 1) % cnt];
			const float dp = pl->a * p[0] + pl->b * p[1] + pl->c * p[2] + pl->d, dq = pl->a * q[0] + pl->b * q[1] + pl->c * q[2] + pl->d;

			if (dp <= 0.0f)
			{
				out[m][0] = p[0]; out[m][1] = p[1]; out[m][2] = p[2];
				m++;
			}
			if ((dp <= 0.0f) != (dq <= 0.0f))
			{
				const float t = dp / (dp - dq);

				out[m][0] = p[0] + (q[0] - p[0]) * t;
				out[m][1] = p[1] + (q[1] - p[1]) * t;
				out[m][2] = p[2] + (q[2] - p[2]) * t;
				m++;
			}
			if (m > 150)
				return 0;
		}
		{
			float (*t)[3] = in;

			in = out;
			out = t;
		}
		cnt = m;
	}
	return cnt == 0;
}

extern UINT32 HWR_PHFrameNo(void);
#define FR_DBG() (hwr_fr_dbg_hi && HWR_PHFrameNo() >= hwr_fr_dbg_lo && HWR_PHFrameNo() <= hwr_fr_dbg_hi)

// ---- the azimuth arc of the view volume (PS2-HW-402) ---------------------------------------------------------------------------------------------------------------------
// A wall stands over its seg: every point of it has the azimuth of the point of the seg below it, so a seg whose azimuth arc (the angles AddLine has already made for the clipper) does not touch the arc of
// the view volume has no wall on the screen, whatever the heights are. The arc is that of the four edges of the pyramid (the cross products of the neighbouring side planes, with the pitch and the roll of the
// view in them), widened by 2 degrees; a view that looks along the vertical axis (up or down is inside the volume) has all azimuths. A few integer operations, the angles are known.
static struct { angle_t fl, fspan; int on; } fra;

static void HWR_FrArc(void)
{
	static const signed char pr[4][4] = {{0, 2, 1, 3}, {2, 1, 0, 3}, {1, 3, 0, 2}, {3, 0, 1, 2}}; // the two planes of an edge (right/top, top/left, left/bottom, bottom/right), the other two side planes
	float az[4], cf, lo = 1e9f, hi = -1e9f;
	int k;
	const float K = 683565275.5764f; // 2^32 / (2 pi)

	fra.on = 0;
	if ((frv.p[0].b <= 0.0f && frv.p[1].b <= 0.0f && frv.p[2].b <= 0.0f && frv.p[3].b <= 0.0f) || (frv.p[0].b >= 0.0f && frv.p[1].b >= 0.0f && frv.p[2].b >= 0.0f && frv.p[3].b >= 0.0f))
		return; // up or down is inside the volume
	for (k = 0; k < 4; k++)
	{
		const fr_plane_t *a = &frv.p[pr[k][0]], *b = &frv.p[pr[k][1]], *c = &frv.p[pr[k][2]], *d = &frv.p[pr[k][3]], *nn = &frv.p[4];
		float rx = a->b * b->c - a->c * b->b, rh = a->c * b->a - a->a * b->c, ry = a->a * b->b - a->b * b->a;
		float dc = rx * c->a + rh * c->b + ry * c->c, dd = rx * d->a + rh * d->b + ry * d->c;

		if (dc > 0.0f && dd > 0.0f)
		{
			rx = -rx; rh = -rh; ry = -ry; dc = -dc; dd = -dd;
		}
		if (dc > 1.0e-6f || dd > 1.0e-6f || rx * nn->a + rh * nn->b + ry * nn->c > 0.0f)
			return; // not an edge ray of the pyramid (degenerate view)
		if (rx * rx + ry * ry < 1.0e-12f)
			return;
		az[k] = atan2f(ry, rx);
	}
	{
		const fr_plane_t *nn = &frv.p[4];

		if (nn->a * nn->a + nn->c * nn->c < 1.0e-6f)
			return;
		cf = atan2f(-nn->c, -nn->a); // the direction of the view (minus the normal of the near plane that points back)
	}
	for (k = 0; k < 4; k++)
	{
		float dl = az[k] - cf;

		while (dl > 3.14159265f)
			dl -= 6.28318531f;
		while (dl < -3.14159265f)
			dl += 6.28318531f;
		if (dl < lo) lo = dl;
		if (dl > hi) hi = dl;
	}
	if (hi - lo > 3.0f)
		return; // wider than a half circle: no use
	lo -= 0.035f;
	hi += 0.035f;
	fra.fl = (angle_t)(INT32)(INT64)((double)(cf + lo) * (double)K);
	fra.fspan = (angle_t)(UINT32)((double)(hi - lo) * (double)K);
	fra.on = 1;
}

// the arc of a seg (from a2 counter clockwise to a1, less than a half circle) does not touch the arc of the view
static inline int HWR_FrArcOut(angle_t a2, angle_t a1)
{
	return fra.on && (angle_t)(a2 - fra.fl) > fra.fspan && (angle_t)(fra.fl - a2) > (angle_t)(a1 - a2);
}

static inline void HWR_FrViewEnd(void)
{
	frv.on = 0;
	hwr_fbox_on = 0;
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

// The prism over the points pts[0..n) between the heights lo and hi (all the planes of a subsector lie in it) is outside one side of the volume
static inline int HWR_FrPrismHidden(const polyvertex_t *pts, int n, float lo, float hi)
{
	int k;

	for (k = 0; k < 5; k++)
	{
		const fr_plane_t *pl = &frv.p[k];
		const float dl = pl->b * lo + pl->d, dh = pl->b * hi + pl->d;
		int i;

		for (i = 0; i < n; i++)
		{
			const float q = pl->a * pts[i].x + pl->c * pts[i].y;

			if (q + dl <= FR_MARGIN || q + dh <= FR_MARGIN)
				goto inside;
		}
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
	I_OutputMsg("HWPROF62 cull: planes tested=%u culled=%u | segs tested=%u culled=%u | subsectors tested=%u culled=%u | arc out=%u exempt=%u\n", frs.ptest / frames, frs.phit / frames, frs.stest / frames, frs.shit / frames, frs.ltest / frames, frs.lhit / frames, frs.aout / frames, frs.aexempt / frames);
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
	I_OutputMsg("HWPROF62 cull: planes tested=%u culled=%u | segs tested=%u culled=%u | subsectors tested=%u culled=%u | arc out=%u exempt=%u\n", frs.ptest / frames, frs.phit / frames, frs.stest / frames, frs.shit / frames, frs.ltest / frames, frs.lhit / frames, frs.aout / frames, frs.aexempt / frames);
	memset(&frs, 0, sizeof frs);
}
#endif // PS2_HWDETAIL

#endif // PS2_PROFILE
