// PS2 GS hardware renderer driver: the sky dome (included by ps2_hwd.c after ps2_hw_draw.inc). PS2-HW-42 (OPT9, HG).
//
// The engine's dome (hw_main.c HWR_BuildSkyDome) is a fixed mesh of 1168 vertices / 1148 triangles whose only per-frame change is the
// view matrix. The old path copied the whole mesh into staging arrays, built triangle index lists and transformed, clipped and packed
// every triangle: 1.6 M EE cycles a frame. Here
//   * the mesh is converted once (vertices with their GS colour words) and kept until the engine rebuilds the dome,
//   * all vertices go through VU0 once per frame (clip coordinates, GS integers, 1/w, outcode),
//   * a quad of a strip whose four vertices are inside the guard band is "clean": runs of clean quads are sent as ONE GS triangle strip
//     (the triangles are the ones of the old index lists, k, k+1, k+2 of the strip), quads entirely outside a plane are dropped,
//     the few quads that cross the camera plane or the guard band take the old per-triangle clipper,
//   * a cap (fan) completely outside is dropped, a visible one goes triangle by triangle through the clipper.
// The pixels are the same as the old path's (same triangles, same vertex values up to the VU0 rounding the old path would have had
// on its VU0 route); sky_fast_ok() refuses meshes the shortcut cannot reproduce (texture coordinates beyond 4 repeats: the old path
// subtracts whole repeats per triangle) and the old path stays available (-hwdbg 4096).

#ifdef HW_VU0

static struct
{
	const gl_skyvertex_t *src; // identity of the converted mesh
	int n, texture, width, height, loopcount;
	FOutVector *v; // x, y, z, s, t per vertex (caps: s = t = 0)
	u32 *rgba; // GS colour words (R | G << 8 | B << 16 | A << 24)
	cv_t *c; // per frame: clip coordinates and s, t
	pv_t *p; // per frame: GS integers
	u8 *oc; // per frame: outcodes
	int cap;
	int ok; // the converted mesh can take the shortcut
} SKY;

static void sky_fast_free(void)
{
	free(SKY.v);
	free(SKY.rgba);
	free(SKY.c);
	free(SKY.p);
	free(SKY.oc);
	memset(&SKY, 0, sizeof SKY);
}

// the mesh for this dome; 0 when it cannot be converted
static int sky_fast_prepare(const gl_sky_t *sky)
{
	int i, n = sky->vertex_count, l;
	float lim = 0.0f;

	if (SKY.src == sky->data && SKY.n == n && SKY.texture == sky->texture && SKY.width == sky->width && SKY.height == sky->height && SKY.loopcount == sky->loopcount)
		return SKY.ok;
	if (n > SKY.cap)
	{
		sky_fast_free();
		SKY.v = malloc((size_t)n * sizeof *SKY.v);
		SKY.rgba = malloc((size_t)n * sizeof *SKY.rgba);
		SKY.c = memalign(16, (size_t)n * sizeof *SKY.c);
		SKY.p = memalign(16, (size_t)n * sizeof *SKY.p);
		SKY.oc = malloc((size_t)n);
		if (!SKY.v || !SKY.rgba || !SKY.c || !SKY.p || !SKY.oc)
		{
			sky_fast_free();
			return 0;
		}
		SKY.cap = n;
	}
	for (i = 0; i < n; i++)
	{
		const gl_skyvertex_t *d = &sky->data[i];

		SKY.v[i].x = d->x;
		SKY.v[i].y = d->y;
		SKY.v[i].z = d->z;
		SKY.v[i].s = d->u;
		SKY.v[i].t = d->v;
		SKY.rgba[i] = (u32)col_lut[d->r] | ((u32)col_lut[d->g] << 8) | ((u32)col_lut[d->b] << 16) | ((u32)col_lut[d->a] << 24);
		SKY.c[i].a[2] = (float)d->r;
		SKY.c[i].a[3] = (float)d->g;
		SKY.c[i].a[4] = (float)d->b;
		SKY.c[i].a[5] = (float)d->a;
	}
	SKY.ok = 1;
	for (l = 0; l < sky->loopcount; l++)
	{
		const gl_skyloopdef_t *loop = &sky->loops[l];
		int first = loop->vertexindex, cnt = loop->vertexcount;

		if (first < 0 || cnt < 3 || first > n - cnt || (loop->mode != HWD_SKYLOOP_FAN && loop->mode != HWD_SKYLOOP_STRIP) || (loop->mode == HWD_SKYLOOP_STRIP && (cnt & 1)))
		{
			SKY.ok = 0;
			break;
		}
		if (!loop->use_texture) // the caps have no texture coordinates (the OpenGL driver samples whatever is there)
			for (i = 0; i < cnt; i++)
				SKY.v[first + i].s = SKY.v[first + i].t = 0.0f;
	}
	for (i = 0; i < n && SKY.ok; i++)
	{
		if (fabsf(SKY.v[i].s) > lim)
			lim = fabsf(SKY.v[i].s);
		if (fabsf(SKY.v[i].t) > lim)
			lim = fabsf(SKY.v[i].t);
	}
	if (lim > 4.0f)
		SKY.ok = 0; // the old path drops whole repeats from the coordinates of every triangle: not reproduced here
	CONS_Printf("HWD sky: %d vertices, %d loops, texture %dx%d, max |s,t| %.2f -> %s\n", n, sky->loopcount, sky->width, sky->height, lim, SKY.ok ? "strips" : "old path");
	SKY.src = sky->data;
	SKY.n = n;
	SKY.texture = sky->texture;
	SKY.width = sky->width;
	SKY.height = sky->height;
	SKY.loopcount = sky->loopcount;
	return SKY.ok;
}

static inline qw_t *sky_put(qw_t *o, const cv_t *c, const pv_t *p, u32 rgba)
{
	const float q = p->q;

	o->f[0] = c->a[0] * P.tsx * q;
	o->f[1] = c->a[1] * P.tsy * q;
	o->f[2] = q;
	o->w[3] = 0;
	o++;
	o->w[0] = rgba & 255;
	o->w[1] = (rgba >> 8) & 255;
	o->w[2] = (rgba >> 16) & 255;
	o->w[3] = rgba >> 24;
	o++;
	o->w[0] = (u32)p->xi;
	o->w[1] = (u32)p->yi;
	o->w[2] = (u32)p->zi;
	o->w[3] = 0;
	return o + 1;
}

// one triangle that needs the clipper (or_ != 0 and and_ == 0)
static void sky_tri_clipped(int ia, int ib, int ic, int or_)
{
	cv_t *cl;
	int cnt, k;
	pack_t pk;
	qw_t *o;

	vbuf_a[0] = SKY.c[ia];
	vbuf_a[1] = SKY.c[ib];
	vbuf_a[2] = SKY.c[ic];
	cnt = clip_poly(vbuf_a, vbuf_b, 3, 6, or_, &cl);
	H.st.clipped++;
	if (cnt < 3)
		return;
	memset(&pk, 0, sizeof pk);
	pk.fmt = 4;
	pk.nreg = 3;
	pk.fog = 255;
	o = pk_alloc((u32)(1 + (cnt - 2) * 9));
	o[0].d[0] = HWGIF_TAG((cnt - 2) * 3, 1, 1, PRIM_TRI | pass_prim(&P.pass[0]), 0, 3);
	o[0].d[1] = GIF_REGS_STQ_RGBA_XYZ;
	o++;
	for (k = 1; k + 1 < cnt; k++)
	{
		o = put_vertex(o, &cl[0], &pk);
		o = put_vertex(o, &cl[k], &pk);
		o = put_vertex(o, &cl[k + 1], &pk);
	}
	H.st.verts_out += (u32)((cnt - 2) * 3);
}

// a run of strip vertices [a, b] as one GS triangle strip
static void sky_strip(int a, int b)
{
	const int nv = b - a + 1, nq = 1 + nv * 3;
	qw_t *o;
	int i;

	if (nv < 3)
		return;
	o = pk_alloc((u32)nq);
	o[0].d[0] = HWGIF_TAG((u32)nv, 1, 1, PRIM_TRISTRIP | pass_prim(&P.pass[0]), 0, 3);
	o[0].d[1] = GIF_REGS_STQ_RGBA_XYZ;
	o++;
	for (i = a; i <= b; i++)
		o = sky_put(o, &SKY.c[i], &SKY.p[i], SKY.rgba[i]);
	H.gsr.valid &= ~(u32)RV_RGBAQ; // packed vertices changed Q and the colour behind the A+D state cache
	H.st.verts_out += (u32)nv;
}

// returns 1 when the dome was drawn
static int sky_dome_fast(gl_sky_t *sky)
{
	float saved_mv[16];
	int i, j, ok = 1;

	if ((ps2hwd_dbg_flags & 4096) || !sky_fast_prepare(sky))
		return 0;
	memcpy(saved_mv, H.mv, sizeof saved_mv);
	m_scale(H.mv, 1.0f, (float)sky->height / 200.0f, 1.0f);
	m_rotate(H.mv, 270.0f, 0.0f, 1.0f, 0.0f);
	H.mat_dirty = 1;
	if (!begin_draw(H.cur_flags, NULL))
		goto done;
	if (!(P.vu0 && P.npass == 1 && P.lmode == LM_NONE && !P.cutx && !P.cuty && !P.water && P.pass[0].tex == (u8)P.textured))
	{
		ok = 0; // a plan the shortcut does not cover: the old path draws it
		{
			static int said;

			if (!said++)
				CONS_Printf("HWD sky: plan not covered (vu0 %d npass %d lmode %d cutx %d cuty %d water %d tex %d/%d)\n", P.vu0, P.npass, P.lmode, P.cutx, P.cuty, P.water, (int)P.pass[0].tex, P.textured);
		}
		goto done;
	}
	{
		const int n = sky->vertex_count;
		u32 t0 = cyc();

		for (i = 0; i < n; i++)
			SKY.oc[i] = (u8)vu0_xform(&SKY.v[i], &SKY.c[i], &SKY.p[i]);
		H.st.verts_in += (u32)n;
		H.st.cyc_draw += cyc() - t0;
	}
	pass_setup(0, 0);
	for (j = 0; j < 2; j++) // untextured caps first, then the textured bands
	{
		for (i = 0; i < sky->loopcount; i++)
		{
			const gl_skyloopdef_t *loop = &sky->loops[i];
			const int first = loop->vertexindex, cnt = loop->vertexcount;
			const u8 *oc = &SKY.oc[first];
			int k;

			if (j == 0 ? loop->use_texture : !loop->use_texture)
				continue;
			if (loop->mode == HWD_SKYLOOP_FAN)
			{
				int all = 0x3F;

				for (k = 0; k < cnt; k++)
					all &= oc[k];
				if (all)
					continue; // the whole cap is outside one plane
				for (k = 1; k + 1 < cnt; k++)
				{
					const int and_ = oc[0] & oc[k] & oc[k + 1], or_ = oc[0] | oc[k] | oc[k + 1];

					H.st.polys++;
					if (and_)
					{
						H.st.rejected++;
						continue;
					}
					if (or_)
					{
						sky_tri_clipped(first, first + k, first + k + 1, or_);
					}
					else
					{
						qw_t *o = pk_alloc(1 + 9);

						o[0].d[0] = HWGIF_TAG(3, 1, 1, PRIM_TRI | pass_prim(&P.pass[0]), 0, 3);
						o[0].d[1] = GIF_REGS_STQ_RGBA_XYZ;
						o++;
						o = sky_put(o, &SKY.c[first], &SKY.p[first], SKY.rgba[first]);
						o = sky_put(o, &SKY.c[first + k], &SKY.p[first + k], SKY.rgba[first + k]);
						sky_put(o, &SKY.c[first + k + 1], &SKY.p[first + k + 1], SKY.rgba[first + k + 1]);
						H.gsr.valid &= ~(u32)RV_RGBAQ;
						H.st.verts_out += 3;
					}
				}
			}
			else
			{
				int run = -1; // first vertex of the open run of clean quads

				for (k = 0; k + 3 < cnt; k += 2)
				{
					const int o0 = oc[k], o1 = oc[k + 1], o2 = oc[k + 2], o3 = oc[k + 3];
					const int and_ = o0 & o1 & o2 & o3, or_ = o0 | o1 | o2 | o3;

					H.st.polys += 2;
					if (!or_)
					{
						if (run < 0)
							run = k;
						continue;
					}
					if (run >= 0)
					{
						sky_strip(first + run, first + k + 1);
						run = -1;
					}
					if (and_)
					{
						H.st.rejected += 2;
						continue;
					}
					// the triangles of this quad, as the old index lists had them: (k, k + 1, k + 2) and (k + 1, k + 3, k + 2)
					{
						const int t0a = o0 & o1 & o2, t0o = o0 | o1 | o2, t1a = o1 & o3 & o2, t1o = o1 | o3 | o2;

						if (!t0a)
						{
							if (t0o)
								sky_tri_clipped(first + k, first + k + 1, first + k + 2, t0o);
							else
								sky_strip(first + k, first + k + 2);
						}
						if (!t1a)
						{
							if (t1o)
								sky_tri_clipped(first + k + 1, first + k + 3, first + k + 2, t1o);
							else
								sky_strip(first + k + 1, first + k + 3);
						}
					}
				}
				if (run >= 0)
					sky_strip(first + run, first + cnt - 1);
			}
		}
	}
done:
	memcpy(H.mv, saved_mv, sizeof H.mv);
	H.mat_dirty = 1;
	return ok;
}

#else
static void sky_fast_free(void) {}
static int sky_dome_fast(gl_sky_t *sky)
{
	(void)sky;
	return 0;
}
#endif
