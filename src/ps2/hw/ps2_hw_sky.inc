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
//
// PS2-HW-110 (OPT11 VU): a texture whose width or height is not a power of two (the 512 x 768 sky of DEMO_002) has its repeats made by cutting at whole texture widths
// (P.cutx / P.cuty, the texture is clamped to its own region): the dome took the old path, 1.45 M cycles a frame. The bands of the dome (and its caps) are bounded by
// whole texture heights / widths of the coordinates (rings at t = k / 4), so no band crosses a repeat: every band is drawn in its own repeat with the whole repeats
// removed from its coordinates (what the old path's cuts and sb / tb make of such a triangle). A mesh with a band that does cross a repeat takes the old path.

#ifdef HW_VU0

#define SKY_MAXLOOPS 32

static struct
{
	const gl_skyvertex_t *src; // identity of the converted mesh
	int n, texture, width, height, loopcount;
	FOutVector *v; // x, y, z, s, t per vertex (caps: s = t = 0)
	u32 *rgba; // GS colour words (R | G << 8 | B << 16 | A << 24)
	cv_t *c; // per frame: clip coordinates and s, t
	pv_t *p; // per frame: GS integers
	u8 *oc; // per frame: outcodes (0xFF = the vertex has not been transformed this frame, PS2-HW-244)
	float (*qs)[4]; // PS2-HW-244: a sphere (x, y, z, radius, in the mesh's own coordinates) around every quad of a strip (slot first / 2 + k / 2) and every cap (slot first / 2)
	int cap;
	int sph; // the spheres of qs are made
	int ok; // the converted mesh can take the shortcut
	int cellx, celly; // every textured loop lies inside one repeat of the texture in s / t: the shortcut also takes a texture that is cut at its repeats on that axis (PS2-HW-110)
	float lsb[SKY_MAXLOOPS], ltb[SKY_MAXLOOPS]; // whole repeats of s, t of a loop
} SKY;

static float sky_sb, sky_tb; // the repeats removed from s, t of the loop being drawn (0 unless the plan cuts at the repeats)

static void sky_fast_free(void)
{
	free(SKY.v);
	free(SKY.rgba);
	free(SKY.c);
	free(SKY.p);
	free(SKY.oc);
	free(SKY.qs);
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
		SKY.qs = malloc(((size_t)n / 2 + 2) * sizeof *SKY.qs);
		if (!SKY.v || !SKY.rgba || !SKY.c || !SKY.p || !SKY.oc || !SKY.qs)
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
	SKY.cellx = SKY.celly = sky->loopcount <= SKY_MAXLOOPS;
	for (l = 0; l < sky->loopcount && SKY.ok && (SKY.cellx || SKY.celly); l++)
	{
		const gl_skyloopdef_t *loop = &sky->loops[l];
		float smn = 1e30f, smx = -1e30f, tmn = 1e30f, tmx = -1e30f;

		for (i = 0; i < loop->vertexcount; i++)
		{
			const FOutVector *v = &SKY.v[loop->vertexindex + i];

			smn = v->s < smn ? v->s : smn;
			smx = v->s > smx ? v->s : smx;
			tmn = v->t < tmn ? v->t : tmn;
			tmx = v->t > tmx ? v->t : tmx;
		}
		SKY.lsb[l] = floorf(smn + 1.0e-4f);
		SKY.ltb[l] = floorf(tmn + 1.0e-4f);
		if (smx - SKY.lsb[l] > 1.0f + 1.0e-4f)
			SKY.cellx = 0; // a band that crosses a repeat: the cuts of the old path
		if (tmx - SKY.ltb[l] > 1.0f + 1.0e-4f)
			SKY.celly = 0;
	}
	// PS2-HW-244: a sphere around every quad of a strip and around every cap, in the coordinates of the mesh (the view transform of the frame is applied to the centre by the
	// test, its gradient bounds the radius: ps2_hw_fx2.inc). The slot of a quad is (first + k) / 2; a loop that does not start at an even vertex has none (-> no sphere test).
	SKY.sph = SKY.ok;
	for (l = 0; l < sky->loopcount && SKY.sph; l++)
		if (sky->loops[l].vertexindex & 1)
			SKY.sph = 0;
	for (l = 0; l < sky->loopcount && SKY.sph; l++)
	{
		const gl_skyloopdef_t *loop = &sky->loops[l];
		const int first = loop->vertexindex, cnt = loop->vertexcount;
		int k, step = loop->mode == HWD_SKYLOOP_FAN ? cnt : 2, len;

		for (k = 0; (loop->mode == HWD_SKYLOOP_FAN ? k == 0 : k + 3 < cnt); k += step)
		{
			float cx = 0.0f, cy = 0.0f, cz = 0.0f, r2 = 0.0f;

			len = loop->mode == HWD_SKYLOOP_FAN ? cnt : 4;
			for (i = 0; i < len; i++)
			{
				cx += SKY.v[first + k + i].x;
				cy += SKY.v[first + k + i].y;
				cz += SKY.v[first + k + i].z;
			}
			cx /= (float)len;
			cy /= (float)len;
			cz /= (float)len;
			for (i = 0; i < len; i++)
			{
				const float dx = SKY.v[first + k + i].x - cx, dy = SKY.v[first + k + i].y - cy, dz = SKY.v[first + k + i].z - cz, d2 = dx * dx + dy * dy + dz * dz;

				r2 = d2 > r2 ? d2 : r2;
			}
			SKY.qs[(first + k) >> 1][0] = cx;
			SKY.qs[(first + k) >> 1][1] = cy;
			SKY.qs[(first + k) >> 1][2] = cz;
			SKY.qs[(first + k) >> 1][3] = sqrtf(r2) * 1.002f + 1.0f;
		}
	}
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

	o->f[0] = (c->a[0] - sky_sb) * P.tsx * q;
	o->f[1] = (c->a[1] - sky_tb) * P.tsy * q;
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
	G.dome_tris++;
	H.st.clipped++;
	if (cnt < 3)
		return;
	memset(&pk, 0, sizeof pk);
	pk.fmt = 4;
	pk.nreg = 3;
	pk.fog = 255;
	pk.sb = sky_sb;
	pk.tb = sky_tb;
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

// the outcode of vertex i, transformed when it has not been this frame (PS2-HW-244; with every vertex made up front the test is a load)
static inline int sky_oc_at(int i)
{
	if (SKY.oc[i] == 0xFF)
	{
		SKY.oc[i] = (u8)vu0_xform(&SKY.v[i], &SKY.c[i], &SKY.p[i]);
		H.st.verts_in++;
	}
	return SKY.oc[i];
}

// PS2-HW-244: the sphere q = (x, y, z, radius) in the mesh's coordinates lies wholly outside one of the planes of the view volume (the four sides, the near plane): a polygon
// inside it has all its vertices outside that plane, and the clipper (or the AND of the outcodes) leaves nothing of it. The radius has its margin already (the prepare).
static inline int sky_sphere_hidden(const ps2cull_t *cs, const float *q)
{
	const float X = q[0], Y = q[1], Z = q[2], R = q[3];
	const float cw = cs->r[3][0] * X + cs->r[3][1] * Y + cs->r[3][2] * Z + cs->r[3][3];
	const float cx = cs->r[0][0] * X + cs->r[0][1] * Y + cs->r[0][2] * Z + cs->r[0][3];
	const float cy = cs->r[1][0] * X + cs->r[1][1] * Y + cs->r[1][2] * Z + cs->r[1][3];
	const float cz = cs->r[2][0] * X + cs->r[2][1] * Y + cs->r[2][2] * Z + cs->r[2][3];

	return (cx - cw) - cs->nxm * R > 0.0f || (cx + cw) + cs->nxp * R < 0.0f || (cy - cw) - cs->nym * R > 0.0f || (cy + cw) + cs->nyp * R < 0.0f || (cz + cw) + cs->nzp * R < 0.0f;
}

// returns 1 when the dome was drawn
static int sky_dome_fast(gl_sky_t *sky)
{
	float saved_mv[16];
	int i, j, ok = 1;
	u32 dome_t1 = 0;
	const ps2cull_t *cs = NULL;

	if ((ps2hwd_dbg_flags & 4096) || !sky_fast_prepare(sky))
		return 0;
	G.dome_n++;
	memcpy(saved_mv, H.mv, sizeof saved_mv);
	m_scale(H.mv, 1.0f, (float)sky->height / 200.0f, 1.0f);
	m_rotate(H.mv, 270.0f, 0.0f, 1.0f, 0.0f);
	H.mat_dirty = 1;
	{
		const u32 tb = cyc();
		const int okb = begin_draw(H.cur_flags, NULL);

		G.dome_begin += cyc() - tb;
		if (!okb)
			goto done;
	}
	if (!(P.vu0 && P.npass == 1 && P.lmode == LM_NONE && (!P.cutx || SKY.cellx) && (!P.cuty || SKY.celly) && !P.water && P.pass[0].tex == (u8)P.textured))
	{
		ok = 0; // a plan the shortcut does not cover: the old path draws it
		{
			static int said;

			if (!said++)
				CONS_Printf("HWD sky: plan not covered (vu0 %d npass %d lmode %d cutx %d cuty %d cells %d/%d water %d tex %d/%d)\n", P.vu0, P.npass, P.lmode, P.cutx, P.cuty, SKY.cellx, SKY.celly, P.water, (int)P.pass[0].tex, P.textured);
		}
		goto done;
	}
	if (!(ps2hwd_dbg_flags & 1024)) // -hwdbg 1024: the old guard band outcodes
		vu0_batch_load(H.mvp, 1.0f, 1.0f, P.kx, P.ky, P.zk, P.ox, P.oy, P.zo + P.zbias, P.zmax); // PS2-HW-56: outcodes against the SCREEN frustum (no guard band) ...
	{
		const int n = sky->vertex_count;
		u32 t0 = cyc();

		cs = (ps2hwd_fx2 & FX2_NOSKY) || !SKY.sph ? NULL : PS2HWD_CullSetup();
		if (cs && !cs->valid)
			cs = NULL;
		if (cs)
		{
			memset(SKY.oc, 0xFF, (size_t)n); // PS2-HW-244: a vertex is transformed when a quad that is not outside the view needs it
		}
		else
		{
			for (i = 0; i < n; i++)
				SKY.oc[i] = (u8)vu0_xform(&SKY.v[i], &SKY.c[i], &SKY.p[i]);
			H.st.verts_in += (u32)n;
		}
		H.st.cyc_draw += cyc() - t0;
		G.dome_xf += cyc() - t0;
	}
	dome_t1 = cyc();
	pass_setup(0, 0);
	{
		// ... a triangle or quad entirely outside one side of the screen draws nothing (the GS scissors it away) and is dropped here without
		// clipping; what crosses the screen edge is clipped only when it also leaves the guard band (the old rule), using the guard band outcodes
		// computed here for those few vertices. The old rule clipped (about 3 400 cycles each) the 144 triangles of a frame that straddle the
		// guard band 78 degrees to the side of the view, where nothing is visible.
		const float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);
#define SKY_GOC(ix) (SKY.c[ix].w <= 0.0f ? 0x3F : outcode(&SKY.c[ix], gx, gy))

		for (j = 0; j < 2; j++) // untextured caps first, then the textured bands
		{
			for (i = 0; i < sky->loopcount; i++)
			{
				const gl_skyloopdef_t *loop = &sky->loops[i];
				const int first = loop->vertexindex, cnt = loop->vertexcount;
				int k;

				if (j == 0 ? loop->use_texture : !loop->use_texture)
					continue;
				sky_sb = (P.cutx && loop->use_texture) ? SKY.lsb[i] : 0.0f; // (the caps have s = t = 0)
				sky_tb = (P.cuty && loop->use_texture) ? SKY.ltb[i] : 0.0f;
				if (loop->mode == HWD_SKYLOOP_FAN)
				{
					int all = 0x3F;

					if (cs && sky_sphere_hidden(cs, SKY.qs[first >> 1]))
						continue; // PS2-HW-244: the sphere around the cap is outside the view
					for (k = 0; k < cnt; k++)
						all &= sky_oc_at(first + k);
					if (all)
						continue; // the whole cap is outside one plane
					for (k = 1; k + 1 < cnt; k++)
					{
						const int c0 = sky_oc_at(first), c1 = sky_oc_at(first + k), c2 = sky_oc_at(first + k + 1);
						const int and_ = c0 & c1 & c2, or_ = c0 | c1 | c2;

						H.st.polys++;
						if (and_)
						{
							H.st.rejected++;
							continue;
						}
						if (or_)
						{
							const int gor = SKY_GOC(first) | SKY_GOC(first + k) | SKY_GOC(first + k + 1);

							if (gor)
							{
								sky_tri_clipped(first, first + k, first + k + 1, gor);
								continue;
							}
						}
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
						int o0, o1, o2, o3, and_, or_;
						int g0 = 0, g1 = 0, g2 = 0, g3 = 0;

						if (cs && sky_sphere_hidden(cs, SKY.qs[(first + k) >> 1]))
						{
							// PS2-HW-244: the sphere around the quad is outside the view: its four vertices are outside one plane (what the AND of their outcodes finds), nothing is made of it
							H.st.polys += 2;
							if (run >= 0)
							{
								sky_strip(first + run, first + k + 1);
								run = -1;
							}
							H.st.rejected += 2;
							continue;
						}
						o0 = sky_oc_at(first + k);
						o1 = sky_oc_at(first + k + 1);
						o2 = sky_oc_at(first + k + 2);
						o3 = sky_oc_at(first + k + 3);
						and_ = o0 & o1 & o2 & o3;
						or_ = o0 | o1 | o2 | o3;
						H.st.polys += 2;
						if (and_)
						{
							if (run >= 0)
							{
								sky_strip(first + run, first + k + 1);
								run = -1;
							}
							H.st.rejected += 2;
							continue;
						}
						if (or_)
						{
							g0 = SKY_GOC(first + k);
							g1 = SKY_GOC(first + k + 1);
							g2 = SKY_GOC(first + k + 2);
							g3 = SKY_GOC(first + k + 3);
						}
						if (!(g0 | g1 | g2 | g3))
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
						// the triangles of this quad, as the old index lists had them: (k, k + 1, k + 2) and (k + 1, k + 3, k + 2)
						{
							const int t0a = o0 & o1 & o2, t0o = g0 | g1 | g2, t1a = o1 & o3 & o2, t1o = g1 | g3 | g2;

							if (!t0a)
							{
								if (t0o)
									sky_tri_clipped(first + k, first + k + 1, first + k + 2, t0o);
								else
									sky_strip(first + k, first + k + 2);
							}
							else
								H.st.rejected++;
							if (!t1a)
							{
								if (t1o)
									sky_tri_clipped(first + k + 1, first + k + 3, first + k + 2, t1o);
								else
									sky_strip(first + k + 1, first + k + 3);
							}
							else
								H.st.rejected++;
						}
					}
					if (run >= 0)
						sky_strip(first + run, first + cnt - 1);
				}
			}
		}
#undef SKY_GOC
	}
	G.dome_emit += cyc() - dome_t1;
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
