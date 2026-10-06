// PS2 GS hardware renderer driver: the common lit polygon (included by ps2_hw_draw.inc in front of emit_poly). PS2-HW-43 (OPT9, HG).
//
// With the shader lighting of PS2-HW-61 almost every world polygon has P.lmode == LM_BANDS: cut_and_emit() computes the depth thresholds
// of the light staircase, finds none (a polygon rarely spans two darkness classes), and emit_piece() sets the GS state up again and packs
// the vertices with a second, scalar transform. This function takes the polygon that
//   * was not clipped (the VU0 results of the transform are still in pvbuf),
//   * needs no texture repeat cut,
//   * lies in ONE darkness class,
// straight to the packer with the GS integers VU0 already made, and sets the GS state up only when it changed (P.serial). Everything
// else goes the general way, which the checks here mirror exactly (cut_and_emit stage 2 -> emit_piece).
// The class function is doom_class() with the lighting constants of the plan precomputed (same operations in the same order, no floorf).

static struct
{
	float base; // startmap * sfudge
	float cap;
	float cf; // cfudge
	u64 have; // bit c: fog[c] / fogon[c] are valid for the plan
	u8 fog[33];
	u8 fogon[33];
} LF;

static void lit_fast_plan(void)
{
	const lightp_t *lp = &P.rs.lp;
	float lightnum = lp->light / 17.0f, startmap;

	lightnum = lightnum < 0.0f ? 0.0f : lightnum > 15.0f ? 15.0f : lightnum;
	startmap = (15.0f - lightnum) * 4.0f;
	LF.base = startmap * lp->sfudge;
	LF.cap = (155.0f - lp->light) * 0.26f;
	LF.cf = lp->cfudge;
	LF.have = 0;
}

// doom_class(&P.rs.lp, z)
static inline int lf_class(float z)
{
	float lightz = z / 16.0f, scale, cm;

	lightz = lightz < 0.0f ? 0.0f : lightz > 127.0f ? 127.0f : lightz;
	scale = 160.0f / (lightz + 1.0f);
	cm = lmaxf(LF.base - scale * 0.5f * LF.cf, LF.cap);
	if (cm < 0.0f)
		return 0;
	if (cm >= 31.0f)
		return 32;
	return (int)cm + 1;
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

// 1 when the polygon was drawn
static int lit_fast_poly(const cv_t *v, const void *pvv, int cnt, float sb, float tb)
{
	const pv_t *pv = (const pv_t *)pvv;
	const pass_t *ps = &P.pass[0];
	float wlo = v[0].w, whi = v[0].w;
	int i, c0, usefog;
	u32 fogF;
	pack_t pk;

	if (!pv || cnt < 3)
		return 0;
	for (i = 1; i < cnt; i++)
	{
		if (v[i].w < wlo)
			wlo = v[i].w;
		if (v[i].w > whi)
			whi = v[i].w;
	}
	c0 = lf_class(zfrag_of_w(wlo));
	if (c0 != lf_class(zfrag_of_w(whi)))
		return 0; // spans two classes: cut_and_emit makes the pieces
	if (!((LF.have >> c0) & 1))
	{
		int on;

		LF.fog[c0] = (u8)fog_of_class(&P.rs, c0, &on);
		LF.fogon[c0] = (u8)on;
		LF.have |= 1ull << c0;
	}
	fogF = LF.fog[c0];
	usefog = ps->fog && LF.fogon[c0];
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
		emit_reglist_fan_pv(v, pv, cnt, &pk, PRIM_TRIFAN | (pass_prim(ps) & ~(usefog ? 0u : (u32)PRIM_FGE)));
	}
	else if (pk.fmt == 0 || pk.fmt == 1)
	{
		qw_t *o = pk_alloc((u32)(1 + cnt * pk.nreg));

		o[0].d[0] = HWGIF_TAG(cnt, 1, 1, PRIM_TRIFAN | (pass_prim(ps) & ~(usefog ? 0u : (u32)PRIM_FGE)), 0, pk.nreg);
		o[0].d[1] = pack_regdesc(pk.fmt);
		o++;
		for (i = 0; i < cnt; i++)
			o = put_vertex_pv(o, &v[i], &pv[i], &pk);
		H.gsr.valid &= ~(u32)RV_RGBAQ;
	}
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
#else
#define LF_PV(vu0, or_) ((const void *)NULL)
static int lit_fast_poly(const cv_t *v, const void *pvv, int cnt, float sb, float tb)
{
	(void)v; (void)pvv; (void)cnt; (void)sb; (void)tb;
	return 0;
}
#endif
