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

#define MDL_MAXV 2048 // vertices of a mesh the indexed path takes
#define MDL_MAXI 3072 // indices per emit_tris call
#define MDL_FAST_MAXV 6144 // vertices of a mesh the fast path takes (the boss BRAK has 6061)
#define MDL_FAST_MAXT 8704 // triangles of a mesh the fast path takes (BRAK 8294)
#define MDL_CORNERS 192 // corners per call of the corner by corner path

typedef struct
{
	s32 xi, yi, zi; // GS X, Y (x16 + 2048), Z24 (the layout of pv_t: VU0 stores the result of the projection here); zi is shifted by 4 for the XYZF formats
	float q; // 1 / w
	qw_t stq; // the ST + Q quadword of the GIF packet: s * tsx * q, t * tsy * q, q (made once per vertex, copied once per triangle corner)
	float w; // eye depth
	u32 ocl; // outcode (bits 0..5)
	u32 pad[2];
	qw_t col4; // gr_modellighting: the RGBAQ quadword of the vertex (the colour of the pass scaled by the light factor of the vertex)
} __attribute__((aligned(64))) mdl_v_t; // 64 bytes: a cache line

// the work areas are zone blocks made when a model needs them (a game without models does not pay for them) and given back under memory pressure (PS2Models reclaim hook)
typedef struct
{
	FOutVector v[MDL_MAXV];
	float l[MDL_MAXV]; // the light factor of each vertex (only with gr_modellighting)
	u32 idx[MDL_MAXI];
} __attribute__((aligned(16))) mdlgen_t;

static mdlgen_t *mdlg; // the indexed path and the corner path
static void *mdlf_blk; // the fast path: mdl_v_t mv[cap_v + 1] (one spare entry: the second lane of the last pair of an odd number of vertices), u16 ord[cap_t], u8 row[cap_t]
static mdl_v_t *mdlf_mv;
static u16 *mdlf_ord;
static u8 *mdlf_row;
static u32 mdlf_cv, mdlf_ct;
static u32 mdl_work_frame = 0xFFFFFFFFu;
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

static void mdl_work_note(void)
{
	mdl_work_frame = Z_FrameCount();
	if (!mdl_lf_ready)
		mdl_init_tables();
}

// the area of the indexed path and the corner path
static int mdl_gen_get(void)
{
	if (mdlg)
		return 1;
	mdlg = (mdlgen_t *)Z_TryMallocAlign(sizeof *mdlg, PU_STATIC, NULL, 6);
	if (!mdlg)
	{
		hw_limit(HW_INVALID, "no memory for the model work area; the mesh is not drawn");
		return 0;
	}
	return 1;
}

static size_t mdlf_size(u32 cv, u32 ct)
{
	return ((size_t)(cv + 1) * sizeof(mdl_v_t) + (size_t)ct * 3 + 63) & ~(size_t)63;
}

// the area of the fast path for a mesh of nv vertices and nt triangles (grown, never shrunk until the zone asks for it back); 0 = no room (the mesh takes another path)
static int mdl_fast_get(u32 nv, u32 nt)
{
	u32 cv, ct;
	void *blk;

	if (mdlf_blk && nv <= mdlf_cv && nt <= mdlf_ct)
		return 1;
	cv = nv <= 1024 ? 1024 : nv <= 2048 ? 2048 : nv <= 4096 ? 4096 : MDL_FAST_MAXV;
	ct = nt <= 2048 ? 2048 : nt <= 4096 ? 4096 : MDL_FAST_MAXT;
	if (mdlf_blk)
	{
		const u32 ocv = mdlf_cv, oct = mdlf_ct;

		Z_Free(mdlf_blk); // the old one first: both would not fit at the sizes that matter
		mdlf_blk = NULL;
		mdlf_cv = mdlf_ct = 0;
		blk = Z_TryMallocAlign(mdlf_size(cv, ct), PU_STATIC, NULL, 6);
		if (!blk)
		{
			cv = ocv; // the area there was
			ct = oct;
			blk = Z_TryMallocAlign(mdlf_size(cv, ct), PU_STATIC, NULL, 6);
		}
	}
	else
	{
		blk = Z_TryMallocAlign(mdlf_size(cv, ct), PU_STATIC, NULL, 6);
	}
	if (!blk)
		return 0;
	mdlf_blk = blk;
	mdlf_cv = cv;
	mdlf_ct = ct;
	mdlf_mv = (mdl_v_t *)blk;
	mdlf_ord = (u16 *)((u8 *)blk + (size_t)(cv + 1) * sizeof(mdl_v_t));
	mdlf_row = (u8 *)(mdlf_ord + ct);
	return nv <= cv && nt <= ct;
}

// the zone wants room (Z_AddReclaimHook of ps2_models.c): the work areas not used in this frame go back; returns the bytes freed
size_t PS2HWD_ModelWorkReclaim(void)
{
	size_t got = 0;

	if (mdl_work_frame == Z_FrameCount())
		return 0;
	if (mdlg)
	{
		got += sizeof *mdlg;
		Z_Free(mdlg);
		mdlg = NULL;
	}
	if (mdlf_blk)
	{
		got += mdlf_size(mdlf_cv, mdlf_ct);
		Z_Free(mdlf_blk);
		mdlf_blk = NULL;
		mdlf_cv = mdlf_ct = 0;
	}
	return got;
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
	FOutVector *o = mdlg->v;
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
			mdlg->l[k] = mdl_lf[mdl_lidx(na, nb, k, lerp)];
}

// the corner by corner path (OPT10): used for meshes the work area cannot hold and for models in the float format
static void mdl_mesh_corners(const mesh_t *mesh, int fi, int ni, float lerp, int tiny, int light)
{
	int start;
	u32 k;

	for (k = 0; k < MDL_CORNERS; k++)
		mdlg->idx[k] = k;
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
			mdlg->v[c].x = xyz[0];
			mdlg->v[c].y = xyz[1];
			mdlg->v[c].z = xyz[2];
			mdlg->v[c].s = mesh->uvs[vi * 2];
			mdlg->v[c].t = mesh->uvs[vi * 2 + 1];
			if (light)
				mdlg->l[c] = tiny ? mdl_lf[mdl_lidx((const signed char *)mesh->tinyframes[fi].normals, (const signed char *)mesh->tinyframes[ni].normals, vi, lerp)] : 1.0f;
		}
		if (!valid)
		{
			hw_limit(HW_INVALID, "model index out of range; batch rejected");
			continue;
		}
		emit_tris(mdlg->v, mdlg->idx, (u32)n, light ? mdlg->l : NULL);
		G.md_slow++;
	}
}

#ifdef HW_VU0
// vu0_xform2 of ps2_hw_vu0.inc with the vertices as floats: in = x, y, z, 1, x, y, z, 1 (16 byte aligned, made by the caller); c0, c1 = 16 byte aligned scratch for the clip coordinates (only w is read back)
static inline void mdl_xform2(const float *in, float *c0, float *c1, void *o0, void *o1, void *f0, void *f1, int *oc0, int *oc1)
{
	u32 f, fa, fb;

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
		"sqc2 $vf10, 0(%6)\n"
		"vftoi0.xyz $vf10, $vf10\n"
		"sqc2 $vf10, 0(%4)\n"
		"vwaitq\n"
		"vmulq.xyz $vf17, $vf15, $Q\n"
		"vmulq.w $vf18, $vf0, $Q\n"
		"vmulaw $ACC, $vf12, $vf0\n"
		"vmadd.xyz $vf18, $vf17, $vf11\n"
		"vmax.z $vf18, $vf18, $vf0\n"
		"vmini.z $vf18, $vf18, $vf13\n"
		"sqc2 $vf18, 0(%7)\n"
		"vftoi0.xyz $vf18, $vf18\n"
		"sqc2 $vf18, 0(%5)\n"
		: "=&r"(f) : "r"(in), "r"(c0), "r"(c1), "r"(o0), "r"(o1), "r"(f0), "r"(f1) : "memory");
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

// one vertex as GIF data (3 quadwords): ST + Q, the colour (fmt 6: the vertex's own, else the pass's), XYZ (fmt 0) or XYZF (fmt 1, 6; zi was shifted by 4 when the vertex was made, fog4 = F << 4)
static inline __attribute__((always_inline)) qw_t *mdl_put_v(qw_t *o, const mdl_v_t *m, const qw_t *colqw, u32 fog4, const int fmt)
{
	o[0] = m->stq;
	o[1] = fmt == 6 ? m->col4 : *colqw;
	o[2].d[0] = (u64)(u32)m->xi | ((u64)(u32)m->yi << 32);
	o[2].d[1] = (u64)(u32)m->zi | ((u64)fog4 << 32);
	return o + 3;
}

// the triangles of `list` (n of them), one GIF run: the vertices of every triangle from mv, the colour and F as given
static inline __attribute__((always_inline)) void mdl_emit_list(const mdl_v_t *mv, const unsigned short *ix, const u16 *list, u32 n, const qw_t *colqw, u32 fog4, const int fmt)
{
	u32 i;

	for (i = 0; i < n; i++)
	{
		const unsigned tt = list[i];
		qw_t *o = ls_reserve(9);

		o = mdl_put_v(o, &mv[ix[tt * 3]], colqw, fog4, fmt);
		o = mdl_put_v(o, &mv[ix[tt * 3 + 1]], colqw, fog4, fmt);
		mdl_put_v(o, &mv[ix[tt * 3 + 2]], colqw, fog4, fmt);
		LS.n += 3;
	}
}

static void mdl_emit_fmt(const mdl_v_t *mv, const unsigned short *ix, const u16 *list, u32 n, const qw_t *colqw, u32 fog4, int fmt)
{
	if (fmt == 6)
		mdl_emit_list(mv, ix, list, n, colqw, fog4, 6);
	else if (fmt == 1)
		mdl_emit_list(mv, ix, list, n, colqw, fog4, 1);
	else
		mdl_emit_list(mv, ix, list, n, colqw, fog4, 0);
}

// the GS state of a row of the colormap (palette lighting), or of the plain plan
static void mdl_set_row(int row, int pal)
{
	if (pal)
	{
		if (row != P.pal_row || P.serial != H.serial)
		{
			P.pass[0].clut_param = pal_param(row);
			pass_setup(0, 0);
			P.serial = H.serial;
			P.pal_row = row;
		}
	}
	else if (P.serial != H.serial)
	{
		pass_setup(0, 0);
		P.serial = H.serial;
	}
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
	mdl_v_t *mv = mdlf_mv;
	u16 *ord = mdlf_ord;
	u8 *rowof = mdlf_row;
	u32 count[34], start[34];
	int k, t, minrow = 255, maxrow = -1, nord = 0;
	static float clip0[4] __attribute__((aligned(16))), clip1[4] __attribute__((aligned(16)));
	static float in[8] __attribute__((aligned(16))) = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
	const pass_t *ps = &P.pass[0];
	const int fmt = light ? 6 : ((bands && !pal && ps->fog) ? 1 : 0);
	const float cull = (float)P.cull;
	const u32 col = ps->col;
	u32 fogtab[34];
	int fogknown = 0, uni = 1, urow = 0, ucls = 0;
	u32 orall = 0; // the outcodes of all vertices or'ed: 0 = the whole mesh is inside the guard band and the near plane
	u32 nrej = 0, nclip = 0;
	float wmin = 1e30f, wmax = -1e30f;
	qw_t colqw;
	const u32 tp0 = cyc();
	u32 tp1, tp2;

	colqw.w[0] = col & 255;
	colqw.w[1] = (col >> 8) & 255;
	colqw.w[2] = (col >> 16) & 255;
	colqw.w[3] = col >> 24;
	vu0_need();
	// 1. every vertex: interpolation, the VU0 transform, the GIF quadwords (ST + Q, zi for the format); the colour with the light factor below
	{
		const int zs = fmt != 0 ? 4 : 0;
		const float *u = uv;

		for (k = 0; k < nv; k += 2, u += 4)
		{
			int o0, o1;
			mdl_v_t *m0 = &mv[k], *m1 = m0 + 1; // (the spare entry takes the second lane of an odd last vertex)

			mdl_pos(pa + k * 3, pb + k * 3, lerp, in);
			if (k + 1 < nv)
				mdl_pos(pa + k * 3 + 3, pb + k * 3 + 3, lerp, in + 4);
			else
				memcpy(in + 4, in, 3 * sizeof(float));
			mdl_xform2(in, clip0, clip1, m0, m1, &m0->col4, &m1->col4, &o0, &o1); // (col4 holds the float screen coordinates until the triangles are classified)
			{
				const float q = m0->q, w = clip0[3];

				m0->w = w;
				m0->ocl = (u32)o0;
				orall |= (u32)o0;
				m0->stq.f[0] = u[0] * tsx * q;
				m0->stq.f[1] = u[1] * tsy * q;
				m0->stq.f[2] = q;
				m0->zi <<= zs;
				if (bands && o0 == 0)
				{
					if (w < wmin)
						wmin = w;
					if (w > wmax)
						wmax = w;
				}
			}
			if (k + 1 < nv)
			{
				const float q = m1->q, w = clip1[3];

				m1->w = w;
				m1->ocl = (u32)o1;
				orall |= (u32)o1;
				m1->stq.f[0] = u[2] * tsx * q;
				m1->stq.f[1] = u[3] * tsy * q;
				m1->stq.f[2] = q;
				m1->zi <<= zs;
				if (bands && o1 == 0)
				{
					if (w < wmin)
						wmin = w;
					if (w > wmax)
						wmax = w;
				}
			}
		}
	}
	G.md_verts += (u32)nv;
	tp1 = cyc();
	G.md_c1 += tp1 - tp0;

	// 2. the light class: when every vertex inside the view is in one darkness class (the class is a non-decreasing function of the depth) the mesh has one row
	if (bands && wmin <= wmax)
	{
		ucls = lf_class_w(wmin);
		uni = ucls == lf_class_w(wmax);
		urow = pal ? pal_row_of(ucls) : ucls;
	}
	// 3. the triangles: flags, cull, (class)
	if (uni && orall == 0)
	{
		// the whole mesh is inside: only the cull test
		for (t = 0; t < nt; t++)
		{
			const mdl_v_t *a = &mv[ix[t * 3]], *b = &mv[ix[t * 3 + 1]], *c = &mv[ix[t * 3 + 2]];
			const float area = (b->col4.f[0] - a->col4.f[0]) * (c->col4.f[1] - a->col4.f[1]) - (c->col4.f[0] - a->col4.f[0]) * (b->col4.f[1] - a->col4.f[1]);

			if (area * cull >= 0.0f)
				nrej++;
			else
				ord[nord++] = (u16)t;
		}
		minrow = maxrow = urow;
	}
	else if (uni)
	{
		for (t = 0; t < nt; t++)
		{
			const mdl_v_t *a = &mv[ix[t * 3]], *b = &mv[ix[t * 3 + 1]], *c = &mv[ix[t * 3 + 2]];
			const u32 oa = a->ocl, ob = b->ocl, oc = c->ocl;
			float area;

			if (oa & ob & oc)
			{
				nrej++;
				continue;
			}
			if (oa | ob | oc)
			{
				nclip++;
				mdl_tri_general(mesh, fi, ni, lerp, light, ix[t * 3], ix[t * 3 + 1], ix[t * 3 + 2]);
				continue;
			}
			area = (b->col4.f[0] - a->col4.f[0]) * (c->col4.f[1] - a->col4.f[1]) - (c->col4.f[0] - a->col4.f[0]) * (b->col4.f[1] - a->col4.f[1]); // screen space, y down: the opposite sign of the clip space test of emit_tris
			if (area * cull >= 0.0f)
			{
				nrej++;
				continue;
			}
			ord[nord++] = (u16)t;
		}
		minrow = maxrow = urow;
	}
	else
	{
		memset(count, 0, sizeof count);
		for (t = 0; t < nt; t++)
		{
			const mdl_v_t *a = &mv[ix[t * 3]], *b = &mv[ix[t * 3 + 1]], *c = &mv[ix[t * 3 + 2]];
			const u32 oa = a->ocl, ob = b->ocl, oc = c->ocl;
			float area;
			int row;

			rowof[t] = 255;
			if (oa & ob & oc)
			{
				nrej++;
				continue;
			}
			if (oa | ob | oc)
			{
				nclip++;
				mdl_tri_general(mesh, fi, ni, lerp, light, ix[t * 3], ix[t * 3 + 1], ix[t * 3 + 2]);
				continue;
			}
			area = (b->col4.f[0] - a->col4.f[0]) * (c->col4.f[1] - a->col4.f[1]) - (c->col4.f[0] - a->col4.f[0]) * (b->col4.f[1] - a->col4.f[1]);
			if (area * cull >= 0.0f)
			{
				nrej++;
				continue;
			}
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
	}
	H.st.polys += (u32)nt;
	H.st.rejected += nrej;
	H.st.clipped += nclip;
	tp2 = cyc();
	G.md_c2 += tp2 - tp1;
	if (!nord)
		return 1;

	// 4. the colour with the light factor of every vertex (col4 held the screen coordinates until now), then the surviving triangles, by row when the rows are CLUTs (a GS state each), in order otherwise
	if (light)
		for (k = 0; k < nv; k++)
		{
			const float lf = mdl_lf[mdl_lidx(na, nb, k, lerp)];

			mv[k].col4.w[0] = (u32)((float)(col & 255) * lf + 0.5f);
			mv[k].col4.w[1] = (u32)((float)((col >> 8) & 255) * lf + 0.5f);
			mv[k].col4.w[2] = (u32)((float)((col >> 16) & 255) * lf + 0.5f);
			mv[k].col4.w[3] = col >> 24;
		}
	ls_close();
	if (uni)
	{
		u32 fog4 = 0;

		if (fmt != 0)
		{
			u32 fog = 255;

			if (!pal)
			{
				int on;
				const u32 f = lf_fog(ucls, &on);

				fog = on ? f : 255;
			}
			fog4 = fog << 4;
		}
		mdl_set_row(pal ? urow : 0, pal);
		LS.nreg = 3;
		LS.fmt = fmt;
		mdl_emit_fmt(mv, ix, ord, (u32)nord, &colqw, fog4, fmt);
	}
	else if (pal)
	{
		u32 s = 0;
		const u32 fog4 = fmt != 0 ? 255u << 4 : 0;

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
				ls_close();
				mdl_set_row(k, 1);
				LS.nreg = 3;
				LS.fmt = fmt;
				mdl_emit_fmt(mv, ix, ord + s, count[k], &colqw, fog4, fmt);
			}
	}
	else
	{
		// fog classes (palette rendering off): F per triangle
		mdl_set_row(0, 0);
		LS.nreg = 3;
		LS.fmt = fmt;
		for (t = 0; t < nt; t++)
		{
			qw_t *o;
			u32 fog = 255;

			if (rowof[t] == 255)
				continue;
			if (fmt != 0)
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
			if (fmt == 6)
			{
				o = mdl_put_v(o, &mv[ix[t * 3]], &colqw, fog << 4, 6);
				o = mdl_put_v(o, &mv[ix[t * 3 + 1]], &colqw, fog << 4, 6);
				mdl_put_v(o, &mv[ix[t * 3 + 2]], &colqw, fog << 4, 6);
			}
			else
			{
				o = mdl_put_v(o, &mv[ix[t * 3]], &colqw, fog << 4, 1);
				o = mdl_put_v(o, &mv[ix[t * 3 + 1]], &colqw, fog << 4, 1);
				mdl_put_v(o, &mv[ix[t * 3 + 2]], &colqw, fog << 4, 1);
			}
			LS.n += 3;
		}
	}
	ls_close();
	H.st.verts_out += (u32)nord * 3u;
	H.gsr.valid &= ~(u32)RV_RGBAQ; // packed vertices changed Q (and the colours) behind the A+D state cache
	G.md_c3 += cyc() - tp2;
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
	mdl_work_note();
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
		{
			const u32 tb = cyc();
			const int ok = begin_draw(flags, surf);

			G.md_ctx += cyc() - tb;
			if (!ok)
				break;
		}
		P.cull = (flipped ^ hflipped ^ pos->flip ^ pos->mirror) ? -1 : 1;
		if (light)
		{
			P.vcol = 1; // the vertices carry their light factor (a[2]); the polygon's colour modulates it (put_vertex format 6)
			P.vlight = 1;
		}
		G.md_meshes++;
		G.md_tris += (u32)mesh->numTriangles;
		{
			int done = 0;

#ifdef HW_VU0
			if (tiny && mesh->numVertices <= MDL_FAST_MAXV && mesh->numTriangles <= MDL_FAST_MAXT && !(ps2hwd_model_ab & 5) && P.vu0 && P.npass == 1 && !P.cutx && !P.cuty && !P.water
				&& !P.pass[0].ramp && P.textured && P.pass[0].tex && !P.wrapx && !P.wrapy && (P.lmode == LM_NONE || P.lmode == LM_BANDS)
				&& mdl_fast_get((u32)mesh->numVertices, (u32)mesh->numTriangles))
				done = mdl_mesh_fast(mesh, fi, ni, ml, light);
#endif
			if (!done && mdl_gen_get())
			{
				if (tiny && mesh->numVertices <= MDL_MAXV && !(ps2hwd_model_ab & 1))
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
							mdlg->idx[k] = ix[k];
						emit_tris(mdlg->v, mdlg->idx, (u32)n, light ? mdlg->l : NULL);
					}
				}
				else
				{
					mdl_mesh_corners(mesh, fi, ni, ml, tiny, light);
				}
			}
		}
		P.vcol = 0;
		P.vlight = 0;
	}
	P.cull = 0;
	memcpy(H.mv, saved_mv, sizeof H.mv);
	H.mat_dirty = 1;
	G.md_cyc += cyc() - t0;
}
