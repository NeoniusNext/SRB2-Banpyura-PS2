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

static inline int water_fast(const cv_t *v, int cnt, piece_t pc)
{
	return band_sweep(v, cnt, pc, 1);
}
