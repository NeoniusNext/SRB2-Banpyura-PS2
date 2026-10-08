// PS2 GS hardware renderer driver: the frame plan of the textures (OPT9 PS2-HW-34, redone in OPT10 PS2-HW-37); included by ps2_hwd.c after ps2_hw_draw.inc.
//
// HWR_RenderBatches (hardware/hw_batching.c) shows the driver every collected polygon before the first of them is drawn. For the big map
// textures and flats (the ones that do not fit the pool in numbers: the 1024 x 1024 cloud planes of GFZ1 alone are 4096 blocks each, the pool 12032)
// the driver measures what the VIEW sees of each polygon: the polygon is transformed and clipped to the viewport (not to the guard band: what
// lies outside the screen costs nothing and must not count), and at every vertex of the clipped polygon the screen size of one texel along u and
// v is taken from the plane's mapping (poly_plane / poly_density, ps2_hw_tex.inc). The texel size of a polygon (its largest, at the vertex nearest
// to the eye) and the screen area of the polygon go into a histogram of the texture. The mip level is chosen from the texel size below which all
// but PLAN_EPS of the texture's visible area lies: level L is enough when no texel is magnified compared with the full size (texel * 2^L <= 1
// pixel), so a plane that is seen far away, a wall at a grazing angle, the sky show the same detail the PC driver shows; the 2 % of the area that
// is nearest (the sliver of a horizon fill polygon, a corner of a wall under the camera) does not decide for the whole image.
// A polygon that has no area (a zero-height wall piece) shows nothing and counts for nothing; a texture none of whose polygons can be seen is
// not uploaded at all. When the exact plan does not fit the budget (a share of the pool) the images that save the most blocks are stored one
// level coarser, which magnifies a texel at most 2:1 (the tolerance of PS2-HW-37).
// hw_SetTexture stores the texture at the planned level (tex_want); a big texture that is drawn outside the batches is measured when its polygon
// is drawn (imm_prepare, below).

static int plan_active;

#define PLAN_MAX 256 // images of one frame's plan (more are stored at full size)
#define PLAN_BINS 32 // texel size histogram: 4 bins per octave, 2^-5 .. 2^3 pixels
#define PLAN_EPS 0.02f // share of an image's visible area that may be magnified by more than the plan allows
#define PLAN_PCT_MAX 75u // the planned images of a frame may take this share of the pool (per cent; the rest is for sprites, patches, the small textures)
#define PLAN_PCT_MIN 35u // ... and this much when the controller (plan_frame_end) finds the working set of the frame does not fit
#define PLAN_PCT_DOWN 6u // per frame in which an image used by the frame was evicted in the frame
#define PLAN_CALM_FRAMES 20 // calm frames before the budget grows by one per cent
#define PLAN_THRASH_BLK 256u // blocks of images used by a frame and evicted in it that make the frame count as thrashing (a few small ones are the fragmentation)
#define PLAN_TOL_EXACT 1.0f // texel magnification allowed by the exact plan
#define PLAN_TOL_RELAX 2.0f // ... and by the relaxed plan (images stored one level coarser to fit the budget)
#define PLAN_BIG_BLOCKS 512u // an image of at least this many blocks (at its exact level) may be stored PLAN_BIG_EXTRA levels coarser than the exact plan when the budget needs it
#define PLAN_BIG_EXTRA 2
#define PLAN_DOWN_BLOCKS 256u // an image is made again at a coarser planned level when that frees at least this many blocks
#define PLAN_TOL_KEEP 1.5f // an image stored at a coarser level than the plan asks for is kept while it magnifies at most this much (no flicker between levels)

typedef struct
{
	GLMipmap_t *key; // the original mipmap of a variant pair
	float total; // visible area of the polygons measured so far, pixels squared
	float smax; // diagnostics: the largest texel seen
	u32 npoly; // contributions measured
	float hist[PLAN_BINS]; // area by texel size
} planinfo_t;

static planinfo_t (*plan_info_p)[PLAN_MAX]; // PS2-171: see ovq (ps2_hw_draw.inc)
#define plan_info (*plan_info_p)
static int plan_n;
static u32 plan_frame; // frame_no + 1 the table belongs to (a frame can plan twice: the skybox view and the main view)

static void plan_sync(void)
{
	if (plan_frame != H.frame_no + 1)
	{
		plan_frame = H.frame_no + 1;
		plan_n = 0;
	}
}

static u32 plan_t0;

void PS2HWD_PlanBegin(void)
{
	plan_t0 = cyc();
	plan_active = H.up && !(ps2hwd_dbg_flags & HWDBG_NOPLAN);
	if (plan_active)
	{
		xf_update();
		plan_sync();
	}
}

// Sutherland-Hodgman against the six planes of the view volume (x, y, z, w only); the vertices are in `a`, `b` is scratch; returns the count, *res the result
static int plan_clip(cv_t *a, cv_t *b, int n, int mask, cv_t **res)
{
	cv_t *in = a, *out = b, *t;
	int p, i;

	for (p = 0; p < 6 && n > 0; p++)
	{
		int m = 0;

		if (!(mask & (1 << p)))
			continue;
		for (i = 0; i < n; i++)
		{
			const cv_t *v0 = &in[i], *v1 = &in[i + 1 == n ? 0 : i + 1];
			float d0 = plane_dist(v0, p, 1.0f, 1.0f), d1 = plane_dist(v1, p, 1.0f, 1.0f);

			if (d0 >= 0.0f)
				out[m++] = *v0;
			if ((d0 >= 0.0f) != (d1 >= 0.0f))
			{
				float tt = d0 / (d0 - d1);
				cv_t *o = &out[m++];

				o->x = v0->x + (v1->x - v0->x) * tt;
				o->y = v0->y + (v1->y - v0->y) * tt;
				o->z = v0->z + (v1->z - v0->z) * tt;
				o->w = v0->w + (v1->w - v0->w) * tt;
			}
		}
		n = m;
		t = in;
		in = out;
		out = t;
	}
	*res = in;
	return n;
}

// what the view sees of polygon verts[0..n) that carries texture m: the contributions (largest texel in pixels, visible area in pixels squared) go to
// pm_s / pm_a, their number is returned (0 = nothing is seen: outside the view, no area). A planar polygon is one contribution; a kinked one (the
// engine's horizon fill) is measured by the triangles of its fan, which is how the GS draws it. A polygon with an unusable texture mapping (an area on
// the screen but no texel size) counts as a texel of 1e6 pixels: full size.
static float pm_s[PS2HWD_MAXPOLY], pm_a[PS2HWD_MAXPOLY];

static int plan_part(cv_t *poly, int n, int or_, float *s, float *area)
{
	static float sx[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA], sy[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA];
	cv_t *v = poly;
	int cnt = n, i, ok = lodc_ok;
	const float px = 0.5f * H.vpw, py = 0.5f * H.vph;
	float su = 0.0f, sv = 0.0f, sum = 0.0f;

	if (or_)
	{
		cnt = plan_clip(poly, vbuf_b, n, or_, &v);
		if (cnt < 3)
			return 0;
	}
	// one pass: the screen position of every vertex (for the area) and the size of a texel along u and v (poly_density) share one reciprocal of w
	for (i = 0; i < cnt; i++)
	{
		const float w = v[i].w;
		float iw, iw2, dxs, dys, sz;

		if (w < 1e-4f)
		{
			ok = 0; // cannot be measured (the polygon is drawn: full size)
			sx[i] = sy[i] = 0.0f;
			continue;
		}
		iw = 1.0f / w;
		sx[i] = v[i].x * iw * px;
		sy[i] = v[i].y * iw * py;
		if (!ok)
			continue;
		iw2 = iw * iw;
		dxs = (lodc[0][0] * w - v[i].x * lodc[2][0]) * iw2 * px;
		dys = (lodc[1][0] * w - v[i].y * lodc[2][0]) * iw2 * py;
		sz = dxs * dxs + dys * dys;
		if (sz > su)
			su = sz;
		dxs = (lodc[0][1] * w - v[i].x * lodc[2][1]) * iw2 * px;
		dys = (lodc[1][1] * w - v[i].y * lodc[2][1]) * iw2 * py;
		sz = dxs * dxs + dys * dys;
		if (sz > sv)
			sv = sz;
	}
	for (i = 0; i < cnt; i++)
	{
		const int j = i + 1 == cnt ? 0 : i + 1;

		sum += sx[i] * sy[j] - sx[j] * sy[i];
	}
	*area = 0.5f * fabsf(sum);
	if (*area < 0.05f)
		return 0; // no area: it draws nothing
	if (!ok || cnt < 3)
	{
		*s = 1.0e6f;
		return 1;
	}
	su = sqrtf(su);
	sv = sqrtf(sv);
	*s = su > sv ? su : sv;
	return 1;
}

static int plan_measure(const GLMipmap_t *m, const FOutVector *verts, unsigned int n)
{
	static int ocs[PS2HWD_MAXPOLY];
	int i, and_ = 0x3F, or_ = 0, cnt = 0;

	for (i = 0; i < (int)n; i++)
	{
		int oc;

		xform_vertex(&verts[i], NULL, &vbuf_a[i]);
		oc = vbuf_a[i].w <= 0.0f ? 0x3F : outcode(&vbuf_a[i], 1.0f, 1.0f);
		ocs[i] = oc;
		and_ &= oc;
		or_ |= oc;
	}
	if (and_)
		return 0; // wholly outside the view volume: it draws nothing
	poly_plane(vbuf_a, (int)n, (float)m->width, (float)m->height);
	if (!lodc_ok || lodc_planar)
	{
		if (plan_part(vbuf_a, (int)n, or_, &pm_s[0], &pm_a[0]))
			cnt = 1;
	}
	else
	{
		cv_t tri[3 + PS2HWD_CLIPEXTRA]; // plan_clip alternates between this and vbuf_b: both must hold the triangle after six clips (a triangle of 3 only overflowed the stack)

		for (i = 1; i + 1 < (int)n; i++)
		{
			int oc = ocs[0] | ocs[i] | ocs[i + 1];

			if (ocs[0] & ocs[i] & ocs[i + 1])
				continue; // outside one plane
			tri[0] = vbuf_a[0];
			tri[1] = vbuf_a[i];
			tri[2] = vbuf_a[i + 1];
			poly_plane(tri, 3, (float)m->width, (float)m->height);
			if (plan_part(tri, 3, oc, &pm_s[cnt], &pm_a[cnt]))
				cnt++;
		}
	}
	if ((ps2hwd_dbg_flags & HWDBG_LODDBG) && H.frame_no % 105 == 60)
		for (i = 0; i < cnt; i++)
			CONS_Printf("HWLOD %s %ux%u n=%u planar=%d part=%d s=%.3f area=%.1f\n", HWR_PS2_TexName(m), (unsigned)m->width, (unsigned)m->height, n, lodc_planar, i, pm_s[i], pm_a[i]);
	return cnt;
}

// is m a texture the plan handles: a map texture or flat of palette indices with at least PLAN_MIN_TEXELS texels
static inline int plan_wants(const GLMipmap_t *m)
{
	return m && (m->regen_kind == 1 || m->regen_kind == 2) && m->format == GL_TEXFMT_P_8 && (u32)m->width * m->height >= PLAN_MIN_TEXELS;
}

// the bin of a texel size: floor(4 log2 s) from the bits of the float
static inline int plan_bin(float s)
{
	union { float f; u32 u; } c;
	int idx;

	c.f = s;
	idx = ((int)((c.u >> 23) & 255u) - 127 + 5) * 4 + (int)((c.u >> 21) & 3u);
	return idx < 0 ? 0 : idx >= PLAN_BINS ? PLAN_BINS - 1 : idx;
}

static inline float plan_bin_hi(int idx) // the upper edge of a bin
{
	static const float oct[8] = {1.0f / 32, 1.0f / 16, 1.0f / 8, 1.0f / 4, 0.5f, 1.0f, 2.0f, 4.0f};

	return oct[idx >> 2] * (1.0f + (float)((idx & 3) + 1) * 0.25f);
}

// the texel size below which all but a share PLAN_EPS of the visible area lies
static float plan_seps(const planinfo_t *pi)
{
	float limit = pi->total * PLAN_EPS, acc = 0.0f;
	int b;

	for (b = PLAN_BINS - 1; b > 0; b--)
	{
		acc += pi->hist[b];
		if (acc > limit)
			break;
	}
	return plan_bin_hi(b);
}

// the plan record of texture m in this frame (its first call in the frame makes it); NULL when the table is full (the texture is then stored at full size)
static planinfo_t *plan_key(GLMipmap_t *m)
{
	GLMipmap_t *k = m->ps2_twin ? m->ps2_twin : m;
	planinfo_t *pi;

	plan_sync();
	if (k->ps2_planfr == H.frame_no + 1)
		return k->ps2_pi < plan_n ? &plan_info[k->ps2_pi] : NULL;
	k->ps2_planfr = H.frame_no + 1;
	k->ps2_want = 255;
	k->ps2_vis = 0;
	if (plan_n >= PLAN_MAX)
	{
		k->ps2_pi = 0xFFFFu;
		return NULL;
	}
	k->ps2_pi = (UINT16)plan_n;
	pi = &plan_info[plan_n++];
	memset(pi, 0, sizeof *pi);
	pi->key = k;
	return pi;
}

static inline void plan_add(planinfo_t *pi, float s, float area)
{
	pi->hist[plan_bin(s)] += area;
	pi->total += area;
	pi->npoly++;
	if (s > pi->smax)
		pi->smax = s;
	pi->key->ps2_vis = 1;
}

void PS2HWD_PlanPolygon(GLMipmap_t *m, const void *vp, unsigned int n)
{
	planinfo_t *pi;
	int i, cnt;

	if (!plan_active || n < 3 || n > PS2HWD_MAXPOLY || !plan_wants(m))
		return;
	pi = plan_key(m);
	if (!pi)
		return;
	{
		const u32 t0 = cyc();

		cnt = plan_measure(m, (const FOutVector *)vp, n);
		TX.plan_np++;
		TX.plan_mcyc += cyc() - t0;
	}
	for (i = 0; i < cnt; i++)
		plan_add(pi, pm_s[i], pm_a[i]);
}

// the most levels an image of this size may be reduced by: every axis keeps at least 16 texels and the size divides
static int plan_lmax(const GLMipmap_t *m)
{
	int lv = 5;

	while (lv > 0 && ((m->width >> lv) < 16 || (m->height >> lv) < 16 || (m->width & ((1u << lv) - 1)) || (m->height & ((1u << lv) - 1))))
		lv--;
	return lv;
}

static inline u32 plan_blocks(const GLMipmap_t *m, int lv)
{
	return tex_plan_blocks(PSM_T8, m->width, m->height, (u32)lv, (u32)lv);
}

// The images drawn outside the batches in the previous frame (translucent planes and walls, the sky dome): they are part of this frame's working
// set as well, but their polygons are not known when the batches are planned. They join the budget with what the last frame measured.
#define IMM_MAX 16
static GLMipmap_t *imm_cur[IMM_MAX], *imm_prev[IMM_MAX];
static int imm_ncur, imm_nprev;
static float imm_ptotal[IMM_MAX], imm_pseps[IMM_MAX];

// the engine drops its textures (a new level, a cache flush): nothing the plan remembers may be touched again
static void plan_reset(void)
{
	plan_n = 0;
	plan_frame = 0;
	imm_ncur = imm_nprev = 0;
	plan_active = 0;
}

// one texture is deleted: the plan forgets it (its record in this frame's table, the carried immediate images)
static void plan_forget(const GLMipmap_t *m)
{
	const GLMipmap_t *k = m->ps2_twin ? m->ps2_twin : m;
	int i;

	for (i = 0; i < plan_n; i++)
		if (plan_info[i].key == k)
		{
			plan_info[i].total = 0.0f; // not visible: nothing is planned for it
			memset(plan_info[i].hist, 0, sizeof plan_info[i].hist);
		}
	for (i = 0; i < imm_nprev; i++)
		if (imm_prev[i] == k)
			imm_prev[i] = imm_prev[--imm_nprev], imm_ptotal[i] = imm_ptotal[imm_nprev], imm_pseps[i] = imm_pseps[imm_nprev], i--;
	for (i = 0; i < imm_ncur; i++)
		if (imm_cur[i] == k)
			imm_cur[i] = imm_cur[--imm_ncur], i--;
}

// called at the end of every frame: the immediate images of this frame are the carried ones of the next
static void plan_frame_end(void)
{
	int i;

	// the controller of the budget: images of the frame that were evicted within the frame mean the working set does not fit the pool (or its pieces)
	if (!H.plan_pct)
		H.plan_pct = PLAN_PCT_MAX;
	if (H.thrash_blk >= PLAN_THRASH_BLK)
	{
		H.plan_pct = H.plan_pct > PLAN_PCT_MIN + PLAN_PCT_DOWN ? H.plan_pct - PLAN_PCT_DOWN : PLAN_PCT_MIN;
		H.plan_calm = 0;
	}
	else if (++H.plan_calm >= PLAN_CALM_FRAMES && H.plan_pct < PLAN_PCT_MAX)
	{
		H.plan_pct++;
		H.plan_calm = 0;
	}
	TX.thrash_n += H.thrash_n;
	TX.thrash_blk += H.thrash_blk;
	H.thrash_n = H.thrash_blk = 0;

	for (i = 0; i < imm_ncur; i++)
	{
		GLMipmap_t *k = imm_cur[i];
		const planinfo_t *pi = (k->ps2_planfr == H.frame_no + 1 && k->ps2_pi < plan_n) ? &plan_info[k->ps2_pi] : NULL;

		imm_prev[i] = k;
		imm_ptotal[i] = pi ? pi->total : 0.0f;
		imm_pseps[i] = pi ? plan_seps(pi) : 0.0f;
		if (pi && pi->smax >= 1.0e5f)
			imm_pseps[i] = 1.0e6f; // not measurable (the sky dome): never relaxed
	}
	imm_nprev = imm_ncur;
	imm_ncur = 0;
}

// Resolve the levels of every image of the plan: exact first, then the budget: the images are stored coarser, one level at a time, where it costs the
// least (area of the image that is blurred more) per block that it saves, up to one level beyond the relaxed tolerance for the big ones.
void PS2HWD_PlanEnd(void)
{
	enum { ENT_MAX = PLAN_MAX + IMM_MAX };
	static GLMipmap_t *key[ENT_MAX];
	static float tot[ENT_MAX];
	static u8 lx[ENT_MAX], cap[ENT_MAX];
	static u32 npl[ENT_MAX];
	static u32 blk_l[ENT_MAX][8]; // blocks of an entry at level l (filled for lx..cap + 1 when the plan is over budget)
	u32 budget, sum = 0, exact_sum;
	int i, n = 0, step;

	if (!plan_active)
		return;
	plan_active = 0;
	TX.plan_cyc += cyc() - plan_t0; // the measuring of the polygons (PlanPolygon) is the time between Begin and End
	if (!H.plan_pct)
		H.plan_pct = PLAN_PCT_MAX;
	budget = H.pool_blocks / 100u * H.plan_pct;
	if (budget > H.plan_reserve)
		budget -= H.plan_reserve;
	for (i = 0; i < plan_n; i++)
	{
		GLMipmap_t *k = plan_info[i].key;
		float s = plan_seps(&plan_info[i]);
		int lm = plan_lmax(k), le, lrx, full;

		key[n] = k;
		npl[n] = plan_info[i].npoly;
		tot[n] = plan_info[i].total;
		if (!(plan_info[i].total > 0.0f))
		{
			lx[n] = cap[n] = 0;
			n++;
			continue;
		}
		le = lod_level_for(s, PLAN_TOL_EXACT);
		lrx = lod_level_for(s, PLAN_TOL_RELAX);
		lx[n] = (u8)(le > lm ? lm : le);
		cap[n] = (u8)(lrx > lm ? lm : lrx);
		if (cap[n] < lx[n])
			cap[n] = lx[n];
		full = k->ps2_full_fr && H.frame_no + 1 - k->ps2_full_fr < PLAN_FULL_KEEP;
		if (full)
			lx[n] = cap[n] = 0; // something draws it at full size without knowing its geometry (the sky dome): that is what it costs
		sum += plan_blocks(k, lx[n]);
		if (!full && plan_blocks(k, lx[n]) >= PLAN_BIG_BLOCKS && lx[n] + PLAN_BIG_EXTRA <= lm)
			cap[n] = (u8)(lx[n] + PLAN_BIG_EXTRA); // a big image may go further when the budget needs it
		n++;
	}
	for (i = 0; i < imm_nprev && n < ENT_MAX; i++)
	{
		GLMipmap_t *k = imm_prev[i];
		int lm = plan_lmax(k), le, lrx, j;

		for (j = 0; j < n && key[j] != k; j++)
			;
		if (j < n || !(imm_ptotal[i] > 0.0f))
			continue; // planned in this frame already / nothing was seen
		key[n] = k;
		npl[n] = 0;
		tot[n] = imm_ptotal[i];
		le = imm_pseps[i] >= 1.0e5f ? 0 : lod_level_for(imm_pseps[i], PLAN_TOL_EXACT);
		lrx = imm_pseps[i] >= 1.0e5f ? 0 : lod_level_for(imm_pseps[i], PLAN_TOL_RELAX);
		lx[n] = (u8)(le > lm ? lm : le);
		cap[n] = (u8)(lrx > lm ? lm : lrx);
		if (cap[n] < lx[n])
			cap[n] = lx[n];
		sum += plan_blocks(k, lx[n]);
		if (imm_pseps[i] < 1.0e5f && plan_blocks(k, lx[n]) >= PLAN_BIG_BLOCKS && lx[n] + PLAN_BIG_EXTRA <= lm)
			cap[n] = (u8)(lx[n] + PLAN_BIG_EXTRA); // (the sky dome, whose polygons are not known, stays)
		n++;
	}
	exact_sum = sum;
	if (sum > budget)
	{
		// the blocks of every image at the levels the loop below looks at, made once (tex_footprint per entry and step was 0.08 M cycles a frame)
		for (i = 0; i < n; i++)
		{
			int l;

			for (l = lx[i]; l <= cap[i] + 1 && l < 8; l++)
				blk_l[i][l] = plan_blocks(key[i], l);
		}
	}
	// over the budget: coarser by one level where a block saved costs the least blurred area
	for (step = 0; sum > budget && step < 3 * ENT_MAX; step++)
	{
		int best = -1;
		float bscore = 0.0f;
		u32 bsave = 0;

		for (i = 0; i < n; i++)
		{
			u32 save;
			float score;

			if (!(tot[i] > 0.0f) || lx[i] >= cap[i])
				continue;
			save = blk_l[i][lx[i]] - blk_l[i][lx[i] + 1];
			if (!save)
				continue;
			score = (float)save / tot[i]; // blocks saved per pixel of blurred area
			if (score > bscore)
			{
				bscore = score;
				bsave = save;
				best = i;
			}
		}
		if (best < 0)
			break;
		sum -= bsave;
		lx[best]++;
	}
	if ((ps2hwd_dbg_flags & HWDBG_LODDBG) && H.frame_no % 105 == 60)
		for (i = 0; i < n; i++)
		{
			const GLMipmap_t *k = key[i];
			const texrec_t *r = rec_of(k);

			if (!r && k->nextcolormap)
				r = rec_of(k->nextcolormap);
			CONS_Printf("HWPLAN %s %ux%u n=%u area=%.0f cap=%d want=%d blk=%u stored=%s lvl %d blk %u%s\n", HWR_PS2_TexName(k), (unsigned)k->width, (unsigned)k->height, (unsigned)npl[i], tot[i], (int)cap[i], (int)lx[i],
				(unsigned)plan_blocks(k, lx[i]), r ? "yes" : "no", r ? (int)r->dx : -1, r ? (unsigned)r->nblk : 0u, i >= plan_n ? " (immediate, carried)" : "");
		}
	H.plan_last = sum; // diagnostics: the planned working set of the last plan
	H.plan_exact = exact_sum;
	for (i = 0; i < n; i++)
	{
		if (i < plan_n)
		{
			key[i]->ps2_want = tot[i] > 0.0f ? lx[i] : 0;
		}
		else
		{
			key[i]->ps2_ihint = lx[i];
			key[i]->ps2_ihfr = H.frame_no + 1;
		}
	}
}

static int imm_level = -1; // level asked for by imm_prepare while settex_now runs (-1 = the plan decides)

// the mip level texture m is stored at when it is made resident now (0 = full size), and whether any polygon of the frame sees it
static u32 tex_want(const GLMipmap_t *m, int *visible)
{
	const GLMipmap_t *k = m->ps2_twin ? m->ps2_twin : m;

	*visible = 1;
	if (imm_level >= 0)
		return (u32)imm_level;
	if (k->ps2_planfr != H.frame_no + 1 || batch_phase != 2 || k->ps2_pi == 0xFFFFu)
		return 0;
	if (k->ps2_full_fr && H.frame_no + 1 - k->ps2_full_fr < PLAN_FULL_KEEP)
		return 0;
	if (!k->ps2_vis)
	{
		*visible = 0;
		return 0;
	}
	return k->ps2_want == 255 ? 0 : k->ps2_want;
}

// an image stored at level `have` is too coarse for the plan (want) beyond the tolerance: it has to be made again at a finer level
static int plan_too_coarse(const GLMipmap_t *m, u32 have, u32 want)
{
	const GLMipmap_t *k = m->ps2_twin ? m->ps2_twin : m;

	if (have <= want || k->ps2_planfr != H.frame_no + 1 || k->ps2_pi >= plan_n)
		return have > want;
	return plan_seps(&plan_info[k->ps2_pi]) * (float)(1u << have) > PLAN_TOL_KEEP;
}

// an image stored finer than the plan wants costs pool blocks for nothing: it is made again at the planned level when that saves a lot and the texels
// of the full size are at hand (the driver's data cache) or the saving is big
static int plan_too_fine(const GLMipmap_t *m, const texrec_t *r, u32 want)
{
	u32 saved;

	if (r->screen || want < (u32)r->dx + 2u || !r->sw)
		return 0;
	saved = rec_blocks(r) - tex_plan_blocks(PSM_T8, r->sw, r->sh, want, want);
	if (rec_blocks(r) < tex_plan_blocks(PSM_T8, r->sw, r->sh, want, want) || saved < PLAN_DOWN_BLOCKS)
		return 0;
	return saved >= 4u * PLAN_DOWN_BLOCKS || dc_find(dc_key(m), r->sw, r->sh, 0) != NULL;
}

// ---- immediate mode (PS2-HW-37) ----
// A big map texture that is drawn outside the batches (the translucent planes and walls of the draw nodes, the sky dome) is selected before its
// polygon is known. hw_SetTexture only remembers it (H.imm_tex); the polygon is measured when it is drawn and the texture is made resident at the
// level that polygon (and the others drawn so far in the frame) need.

static void settex_now(GLMipmap_t *TexInfo); // ps2_hwd.c

// verts == NULL: the geometry is not known (the sky dome): full size
static void imm_prepare(const FOutVector *verts, unsigned int n)
{
	GLMipmap_t *m = H.imm_tex, *k;
	planinfo_t *pi;
	int lv, lm;

	u32 t0 = cyc();

	if (!m)
		return;
	k = m->ps2_twin ? m->ps2_twin : m;
	pi = plan_key(m);
	H.cur_missing = 0;
	if (k->ps2_imfr != H.frame_no + 1)
	{
		k->ps2_imfr = H.frame_no + 1;
		if (imm_ncur < IMM_MAX)
			imm_cur[imm_ncur++] = k;
	}
	if (verts)
	{
		int i, cnt;

		xf_update();
		cnt = (n < 3 || n > PS2HWD_MAXPOLY) ? 0 : plan_measure(m, verts, n);
		if (!cnt)
		{
			H.cur_tex = NOREC;
			H.cur_missing = 1; // nothing of this polygon is visible: nothing is drawn, nothing is uploaded
			TX.invisible++;
			return;
		}
		for (i = 0; pi && i < cnt; i++)
			plan_add(pi, pm_s[i], pm_a[i]);
		lv = pi ? lod_level_for(plan_seps(pi), PLAN_TOL_EXACT) : 0;
	}
	else
	{
		if (pi)
			plan_add(pi, 1.0e6f, 1.0f);
		k->ps2_full_fr = H.frame_no + 1; // drawn at full size, whatever else says: the batches of the frame must not make it coarser (and back, every frame)
		lv = 0;
	}
	k->ps2_vis = 1;
	if (verts && k->ps2_ihfr && H.frame_no + 1 - k->ps2_ihfr <= 2)
		lv = k->ps2_ihint; // the plan of the whole previous frame (with the budget) decides: the polygons seen so far in this frame would make the level flip from polygon to polygon
	lm = plan_lmax(k);
	imm_level = lv > lm ? lm : lv;
	if (!verts)
		imm_level = 0;
	settex_now(m);
	imm_level = -1;
	TX.imm_cyc += cyc() - t0;
}

// ---- sprites that draw nothing (PS2-HW-72) ----
// The GS puts a pixel on the screen only when the centre of the pixel is inside the primitive (in this driver's coordinates: the integer points, see P.ox:
// the -0.5 shift). A sprite quad that lies between pixel centres, or outside the view, draws nothing, and everything the engine and the driver do for it
// (vissprite, sort, patch lookup, polygon set-up, packing) is wasted: 4000+ EE cycles. The engine asks here for the quad it would draw (the four vertices
// as HWR_DrawSprite hands them to HWR_ProcessPolygon); true = it cannot show a pixel: all four vertices lie outside one side of the view volume, or the
// screen box of the quad contains no pixel centre (quad_no_centre: widened by 0.1 pixel, the GS snaps vertices to 1/16 pixel). A quad that reaches the near plane is
// never called hidden (it is clipped by the driver).
#include "ps2_hw_quad.h" // quad_no_centre (host test tools/ps2/hw_quad_hosttest.py)

extern unsigned int hwr_geo_off; // -hwgo (hardware/hw_gcache.inc): the OPT11 engine side optimisations that are switched off

static int quad_hidden_ref(const void *vp)
{
	const FOutVector *v = (const FOutVector *)vp;
	float x0 = 1.0e30f, x1 = -1.0e30f, y0 = 1.0e30f, y1 = -1.0e30f, hx, hy, cx, cy;
	float sx[4], sy[4];
	int oc[4], k, and_ = 0x3F;
	// OPT11 (PS2-HW-86): the corners 0 and 2 first. When they are 8 pixels apart in x or y and not both outside one plane, the answer of the four corner test is "not hidden"
	// for sure (the box of all four is as big, the AND of the four outcodes is 0, and a corner that is too near the eye gives "not hidden" as well): the other two corners are
	// not transformed. The order of the transforms is the only difference from the loop of PS2-HW-72; -hwgo 4: the four corners always.
	static const unsigned char ord[4] = {0, 2, 1, 3};

	if (!H.up || H.vpw < 1.0f || H.vph < 1.0f || (ps2hwd_dbg_flags & HWDBG_NOPLAN))
		return 0;
	xf_update();
	hx = H.vpw * 0.5f;
	hy = H.vph * 0.5f;
	cx = H.vpx + hx - 0.5f;
	cy = H.vpy + hy - 0.5f;
	spr_hid_test++;
	for (k = 0; k < 4; k++)
	{
		cv_t c;
		float iw;
		const int i = ord[k];

		xform_vertex(&v[i], NULL, &c);
		if (c.w < 2.0f * H.near_plane)
			return 0; // near the eye (or behind it): the clipper decides
		oc[k] = outcode(&c, 1.0f, 1.0f);
		and_ &= oc[k];
		iw = 1.0f / c.w;
		sx[k] = cx + c.x * iw * hx;
		sy[k] = cy - c.y * iw * hy;
		if (k == 1 && !(hwr_geo_off & 4) && !(oc[0] & oc[1]) && (fabsf(sx[1] - sx[0]) > 8.0f || fabsf(sy[1] - sy[0]) > 8.0f))
			return 0;
	}
	for (k = 0; k < 4; k++)
	{
		if (sx[k] < x0)
			x0 = sx[k];
		if (sx[k] > x1)
			x1 = sx[k];
		if (sy[k] < y0)
			y0 = sy[k];
		if (sy[k] > y1)
			y1 = sy[k];
	}
	if (and_)
	{
		spr_hid_yes++;
		return 1; // wholly outside the view volume
	}
	if (x1 - x0 > 8.0f || y1 - y0 > 8.0f)
		return 0; // big: it covers a centre for sure
	if (quad_no_centre(x0, x1, y0, y1))
	{
		spr_hid_yes++;
		return 1; // no pixel centre in the box
	}
	return 0;
}

// ---- PS2-HW-220 (OPT11 round 2, VU2): the four corners on VU0 ----
// The sprite test of the front end (1100..1300 calls a frame in DEMO_004, two thirds of them "hidden") cost 460 cycles a call: four scalar transforms (the early
// out of PS2-HW-86 helps only the big visible ones). Here the four corners go through VU0 in macro mode at once: the multiply chains of the four corners, the four
// clip tests (|x|, |y|, |z| against |w|: the screen frustum, one 24 bit flag history), the clip coordinates back to memory. The matrix lives in vf21..vf24 (the registers
// of the plans, vf1..vf18, are not touched: the plan of the draw that is going on stays loaded), loaded again only when the transform changed (xf_ver).
// VU0 arithmetic truncates: a corner that lies on a clip plane within 1e-7 may change sides; the answer is "hidden" only for a quad that holds no pixel centre
// either way (the box is widened by 0.1 pixel), so no pixel can come from such a quad. -hwqh 1: the scalar test of PS2-HW-72/86; -hwqh 2: both, the answers
// compared (HWSPRCULL prints the number of differences).

#ifdef HW_VU0
static u32 qh_ver = ~0u;

static int quad_hidden_vu0(const FOutVector *v)
{
	static float in[16] __attribute__((aligned(16))) = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
	static float cl[16] __attribute__((aligned(16)));
	float hx, hy, cx, cy, nw, x0, x1, y0, y1;
	float sx, sy, iw;
	u32 f;
	int k;

	xf_update();
	if (qh_ver != xf_ver)
	{
		const float *m = H.mvp;

		__asm__ volatile(
			"lqc2 $vf21, 0x00(%0)\n"
			"lqc2 $vf22, 0x10(%0)\n"
			"lqc2 $vf23, 0x20(%0)\n"
			"lqc2 $vf24, 0x30(%0)\n"
			: : "r"(m) : "memory");
		qh_ver = xf_ver;
	}
	in[0] = v[0].x;
	in[1] = v[0].y;
	in[2] = v[0].z;
	in[4] = v[1].x;
	in[5] = v[1].y;
	in[6] = v[1].z;
	in[8] = v[2].x;
	in[9] = v[2].y;
	in[10] = v[2].z;
	in[12] = v[3].x;
	in[13] = v[3].y;
	in[14] = v[3].z;
	__asm__ volatile(
		"lqc2 $vf25, 0x00(%1)\n"
		"lqc2 $vf26, 0x10(%1)\n"
		"lqc2 $vf27, 0x20(%1)\n"
		"lqc2 $vf28, 0x30(%1)\n"
		"vmulaw $ACC, $vf24, $vf0\n"
		"vmaddax $ACC, $vf21, $vf25\n"
		"vmadday $ACC, $vf22, $vf25\n"
		"vmaddz $vf29, $vf23, $vf25\n"
		"vmulaw $ACC, $vf24, $vf0\n"
		"vmaddax $ACC, $vf21, $vf26\n"
		"vmadday $ACC, $vf22, $vf26\n"
		"vmaddz $vf30, $vf23, $vf26\n"
		"vmulaw $ACC, $vf24, $vf0\n"
		"vmaddax $ACC, $vf21, $vf27\n"
		"vmadday $ACC, $vf22, $vf27\n"
		"vmaddz $vf31, $vf23, $vf27\n"
		"vmulaw $ACC, $vf24, $vf0\n"
		"vmaddax $ACC, $vf21, $vf28\n"
		"vmadday $ACC, $vf22, $vf28\n"
		"vmaddz $vf20, $vf23, $vf28\n"
		"vclipw.xyz $vf29, $vf29\n"
		"vclipw.xyz $vf30, $vf30\n"
		"vclipw.xyz $vf31, $vf31\n"
		"vclipw.xyz $vf20, $vf20\n"
		"sqc2 $vf29, 0x00(%2)\n"
		"sqc2 $vf30, 0x10(%2)\n"
		"sqc2 $vf31, 0x20(%2)\n"
		"sqc2 $vf20, 0x30(%2)\n"
		"cfc2 %0, $vi18\n"
		: "=&r"(f) : "r"(in), "r"(cl) : "memory");
	nw = 2.0f * H.near_plane;
	if (cl[3] < nw || cl[7] < nw || cl[11] < nw || cl[15] < nw)
	{
		qh_near++;
		return 0; // near the eye (or behind it): the clipper decides
	}
	if (f & (f >> 6) & (f >> 12) & (f >> 18) & 0x3Fu)
	{
		qh_and++;
		spr_hid_yes++;
		return 1; // wholly outside the view volume
	}
	hx = H.vpw * 0.5f;
	hy = H.vph * 0.5f;
	cx = H.vpx + hx - 0.5f;
	cy = H.vpy + hy - 0.5f;
	x0 = y0 = 1.0e30f;
	x1 = y1 = -1.0e30f;
	for (k = 0; k < 4; k++)
	{
		iw = 1.0f / cl[4 * k + 3];
		sx = cx + cl[4 * k] * iw * hx;
		sy = cy - cl[4 * k + 1] * iw * hy;
		x0 = hw_minf(x0, sx);
		x1 = hw_maxf(x1, sx);
		y0 = hw_minf(y0, sy);
		y1 = hw_maxf(y1, sy);
	}
	if (x1 - x0 > 8.0f || y1 - y0 > 8.0f)
	{
		qh_big++;
		return 0; // big: it covers a centre for sure
	}
	if (quad_no_centre(x0, x1, y0, y1))
	{
		qh_nocen++;
		spr_hid_yes++;
		return 1; // no pixel centre in the box
	}
	qh_vis++;
	return 0;
}
#endif

int PS2HWD_QuadHidden(const void *vp)
{
#ifdef HW_VU0
	if (!(qh_mode & 1) && !(ps2hwd_dbg_flags & 128))
	{
		int r;

		if (!H.up || H.vpw < 1.0f || H.vph < 1.0f || (ps2hwd_dbg_flags & HWDBG_NOPLAN))
			return 0;
		spr_hid_test++;
		r = quad_hidden_vu0((const FOutVector *)vp);
		if (qh_mode & 2)
		{
			const unsigned t = spr_hid_test, y = spr_hid_yes;
			const int ref = quad_hidden_ref(vp);

			spr_hid_test = t;
			spr_hid_yes = y;
			if (ref != r)
				qh_diff++;
		}
		return r;
	}
#endif
	return quad_hidden_ref(vp);
}
