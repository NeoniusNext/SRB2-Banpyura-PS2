// PS2 GS hardware renderer driver: the frame plan of the batches (OPT9 PS2-HW-34, redone in OPT10 PS2-HW-37); included by ps2_hwd.c after ps2_hw_draw.inc.
//
// HWR_RenderBatches (hardware/hw_batching.c) shows the driver every collected polygon before the first of them is drawn. For the big map
// textures and flats (the ones that do not fit the pool in numbers: the 1024 x 1024 cloud planes of GFZ1 alone are 4096 blocks each, the pool 12032)
// the driver measures what the VIEW sees of each polygon: the polygon is transformed and clipped to the viewport (not to the guard band: what
// lies outside the screen costs nothing and must not count), and at every vertex of the clipped polygon the screen size of one texel along u and
// v is taken from the plane's mapping (poly_plane / poly_density, ps2_hw_tex.inc). The largest texel any polygon of the frame shows of a texture
// decides its mip level: level L is enough when no texel is magnified compared with the full size (texel * 2^L <= 1 pixel, the exact plan),
// so a plane that is seen far away, a wall at a grazing angle, the sky show the same detail the PC driver shows. A polygon that has no area (a
// zero-height wall piece) shows nothing and counts for nothing; a texture none of whose polygons can be seen is not uploaded at all.
// When the exact plan does not fit the budget (a share of the pool) the images that save the most blocks are stored one level coarser, which
// magnifies a texel at most 2:1 (the tolerance of PS2-HW-37).
// hw_SetTexture stores the texture at the planned level (tex_want); a texture that is drawn without a plan (not batched) is stored at full size
// and stays so for a while (ps2_full_fr).

static int plan_active;

#define PLAN_MAX 256 // images of one frame's plan (more are stored at full size)
static GLMipmap_t *plan_list[PLAN_MAX];
static int plan_n;
static u32 plan_frame; // frame_no + 1 the list belongs to (a frame can plan twice: the skybox view and the main view)

#define PLAN_BUDGET_NUM 3 // the planned images of a frame may take this share of the pool: 3/4 (the rest is for sprites, patches, the small textures)
#define PLAN_BUDGET_DEN 4
#define PLAN_TOL_EXACT 1.0f // texel magnification allowed by the exact plan
#define PLAN_TOL_RELAX 2.0f // ... and by the relaxed plan (images stored one level coarser to fit the budget)
#define PLAN_TOL_KEEP 1.5f // an image stored at a coarser level than the plan asks for is kept while it magnifies at most this much (no flicker between levels)

void PS2HWD_PlanBegin(void)
{
	plan_active = H.up && !(ps2hwd_dbg_flags & HWDBG_NOPLAN);
	if (plan_active)
	{
		xf_update();
		if (plan_frame != H.frame_no + 1)
		{
			plan_frame = H.frame_no + 1;
			plan_n = 0;
		}
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

// twice the area of the clipped polygon in pixels squared (a polygon without area draws nothing)
static float plan_area2(const cv_t *c, int n)
{
	float px = 0.5f * H.vpw, py = 0.5f * H.vph, sum = 0.0f;
	int i;

	for (i = 0; i < n; i++)
	{
		const cv_t *a = &c[i], *b = &c[i + 1 == n ? 0 : i + 1];
		float ax = a->x / a->w * px, ay = a->y / a->w * py, bx = b->x / b->w * px, by = b->y / b->w * py;

		sum += ax * by - bx * ay;
	}
	return fabsf(sum);
}

void PS2HWD_PlanPolygon(GLMipmap_t *m, const void *vp, unsigned int n)
{
	const FOutVector *verts = (const FOutVector *)vp;
	GLMipmap_t *k;
	cv_t *v;
	int i, cnt, and_ = 0x3F, or_ = 0, measured;
	float su, sv;

	if (!plan_active || !m || n < 3 || n > PS2HWD_MAXPOLY || (m->regen_kind != 1 && m->regen_kind != 2) || m->format != GL_TEXFMT_P_8
		|| (u32)m->width * m->height < PLAN_MIN_TEXELS)
		return;
	k = m->ps2_twin ? m->ps2_twin : m;
	if (k->ps2_planfr != H.frame_no + 1)
	{
		k->ps2_planfr = H.frame_no + 1;
		k->ps2_want = 255;
		k->ps2_vis = 0;
		k->ps2_su = k->ps2_sv = 0.0f;
		if (plan_n < PLAN_MAX)
			plan_list[plan_n++] = k;
		else
			k->ps2_su = k->ps2_sv = 1.0e6f; // not resolved by the plan: full size
	}
	if (k->ps2_su >= PLAN_TOL_RELAX && k->ps2_sv >= PLAN_TOL_RELAX)
	{
		k->ps2_vis = 1; // full size whatever the other polygons say: no need to measure this one
		return;
	}
	for (i = 0; i < (int)n; i++)
	{
		int oc;

		xform_vertex(&verts[i], NULL, &vbuf_a[i]);
		oc = vbuf_a[i].w <= 0.0f ? 0x3F : outcode(&vbuf_a[i], 1.0f, 1.0f);
		and_ &= oc;
		or_ |= oc;
	}
	if (and_)
		return; // wholly outside the view volume: it draws nothing
	measured = 0;
	poly_plane(vbuf_a, (int)n, (float)m->width, (float)m->height); // before the clipping reuses the buffer
	v = vbuf_a;
	cnt = (int)n;
	if (or_)
	{
		cnt = plan_clip(vbuf_a, vbuf_b, (int)n, or_, &v);
		if (cnt < 3)
			return;
	}
	if (plan_area2(v, cnt) < 0.05f)
		return; // no area: it draws nothing
	measured = poly_density(v, cnt);
	su = measured ? lod_su : 1.0e6f; // a polygon that cannot be measured (its texture mapping is degenerate) is drawn at full size
	sv = measured ? lod_sv : 1.0e6f;
	if ((ps2hwd_dbg_flags & HWDBG_LODDBG) && H.frame_no % 105 == 60)
	{
		CONS_Printf("HWLOD %s %ux%u n=%u cnt=%d su=%.3f sv=%.3f\n", HWR_PS2_TexName(m), (unsigned)m->width, (unsigned)m->height, n, cnt, su, sv);
		if (!measured)
			for (i = 0; i < cnt; i++)
				CONS_Printf("HWLOD   v%d x=%.1f y=%.1f w=%.2f s=%.3f t=%.3f\n", i, v[i].x, v[i].y, v[i].w, v[i].a[0], v[i].a[1]);
	}
	k->ps2_vis = 1;
	if (su > k->ps2_su)
		k->ps2_su = su;
	if (sv > k->ps2_sv)
		k->ps2_sv = sv;
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

// Resolve the levels of every image of the plan: exact first, then the budget (the images that save the most go one level coarser).
void PS2HWD_PlanEnd(void)
{
	u32 budget, sum = 0, exact_sum;
	int i, step;
	static u8 lx[PLAN_MAX], lr[PLAN_MAX];

	if (!plan_active)
		return;
	plan_active = 0;
	budget = H.pool_blocks / PLAN_BUDGET_DEN * PLAN_BUDGET_NUM;
	if (budget > H.plan_reserve)
		budget -= H.plan_reserve;
	for (i = 0; i < plan_n; i++)
	{
		GLMipmap_t *k = plan_list[i];
		float s = k->ps2_su > k->ps2_sv ? k->ps2_su : k->ps2_sv;
		int lm = plan_lmax(k), le, lrx;

		if (!k->ps2_vis)
		{
			lx[i] = lr[i] = 0;
			continue;
		}
		le = lod_level_for(s, PLAN_TOL_EXACT);
		lrx = lod_level_for(s, PLAN_TOL_RELAX);
		lx[i] = (u8)(le > lm ? lm : le);
		lr[i] = (u8)(lrx > lm ? lm : lrx);
		sum += plan_blocks(k, lx[i]);
	}
	exact_sum = sum;
	// over the budget: relax the images by the blocks they save, the largest saving first (a few passes: n is small and so is the number of candidates)
	for (step = 0; sum > budget && step < PLAN_MAX; step++)
	{
		int best = -1;
		u32 bsave = 0;

		for (i = 0; i < plan_n; i++)
		{
			u32 save;

			if (!plan_list[i]->ps2_vis || lx[i] >= lr[i])
				continue;
			save = plan_blocks(plan_list[i], lx[i]) - plan_blocks(plan_list[i], lx[i] + 1);
			if (save > bsave)
			{
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
		for (i = 0; i < plan_n; i++)
		{
			const GLMipmap_t *k = plan_list[i];
			const texrec_t *r = rec_of(k);

			if (!r && k->nextcolormap)
				r = rec_of(k->nextcolormap);
			CONS_Printf("HWPLAN %s %ux%u su=%.3f sv=%.3f vis=%d exact=%d relax=%d want=%d blk=%u stored=%s lvl %d blk %u\n", HWR_PS2_TexName(k), (unsigned)k->width, (unsigned)k->height, k->ps2_su, k->ps2_sv,
				(int)k->ps2_vis, (int)lod_level_for(k->ps2_su > k->ps2_sv ? k->ps2_su : k->ps2_sv, PLAN_TOL_EXACT), (int)lr[i], (int)lx[i], (unsigned)plan_blocks(k, lx[i]), r ? "yes" : "no", r ? (int)r->dx : -1, r ? (unsigned)r->nblk : 0u);
		}
	H.plan_last = sum; // diagnostics: the planned working set of the last plan
	H.plan_exact = exact_sum;
	for (i = 0; i < plan_n; i++)
		plan_list[i]->ps2_want = plan_list[i]->ps2_vis ? lx[i] : 0;
}

// the mip level texture m is stored at when it is made resident now (0 = full size), and whether any polygon of the frame sees it
static u32 tex_want(const GLMipmap_t *m, int *visible)
{
	const GLMipmap_t *k = m->ps2_twin ? m->ps2_twin : m;

	*visible = 1;
	if (k->ps2_planfr != H.frame_no + 1 || batch_phase != 2)
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
	float s = k->ps2_su > k->ps2_sv ? k->ps2_su : k->ps2_sv;

	if (have <= want)
		return 0;
	return s * (float)(1u << have) > PLAN_TOL_KEEP;
}
