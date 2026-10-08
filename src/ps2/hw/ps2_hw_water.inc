// PS2 GS hardware renderer driver: the water polygon in one sweep (included by ps2_hw_draw.inc in front of emit_water_poly). PS2-HW-47 (OPT10, HG).
//
// PS2-HW-63 cuts a PF_Ripple polygon at the eye depths of the ripple bands (a constant shift of the texture coordinates per band) and, inside
// every band, at the steps of the light staircase. The old code clipped the polygon band by band (two clip_attr passes over the rest of the polygon
// per band, then cut_and_emit's stages for every piece): 20 000 cycles for a water polygon, 11 % of a GFZ1 frame.
// Here the convex polygon is split along its two monotone chains (minimum to maximum eye depth) in one pass: every plane (ripple band edge or
// darkness step, merged in depth order) crosses each chain once, the crossing is interpolated from the lower end of its edge exactly as clip_attr
// does (both neighbouring pieces share the point), and the piece between two planes is the points of the two chains in between. Each piece goes
// to emit_piece with the band's texture shift and its darkness class (what cut_and_emit stage 2 finds for it).
//
// Deviation (PS2-HW-47): the ripple bands are 16 units deep up to z = 160, 32 units up to z = 320 and there is no ripple beyond (the old code:
// 16 units up to 640). The offset of the shader is at most 0.025 of the texture (1.6 texels of a 64 texel flat), a texel is below 0.25 pixel at
// z = 640 and about 0.5 pixel at z = 320 (320 pixel wide view, 90 degree field of view): the far bands moved the picture by less than half a pixel.

#define HWDBG_OLDWATER 0x40000000 // -hwdbg 1073741824 (0x10000000 is HT's IMMDBG): the old band by band water polygon (A/B)
#define HWDBG_BANDSWEEP 512 // -hwdbg 512: band_sweep (not cut_and_emit) makes the light staircase pieces of ordinary polygons. OFF by default: no gain (6 % of the cut polygons' cost,
// 0.01 M of 11 M) and its pieces differ from cut_and_emit by up to 1/16 pixel, which flips texels along the texel edges of a sharp texture: 2 % of the pixels of a GFZ1 frame
#define HWDBG_WATERPOL 0x20000000 // -hwdbg 536870912: the sweep with the old ripple bands (16 units up to 640): equivalence test against HWDBG_OLDWATER
#define WF_MAXV 24 // vertices of a polygon the sweep takes (more: the old code)
#define WF_NPLANE 80 // planes (ripple bands + darkness steps) per polygon
#define RIPPLE_MAXPLANES 40

static float ripple_planes[RIPPLE_MAXPLANES] = {16.0f, 32.0f, 48.0f, 64.0f, 80.0f, 96.0f, 112.0f, 128.0f, 144.0f, 160.0f, 192.0f, 224.0f, 256.0f, 288.0f, 320.0f};
static int ripple_nplanes = 15;
static float ripple_zmax2 = 320.0f;
static int ripple_pol_old = -1; // the policy the table was made for (0 = PS2-HW-47, 1 = old)

// PS2-HW-57: the shift of a band that lies between two table planes is the same for every polygon of the frame: made once per level time
static float rt_ds[RIPPLE_MAXPLANES + 2], rt_dt[RIPPLE_MAXPLANES + 2];
static u8 rt_ok[RIPPLE_MAXPLANES + 2];
static int rt_time = -1;

static void ripple_policy(int old)
{
	int i;

	ripple_pol_old = old;
	rt_time = -1;
	if (old)
	{
		for (i = 0; i < RIPPLE_MAXPLANES; i++)
			ripple_planes[i] = 16.0f * (float)(i + 1);
		ripple_nplanes = RIPPLE_MAXPLANES;
		ripple_zmax2 = RIPPLE_ZMAX;
	}
	else
	{
		static const float tab[15] = {16.0f, 32.0f, 48.0f, 64.0f, 80.0f, 96.0f, 112.0f, 128.0f, 144.0f, 160.0f, 192.0f, 224.0f, 256.0f, 288.0f, 320.0f};

		for (i = 0; i < 15; i++)
			ripple_planes[i] = tab[i];
		ripple_nplanes = 15;
		ripple_zmax2 = 320.0f;
	}
}

static cv_t wf_ca[WF_MAXV + WF_NPLANE + 2], wf_cb[WF_MAXV + WF_NPLANE + 2], wf_piece[2 * (WF_MAXV + 2) + 2];

// The chain of the polygon from vertex `from` to vertex `to` (step dir): its vertices and the crossings of the planes cw[0..m) (ascending,
// strictly inside the depth range). bidx[j] = index in out[] of the point on plane j. Returns the number of points or -1 (not monotone).
static int wf_chain(const cv_t *v, int cnt, int from, int to, int dir, const float *cw, int m, cv_t *out, int *bidx)
{
	int n = 0, pj = 0, i = from;

	out[n++] = v[from];
	while (i != to)
	{
		int nx = i + dir;
		const cv_t *a, *b;

		if (nx < 0)
			nx = cnt - 1;
		else if (nx >= cnt)
			nx = 0;
		a = &v[i];
		b = &v[nx];
		if (b->w < a->w)
			return -1;
		while (pj < m && cw[pj] <= a->w) // a plane the chain stands on already (a vertex at exactly that depth)
			bidx[pj++] = n - 1;
		while (pj < m && cw[pj] < b->w)
		{
			const float cut = cw[pj], t = (cut - a->w) / (b->w - a->w);
			cv_t *o = &out[n];

			*o = *a;
			o->x = a->x + (b->x - a->x) * t;
			o->y = a->y + (b->y - a->y) * t;
			o->z = a->z + (b->z - a->z) * t;
			o->w = cut;
			o->a[0] = a->a[0] + (b->a[0] - a->a[0]) * t;
			o->a[1] = a->a[1] + (b->a[1] - a->a[1]) * t;
			bidx[pj++] = n++;
		}
		out[n++] = *b;
		i = nx;
	}
	while (pj < m && cw[pj] <= out[n - 1].w) // planes at exactly the depth of the last vertex
		bidx[pj++] = n - 1;
	return pj == m ? n : -1;
}

// 1 when the polygon was drawn. ripple = 0: only the steps of the light staircase cut the polygon (PS2-HW-48: what cut_and_emit's stage 2 does, in one sweep)
static int band_sweep(const cv_t *v, int cnt, piece_t pc, int ripple)
{
	float zc[WF_NPLANE], cw[WF_NPLANE], rz[RIPPLE_MAXPLANES], th[WF_NPLANE], bds[RIPPLE_MAXPLANES + 1], bdt[RIPPLE_MAXPLANES + 1], z0, z1, wlo, whi;
	int icl[WF_NPLANE + 1], ib[WF_NPLANE + 1], cls[WF_NPLANE], bidxa[WF_NPLANE], bidxb[WF_NPLANE];
	int i, imin = 0, imax = 0, m = 0, nr = 0, nc = 0, na_, nb_, f = 0, j, cls0 = -1;

	if ((ripple ? (ps2hwd_dbg_flags & HWDBG_OLDWATER) != 0 : !(ps2hwd_dbg_flags & HWDBG_BANDSWEEP)) || cnt < 3 || cnt > WF_MAXV || P.cutx || P.cuty || P.lmode != LM_BANDS)
		return 0;
	if (ripple_pol_old != ((ps2hwd_dbg_flags & HWDBG_WATERPOL) != 0))
		ripple_policy((ps2hwd_dbg_flags & HWDBG_WATERPOL) != 0);
	wlo = whi = v[0].w;
	for (i = 1; i < cnt; i++)
	{
		if (v[i].w < wlo)
		{
			wlo = v[i].w;
			imin = i;
		}
		if (v[i].w > whi)
		{
			whi = v[i].w;
			imax = i;
		}
	}
	z0 = zfrag_of_w(wlo);
	z1 = zfrag_of_w(whi);
	// ripple planes strictly inside (z0, z1) and the light staircase steps
	if (!ripple)
		f = ripple_nplanes; /* no ripple planes: the single band beyond the last one has no shift */
	while (f < ripple_nplanes && ripple_planes[f] <= z0)
		f++;
	for (i = f; i < ripple_nplanes && ripple_planes[i] < z1; i++)
		rz[nr++] = ripple_planes[i];
	if (P.oldpath)
	{
		nc = doom_thresholds(&P.rs.lp, z0, z1, th, cls, WF_NPLANE, &cls0);
	}
	else
	{
		int c1;

		cls0 = lf_class_w(wlo); // PS2-HW-59: the plan's memory of classes and step depths (LM_BANDS: lit_fast_plan ran in begin_draw)
		c1 = lf_class_w(whi);
		nc = cls0 == c1 ? 0 : lf_thresholds(cls0, c1, z0, z1, th, cls, WF_NPLANE);
	}
	if (nr + nc > WF_NPLANE)
		return 0;
	// merge: interval j lies between plane j - 1 and plane j; its ripple band and darkness class
	{
		int ir = 0, ic = 0, band = 0, cur = cls0;

		ib[0] = 0;
		icl[0] = cls0;
		while (ir < nr || ic < nc)
		{
			const float zr = ir < nr ? rz[ir] : 1e30f, zt = ic < nc ? th[ic] : 1e30f;

			if (zr < zt)
			{
				zc[m] = zr;
				band++;
				ir++;
			}
			else if (zt < zr)
			{
				zc[m] = zt;
				cur = cls[ic];
				ic++;
			}
			else
			{
				zc[m] = zr;
				band++;
				cur = cls[ic];
				ir++;
				ic++;
			}
			m++;
			ib[m] = band;
			icl[m] = cur;
		}
	}
	for (j = 0; j < m; j++)
		cw[j] = w_of_zfrag(zc[j]);
	// the texture shift of every ripple band: the offset of the shader at the middle of the band's part of the polygon
	if (!ripple_ready)
	{
		for (i = 0; i <= 256; i++)
			ripple_sin[i] = sinf((float)i * (6.28318530718f / 256.0f));
		ripple_ready = 1;
	}
	if (rt_time != (int)H.leveltime)
	{
		memset(rt_ok, 0, sizeof rt_ok);
		rt_time = (int)H.leveltime;
	}
	for (j = 0; j <= nr; j++)
	{
		const float lo0 = j == 0 ? (f > 0 ? ripple_planes[f - 1] : 0.0f) : rz[j - 1];
		const float hi0 = f + j < ripple_nplanes ? ripple_planes[f + j] : ripple_zmax2;
		float lo = lo0, hi = hi0;

		if (lo < z0)
			lo = z0;
		if (hi > z1)
			hi = z1;
		if (lo0 >= ripple_zmax2)
			bds[j] = bdt[j] = 0.0f;
		else if (lo == lo0 && hi == hi0 && f + j <= RIPPLE_MAXPLANES)
		{
			const int b = f + j; // between the planes b - 1 and b of the table: the same for every polygon of this level time

			if (!rt_ok[b])
			{
				ripple_offset(0.5f * (lo + hi), &rt_ds[b], &rt_dt[b]);
				rt_ok[b] = 1;
			}
			bds[j] = rt_ds[b];
			bdt[j] = rt_dt[b];
		}
		else
			ripple_offset(0.5f * (lo + hi), &bds[j], &bdt[j]);
	}
	na_ = wf_chain(v, cnt, imin, imax, 1, cw, m, wf_ca, bidxa);
	if (na_ < 0)
		return 0;
	nb_ = wf_chain(v, cnt, imin, imax, -1, cw, m, wf_cb, bidxb);
	if (nb_ < 0)
		return 0;
	for (j = 0; j <= m; j++)
	{
		const int sa = j == 0 ? 0 : bidxa[j - 1], ea = j == m ? na_ - 1 : bidxa[j];
		const int sb = j == 0 ? 0 : bidxb[j - 1], eb = j == m ? nb_ - 1 : bidxb[j];
		int np = 0, k;
		piece_t p2 = pc;

		for (k = sa; k <= ea; k++)
			wf_piece[np++] = wf_ca[k];
		for (k = eb; k >= sb; k--)
		{
			if (j == m && k == eb)
				continue; // the last vertex is on both chains
			if (j == 0 && k == 0)
				continue; // so is the first
			wf_piece[np++] = wf_cb[k];
		}
		if (np < 3)
			continue;
		p2.sb += bds[ib[j]];
		p2.tb += bdt[ib[j]];
		p2.cls = icl[j];
		emit_piece(wf_piece, np, &p2);
		G.wf_pieces++;
	}
	G.wf_polys++;
	G.wf_bands += (u32)nr;
	H.st.bands += (u32)m + 1;
	return 1;
}


// ======================================================================================================================================
// PS2-HW-120 (OPT11, FX): the ripple without seams.
//
// The OPT10 sweep above gives every depth band a CONSTANT shift of the texture coordinates: at the edge of two bands the texture jumps by the
// difference of the two shifts (up to 2 pi * 0.025 * 16 / 160 of a repeat = 0.6 texel on a 64 texel flat, 2.5 texels on the 256 texel DSWATER flats of
// Deep Sea): a visible line along every band edge, and the bands stop at z = 320 (a 256 texel flat still moves 3 pixels there).
//
// Here the shift belongs to the VERTICES. The shader's offset is a function of the eye depth z of the fragment alone,
//     (ds, dt) = amp * (sin a, cos a), a = a0 - K z, K = pi * 0.0125 (period 160 units of depth), a0 = 2 * (leveltime - 1 + frac) / 35 (PS2-HW-125),
// and z is an affine function of the position on a planar polygon, so the GS (S/Q, T/Q interpolated in screen space = linear in the polygon) draws
// the exact offset if every vertex carries (s - ds(z_v), t - dt(z_v)) and the polygon has a vertex at every depth where the sine is sampled: between
// two sample planes the offset is linear in z (the chord of the sine), at the plane it is exact, and a point shared by two pieces (or by two
// neighbouring polygons: the planes are global, taken from a table) has the same value on both sides: C0, no seam.
// The error of the chord is amp * (K dz)^2 / 8 (dz = distance of two planes), in pixels amp_px(z) * (K dz)^2 / 8 with amp_px = 0.025 * S * F / z
// (S = texture width = world size of one repeat, F = focal length in pixels): the table puts the planes so that this stays below RP_EPS pixels (S = 256:
// 12 units apart at z = 100, 25 at z = 400). The nodes are drawn slightly outside the sine ((1 + (K dz)^2 / 16) * amplitude: the chord of a sine lies
// inside it by the factor 1 - (K dz)^2 / 8, so the error between the nodes becomes +-(K dz)^2 / 16 instead of one sided (K dz)^2 / 8: half).
// Beyond the depth where the amplitude falls below RP_PX_END pixels the ripple fades out (linear in z between the depths of RP_PX_FULL and RP_PX_END
// pixels, the amplitude is a function of z too, so the fade is continuous as well).
// The polygon is one triangle strip per darkness class: the two monotone chains from its nearest to its farthest vertex are walked together in depth order
// and every point (a vertex of the polygon or the crossing of a chain with a plane) becomes a strip vertex as it is made: no copies, one GIF packet.
// ======================================================================================================================================

#define HWDBG_WATERBANDS 1 // -hwwater 1: the OPT10 sweep (a constant shift per band, ripple to z = 320): A/B of PS2-HW-120 (the bit of ps2hwd_water_ab, not of -hwdbg: 0x10000 of -hwdbg is the plane cache of hw_main.c)
#define RT_NCLASS 5 // texture widths 32, 64, 128, 256, 512 (a power of two class; wider textures use the last)
#define RT_MAXN 120 // planes of a table
#define WS_NPLANE 160 // planes of one polygon (ripple planes + darkness steps)
#define RP_EPS 0.70f // pixels of chord error the table plans for (half of it remains between the nodes)
#define RP_GAP 3.0f // planes closer than this on the screen (pixels) are left out
#define HWDBG_WATERNOGAP 2 // -hwwater 2: every plane of the table (A/B of the screen distance rule)
#define RP_PX_FULL 1.50f // amplitude in pixels at which the fade-out starts
#define RP_PX_END 0.80f // ... and ends
#define RP_DZ_MIN 8.0f
#define RP_DZ_MAX 64.0f
#define RP_K 0.039269908f // rad per unit of eye depth (pi * 0.0125)

typedef struct
{
	float z[RT_MAXN]; // eye depths of the planes (ascending)
	float w[RT_MAXN]; // and their depths w (clip space) for the near plane the table was made for
	float ds[RT_MAXN], dt[RT_MAXN]; // the shift of the texture coordinates on a plane at the level time of ver[]
	u32 ver[RT_MAXN];
	int n;
	float zfull, zend, taper; // full amplitude up to zfull, none from zend, linear in between: amplitude = (zend - z) * taper
	float dk, dmax; // the nodes are drawn (1 + min(dk * (z + 6), dmax)) times the sine
	float focal, near; // F and the near plane the table was made for
} rtab_t;

static rtab_t rtab[RT_NCLASS];
static float rp_u0; // phase of z = 0 in 1/256 turns (+ 64 turns so that the index stays positive)
static int rp_time = -1;
static u32 rp_ver = 1; // changes with the level time: the shifts of the planes of a table are valid for one
static float rp_near, rp_zk; // zfrag_of_w without its division

static void ripple_init_table(void)
{
	int i;

	if (ripple_ready)
		return;
	for (i = 0; i <= 256; i++)
		ripple_sin[i] = sinf((float)i * (6.28318530718f / 256.0f));
	ripple_ready = 1;
}

static void ripple_build(rtab_t *t, float S, float F)
{
	const float apx = RIPPLE_AMP * S * F; // amplitude in pixels = apx / z
	const float wk = (FAR_CLIPPING_PLANE - H.near_plane) / FAR_CLIPPING_PLANE;
	float z = 0.0f;
	int i;

	t->focal = F;
	t->near = H.near_plane;
	t->n = 0;
	t->zfull = apx * (1.0f / RP_PX_FULL);
	t->zend = apx * (1.0f / RP_PX_END);
	if (t->zend > 9000.0f)
		t->zend = 9000.0f;
	if (t->zfull > t->zend * 0.5f)
		t->zfull = t->zend * 0.5f;
	for (;;)
	{
		float dz = sqrtf(8.0f * RP_EPS * (z + 6.0f) / apx) * (1.0f / RP_K);

		dz = dz < RP_DZ_MIN ? RP_DZ_MIN : dz > RP_DZ_MAX ? RP_DZ_MAX : dz;
		z += dz;
		if (z >= t->zend)
			break;
		if (t->n == RT_MAXN)
		{
			t->zend = t->z[RT_MAXN - 1] + RP_DZ_MAX; // the table ends here: the fade ends with it
			if (t->zfull > t->zend * 0.5f)
				t->zfull = t->zend * 0.5f;
			break;
		}
		t->z[t->n++] = z;
	}
	t->taper = RIPPLE_AMP / (t->zend - t->zfull);
	t->dk = RP_EPS / (2.0f * apx); // (K dz)^2 / 16 with (K dz)^2 = 8 eps (z + 6) / apx
	t->dmax = RP_K * RP_DZ_MAX * RP_K * RP_DZ_MAX * (1.0f / 16.0f);
	for (i = 0; i < t->n; i++)
	{
		t->w[i] = H.near_plane + t->z[i] * wk;
		t->ver[i] = 0;
	}
}

// the table for a texture of width sw (the world size of one repeat of a flat) and the view
static const rtab_t *ripple_table(int sw)
{
	const float F = 0.5f * H.vpw * H.proj[0];
	int c = 0;
	rtab_t *t;

	while (c < RT_NCLASS - 1 && (32 << c) < sw)
		c++;
	t = &rtab[c];
	if (t->n == 0 || fabsf(t->focal - F) > 0.25f || t->near != H.near_plane)
	{
		ripple_init_table();
		ripple_build(t, (float)(32 << c), F < 8.0f ? 8.0f : F);
	}
	return t;
}

// phase of the level time in 1/256 turns: the shader's time is (leveltime - 1 + rendertimefrac) / 35 seconds, 2 rad per second (PS2-HW-125: water_phase_rad, reduced in double);
// and the eye depth constants of the view
static float rp_frac = -1.0f;

static inline void ripple_time_set(void)
{
	rp_near = H.near_plane;
	rp_zk = FAR_CLIPPING_PLANE / (FAR_CLIPPING_PLANE - H.near_plane);
	if (rp_time == H.leveltime && rp_frac == H.lt_frac)
		return;
	rp_time = H.leveltime;
	rp_frac = H.lt_frac;
	rp_ver++;
	rp_u0 = water_phase_rad() * (256.0f / 6.28318530718f) + 16384.0f;
}

// the shift (ds, dt) of the texture coordinates at eye depth z: amp * (sin a, cos a), a = a0 - K z
static inline void ripple_off(const rtab_t *t, float z, float *ds, float *dt)
{
	float g, x, f, s0, c0, d;
	int i;

	if (z >= t->zend)
	{
		*ds = *dt = 0.0f;
		return;
	}
	g = z <= t->zfull ? RIPPLE_AMP : (t->zend - z) * t->taper;
	d = t->dk * (z + 6.0f);
	g *= 1.0f + (d < t->dmax ? d : t->dmax);
	x = rp_u0 - z * 1.6f; // a / (2 pi) * 256: K * 256 / (2 pi) = 1.6
	i = (int)x;
	f = x - (float)i;
	s0 = ripple_sin[i & 255];
	c0 = ripple_sin[(i + 64) & 255];
	*ds = g * (s0 + (ripple_sin[(i + 1) & 255] - s0) * f);
	*dt = g * (c0 + (ripple_sin[(i + 65) & 255] - c0) * f);
}

// the same on a plane of the table (once per level time)
static inline void ripple_off_plane(rtab_t *t, int j, float *ds, float *dt)
{
	if (t->ver[j] != rp_ver)
	{
		ripple_off(t, t->z[j], &t->ds[j], &t->dt[j]);
		t->ver[j] = rp_ver;
	}
	*ds = t->ds[j];
	*dt = t->dt[j];
}

/* #define HW_FXPROF 1 -- cycles per phase of water_sweep (HWFX3 lines every 105 frames) */
#ifdef HW_FXPROF
static u32 fx_c[8], fx_n[8], fx_frame = 0xFFFFFFFFu;
#define FXT(i) do { const u32 t_ = cyc(); fx_c[i] += t_ - fx_t; fx_t = t_; } while (0)
#else
#define FXT(i) ((void)0)
#endif

#define WS_STRIPMAX (2 * (WF_MAXV + WS_NPLANE + 2) + 3 * WF_MAXV + 16)
static cv_t ws_out[WS_STRIPMAX];

// the walk of one chain of the polygon: the edge it stands on (a -> b), the interpolation of the edge, the latest strip vertex of the chain
typedef struct
{
	int i; // index of the vertex a
	int dir; // +1 / -1
	const cv_t *b; // the vertex the edge goes to
	float aw, bw, inv; // depths of the ends, 1 / (bw - aw)
	float ax, ay, az, a0, a1; // the vertex a
	float dx, dy, dz, d0, d1; // b - a
	int last; // index in ws_out of the latest point of the chain
} wchain_t;

static inline void wc_edge(wchain_t *c, const cv_t *v, int cnt)
{
	int nx = c->i + c->dir;
	const cv_t *a = &v[c->i];

	nx = nx < 0 ? cnt - 1 : nx >= cnt ? 0 : nx;
	c->b = &v[nx];
	c->aw = a->w;
	c->bw = c->b->w;
	c->inv = c->bw > c->aw ? 1.0f / (c->bw - c->aw) : 0.0f;
	c->ax = a->x;
	c->ay = a->y;
	c->az = a->z;
	c->a0 = a->a[0];
	c->a1 = a->a[1];
	c->dx = c->b->x - c->ax;
	c->dy = c->b->y - c->ay;
	c->dz = c->b->z - c->az;
	c->d0 = c->b->a[0] - c->a0;
	c->d1 = c->b->a[1] - c->a1;
}

// the point of the chain on the plane of depth cut (the edge reaches beyond it), the shift (dsv, dtv) of the plane applied
static inline void wc_cross(cv_t *o, const wchain_t *c, float cut, float dsv, float dtv)
{
	const float tt = (cut - c->aw) * c->inv;

	o->x = c->ax + c->dx * tt;
	o->y = c->ay + c->dy * tt;
	o->z = c->az + c->dz * tt;
	o->w = cut;
	o->a[0] = c->a0 + c->d0 * tt - dsv;
	o->a[1] = c->a1 + c->d1 * tt - dtv;
}

static int lit_fast_poly(const cv_t *v, const void *pvv, int cnt, float sb, float tb); // ps2_hw_fast.inc (PS2-HW-43)

// 1 when the polygon was drawn. `cut`: the polygon needs the repeat cuts (a power of two texture over 4096 texels, a width that is not a power of two): water_generic
static int water_sweep(const cv_t *v, const void *pv, int cnt, piece_t pc, int na, int cut)
{
	float zc[WS_NPLANE], cw[WS_NPLANE], th[WS_NPLANE], z0, z1, wlo, whi;
	int icl[WS_NPLANE + 1], cls[WS_NPLANE], pidx[WS_NPLANE];
	int i, imin = 0, imax = 0, m = 0, nr = 0, nc = 0, cls0 = -1, f, n, lastc, j, nseg = 0;
	rtab_t *t;
	const float *rzp;
	wchain_t ch[2];
	float gapk;
#ifdef HW_FXPROF
	u32 fx_t = cyc();
#endif

	(void)na;
	if (cut || cnt < 3 || cnt > WF_MAXV || P.lmode != LM_BANDS || !P.r)
		return 0;
	wlo = whi = v[0].w;
	for (i = 1; i < cnt; i++)
	{
		if (v[i].w < wlo)
		{
			wlo = v[i].w;
			imin = i;
		}
		if (v[i].w > whi)
		{
			whi = v[i].w;
			imax = i;
		}
	}
	if (imin == imax)
		return 0; // all vertices at one depth (a polygon facing the view): water_generic
	// both chains from the nearest to the farthest vertex must rise (a convex polygon: they do, up to the rounding of equal depths)
	for (i = imin; i != imax; i = i + 1 == cnt ? 0 : i + 1)
		if (v[i + 1 == cnt ? 0 : i + 1].w < v[i].w)
			return 0;
	for (i = imin; i != imax; i = i == 0 ? cnt - 1 : i - 1)
		if (v[i == 0 ? cnt - 1 : i - 1].w < v[i].w)
			return 0;
	z0 = zfrag_of_w(wlo);
	z1 = zfrag_of_w(whi);
	t = (rtab_t *)ripple_table((int)P.r->sw);
	ripple_time_set();
	if (z0 >= t->zend)
	{
		// no ripple that far: an ordinary lit polygon (the common path of the walls and floors; cut_and_emit costs three times as much)
		if (P.npass == 1 && !P.pass[0].ramp && P.pass[0].tex == (u8)P.textured && !P.oldpath && lit_fast_poly(v, pv, cnt, pc.sb, pc.tb))
			return 1;
		cut_and_emit(v, cnt, 0, pc, na);
		return 1;
	}
	// the planes strictly inside (z0, z1): binary search for the first
	{
		int lo = 0, hi = t->n;

		while (lo < hi)
		{
			const int mid = (lo + hi) >> 1;

			if (t->z[mid] <= z0)
				lo = mid + 1;
			else
				hi = mid;
		}
		f = lo;
		rzp = &t->z[f];
		while (f + nr < t->n && rzp[nr] < z1)
			nr++;
	}
	if (P.oldpath)
	{
		nc = doom_thresholds(&P.rs.lp, z0, z1, th, cls, WS_NPLANE, &cls0);
	}
	else
	{
		int c1;

		cls0 = lf_class_w(wlo);
		c1 = lf_class_w(whi);
		nc = cls0 == c1 ? 0 : lf_thresholds(cls0, c1, z0, z1, th, cls, WS_NPLANE);
	}
	// planes closer than RP_GAP pixels on the screen are not worth a vertex pair (a floor: the screen position of the depth z is Fh / z, the polygon's screen length
	// L between its nearest and farthest vertex gives Fh = L z0 z1 / (z1 - z0), a plane of depth z moves RP_GAP pixels on the screen when z changes by k z^2)
	{
		const float sx = (v[imax].x / v[imax].w - v[imin].x / v[imin].w) * P.kx, sy = (v[imax].y / v[imax].w - v[imin].y / v[imin].w) * P.ky;
		const float L = sqrtf(sx * sx + sy * sy) * (1.0f / 16.0f), zz = z0 < 8.0f ? 8.0f : z0;

		gapk = RP_GAP * (z1 - zz) / (L * zz * z1 + 1e-3f);
		if (ps2hwd_water_ab & HWDBG_WATERNOGAP)
			gapk = 0.0f;
	}
	// merge the ripple planes and the darkness steps in depth order; interval j lies between plane j - 1 and plane j
	{
		int ir = 0, ic = 0, cur = cls0;
		float lastz = z0;

		icl[0] = cls0;
		while ((ir < nr || ic < nc) && m < WS_NPLANE)
		{
			const float zr = ir < nr ? rzp[ir] : 1e30f, zt = ic < nc ? th[ic] : 1e30f;

			if (zr < zt)
			{
				if (zr - lastz < gapk * zr * zr) // too close to the previous plane on the screen
				{
					ir++;
					continue;
				}
				zc[m] = zr;
				cw[m] = t->w[f + ir];
				pidx[m] = f + ir;
				lastz = zr;
				ir++;
			}
			else
			{
				if (zt < zr)
				{
					zc[m] = zt;
					cw[m] = w_of_zfrag(zt);
					pidx[m] = -1;
					lastz = zt;
				}
				else
				{
					zc[m] = zr;
					cw[m] = t->w[f + ir];
					pidx[m] = f + ir;
					lastz = zr;
					ir++;
				}
				cur = cls[ic];
				ic++;
			}
			m++;
			icl[m] = cur;
		}
	}
	FXT(0);
	// ---- the strips ----
	// A strip draws the triangle of every vertex with the two before it. The chain 0 (from the nearest vertex, step +1) and the chain 1 (step -1) take
	// turns plane by plane: a vertex of the chain that did not move last. Where one chain moves twice in a row (a vertex of the polygon between two planes) the
	// front edge is repeated: two degenerate triangles, they cover no pixel. One strip for every run of intervals of one darkness class.
	ch[0].i = ch[1].i = imin;
	ch[0].dir = 1;
	ch[1].dir = -1;
	wc_edge(&ch[0], v, cnt);
	wc_edge(&ch[1], v, cnt);
	{
		float dsv, dtv;

		ripple_off(t, z0, &dsv, &dtv);
		ws_out[0].x = v[imin].x;
		ws_out[0].y = v[imin].y;
		ws_out[0].z = v[imin].z;
		ws_out[0].w = v[imin].w;
		ws_out[0].a[0] = v[imin].a[0] - dsv;
		ws_out[0].a[1] = v[imin].a[1] - dtv;
		ws_out[1] = ws_out[0];
		ch[0].last = 0;
		ch[1].last = 1;
		lastc = 1;
		n = 2;
	}
	for (j = 0; j < m; j++)
	{
		float dsv, dtv;
		const float cutw = cw[j];
		int X;

		if (pidx[j] >= 0)
			ripple_off_plane(t, pidx[j], &dsv, &dtv);
		else
			ripple_off(t, zc[j], &dsv, &dtv);
		for (X = 0; X < 2; X++)
		{
			wchain_t *c = &ch[X];

			while (c->bw < cutw) // vertices of the polygon before this plane
			{
				cv_t *o;

				if (lastc == X)
				{
					ws_out[n] = ws_out[ch[X].last];
					ws_out[n + 1] = ws_out[ch[1 - X].last];
					ch[X].last = n;
					ch[1 - X].last = n + 1;
					n += 2;
					lastc = 1 - X;
				}
				o = &ws_out[n];
				{
					float vds, vdt;

					ripple_off(t, (c->bw - rp_near) * rp_zk, &vds, &vdt);
					o->x = c->b->x;
					o->y = c->b->y;
					o->z = c->b->z;
					o->w = c->bw;
					o->a[0] = c->b->a[0] - vds;
					o->a[1] = c->b->a[1] - vdt;
				}
				c->last = n++;
				lastc = X;
				c->i += c->dir;
				c->i = c->i < 0 ? cnt - 1 : c->i >= cnt ? 0 : c->i;
				wc_edge(c, v, cnt);
			}
			if (lastc == X)
			{
				ws_out[n] = ws_out[ch[X].last];
				ws_out[n + 1] = ws_out[ch[1 - X].last];
				ch[X].last = n;
				ch[1 - X].last = n + 1;
				n += 2;
				lastc = 1 - X;
			}
			wc_cross(&ws_out[n], c, cutw, dsv, dtv);
			c->last = n++;
			lastc = X;
		}
		if (icl[j] != icl[j + 1])
		{
			// the darkness class changes here: draw the strip of the run and start the next one from the two points on this plane
			piece_t p2 = pc;
			cv_t t0 = ws_out[ch[0].last], t1 = ws_out[ch[1].last];

			p2.cls = icl[j];
			p2.ripple = 1;
			FXT(1);
			emit_piece(ws_out, n, &p2);
			G.wf_pieces++;
			nseg++;
#ifdef HW_FXPROF
			fx_n[3] += (u32)n;
#endif
			FXT(4);
			ws_out[0] = t0;
			ws_out[1] = t1;
			ch[0].last = 0;
			ch[1].last = 1;
			lastc = 1;
			n = 2;
		}
	}
	// the rest of the polygon: the vertices of chain 0 up to the farthest one, then those of chain 1 before it
	{
		int X;

		for (X = 0; X < 2; X++)
		{
			wchain_t *c = &ch[X];

			while (c->i != imax)
			{
				cv_t *o;
				float vds, vdt;
				const int last_of_chain = c->b == &v[imax];

				if (last_of_chain && X == 1)
					break; // the farthest vertex was the last of chain 0
				if (lastc == X)
				{
					ws_out[n] = ws_out[ch[X].last];
					ws_out[n + 1] = ws_out[ch[1 - X].last];
					ch[X].last = n;
					ch[1 - X].last = n + 1;
					n += 2;
					lastc = 1 - X;
				}
				o = &ws_out[n];
				ripple_off(t, (c->bw - rp_near) * rp_zk, &vds, &vdt);
				o->x = c->b->x;
				o->y = c->b->y;
				o->z = c->b->z;
				o->w = c->bw;
				o->a[0] = c->b->a[0] - vds;
				o->a[1] = c->b->a[1] - vdt;
				c->last = n++;
				lastc = X;
				c->i += c->dir;
				c->i = c->i < 0 ? cnt - 1 : c->i >= cnt ? 0 : c->i;
				if (c->i != imax)
					wc_edge(c, v, cnt);
			}
		}
	}
	if (n >= 3)
	{
		piece_t p2 = pc;

		p2.cls = icl[m];
		p2.ripple = 1;
		FXT(1);
		emit_piece(ws_out, n, &p2);
		G.wf_pieces++;
		nseg++;
#ifdef HW_FXPROF
		fx_n[3] += (u32)n;
#endif
		FXT(4);
	}
#ifdef HW_FXPROF
	fx_n[0]++;
	fx_n[1] += (u32)m;
	fx_n[2] += (u32)nr;
	fx_n[4] += (u32)nc;
	fx_n[5] += (u32)nseg;
#endif
	(void)nseg;
	G.wf_polys++;
	G.wf_bands += (u32)m;
	H.st.bands += (u32)m + 1;
	return 1;
}


// the shift of the texture coordinates of the vertices of a piece (the general path)
static void ripple_bake(cv_t *p, int n, const rtab_t *t)
{
	int i;

	for (i = 0; i < n; i++)
	{
		float ds, dt;

		ripple_off(t, (p[i].w - rp_near) * rp_zk, &ds, &dt);
		p[i].a[0] -= ds;
		p[i].a[1] -= dt;
	}
}

// put_vertex for the formats 0 / 1 (ST, RGBAQ, XYZ2 / XYZF2) with the constants of the plan in registers: the packets are the same, bit for bit (-hwwater 4 uses
// put_vertex: the host test compares both). The plan is a global, and the stores of the packet (floats) may alias it, so put_vertex reloads a dozen constants per vertex.
static qw_t *ws_pack(qw_t *o, const cv_t *v, int cnt, const pack_t *pk)
{
	const float zo = P.zo, zk = P.zk, zbias = P.zbias, zmax = P.zmax, ox = P.ox, kx = P.kx, oy = P.oy, ky = P.ky, tsx = P.tsx, tsy = P.tsy, sb = pk->sb, tb = pk->tb;
	const u32 c0 = pk->col & 255, c1 = (pk->col >> 8) & 255, c2 = (pk->col >> 16) & 255, c3 = pk->col >> 24;
	const int fogfmt = pk->fmt == 1;
	const u32 fogw = pk->fog << 4;
	int i;

	for (i = 0; i < cnt; i++, o += 3)
	{
		const float q = 1.0f / v[i].w;
		float zf = zo + v[i].z * q * zk + zbias;
		const int xi = (int)(ox + v[i].x * q * kx);
		const int yi = (int)(oy + v[i].y * q * ky);
		u32 zi;

		zf = clampf(zf, 0.0f, zmax);
		zi = (u32)(int)zf;
		o[0].f[0] = (v[i].a[0] - sb) * tsx * q;
		o[0].f[1] = (v[i].a[1] - tb) * tsy * q;
		o[0].f[2] = q;
		o[0].w[3] = 0;
		o[1].w[0] = c0;
		o[1].w[1] = c1;
		o[1].w[2] = c2;
		o[1].w[3] = c3;
		o[2].w[0] = (u32)xi;
		o[2].w[1] = (u32)yi;
		o[2].w[2] = fogfmt ? zi << 4 : zi;
		o[2].w[3] = fogfmt ? fogw : 0;
	}
	H.gsr.valid &= ~(u32)RV_RGBAQ; // packed vertices changed Q behind the A+D state cache
	return o;
}

static cv_t wpiece[CUTBUF], wrem[2][CUTBUF];

// The general path (more than WF_MAXV vertices, not monotone, no light bands): the polygon is clipped plane by plane (clip_attr) and every piece
// gets the shift of its own vertices before cut_and_emit does the rest (repeats, darkness steps): continuous as well, just slower.
static void water_generic(const cv_t *v, int cnt, piece_t pc, int na)
{
	float wlo = v[0].w, whi = v[0].w, z0, z1;
	const cv_t *rem = v;
	int nrem = cnt, i, k, which = 0;
	const rtab_t *t;

	for (i = 1; i < cnt; i++)
	{
		if (v[i].w < wlo)
			wlo = v[i].w;
		if (v[i].w > whi)
			whi = v[i].w;
	}
	z0 = zfrag_of_w(wlo);
	z1 = zfrag_of_w(whi);
	t = P.r ? ripple_table((int)P.r->sw) : NULL;
	if (!t || z0 >= t->zend)
	{
		cut_and_emit(v, cnt, 0, pc, na);
		return;
	}
	ripple_time_set();
	for (k = 0; k < t->n && t->z[k] <= z0; k++)
		;
	for (;; k++)
	{
		const float cutz = k < t->n ? t->z[k] : 1e30f;
		int np;
		cv_t *rb = wrem[which];

		if (cutz >= z1 || cutz >= t->zend)
		{
			np = nrem;
			memcpy(wpiece, rem, (size_t)np * sizeof(cv_t));
			ripple_bake(wpiece, np, t);
			cut_and_emit(wpiece, np, 0, pc, na);
			return;
		}
		{
			const float cutw = w_of_zfrag(cutz);
			int nn;

			np = clip_attr(rem, nrem, wpiece, 6, cutw, 0, na);
			nn = clip_attr(rem, nrem, rb, 6, cutw, 1, na);
			if (np >= 3)
			{
				ripple_bake(wpiece, np, t);
				cut_and_emit(wpiece, np, 0, pc, na);
				H.st.bands++;
			}
			if (nn < 3)
				return;
			rem = rb;
			nrem = nn;
			which ^= 1;
		}
	}
}

// the old band by band path (HWDBG_OLDWATER, and what the OPT10 sweep falls back to)
static void emit_water_poly_old(const cv_t *v, int cnt, piece_t pc, int na)
{
	float wlo = v[0].w, whi = v[0].w, z0, z1, ds, dt, step = RIPPLE_STEP;
	const cv_t *rem = v;
	int nrem = cnt, i, k, which = 0;
	piece_t p2;

	ripple_init_table();
	for (i = 1; i < cnt; i++)
	{
		if (v[i].w < wlo)
			wlo = v[i].w;
		if (v[i].w > whi)
			whi = v[i].w;
	}
	z0 = zfrag_of_w(wlo);
	z1 = zfrag_of_w(whi);
	if (z0 >= RIPPLE_ZMAX)
	{
		cut_and_emit(v, cnt, 0, pc, na);
		return;
	}
	k = z0 > 0.0f ? (int)(z0 / step) : 0;
	for (;; k++)
	{
		float cutz = (float)(k + 1) * step;

		if (cutz >= z1 || cutz >= RIPPLE_ZMAX)
		{
			// the last piece: the rest of the polygon
			p2 = pc;
			if (cutz >= RIPPLE_ZMAX && (float)k * step >= RIPPLE_ZMAX)
			{
				ds = dt = 0.0f;
			}
			else
			{
				float lo = (float)k * step, hi = z1 < RIPPLE_ZMAX ? z1 : RIPPLE_ZMAX;

				if (lo < z0)
					lo = z0;
				ripple_offset(0.5f * (lo + hi), &ds, &dt);
			}
			p2.sb += ds;
			p2.tb += dt;
			cut_and_emit(rem, nrem, 0, p2, na);
			return;
		}
		{
			float cutw = w_of_zfrag(cutz);
			int np = clip_attr(rem, nrem, wpiece, 6, cutw, 0, na);
			cv_t *rb = wrem[which];
			int nn = clip_attr(rem, nrem, rb, 6, cutw, 1, na);

			if (np >= 3)
			{
				float lo = (float)k * step;

				if (lo < z0)
					lo = z0;
				ripple_offset(0.5f * (lo + cutz), &ds, &dt);
				p2 = pc;
				p2.sb += ds;
				p2.tb += dt;
				cut_and_emit(wpiece, np, 0, p2, na);
				H.st.bands++;
			}
			if (nn < 3)
				return;
			rem = rb;
			nrem = nn;
			which ^= 1;
		}
	}
}

// the water polygon of emit_poly (P.water): `big` = more texels than one GS primitive may span (the pieces are cut at whole repeats)
static void emit_water_poly(const cv_t *v, int cnt, piece_t pc, int na, int big, const void *pv)
{
	const int dbg = ps2hwd_dbg_flags;
#ifdef HW_FXPROF
	if (H.frame_no % 105 == 104 && fx_frame != H.frame_no)
	{
		fx_frame = H.frame_no;
		CONS_Printf("HWFX3 water_sweep per window: polys=%u planes=%u (table planes in range %u, class planes %u) strips=%u stripverts=%u | cycles prep=%u gen=%u emit=%u\n", fx_n[0], fx_n[1], fx_n[2], fx_n[4], fx_n[5], fx_n[3], fx_c[0], fx_c[1], fx_c[4]);
		{
			int ci;

			for (ci = 0; ci < RT_NCLASS; ci++)
				if (rtab[ci].n)
					CONS_Printf("HWFX3 table S=%d: planes=%d zfull=%.0f zend=%.0f focal=%.1f near=%.2f dk=%.4f\n", 32 << ci, rtab[ci].n, rtab[ci].zfull, rtab[ci].zend, rtab[ci].focal, rtab[ci].near, rtab[ci].dk);
		}
		memset(fx_c, 0, sizeof fx_c);
		memset(fx_n, 0, sizeof fx_n);
	}
#endif

	if (dbg & HWDBG_OLDWATER)
	{
		if (big)
			cut_and_emit(v, cnt, 0, pc, na); // OPT10: no ripple on polygons of this size
		else
			emit_water_poly_old(v, cnt, pc, na);
		return;
	}
	if (ps2hwd_water_ab & HWDBG_WATERBANDS)
	{
		if (big)
			cut_and_emit(v, cnt, 0, pc, na);
		else if (!band_sweep(v, cnt, pc, 1))
			emit_water_poly_old(v, cnt, pc, na);
		return;
	}
	if (water_sweep(v, pv, cnt, pc, na, big || P.cutx || P.cuty))
		return;
	water_generic(v, cnt, pc, na);
}
