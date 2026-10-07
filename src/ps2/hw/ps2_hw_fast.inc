// PS2 GS hardware renderer driver: the common lit polygon (included by ps2_hw_draw.inc in front of emit_poly). PS2-HW-43 (OPT9, HG).
//
// With the shader lighting of PS2-HW-61 almost every world polygon has P.lmode == LM_BANDS: cut_and_emit() computes the depth thresholds
// of the light staircase, finds none (a polygon rarely spans two darkness classes), and emit_piece() sets the GS state up again and packs
// the vertices with a second, scalar transform. This function takes the polygon that
//   * needs no texture repeat cut,
//   * lies in ONE darkness class,
// straight to the packer, and sets the GS state up only when it changed (P.serial). An unclipped polygon takes the GS integers VU0 already
// made (the results of the transform are in pvbuf); a clipped one (PS2-HW-62: it used to go the general way for that reason alone) is
// packed with the scalar projection of emit_reglist_fan / put_vertex, exactly as emit_piece does it. Everything else goes the general way,
// which the checks here mirror exactly (cut_and_emit stage 2 -> emit_piece).
// The class function is doom_class() with the lighting constants of the plan precomputed (same operations in the same order, no floorf).

static struct
{
	float base; // startmap * sfudge
	float cap;
	float cf; // cfudge
	u64 have; // bit c: fog[c] / fogon[c] are valid for the plan
	u8 fog[33];
	u8 fogon[33];
	u64 thz_have; // bit m: thz[m] = doom_depth_of_level(lp, m) is valid for the plan (PS2-HW-62)
	float thz[33];
	u64 known; // bit c: [lo[c], hi[c]] is a range of eye depths (w) proven to be in class c (PS2-HW-60)
	float lo[33], hi[33];
	int last;
	int ok; // base / cap / cf are valid for the plan (they are calculated when the first class is asked for: a plan often draws nothing that needs them)
} LF;

static void lf_params(void)
{
	const lightp_t *lp = &P.rs.lp;
	float lightnum = lp->light / 17.0f, startmap;

	lightnum = lightnum < 0.0f ? 0.0f : lightnum > 15.0f ? 15.0f : lightnum;
	startmap = (15.0f - lightnum) * 4.0f;
	LF.base = startmap * lp->sfudge;
	LF.cap = (155.0f - lp->light) * 0.26f;
	LF.cf = lp->cfudge;
	LF.ok = 1;
}

static void lit_fast_plan(void)
{
	LF.ok = 0;
	LF.have = 0;
	LF.thz_have = 0;
	LF.known = 0;
	LF.last = 0;
}

// doom_class(&P.rs.lp, z)
static inline int lf_class(float z)
{
	float lightz = z / 16.0f, scale, cm;

	if (!LF.ok)
		lf_params();
	lightz = lightz < 0.0f ? 0.0f : lightz > 127.0f ? 127.0f : lightz;
	scale = 160.0f / (lightz + 1.0f);
	cm = lmaxf(LF.base - scale * 0.5f * LF.cf, LF.cap);
	if (cm < 0.0f)
		return 0;
	if (cm >= 31.0f)
		return 32;
	return (int)cm + 1;
}

// lf_class(zfrag_of_w(w)) with a memory: the class is a non-decreasing function of w, so when two eye depths of the plan had class c,
// every depth in between has it too. A polygon almost always lies between depths already seen in its class: two compares instead of
// two divisions and the class arithmetic (this was 2 % of the frame).
static int lf_class_w_slow(float w)
{
	const int c = lf_class(zfrag_of_w(w));

	if (!((LF.known >> c) & 1))
	{
		LF.known |= 1ull << c;
		LF.lo[c] = LF.hi[c] = w;
	}
	else
	{
		if (w < LF.lo[c])
			LF.lo[c] = w;
		if (w > LF.hi[c])
			LF.hi[c] = w;
	}
	LF.last = c;
	return c;
}

static inline int lf_class_w(float w)
{
	const int c = LF.last;

	if (((LF.known >> c) & 1) && w >= LF.lo[c] && w <= LF.hi[c])
		return c;
	return lf_class_w_slow(w);
}

// the fog of darkness class `cls` for the plan (fog_of_class with a memory); *on = fog is on
static u32 lf_fog(int cls, int *on)
{
	if (!((LF.have >> cls) & 1))
	{
		int o;

		LF.fog[cls] = (u8)fog_of_class(&P.rs, cls, &o);
		LF.fogon[cls] = (u8)o;
		LF.have |= 1ull << cls;
	}
	*on = LF.fogon[cls];
	return LF.fog[cls];
}

// doom_thresholds() for the classes c0 / c1 of the ends z0 / z1 (lf_class_w of the depth range; c0 != c1 here), with the depth of every class step
// remembered for the plan: the same decisions on the same doom_depth_of_level values, without the three divisions each of them costs.
static int lf_thresholds(int c0, int c1, float z0, float z1, float *th, int *cls, int max)
{
	int c, n = 0;

	for (c = c0 + 1; c <= c1; c++)
	{
		float z;

		if (!((LF.thz_have >> (c - 1)) & 1))
		{
			LF.thz[c - 1] = doom_depth_of_level(&P.rs.lp, c - 1); // class c starts where floor(colormap) reaches c - 1
			LF.thz_have |= 1ull << (c - 1);
		}
		z = LF.thz[c - 1];
		if (z <= z0)
			continue;
		if (z >= z1)
			break;
		if (n && z <= th[n - 1])
		{
			cls[n - 1] = c; // two classes begin at the same depth: the later one wins
			continue;
		}
		if (n == max)
			break;
		th[n] = z;
		cls[n] = c;
		n++;
	}
	return n;
}

#ifdef HW_VU0
#define LF_PV(vu0, or_) (((vu0) && !(or_)) ? (const void *)pvbuf : NULL)
// put_vertex() with the GS integers of the VU0 transform
static inline qw_t *put_vertex_pv(qw_t *o, const cv_t *s, const pv_t *p, const pack_t *pk)
{
	const float q = p->q;

	o->f[0] = (s->a[0] - pk->sb) * P.tsx * q;
	o->f[1] = (s->a[1] - pk->tb) * P.tsy * q;
	o->f[2] = q;
	o->w[3] = 0;
	o++;
	o->w[0] = pk->col & 255;
	o->w[1] = (pk->col >> 8) & 255;
	o->w[2] = (pk->col >> 16) & 255;
	o->w[3] = pk->col >> 24;
	o++;
	o->w[0] = (u32)p->xi;
	o->w[1] = (u32)p->yi;
	if (pk->fmt == 1)
	{
		o->w[2] = (u32)p->zi << 4;
		o->w[3] = pk->fog << 4;
	}
	else
	{
		o->w[2] = (u32)p->zi;
		o->w[3] = 0;
	}
	return o + 1;
}
#else
#define LF_PV(vu0, or_) ((const void *)NULL)
#endif

// 1 when the polygon was drawn
static int lit_fast_poly(const cv_t *v, const void *pvv, int cnt, float sb, float tb)
{
#ifdef HW_VU0
	const pv_t *pv = (const pv_t *)pvv;
#endif
	const pass_t *ps = &P.pass[0];
	float wlo = v[0].w, whi = v[0].w;
	int i, c0, usefog;
	u32 fogF;
	pack_t pk;

	(void)pvv;
	if (cnt < 3)
		return 0;
	for (i = 1; i < cnt; i++)
	{
		if (v[i].w < wlo)
			wlo = v[i].w;
		if (v[i].w > whi)
			whi = v[i].w;
	}
	c0 = lf_class_w(wlo);
	if (c0 != lf_class_w(whi))
		return 0; // spans two classes: cut_and_emit makes the pieces
	{
		int on;

		fogF = lf_fog(c0, &on);
		usefog = ps->fog && on;
	}
	if (P.serial != H.serial)
	{
		pass_setup(0, 0);
		P.serial = H.serial;
	}
	pk.sb = sb;
	pk.tb = tb;
	pk.fog = fogF;
	pk.ramp = 0;
	pk.col = ps->col;
	pk.fmt = ps->tex ? (usefog ? 1 : 0) : (usefog ? 3 : 2);
	if (pk.fmt == 0)
	{
		int unitq = 1;

		for (i = 0; i < cnt; i++)
			unitq &= v[i].w == 1.0f;
		if (unitq)
			pk.fmt = 5;
	}
	pk.nreg = pack_regs(pk.fmt);
	if (1 + cnt * pk.nreg <= 16)
	{
#ifdef HW_VU0
		if (pv)
			emit_reglist_fan_pv(v, pv, cnt, &pk, PRIM_TRIFAN | (pass_prim(ps) & ~(usefog ? 0u : (u32)PRIM_FGE)));
		else
#endif
			emit_reglist_fan(v, cnt, &pk, PRIM_TRIFAN | (pass_prim(ps) & ~(usefog ? 0u : (u32)PRIM_FGE))); // PS2-HW-62: a clipped polygon (no VU0 integers): the packer of emit_piece
	}
#ifdef HW_VU0
	else if (pv && (pk.fmt == 0 || pk.fmt == 1))
	{
		qw_t *o = pk_alloc((u32)(1 + cnt * pk.nreg));

		o[0].d[0] = HWGIF_TAG(cnt, 1, 1, PRIM_TRIFAN | (pass_prim(ps) & ~(usefog ? 0u : (u32)PRIM_FGE)), 0, pk.nreg);
		o[0].d[1] = pack_regdesc(pk.fmt);
		o++;
		for (i = 0; i < cnt; i++)
			o = put_vertex_pv(o, &v[i], &pv[i], &pk);
		H.gsr.valid &= ~(u32)RV_RGBAQ;
	}
#endif
	else
	{
		qw_t *o = pk_alloc((u32)(1 + cnt * pk.nreg));

		o[0].d[0] = HWGIF_TAG(cnt, 1, 1, PRIM_TRIFAN | (pass_prim(ps) & ~(usefog ? 0u : (u32)PRIM_FGE)), 0, pk.nreg);
		o[0].d[1] = pack_regdesc(pk.fmt);
		o++;
		for (i = 0; i < cnt; i++)
			o = put_vertex(o, &v[i], &pk);
	}
	H.st.verts_out += (u32)cnt;
	return 1;
}
