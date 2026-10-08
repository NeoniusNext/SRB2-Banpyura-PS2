// Wireframe on the GS (included by ps2_hwd.c). OPT11-MODEL (PS2-HW-270): see docs/GATES/g1/opt11-MODEL.md.
//
// The OpenGL driver has two wire modes:
//   PF_WireFrame     (r_opengl.c DrawPolygon: GL_LINES instead of GL_TRIANGLE_FAN) the vertices are consecutive PAIRS, one line each: the bounding boxes of renderhitbox (HWR_DrawBoundingBox, 24 vertices);
//   HWD_SET_WIREFRAME (glPolygonMode(GL_LINE)) the walls and planes of the frame (gr_wireframe On with devmode: the sky is not drawn) come out as the edges of their triangles.
// Both draw GS lines of one colour, with the depth test of the polygon's flags. The lines are clipped to the guard band and the near plane here (a GS coordinate is 12.4 bits: a line
// that leaves the 4096 pixel space wraps), the second mode uses the colour of the polygon (not its texture: the lines of the OpenGL driver are textured).

static int wire_mode;

// the part of the segment a-b inside the six planes of the clip volume (parametric, Liang-Barsky), 0 = nothing
static int wire_clip(const cv_t *a, const cv_t *b, cv_t *oa, cv_t *ob)
{
	const float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);
	float t0 = 0.0f, t1 = 1.0f;
	int p;

	for (p = 0; p < 6; p++)
	{
		const float da = plane_dist(a, p, gx, gy), db = plane_dist(b, p, gx, gy);

		if (da < 0.0f && db < 0.0f)
			return 0;
		if (da < 0.0f)
		{
			const float t = da / (da - db);

			if (t > t0)
				t0 = t;
		}
		else if (db < 0.0f)
		{
			const float t = da / (da - db);

			if (t < t1)
				t1 = t;
		}
	}
	if (t0 >= t1)
		return 0;
	oa->x = a->x + (b->x - a->x) * t0;
	oa->y = a->y + (b->y - a->y) * t0;
	oa->z = a->z + (b->z - a->z) * t0;
	oa->w = a->w + (b->w - a->w) * t0;
	oa->a[0] = a->a[0] + (b->a[0] - a->a[0]) * t0;
	oa->a[1] = a->a[1] + (b->a[1] - a->a[1]) * t0;
	ob->x = a->x + (b->x - a->x) * t1;
	ob->y = a->y + (b->y - a->y) * t1;
	ob->z = a->z + (b->z - a->z) * t1;
	ob->w = a->w + (b->w - a->w) * t1;
	ob->a[0] = a->a[0] + (b->a[0] - a->a[0]) * t1;
	ob->a[1] = a->a[1] + (b->a[1] - a->a[1]) * t1;
	return 1;
}

#define WIRE_SEGS 48 // segments per GIF packet

static struct
{
	cv_t v[2 * WIRE_SEGS];
	int n; // segments collected
} WR;

static void wire_flush(void)
{
	pack_t pk;
	qw_t *p, *o;
	int i;

	if (!WR.n)
		return;
	if (P.serial != H.serial)
	{
		pass_setup(0, 0);
		P.serial = H.serial;
	}
	memset(&pk, 0, sizeof pk);
	pk.fmt = P.pass[0].tex ? 0 : 2; // a textured polygon draws its lines textured (the colour of the texture, unlit: the OpenGL lines are the lit texture)
	pk.nreg = pack_regs(pk.fmt);
	pk.col = P.pass[0].col;
	p = pk_alloc((u32)(1 + 2 * WR.n * pk.nreg));
	p[0].d[0] = HWGIF_TAG((u32)(2 * WR.n), 1, 1, PRIM_LINE | (pass_prim(&P.pass[0]) & ~(u32)PRIM_FGE), 0, pk.nreg);
	p[0].d[1] = pack_regdesc(pk.fmt);
	o = p + 1;
	for (i = 0; i < 2 * WR.n; i++)
		o = put_vertex(o, &WR.v[i], &pk);
	H.st.verts_out += (u32)(2 * WR.n);
	H.gsr.valid &= ~(u32)RV_RGBAQ;
	WR.n = 0;
}

// one segment between two clip space vertices
static void wire_seg(const cv_t *a, const cv_t *b)
{
	cv_t ca, cb;

	if (!wire_clip(a, b, &ca, &cb))
		return;
	WR.v[2 * WR.n] = ca;
	WR.v[2 * WR.n + 1] = cb;
	if (++WR.n >= WIRE_SEGS)
		wire_flush();
}

// the plan of a wire draw: the flat colour of the polygon, no texture, no light (the colour of the surface is what the engine lit)
static int wire_begin(u32 flags, const FSurfaceInfo *surf)
{
	const u32 wf = flags & ~(u32)(PF_ColorMapped | PF_Ripple | PF_Decal | PF_WireFrame | PF_Fog);

	if (flags & PF_Invisible)
		return 0;
	if (!begin_draw(wf, surf))
		return 0;
	if (P.npass < 1)
		return 0;
	if (P.textured && (P.cutx || P.cuty || P.pass[0].clut_kind || P.pass[0].ramp))
	{
		// a texture the plain textured line cannot take (not a power of two axis, a CLUT of another kind): the flat colour plan
		if (!begin_draw(wf | PF_NoTexture, surf))
			return 0;
	}
	WR.n = 0;
	return 1;
}

// consecutive vertex pairs (PF_WireFrame)
static void wire_pairs(const FSurfaceInfo *surf, u32 flags, const FOutVector *v, int n)
{
	int i;

	if (!wire_begin(flags, surf))
		return;
	xf_update();
	for (i = 0; i + 1 < n; i += 2)
	{
		cv_t a, b;

		xform_vertex(&v[i], NULL, &a);
		xform_vertex(&v[i + 1], NULL, &b);
		wire_seg(&a, &b);
	}
	wire_flush();
}

// the edges of a triangle fan: the outline and the diagonals from the first vertex (what glPolygonMode(GL_LINE) makes of GL_TRIANGLE_FAN)
static void wire_fan_edges(const FOutVector *v, const u32 *idx, int n)
{
	cv_t cw[PS2HWD_MAXPOLY];
	int i;

	if (n < 3 || n > PS2HWD_MAXPOLY)
		return;
	for (i = 0; i < n; i++)
		xform_vertex(&v[idx ? idx[i] : (u32)i], NULL, &cw[i]);
	for (i = 0; i < n; i++)
		wire_seg(&cw[i], &cw[i + 1 == n ? 0 : i + 1]);
	for (i = 2; i + 1 < n; i++)
		wire_seg(&cw[0], &cw[i]);
}

// walls and planes in the wire mode of the engine: one polygon (a fan)
static void wire_poly(const FSurfaceInfo *surf, u32 flags, const FOutVector *v, int n)
{
	if (!wire_begin(flags, surf))
		return;
	xf_update();
	wire_fan_edges(v, NULL, n);
	wire_flush();
}

// a batch of fans (PS2HWD_DrawFans: desc = first vertex, count, light per fan)
static void wire_fans(const FSurfaceInfo *surf, u32 flags, const FOutVector *base, unsigned int nfans, const unsigned int *desc)
{
	unsigned int i;

	if (!wire_begin(flags, surf))
		return;
	xf_update();
	for (i = 0; i < nfans; i++)
		wire_fan_edges(base + desc[3 * i], NULL, (int)desc[3 * i + 1]);
	wire_flush();
}

// indexed triangles (hw_DrawIndexedTriangles)
static void wire_tris(const FSurfaceInfo *surf, u32 flags, const FOutVector *v, const u32 *idx, u32 nidx)
{
	u32 i;

	if (!wire_begin(flags, surf))
		return;
	xf_update();
	for (i = 0; i + 2 < nidx; i += 3)
		wire_fan_edges(v, idx + i, 3);
	wire_flush();
}
