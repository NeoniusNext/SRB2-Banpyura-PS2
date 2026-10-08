// Models (MD3) on the GS (included by ps2_hwd.c). OPT11-MODEL (PS2-HW-264..): see docs/GATES/g1/opt11-MODEL.md.
//
// A model draws as the OpenGL driver does it (r_opengl.c DrawModelEx): the object matrix (position, angles, roll, scale; the 1/64 of the tinyframe vertices), the vertices of the two frames
// interpolated as shorts, the triangles of the mesh by index, the polygon's light (PF_ColorMapped, as sprites: the sector light through the CLUT rows / fog bands of the polygon plan), and with
// gr_modellighting the vertex shader's factor min(0.75 + max(n.y, 0), 1) (r_opengl.c / hw_shaders.h: dot(gl_Normal, (0, 1, 0)) with the model's own up component of the normal).
//
// Three ways to draw a mesh, from the cheapest:
//   mdl_mesh_fast     (PS2-HW-265) one pass, one light class per TRIANGLE (no cuts at the steps of the light staircase), the vertices transformed once on VU0, GIF packing here;
//   the indexed path  every vertex interpolated once, emit_tris (cuts, clip, general plans) with an index list: multi pass blends, texture repeats, no VU0, -hwmodel 4;
//   the corner path   the OPT10 code (every corner interpolated and transformed): meshes of more than MDL_MAXV vertices (the boss BRAK has 6061), -hwmodel 1.

#define MDL_MAXV 2048 // vertices of a mesh drawn through the work area
#define MDL_MAXI 3072 // indices per emit_tris call
#define MDL_MAXT 3072 // triangles of a mesh the fast path takes
#define MDL_CORNERS 192 // corners per call of the corner by corner path

typedef struct
{
	s32 xi, yi, zi; // GS X, Y (x16 + 2048), Z24 (the layout of pv_t: VU0 stores the result of the projection here)
	float q; // 1 / w
	float w; // eye depth
	float sq, tq; // s * tsx * q, t * tsy * q
	u32 ocl; // outcode (bits 0..5) | index into mdl_lf of the light factor << 8
} __attribute__((aligned(16))) mdl_v_t;

typedef struct
{
	union
	{
		struct
		{
			FOutVector v[MDL_MAXV];
			float l[MDL_MAXV]; // the light factor of each vertex (only with gr_modellighting)
			u32 idx[MDL_MAXI];
		} g;
		struct
		{
			mdl_v_t mv[MDL_MAXV + 1]; // one spare entry: the second lane of the last pair of an odd number of vertices
			u16 ord[MDL_MAXT];
			u8 row[MDL_MAXT];
		} f;
	} u;
} __attribute__((aligned(16))) mdlwork_t;

static mdlwork_t *mdlw; // made when the first model is drawn (zone block: a game without models does not pay for it)
static float mdl_lf[256]; // the light factor by the byte of the normal: index = c + 128
static int mdl_lf_ready;

static void mdl_init_tables(void)
{
	int c;

	for (c = -128; c < 128; c++)
	{
		// gl_Normal of a GL_BYTE normal array: (2c + 1) / 255; light = min(0.75 + max(n, 0), 1)
		const float n = (float)(2 * c + 1) * (1.0f / 255.0f);
		float l = 0.75f + (n > 0.0f ? n : 0.0f);

		mdl_lf[c + 128] = l > 1.0f ? 1.0f : l;
	}
	mdl_lf_ready = 1;
}

static int mdl_work_get(void)
{
	if (mdlw)
		return 1;
	mdlw = (mdlwork_t *)Z_TryMallocAlign(sizeof *mdlw, PU_STATIC, NULL, 6);
	if (!mdlw)
	{
		hw_limit(HW_INVALID, "no memory for the model work area; models are not drawn");
		return 0;
	}
	if (!mdl_lf_ready)
		mdl_init_tables();
	return 1;
}

// the position of vertex vi at the interpolated frame: (short)(a + pol * (b - a)) as the OpenGL driver does it
static inline void mdl_pos(const short *a, const short *b, float lerp, float *o)
{
	if (lerp == 0.0f)
	{
		o[0] = (float)a[0];
		o[1] = (float)a[1];
		o[2] = (float)a[2];
	}
	else
	{
		o[0] = (float)(short)((float)a[0] + lerp * (float)(b[0] - a[0]));
		o[1] = (float)(short)((float)a[1] + lerp * (float)(b[1] - a[1]));
		o[2] = (float)(short)((float)a[2] + lerp * (float)(b[2] - a[2]));
	}
}

// the index into mdl_lf of the interpolated normal byte (c + 128)
static inline int mdl_lidx(const signed char *na, const signed char *nb, int vi, float lerp)
{
	if (lerp == 0.0f)
		return na[vi] + 128;
	return (signed char)(int)((float)na[vi] + lerp * (float)(nb[vi] - na[vi])) + 128; // (char)(a + pol * (b - a))
}

// the vertices (and light factors) of one mesh at the interpolated frame fi..ni into the work area (the indexed path)
static void mdl_interp(const mesh_t *mesh, int fi, int ni, float lerp, int light)
{
	const int nv = mesh->numVertices;
	const short *a = mesh->tinyframes[fi].vertices, *b = mesh->tinyframes[ni].vertices;
	const signed char *na = (const signed char *)mesh->tinyframes[fi].normals, *nb = (const signed char *)mesh->tinyframes[ni].normals;
	const float *uv = mesh->uvs;
	FOutVector *o = mdlw->u.g.v;
	int k;

	for (k = 0; k < nv; k++, a += 3, b += 3)
	{
		float p[3];

		mdl_pos(a, b, lerp, p);
		o[k].x = p[0];
		o[k].y = p[1];
		o[k].z = p[2];
		o[k].s = uv[k * 2];
		o[k].t = uv[k * 2 + 1];
	}
	if (light)
		for (k = 0; k < nv; k++)
			mdlw->u.g.l[k] = mdl_lf[mdl_lidx(na, nb, k, lerp)];
}

// the corner by corner path (OPT10): used for meshes the work area cannot hold and for models in the float format
static void mdl_mesh_corners(const mesh_t *mesh, int fi, int ni, float lerp, int tiny, int light)
{
	int start;
	u32 k;

	for (k = 0; k < MDL_CORNERS; k++)
		mdlw->u.g.idx[k] = k;
	for (start = 0; start < mesh->numTriangles * 3; start += MDL_CORNERS)
	{
		int n = mesh->numTriangles * 3 - start, valid = 1, c;

		if (n > MDL_CORNERS)
			n = MDL_CORNERS;
		for (c = 0; c < n; c++)
		{
			const int vi = tiny ? mesh->indices[start + c] : start + c;
			float xyz[3];
			int j;

			if (vi < 0 || vi >= mesh->numVertices)
			{
				valid = 0;
				break;
			}
			if (tiny)
			{
				mdl_pos(mesh->tinyframes[fi].vertices + vi * 3, mesh->tinyframes[ni].vertices + vi * 3, lerp, xyz);
			}
			else
			{
				const float *a = mesh->frames[fi].vertices, *b = mesh->frames[ni].vertices;

				for (j = 0; j < 3; j++)
					xyz[j] = a[vi * 3 + j] + lerp * (b[vi * 3 + j] - a[vi * 3 + j]);
			}
			mdlw->u.g.v[c].x = xyz[0];
			mdlw->u.g.v[c].y = xyz[1];
			mdlw->u.g.v[c].z = xyz[2];
			mdlw->u.g.v[c].s = mesh->uvs[vi * 2];
			mdlw->u.g.v[c].t = mesh->uvs[vi * 2 + 1];
			if (light)
				mdlw->u.g.l[c] = tiny ? mdl_lf[mdl_lidx((const signed char *)mesh->tinyframes[fi].normals, (const signed char *)mesh->tinyframes[ni].normals, vi, lerp)] : 1.0f;
		}
		if (!valid)
		{
			hw_limit(HW_INVALID, "model index out of range; batch rejected");
			continue;
		}
		emit_tris(mdlw->u.g.v, mdlw->u.g.idx, (u32)n, light ? mdlw->u.g.l : NULL);
		G.md_slow++;
	}
}

#ifdef HW_VU0
// vu0_xform2 of ps2_hw_vu0.inc with the vertices as floats: p0, p1 = x, y, z; c0, c1 = 16 byte aligned scratch for the clip coordinates (only w is read back)
static inline void mdl_xform2(const float *p0, const float *p1, float *c0, float *c1, void *o0, void *o1, int *oc0, int *oc1)
{
	float in[8] __attribute__((aligned(16)));
	u32 f, fa, fb;

	in[0] = p0[0];
	in[1] = p0[1];
	in[2] = p0[2];
	in[3] = 1.0f;
	in[4] = p1[0];
	in[5] = p1[1];
	in[6] = p1[2];
	in[7] = 1.0f;
	__asm__ volatile(
		"lqc2 $vf5, 0(%1)\n"
		"lqc2 $vf14, 16(%1)\n"
		"vmulaw $ACC, $vf4, $vf0\n"
		"vmaddax $ACC, $vf1, $vf5\n"
		"vmadday $ACC, $vf2, $vf5\n"
		"vmaddz $vf6, $vf3, $vf5\n"
		"vmulaw $ACC, $vf4, $vf0\n"
		"vmaddax $ACC, $vf1, $vf14\n"
		"vmadday $ACC, $vf2, $vf14\n"
		"vmaddz $vf15, $vf3, $vf14\n"
		"vmul $vf7, $vf6, $vf8\n"
		"vmul $vf16, $vf15, $vf8\n"
		"vclipw.xyz $vf7, $vf7\n"
		"vclipw.xyz $vf16, $vf16\n"
		"vdiv $Q, $vf0w, $vf6w\n"
		"sqc2 $vf6, 0(%2)\n"
		"sqc2 $vf15, 0(%3)\n"
		"cfc2 %0, $vi18\n"
		"vwaitq\n"
		"vmulq.xyz $vf9, $vf6, $Q\n"
		"vmulq.w $vf10, $vf0, $Q\n"
		"vdiv $Q, $vf0w, $vf15w\n"
		"vmulaw $ACC, $vf12, $vf0\n"
		"vmadd.xyz $vf10, $vf9, $vf11\n"
		"vmax.z $vf10, $vf10, $vf0\n"
		"vmini.z $vf10, $vf10, $vf13\n"
		"vftoi0.xyz $vf10, $vf10\n"
		"sqc2 $vf10, 0(%4)\n"
		"vwaitq\n"
		"vmulq.xyz $vf17, $vf15, $Q\n"
		"vmulq.w $vf18, $vf0, $Q\n"
		"vmulaw $ACC, $vf12, $vf0\n"
		"vmadd.xyz $vf18, $vf17, $vf11\n"
		"vmax.z $vf18, $vf18, $vf0\n"
		"vmini.z $vf18, $vf18, $vf13\n"
		"vftoi0.xyz $vf18, $vf18\n"
		"sqc2 $vf18, 0(%5)\n"
		: "=&r"(f) : "r"(in), "r"(c0), "r"(c1), "r"(o0), "r"(o1) : "memory");
	fa = (f >> 6) & 0x3F;
	fb = f & 0x3F;
	fa = ((fa & 0x15u) << 1) | ((fa >> 1) & 0x15u);
	fb = ((fb & 0x15u) << 1) | ((fb >> 1) & 0x15u);
	*oc0 = c0[3] <= 0.0f ? 0x3F : (int)fa;
	*oc1 = c1[3] <= 0.0f ? 0x3F : (int)fb;
}

// one triangle through the general path (cut at the guard band / near plane): its three vertices again as FOutVector
static void mdl_tri_general(const mesh_t *mesh, int fi, int ni, float lerp, int light, unsigned a, unsigned b, unsigned c)
{
	static FOutVector tv[3];
	static float tl[3];
	static const u32 ti[3] = {0, 1, 2};
	const unsigned id[3] = {a, b, c};
	int k;

	ls_close();
	for (k = 0; k < 3; k++)
	{
		const unsigned vi = id[k];
		float p[3];

		mdl_pos(mesh->tinyframes[fi].vertices + vi * 3, mesh->tinyframes[ni].vertices + vi * 3, lerp, p);
		tv[k].x = p[0];
		tv[k].y = p[1];
		tv[k].z = p[2];
		tv[k].s = mesh->uvs[vi * 2];
		tv[k].t = mesh->uvs[vi * 2 + 1];
		if (light)
			tl[k] = mdl_lf[mdl_lidx((const signed char *)mesh->tinyframes[fi].normals, (const signed char *)mesh->tinyframes[ni].normals, (int)vi, lerp)];
	}
	emit_tris(tv, ti, 3, light ? tl : NULL);
	G.md_slow++;
}

// one vertex as GIF data: ST + Q, the colour (fmt 6: scaled by the light factor of the vertex), XYZ (fmt 0) or XYZF (fmt 1, 6)
static inline qw_t *mdl_put_v(qw_t *o, const mdl_v_t *m, u32 col, u32 fog, int fmt)
{
	o->f[0] = m->sq;
	o->f[1] = m->tq;
	o->f[2] = m->q;
	o->w[3] = 0;
	o++;
	if (fmt == 6)
	{
		const float lf = mdl_lf[(m->ocl >> 8) & 255];

		o->w[0] = (u32)((float)(col & 255) * lf + 0.5f);
		o->w[1] = (u32)((float)((col >> 8) & 255) * lf + 0.5f);
		o->w[2] = (u32)((float)((col >> 16) & 255) * lf + 0.5f);
		o->w[3] = col >> 24;
	}
	else
	{
		o->w[0] = col & 255;
		o->w[1] = (col >> 8) & 255;
		o->w[2] = (col >> 16) & 255;
		o->w[3] = col >> 24;
	}
	o++;
	o->w[0] = (u32)m->xi;
	o->w[1] = (u32)m->yi;
	if (fmt == 0)
	{
		o->w[2] = (u32)m->zi;
		o->w[3] = 0;
	}
	else
	{
		o->w[2] = (u32)m->zi << 4;
		o->w[3] = fog << 4;
	}
	return o + 1;
}

// 1 = the mesh was drawn (every triangle went through one of the two ways)
static int mdl_mesh_fast(const mesh_t *mesh, int fi, int ni, float lerp, int light)
{
	const int nv = mesh->numVertices, nt = mesh->numTriangles;
	const unsigned short *ix = mesh->indices;
	const short *pa = mesh->tinyframes[fi].vertices, *pb = mesh->tinyframes[ni].vertices;
	const signed char *na = (const signed char *)mesh->tinyframes[fi].normals, *nb = (const signed char *)mesh->tinyframes[ni].normals;
	const float *uv = mesh->uvs;
	const float tsx = P.tsx, tsy = P.tsy;
	const int pal = P.pal, bands = P.lmode == LM_BANDS;
	mdl_v_t *mv = mdlw->u.f.mv;
	u16 *ord = mdlw->u.f.ord;
	u8 *rowof = mdlw->u.f.row;
	u32 count[34], start[34];
	int k, t, minrow = 255, maxrow = -1, nord = 0;
	static float clip0[4] __attribute__((aligned(16))), clip1[4] __attribute__((aligned(16)));
	const pass_t *ps = &P.pass[0];
	const int fmt = light ? 6 : ((bands && !pal && ps->fog) ? 1 : 0);
	const float cull = (float)P.cull;
	const u32 col = ps->col;
	u32 fogtab[34];
	int fogknown = 0;

	vu0_need();
	// 1. every vertex: interpolation, the VU0 transform, the texture coordinates over w
	for (k = 0; k + 1 < nv; k += 2)
	{
		float p0[3], p1[3];
		int o0, o1;
		mdl_v_t *m0 = &mv[k], *m1 = &mv[k + 1];

		mdl_pos(pa + k * 3, pb + k * 3, lerp, p0);
		mdl_pos(pa + k * 3 + 3, pb + k * 3 + 3, lerp, p1);
		mdl_xform2(p0, p1, clip0, clip1, m0, m1, &o0, &o1);
		m0->w = clip0[3];
		m1->w = clip1[3];
		m0->sq = uv[k * 2] * tsx * m0->q;
		m0->tq = uv[k * 2 + 1] * tsy * m0->q;
		m1->sq = uv[k * 2 + 2] * tsx * m1->q;
		m1->tq = uv[k * 2 + 3] * tsy * m1->q;
		m0->ocl = (u32)o0;
		m1->ocl = (u32)o1;
	}
	if (nv & 1)
	{
		float p0[3];
		int o0, o1;
		mdl_v_t *m0 = &mv[nv - 1], *m1 = &mv[nv]; // the spare entry takes the second lane

		mdl_pos(pa + (nv - 1) * 3, pb + (nv - 1) * 3, lerp, p0);
		mdl_xform2(p0, p0, clip0, clip1, m0, m1, &o0, &o1);
		m0->w = clip0[3];
		m0->sq = uv[(nv - 1) * 2] * tsx * m0->q;
		m0->tq = uv[(nv - 1) * 2 + 1] * tsy * m0->q;
		m0->ocl = (u32)o0;
	}
	if (light)
		for (k = 0; k < nv; k++)
			mv[k].ocl |= (u32)mdl_lidx(na, nb, k, lerp) << 8;
	G.md_verts += (u32)nv;

	// 2. the triangles: flags, cull, class (the colormap row for palette lighting, the darkness class for fog)
	memset(count, 0, sizeof count);
	for (t = 0; t < nt; t++)
	{
		const mdl_v_t *a = &mv[ix[t * 3]], *b = &mv[ix[t * 3 + 1]], *c = &mv[ix[t * 3 + 2]];
		const u32 oa = a->ocl & 0x3F, ob = b->ocl & 0x3F, oc = c->ocl & 0x3F;
		float area;
		int row = 0;

		rowof[t] = 255;
		H.st.polys++;
		if (oa & ob & oc)
		{
			H.st.rejected++;
			continue;
		}
		if (oa | ob | oc)
		{
			H.st.clipped++;
			mdl_tri_general(mesh, fi, ni, lerp, light, ix[t * 3], ix[t * 3 + 1], ix[t * 3 + 2]);
			continue;
		}
		area = (float)(b->xi - a->xi) * (float)(c->yi - a->yi) - (float)(c->xi - a->xi) * (float)(b->yi - a->yi); // screen space, y down: the opposite sign of the clip space test of emit_tris
		if (area * cull >= 0.0f)
		{
			H.st.rejected++;
			continue;
		}
		if (bands)
		{
			const int cls = lf_class_w((a->w + b->w + c->w) * (1.0f / 3.0f));

			row = pal ? pal_row_of(cls) : cls;
		}
		rowof[t] = (u8)row;
		if (row < minrow)
			minrow = row;
		if (row > maxrow)
			maxrow = row;
		count[row]++;
		nord++;
	}
	if (!nord)
		return 1;

	// 3. the surviving triangles, by row when the rows are CLUTs (a GS state each), in order otherwise
	if (pal && minrow != maxrow)
	{
		u32 s = 0;

		for (k = minrow; k <= maxrow; k++)
		{
			start[k] = s;
			s += count[k];
		}
		for (t = 0; t < nt; t++)
			if (rowof[t] != 255)
				ord[start[rowof[t]]++] = (u16)t;
		for (k = minrow, s = 0; k <= maxrow; s += count[k], k++)
			if (count[k])
			{
				u32 n = count[k], i;

				ls_close();
				if (k != P.pal_row || P.serial != H.serial)
				{
					P.pass[0].clut_param = pal_param(k);
					pass_setup(0, 0);
					P.serial = H.serial;
					P.pal_row = k;
				}
				LS.nreg = 3;
				LS.fmt = fmt;
				for (i = 0; i < n; i++)
				{
					const unsigned tt = ord[s + i];
					qw_t *o = ls_reserve(9);

					o = mdl_put_v(o, &mv[ix[tt * 3]], col, 0, fmt);
					o = mdl_put_v(o, &mv[ix[tt * 3 + 1]], col, 0, fmt);
					mdl_put_v(o, &mv[ix[tt * 3 + 2]], col, 0, fmt);
					LS.n += 3;
				}
			}
	}
	else
	{
		ls_close();
		if (pal)
		{
			if (minrow != P.pal_row || P.serial != H.serial)
			{
				P.pass[0].clut_param = pal_param(minrow);
				pass_setup(0, 0);
				P.serial = H.serial;
				P.pal_row = minrow;
			}
		}
		else if (P.serial != H.serial)
		{
			pass_setup(0, 0);
			P.serial = H.serial;
		}
		LS.nreg = 3;
		LS.fmt = fmt;
		for (t = 0; t < nt; t++)
		{
			qw_t *o;
			u32 fog = 255;

			if (rowof[t] == 255)
				continue;
			if (fmt != 0 && !pal)
			{
				const int cls = rowof[t];

				if (!fogknown)
				{
					memset(fogtab, 0xFF, sizeof fogtab);
					fogknown = 1;
				}
				if (fogtab[cls] == 0xFFFFFFFFu)
				{
					int on;
					const u32 f = lf_fog(cls, &on);

					fogtab[cls] = on ? f : 255;
				}
				fog = fogtab[cls];
			}
			o = ls_reserve(9);
			o = mdl_put_v(o, &mv[ix[t * 3]], col, fog, fmt);
			o = mdl_put_v(o, &mv[ix[t * 3 + 1]], col, fog, fmt);
			mdl_put_v(o, &mv[ix[t * 3 + 2]], col, fog, fmt);
			LS.n += 3;
		}
	}
	ls_close();
	H.st.verts_out += (u32)nord * 3u;
	H.gsr.valid &= ~(u32)RV_RGBAQ; // packed vertices changed Q (and the colours) behind the A+D state cache
	return 1;
}
#endif // HW_VU0

// PS2-HW-266: the model of an object that is a few pixels big is the sprite (the engine draws it when this says no): the model of a ring is 140 triangles, DEMO_001 shows a hundred
// of them, most far away. By size on the screen: the radius of the model's sphere in pixels (-hwmodelpx N, default 8; 0 = every model); and a limit on the triangles of the models
// of a frame (-hwmodeltris N, default 16000; 0 = none).
int ps2hwd_model_px = 8;
int ps2hwd_model_tris = 16000;
static u32 mdl_budget_frame = 0xFFFFFFFFu, mdl_budget_used;

int PS2HWD_ModelWanted(float x, float y, float z, float world_radius, unsigned int tris)
{
	float w, pr;

	if (!H.up)
		return 1;
	if (H.frame_no != mdl_budget_frame)
	{
		mdl_budget_frame = H.frame_no;
		mdl_budget_used = 0;
	}
	xf_update();
	w = H.mvp[3] * x + H.mvp[7] * z + H.mvp[11] * y + H.mvp[15]; // the engine's (x, y, height) are the GL (x, z, y) of draw_model
	if (w > 1.0f)
	{
		pr = world_radius * H.proj[5] * H.vph * 0.5f / w;
		if (pr < (float)ps2hwd_model_px)
		{
			G.md_lod++;
			return 0;
		}
		// the sprites go far to near: the models that are already there must not take the room of the one in front of the camera: a model of 40 pixels and more is never refused
		if (pr >= 40.0f)
		{
			mdl_budget_used += tris;
			return 1;
		}
	}
	if (ps2hwd_model_tris > 0 && mdl_budget_used + tris > (u32)ps2hwd_model_tris)
	{
		G.md_over++;
		return 0;
	}
	mdl_budget_used += tris;
	return 1;
}

static void draw_model(model_t *model, INT32 frameIndex, float duration, float tics,
	INT32 nextFrameIndex, FTransform *pos, float hscale, float vscale,
	UINT8 flipped, UINT8 hflipped, FSurfaceInfo *surf)
{
	float saved_mv[16], object_mv[16], lerp = 0.0f;
	const u32 t0 = cyc();
	u32 flags;
	int mi;
	const int light_on = H.model_light && !(ps2hwd_model_ab & 2);

	if (!H.up || !model || !pos || !surf || frameIndex < 0 || nextFrameIndex < -1)
		return;
	if (!mdl_work_get())
		return;
	G.md_draws++;
	if (duration > 0.0f && tics >= 0.0f)
		lerp = clampf((duration - tics) / duration, 0.0f, 1.0f);
	memcpy(saved_mv, H.mv, sizeof saved_mv);
	m_translate(H.mv, pos->x, pos->z, pos->y);
	m_rotate(H.mv, pos->anglez, 0.0f, 0.0f, -1.0f);
	m_rotate(H.mv, pos->anglex, 1.0f, 0.0f, 0.0f);
	m_rotate(H.mv, pos->angley, 0.0f, -1.0f, 0.0f);
	if (pos->roll)
	{
		m_translate(H.mv, pos->centerx, pos->centery, 0.0f);
		m_rotate(H.mv, pos->rollangle, pos->rollx, 0.0f, pos->rollz);
		m_translate(H.mv, -pos->centerx, -pos->centery, 0.0f);
	}
	m_scale(H.mv, hscale * 0.5f, vscale * (flipped ? -0.5f : 0.5f), hscale * (hflipped ? -0.5f : 0.5f));
	memcpy(object_mv, H.mv, sizeof object_mv);
	flags = (u32)surf->PolyFlags | PF_Modulated;
	// PS2-HW-126: the engine selects SHADER_MODEL (HWD.pfnSetShader) and the OpenGL driver always runs it, so hw_md2.c does not put PF_ColorMapped into the surface flags;
	// begin_draw lights only PF_ColorMapped polygons: a model drew at full brightness whatever the sector light, colormap or RF_FULLDARK said.
	if (H.shaders_on && H.shader >= 0 && H.shader <= 5)
		flags |= PF_ColorMapped;
	if (surf->PolyFlags & (PF_Additive | PF_Subtractive | PF_ReverseSubtract | PF_Multiplicative))
		flags |= PF_Occlude;
	else if (surf->PolyColor.s.alpha == 255)
		flags |= PF_Occlude | PF_Masked;
	for (mi = 0; mi < model->numMeshes; mi++)
	{
		const mesh_t *mesh = &model->meshes[mi];
		int fi, ni, tiny = mesh->tinyframes != NULL, light;
		float ml; // the interpolation of this mesh (a mesh whose two frames are the same needs none)

		if (mesh->numFrames <= 0 || mesh->numTriangles < 0 || mesh->numTriangles > 0x1FFFFFFF
			|| !mesh->uvs || (tiny ? !mesh->indices : !mesh->frames))
		{
			hw_limit(HW_INVALID, "invalid model mesh; draw rejected");
			continue;
		}
		fi = frameIndex % mesh->numFrames;
		ni = nextFrameIndex == -1 ? fi : nextFrameIndex % mesh->numFrames;
		if (tiny ? (!mesh->tinyframes[fi].vertices || !mesh->tinyframes[ni].vertices)
			: (!mesh->frames[fi].vertices || !mesh->frames[ni].vertices))
		{
			hw_limit(HW_INVALID, "model frame vertices unavailable; mesh rejected");
			continue;
		}
		ml = fi == ni ? 0.0f : lerp;
		light = light_on && tiny && mesh->tinyframes[fi].normals && mesh->tinyframes[ni].normals;
		memcpy(H.mv, object_mv, sizeof H.mv);
		if (tiny)
			m_scale(H.mv, 1.0f / 64.0f, 1.0f / 64.0f, 1.0f / 64.0f);
		H.mat_dirty = 1;
		if (!begin_draw(flags, surf))
			break;
		P.cull = (flipped ^ hflipped ^ pos->flip ^ pos->mirror) ? -1 : 1;
		if (light)
		{
			P.vcol = 1; // the vertices carry their light factor (a[2]); the polygon's colour modulates it (put_vertex format 6)
			P.vlight = 1;
		}
		G.md_meshes++;
		G.md_tris += (u32)mesh->numTriangles;
		if (tiny && mesh->numVertices <= MDL_MAXV && !(ps2hwd_model_ab & 1))
		{
#ifdef HW_VU0
			if (mesh->numTriangles <= MDL_MAXT && !(ps2hwd_model_ab & 4) && P.vu0 && P.npass == 1 && !P.cutx && !P.cuty && !P.water && !P.pass[0].ramp && P.textured && P.pass[0].tex
				&& !P.wrapx && !P.wrapy && (P.lmode == LM_NONE || P.lmode == LM_BANDS) && mdl_mesh_fast(mesh, fi, ni, ml, light))
			{
				// drawn by the fast path
			}
			else
#endif
			{
				int start, total = mesh->numTriangles * 3;

				G.md_verts += (u32)mesh->numVertices;
				mdl_interp(mesh, fi, ni, ml, light);
				for (start = 0; start < total; start += MDL_MAXI)
				{
					const int n = total - start < MDL_MAXI ? total - start : MDL_MAXI;
					const unsigned short *ix = mesh->indices + start;
					int k;

					for (k = 0; k < n; k++)
						mdlw->u.g.idx[k] = ix[k];
					emit_tris(mdlw->u.g.v, mdlw->u.g.idx, (u32)n, light ? mdlw->u.g.l : NULL);
				}
			}
		}
		else
		{
			mdl_mesh_corners(mesh, fi, ni, ml, tiny, light);
		}
		P.vcol = 0;
		P.vlight = 0;
	}
	P.cull = 0;
	memcpy(H.mv, saved_mv, sizeof H.mv);
	H.mat_dirty = 1;
	G.md_cyc += cyc() - t0;
}
