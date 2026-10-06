// PS2 GS hardware renderer driver: render state, polygon transform and GIF packing (included by ps2_hwd.c).
//
// A polygon is drawn according to a plan made by begin_draw(): up to MAXPASS GS passes (the PC blend modes that the GS
// alpha equation cannot do in one pass use multi-pass compositions, with remaining format/effect gaps documented in
// docs/HW_RENDERER.md), optionally cut into NPOT repeat periods and steps of the Doom light staircase.

static u8 col_lut[256]; // engine colour/alpha 0..255 -> GS 0..0x80 (0x80 = 1.0)

static void col_lut_init(void)
{
	int i;

	for (i = 0; i < 256; i++)
		col_lut[i] = (u8)((i * 128 + 127) / 255);
}

#define TEST_BITS(ate, atst, aref) ((u64)(ate) | ((u64)(atst) << 1) | ((u64)(aref) << 4))
enum
{
	ATST_ALWAYS = 1,
	ATST_GREATER = 6,
	ATST_NOTEQUAL = 7,
};
// ALPHA_1: (A - B) * C >> 7 + D ; A, B, D: 0 Cs, 1 Cd, 2 zero ; C: 0 As, 1 Ad, 2 FIX
#define GSA(a, b, c, d) ((u64)(a) | ((u64)(b) << 2) | ((u64)(c) << 4) | ((u64)(d) << 6))
#define GSFIX(v) ((u64)(v) << 32)
#define ALPHA_TRANS GSA(0, 1, 0, 1) // (Cs - Cd) * As + Cd
#define ALPHA_ADD GSA(0, 2, 0, 1) // Cs * As + Cd
#define ALPHA_RSUB GSA(2, 0, 0, 1) // Cd - Cs * As
#define ALPHA_KEEPDST GSA(2, 1, 0, 1) // Cd * (1 - As)
#define ALPHA_SRCPLUS (GSA(0, 2, 2, 1) | GSFIX(0x80)) // Cs + Cd
#define ALPHA_SRCMINUS (GSA(0, 1, 2, 2) | GSFIX(0x80)) // Cs - Cd, clamped at 0
#define ALPHA_DSTSCALE GSA(1, 2, 0, 2) // Cd * As
#define ALPHA_SRCSCALE GSA(0, 2, 0, 2) // Cs * As
#define ALPHA_AD GSA(0, 1, 1, 1) // (Cs - Cd) * Ad + Cd (destination alpha as the mask)

#define MAXPASS 4
static void lit_fast_plan(void); // ps2_hw_fast.inc (PS2-HW-43)
#define PRIM_IIP (1u << 3)
#define PRIM_TME (1u << 4)
#define PRIM_FGE (1u << 5)
#define PRIM_ABE (1u << 6)

typedef struct
{
	u64 alpha, test;
	u32 fbmsk; // bits set protect the frame buffer
	u32 col; // RGBAQ value (R | G << 8 | B << 16 | A << 24), GS scale
	u32 clut_kind, clut_param; // 0 = the texture's own palette image
	u8 clut_keyed;
	u8 abe, zwrite, decal; // alpha blending, depth write, TFX = DECAL
	u8 fog; // FGE: bands use the GS fog
	u8 ramp; // overlay pass: the texture is the depth ramp (drawn through the overlay queue)
	u8 tex; // samples the polygon's texture (else flat colour)
	u8 pad;
} pass_t;

// light ramp / fog specification (a polygon's lighting; hashed into a CLUT key)
typedef struct
{
	lightp_t lp;
	u32 fade; // fade colour 0x00BBGGRR
	u32 tint; // tint colour + alpha, 0xAABBGGRR
	u8 alpha; // PolyColor alpha (fog blocks)
	u8 variant; // RV_*
	u8 scale; // 0 fine (zmax 256), 1 (512), 2 coarse (2032)
} rampspec_t;

enum
{
	RV_FADE = 1, // alpha = darkness, colour = fade colour (decal): the overlay of a solid polygon
	RV_FOG_R, RV_FOG_G, RV_FOG_B, // alpha = channel of the fog block's source colour: destination *= source colour
	RV_FOG_ADD, // colour = fog block source colour, alpha = its alpha: destination += colour * alpha
};

enum { LM_NONE = 0, LM_BANDS, LM_RAMP };

// the plan of the polygon being drawn (set by begin_draw)
static struct
{
	int textured;
	texrec_t *r;
	u32 flags;
	int npass;
	pass_t pass[MAXPASS];
	int wrapx, wrapy; // the texture repeats in s / t
	int cutx, cuty; // the repeat has to be made by cutting at whole texture widths (size not a power of two)
	int linear;
	u64 clamp; // CLAMP_1 for the texture
	int lmode; // LM_*
	rampspec_t rs; // light of the polygon
	u32 fogcol; // FOGCOL (band mode) 0x00BBGGRR
	int water;
	float zo, zk, zmax, zbias;
	float ox, oy, kx, ky;
	float tsx, tsy; // texture coordinate scale (power of two sizes: 1)
	float ra, rb; // ramp coordinate: s = ra * w + rb
	int ramp_scale;
	int vcol; // vertices carry their own colours (sky dome)
	int cull; // 0 two-sided; +1 keep CCW; -1 keep CW (models)
	int fast; // one plain pass, no cuts: polygons take emit_fast_fan (state set up once per plan, see serial)
	int litfast; // as fast, but with the light staircase (LM_BANDS): polygons of one darkness class take lit_fast_poly (ps2_hw_fast.inc, PS2-HW-43)
	int vuok, vu; // the plan can be drawn by the VU1 program (vuok); PS2HWD_DrawFans turns vu on for the polygons of a batch (ps2_hw_vu1.inc, PS2-HW-45)
	int vu0; // the plan's constants are in VU0 (vu0_batch_load): emit_poly transforms on VU0
	u32 serial; // H.serial when pass 0 was last set up for this plan
} P;

// ---- write the registers that differ from what the GS has (one A+D tag) ----

typedef struct
{
	u64 test, alpha, clamp, tex0, tex1, zbuf, frame, scissor, fogcol;
	u64 rgbaq;
	int use_tex, use_fog;
} gsstate_t;

static void state_flush(const gsstate_t *s)
{
	u64 vals[10];
	u32 regs[10];
	int n = 0, i;
	const u64 cld_mask = 7ull << 61;

	if (!(H.gsr.valid & RV_FRAME) || H.gsr.frame != s->frame)
	{
		vals[n] = s->frame;
		regs[n++] = GSR_FRAME_1;
		H.gsr.frame = s->frame;
		H.gsr.valid |= RV_FRAME;
	}
	if (!(H.gsr.valid & RV_ZBUF) || H.gsr.zbuf != s->zbuf)
	{
		vals[n] = s->zbuf;
		regs[n++] = GSR_ZBUF_1;
		H.gsr.zbuf = s->zbuf;
		H.gsr.valid |= RV_ZBUF;
	}
	if (!(H.gsr.valid & RV_SCISSOR) || H.gsr.scissor != s->scissor)
	{
		vals[n] = s->scissor;
		regs[n++] = GSR_SCISSOR_1;
		H.gsr.scissor = s->scissor;
		H.gsr.valid |= RV_SCISSOR;
	}
	if (!(H.gsr.valid & RV_TEST) || H.gsr.test != s->test)
	{
		vals[n] = s->test;
		regs[n++] = GSR_TEST_1;
		H.gsr.test = s->test;
		H.gsr.valid |= RV_TEST;
	}
	if (!(H.gsr.valid & RV_ALPHA) || H.gsr.alpha != s->alpha)
	{
		vals[n] = s->alpha;
		regs[n++] = GSR_ALPHA_1;
		H.gsr.alpha = s->alpha;
		H.gsr.valid |= RV_ALPHA;
	}
	if (s->use_fog && (!(H.gsr.valid & RV_FOGCOL) || H.gsr.fogcol != s->fogcol))
	{
		vals[n] = s->fogcol;
		regs[n++] = GSR_FOGCOL;
		H.gsr.fogcol = s->fogcol;
		H.gsr.valid |= RV_FOGCOL;
	}
	if (s->use_tex)
	{
		if (!(H.gsr.valid & RV_CLAMP) || H.gsr.clamp != s->clamp)
		{
			vals[n] = s->clamp;
			regs[n++] = GSR_CLAMP_1;
			H.gsr.clamp = s->clamp;
			H.gsr.valid |= RV_CLAMP;
		}
		if (!(H.gsr.valid & RV_TEX1) || H.gsr.tex1 != s->tex1)
		{
			vals[n] = s->tex1;
			regs[n++] = GSR_TEX1_1;
			H.gsr.tex1 = s->tex1;
			H.gsr.valid |= RV_TEX1;
		}
		if (!(H.gsr.valid & RV_TEX0) || (H.gsr.tex0 & ~cld_mask) != (s->tex0 & ~cld_mask) || (s->tex0 & cld_mask))
		{
			vals[n] = s->tex0;
			regs[n++] = GSR_TEX0_1;
			H.gsr.tex0 = s->tex0;
			H.gsr.valid |= RV_TEX0;
		}
	}
	if (!(H.gsr.valid & RV_RGBAQ) || H.gsr.rgbaq != s->rgbaq)
	{
		vals[n] = s->rgbaq;
		regs[n++] = GSR_RGBAQ;
		H.gsr.rgbaq = s->rgbaq;
		H.gsr.valid |= RV_RGBAQ;
	}
	if (n)
	{
		qw_t *p = ad_alloc((u32)n);

		for (i = 0; i < n; i++)
			ad_set(&p[i], vals[i], regs[i]);
		H.st.state_writes += (u32)n;
	}
}

static u64 scissor_current(void)
{
	return scissor_reg((int)H.sciss_x0, (int)H.sciss_y0, (int)H.sciss_x1, (int)H.sciss_y1);
}

static inline u64 rgbaq_of(u32 col)
{
	return (u64)col | ((u64)0x3F800000u << 32); // Q = 1.0
}

// TEX0 / TEX1 / CLAMP of a texture record. wms/wmt: 0 repeat, 1 clamp. cbp: block of the CLUT (palette textures)
static void tex_regs(const texrec_t *r, int wms, int wmt, int linear, u32 cbp, int decal, u64 *tex0, u64 *tex1, u64 *clamp)
{
	u64 t0, cld = 0;

	*clamp = (u64)((r->region & 1) ? 2 : wms) | ((u64)((r->region & 2) ? 2 : wmt) << 2) | ((u64)(r->uw - 1) << 14) | ((u64)(r->vh - 1) << 34); // REGION_CLAMP (2): MINU = MINV = 0, MAXU = w - 1, MAXV = h - 1
	*tex1 = 1ull | ((u64)linear << 5) | ((u64)linear << 6); // LCM = 1 (fixed LOD), MMAG, MMIN
	t0 = (u64)r->blk | ((u64)r->tbw << 14) | ((u64)r->psm << 20) | ((u64)r->tw << 26) | ((u64)r->th << 30) | ((u64)(r->notcc ? 0 : 1) << 34) | ((u64)(decal ? 1 : 0) << 35);
	if (r->psm == PSM_T8)
	{
		t0 |= (u64)cbp << 37; // CPSM = CT32, CSM1, CSA = 0
		if (H.clut_loaded != cbp)
		{
			cld = 1;
			H.clut_loaded = cbp;
		}
	}
	*tex0 = t0 | (cld << 61);
}

static void lru_touch(int i)
{
	H.rec[i].stamp = H.frame_no;
	i = img_of(i); // OPT9 (PS2-HW-30): a view shares the LRU position of its image
	H.rec[i].stamp = H.frame_no;
	if (H.lru_head != i)
	{
		lru_unlink(i);
		lru_push_head(i);
	}
}

// ---- overlay queue: ramp passes of solid polygons, drawn later in one state ----

#define OVQ_QW 4096
static qw_t ovq[OVQ_QW] __attribute__((aligned(64)));
static struct
{
	u32 n; // qwords queued
	u32 clut; // block of the ramp CLUT
	u32 fade;
	int open;
	u64 tex0, tex1, clamp;
} OV;

static void ov_flush(void);
#define ov_flush_pending ov_flush

// ---- the depth ramp: a shared 256 x 1 identity texture; a CLUT image per lighting turns the interpolated eye depth into darkness ----

static int ramp_tex = NOREC; // record of the identity texture
static GLMipmap_t ramp_mip;

static int ramp_get(void)
{
	if (ramp_tex != NOREC && H.rec[ramp_tex].used && H.rec[ramp_tex].owner == &ramp_mip)
		return ramp_tex;
	{
		static u8 ident[256];
		int i;

		for (i = 0; i < 256; i++)
			ident[i] = (u8)i;
		memset(&ramp_mip, 0, sizeof ramp_mip);
		ramp_mip.format = GL_TEXFMT_P_8;
		ramp_mip.width = 256;
		ramp_mip.height = 1;
		ramp_mip.flags = 0;
		ramp_mip.data = ident;
		ramp_tex = tex_upload(&ramp_mip);
		ramp_mip.data = NULL;
		if (ramp_tex != NOREC)
		{
			H.rec[ramp_tex].region = 0;
			H.rec[ramp_tex].npot_x = H.rec[ramp_tex].npot_y = 0;
			H.rec[ramp_tex].pin = 1;
		}
	}
	return ramp_tex;
}

static const float ramp_zmax[3] = {256.0f, 512.0f, LIGHT_ZMAX};

static inline float zfrag_of_w(float w)
{
	return (w - H.near_plane) * (FAR_CLIPPING_PLANE / (FAR_CLIPPING_PLANE - H.near_plane));
}

static inline float w_of_zfrag(float z)
{
	return H.near_plane + z * ((FAR_CLIPPING_PLANE - H.near_plane) / FAR_CLIPPING_PLANE);
}

static u32 spec_hash(const rampspec_t *s)
{
	const u32 *w = (const u32 *)(const void *)s;
	u32 h = 2166136261u, i;
	rampspec_t c = *s; // padding bytes are not part of the hash

	(void)w;
	w = (const u32 *)(const void *)&c;
	for (i = 0; i < sizeof c / 4; i++)
	{
		h ^= w[i];
		h *= 16777619u;
	}
	return h;
}

// the 256 CLUT entries of a ramp (index order, GS scale)
static void ramp_entries(const rampspec_t *s, u32 *e)
{
	float zmax = ramp_zmax[s->scale];
	int i;
	float tint[3], tinta = (float)(s->tint >> 24) * (1.0f / 255.0f);
	float fr = (float)(s->fade & 255) * (1.0f / 255.0f), fg = (float)((s->fade >> 8) & 255) * (1.0f / 255.0f), fb = (float)((s->fade >> 16) & 255) * (1.0f / 255.0f);

	tint[0] = (float)(s->tint & 255) * (1.0f / 255.0f);
	tint[1] = (float)((s->tint >> 8) & 255) * (1.0f / 255.0f);
	tint[2] = (float)((s->tint >> 16) & 255) * (1.0f / 255.0f);
	for (i = 0; i < 256; i++)
	{
		float z = (i + 0.5f) * (zmax / 256.0f);
		int cls = doom_class(&s->lp, z), d128 = doom_darkness128(&s->lp, cls);
		float d = (float)d128 * (1.0f / 128.0f);

		if (s->variant == RV_FADE)
		{
			e[i] = s->fade | ((u32)d128 << 24);
		}
		else
		{
			// fog block: base colour white with alpha A; tint, then fade (GLSL_SOFTWARE_TINT_EQUATION / FADE_EQUATION)
			float c[3] = {1.0f, 1.0f, 1.0f}, a = (float)s->alpha * (1.0f / 255.0f);
			int k;

			if (tinta > 0.0f)
			{
				float st = sqrtf(tinta), bright = sqrtf(3.0f);

				for (k = 0; k < 3; k++)
				{
					c[k] = bright * (tint[k] * st) + c[k] * (1.0f - st);
					c[k] = c[k] < 0.0f ? 0.0f : c[k] > 1.0f ? 1.0f : c[k];
				}
			}
			c[0] += (fr - c[0]) * d;
			c[1] += (fg - c[1]) * d;
			c[2] += (fb - c[2]) * d;
			a += (1.0f - a) * d; // the fade colour's alpha is 1
			if (s->variant == RV_FOG_ADD)
				e[i] = (u32)(c[0] * 255.0f + 0.5f) | ((u32)(c[1] * 255.0f + 0.5f) << 8) | ((u32)(c[2] * 255.0f + 0.5f) << 16) | ((u32)(a * 128.0f + 0.5f) << 24);
			else
				e[i] = 0x00FFFFFFu | ((u32)(c[s->variant - RV_FOG_R] * 128.0f + 0.5f) << 24);
		}
	}
}

// CLUT image of a ramp: key = its hash (the cache holds the 124-4 most recent)
static u32 ramp_clut(const rampspec_t *s)
{
	u64 key = (u64)CK_RAMP | ((u64)spec_hash(s) << 16) | ((u64)s->variant << 8);
	int i, freeslot = -1, lru = -1, victim;
	static u32 e[256] __attribute__((aligned(64)));
	static u32 sw[256] __attribute__((aligned(64)));

	H.clut_tick++;
	for (i = CLUT_FIXED; i < CLUT_SLOTS; i++)
	{
		if (H.clut_ent[i].key == key)
		{
			H.clut_ent[i].stamp = H.clut_tick;
			return H.clut_base + (u32)i * CLUT_BLOCKS;
		}
		if (!H.clut_ent[i].key)
		{
			if (freeslot < 0)
				freeslot = i;
		}
		else if (lru < 0 || H.clut_ent[i].stamp < H.clut_ent[lru].stamp)
		{
			lru = i;
		}
	}
	victim = freeslot >= 0 ? freeslot : lru;
	if (freeslot < 0 && OV.n)
		ov_flush_pending(); // the queue may reference the image that is about to be replaced
	H.clut_ent[victim].key = key;
	H.clut_ent[victim].stamp = H.clut_tick;
	ramp_entries(s, e);
	for (i = 0; i < 256; i++)
		sw[clut_slot((u32)i)] = e[i];
	clut_write(H.clut_base + (u32)victim * CLUT_BLOCKS, sw);
	return H.clut_base + (u32)victim * CLUT_BLOCKS;
}

static void ov_flush(void)
{
	gsstate_t s;
	qw_t *p;

	if (!OV.n)
		return;
	memset(&s, 0, sizeof s);
	s.test = TEST_BITS(1, ATST_NOTEQUAL, 0) | (1ull << 16) | (2ull << 17); // alpha 0 is nothing; Z GEQUAL
	s.alpha = ALPHA_TRANS;
	s.zbuf = zbuf_reg(1);
	s.frame = frame_reg(H.target, 0);
	s.scissor = scissor_current();
	s.use_tex = 1;
	s.clamp = 1ull | (1ull << 2); // clamp both ways: beyond the ramp the last entry holds
	s.tex1 = 1ull;
	{
		const texrec_t *r = &H.rec[ramp_tex];

		tex_regs(r, 1, 1, 0, OV.clut, 1, &s.tex0, &s.tex1, &s.clamp);
		s.clamp = 1ull | (1ull << 2);
	}
	s.rgbaq = rgbaq_of(0);
	state_flush(&s);
	p = pk_alloc(OV.n);
	memcpy(p, ovq, (size_t)OV.n * sizeof(qw_t));
	H.st.passes++;
	OV.n = 0;
}

static void ov_flush_all(void)
{
	if (OV.n)
		ov_flush();
}

// ---- begin_draw: the plan ----

static void pass_flat(pass_t *ps)
{
	memset(ps, 0, sizeof *ps);
	ps->zwrite = 0;
}

// PS2-HW-66: census of the effects the engine asks for: a line "HWFX frame N: ..." every 300 frames lists what was drawn in them
// (which map exercises fog blocks, water, colormaps, additive/subtractive blends...: the scenes for the parity pictures)
static u32 fx_seen, fx_report_frame;

static void fx_note(u32 bit)
{
	fx_seen |= bit;
}

static void fx_report(void)
{
	static const char *const fxn[] = {"translucent", "additive", "subtractive", "revsub", "multiply", "environment", "fogblock", "water", "tint", "fade-colour",
		"decal", "invisible", "dome", "nodepth", "alpha-poly", "ripple-flag", "slope-or-tilt"};
	char line[256];
	u32 i;
	size_t n = 0;

	for (i = 0; i < sizeof fxn / sizeof fxn[0] && n < sizeof line - 24; i++)
		if (fx_seen & (1u << i))
			n += (size_t)snprintf(line + n, sizeof line - n, "%s ", fxn[i]);
	line[n] = '\0';
	CONS_Printf("HWFX frame %u: %s\n", (unsigned)H.frame_no, line);
	fx_seen = 0;
}

// Prepare everything for a polygon with these engine flags. Returns 0 when the polygon must not be drawn.
static void imm_prepare(const FOutVector *verts, unsigned int n); // ps2_hw_plan.inc

static int begin_draw(u32 flags, const FSurfaceInfo *surf)
{
	u32 blend;
	int textured, dbg = ps2hwd_dbg_flags, lit = 0, wall = 0, partial_tex = 0, i;
	texrec_t *r = NULL;
	int av = 255, cr = 255, cg = 255, cb = 255;
	int occlude, invisible;
	pass_t *ps;

	if (flags & PF_Corona) // needs a depth read-back; the engine only uses it with ALAM_LIGHTING, which is not built
	{
		hw_limit(HW_CORONA, "corona depth visibility is not implemented; draw rejected");
		return 0;
	}
	if (H.imm_tex && !(flags & PF_NoTexture) && H.cur_tex == NOREC && !H.cur_missing)
		imm_prepare(NULL, 0); // a draw that did not go through hw_DrawPolygon (the sky dome): the geometry is unknown, the texture is made resident at full size
	if (!(flags & PF_NoTexture) && H.cur_tex == NOREC && H.cur_missing)
	{
		H.st.tex_skipped++;
		return 0;
	}
	frame_prepare(0);

	textured = !(flags & PF_NoTexture) && H.cur_tex != NOREC;
	if (textured)
		r = &H.rec[H.cur_tex];
	if ((flags & PF_Modulated) && surf)
	{
		av = surf->PolyColor.s.alpha;
		cr = surf->PolyColor.s.red;
		cg = surf->PolyColor.s.green;
		cb = surf->PolyColor.s.blue;
	}
	blend = flags & PF_Blending;
	occlude = (flags & PF_Occlude) && !(dbg & 8);
	invisible = (flags & PF_Invisible) != 0;
	if (H.frame_no % 300 == 299 && fx_report_frame != H.frame_no)
	{
		fx_report_frame = H.frame_no;
		fx_report();
	}
	if (blend == (PF_Translucent & PF_Blending) && (flags & PF_Translucent))
		fx_note(1u << 0);
	else if (blend == (PF_Additive & PF_Blending))
		fx_note(1u << 1);
	else if (blend == (PF_Subtractive & PF_Blending))
		fx_note(1u << 2);
	else if (blend == (PF_ReverseSubtract & PF_Blending))
		fx_note(1u << 3);
	else if (blend == (PF_Multiplicative & PF_Blending))
		fx_note(1u << 4);
	else if (blend == (PF_Environment & PF_Blending))
		fx_note(1u << 5);
	else if (blend == PF_Fog)
		fx_note(1u << 6);
	if (flags & PF_Decal)
		fx_note(1u << 10);
	if (invisible)
		fx_note(1u << 11);
	if (flags & PF_NoDepthTest)
		fx_note(1u << 13);
	if (av != 255)
		fx_note(1u << 14);
	if (flags & PF_Ripple)
		fx_note(1u << 15);
	partial_tex = textured && r->psm == PSM_CT32 && !r->notcc && !r->screen;

	// shader lighting: PF_ColorMapped polygons under a built-in lit shader
	if (H.shaders_on && (flags & PF_ColorMapped) && surf && H.shader >= 0 && H.shader <= 5)
	{
		lit = 1;
		wall = H.shader == 1 || H.shader == 2 || H.shader == 3; // wall fudges: wall, sprite, model; floor fudges: floor, water, fog
		light_setup(&P.rs.lp, (int)surf->LightInfo.light_level, wall, (int)surf->LightInfo.fade_start, (int)surf->LightInfo.fade_end);
		P.rs.fade = (u32)surf->FadeColor.s.red | ((u32)surf->FadeColor.s.green << 8) | ((u32)surf->FadeColor.s.blue << 16);
		P.rs.tint = surf->TintColor.rgba;
		P.rs.alpha = (u8)av;
		if ((dbg & 4096) && H.frame_no == 150) // -hwdbg 4096: the light parameters of every lit polygon of frame 150
			CONS_Printf("HWT lit sh=%d fl=0x%x a=%d L=%d fs=%d fe=%d fade=%06x tint=%08x tex=%s\n", H.shader, (unsigned)flags, av, (int)P.rs.lp.light, P.rs.lp.fade_start, P.rs.lp.fade_end, (unsigned)P.rs.fade, (unsigned)P.rs.tint, textured && r->owner ? HWR_PS2_TexName(r->owner) : "-");
		if ((P.rs.tint >> 24) != 0)
			fx_note(1u << 8);
		if (P.rs.fade != 0 || P.rs.lp.fade_start != 0 || P.rs.lp.fade_end != 31)
			fx_note(1u << 9);
		if (H.shader == 4)
			fx_note(1u << 7);
	}
	P.rs.variant = 0;
	P.rs.scale = 0;
	P.lmode = LM_NONE;
	P.fogcol = 0;
	P.water = lit && H.shader == 4 && (flags & PF_Ripple);
	// PS2-HW-63: a PF_Ripple polygon is made by emit_water_poly (depth bands with shifted texture coordinates)

	P.textured = textured;
	P.r = r;
	P.flags = flags;
	P.vcol = 0;
	P.cull = 0;
	P.npass = 0;
	P.zbias = (flags & PF_Decal) ? (float)cfg.decal_bias : 0.0f;
	P.wrapx = P.wrapy = 0;
	P.cutx = P.cuty = 0;
	P.tsx = P.tsy = 1.0f;
	if (flags & PF_WireFrame)
		hw_limit(HW_WIREFRAME, "wireframe flag is not implemented; filled geometry drawn");

	if (textured)
	{
		int wms = r->wrapx ? 0 : 1, wmt = r->wrapy ? 0 : 1; // 0 = repeat, 1 = clamp
		int linear;

		if (flags & PF_ForceWrapX)
			wms = 0;
		if (flags & PF_RemoveYWrap)
			wmt = 1;
		if (flags & PF_ForceWrapY)
			wmt = 0;
		P.wrapx = !wms;
		P.wrapy = !wmt;
		P.cutx = P.wrapx && r->npot_x;
		P.cuty = P.wrapy && r->npot_y;
		P.tsx = r->sxs;
		P.tsy = r->sys;
		switch (H.tex_filter)
		{
			case HWD_SET_TEXTUREFILTER_BILINEAR:
			case HWD_SET_TEXTUREFILTER_TRILINEAR:
			case HWD_SET_TEXTUREFILTER_MIXED1:
				linear = 1;
				break;
			default:
				linear = 0;
				break;
		}
		if (r->nearest_only)
			linear = 0; // PC: textures with holes are never filtered
		if (r->screen)
			linear = 1;
		P.linear = linear;
		// a non power of two axis is clamped to the texture's own region; its repeat was turned into cuts
		P.clamp = (u64)((r->region & 1) ? 2 : wms) | ((u64)((r->region & 2) ? 2 : wmt) << 2) | ((u64)(r->uw - 1) << 14) | ((u64)(r->vh - 1) << 34);
		lru_touch(H.cur_tex);
		{
			texrec_t *im = &H.rec[img_of(H.cur_tex)];

			if (im->imgws != H.frame_no + 1) // OPT9: the images (not the records) of the working set
			{
				im->imgws = H.frame_no + 1;
				TX.need_blk += im->nblk;
				TX.need_n++;
				if (!(im->owner && im->owner->regen_kind && im->psm == PSM_T8 && (u32)im->sw * im->sh >= PLAN_MIN_TEXELS))
				{
					H.unplanned_cur += im->nblk; // PS2-HW-37: what the plan has to leave room for
					if ((ps2hwd_dbg_flags & HWDBG_LODDBG) && H.frame_no % 105 == 60 && im->owner)
						CONS_Printf("HWUP %s %ux%u psm=%d clut=%d blk=%u lvl=%d\n", HWR_PS2_TexName(im->owner), (unsigned)im->w, (unsigned)im->h, (int)im->psm, (int)im->clut, (unsigned)im->nblk, (int)im->dx);
				}
				else
				{
					const GLMipmap_t *pk = im->owner->ps2_twin ? im->owner->ps2_twin : im->owner;

					if (pk->ps2_planfr == H.frame_no + 1)
					{
						TX.need_in_plan += im->nblk;
					}
					else
					{
						TX.need_out_plan += im->nblk; // a big map texture drawn without the plan (sky dome, translucent walls, ...): stored at full size
						if ((ps2hwd_dbg_flags & HWDBG_LODDBG) && H.frame_no % 105 == 60)
							CONS_Printf("HWNP %s %ux%u lvl=%d blk=%u phase=%d\n", HWR_PS2_TexName(im->owner), (unsigned)im->sw, (unsigned)im->sh, (int)im->dx, (unsigned)im->nblk, batch_phase);
					}
				}
			}
		}
		if (r->ws != H.frame_no + 1) // working set statistics: every texture counted once per frame
		{
			int k;

			r->ws = H.frame_no + 1;
			H.st.ws_blocks += r->nblk;
			H.st.ws_tex++;
			if (H.pred_frame != H.frame_no + 1)
			{
				memset(H.pred, 0, sizeof H.pred);
				H.pred_frame = H.frame_no + 1;
			}
			for (k = 0; k < NCAPS; k++)
				H.pred[k] += r->fp[k];
		}
	}

	// ---- blend / pass plan ----
	{
		u32 cvr, cvg, cvb, cva = col_lut[av];
		u64 atest_trans = TEST_BITS(1, ATST_NOTEQUAL, 0), atest_mask = TEST_BITS(1, ATST_GREATER, 0x40);
		int tint_flat = 0;
		u32 ccr = (u32)cr, ccg = (u32)cg, ccb = (u32)cb;

		// tint of the shader equation on a flat colour
		if (lit && (P.rs.tint >> 24) != 0 && !textured && blend != PF_Fog)
		{
			float ta = (float)(P.rs.tint >> 24) * (1.0f / 255.0f), st = sqrtf(ta), fr = (float)cr * (1.0f / 255.0f), fg = (float)cg * (1.0f / 255.0f), fbl = (float)cb * (1.0f / 255.0f);
			float bright = sqrtf(fr * fr + fg * fg + fbl * fbl), o[3];
			int k;

			o[0] = bright * ((float)(P.rs.tint & 255) * (1.0f / 255.0f) * st) + fr * (1.0f - st);
			o[1] = bright * ((float)((P.rs.tint >> 8) & 255) * (1.0f / 255.0f) * st) + fg * (1.0f - st);
			o[2] = bright * ((float)((P.rs.tint >> 16) & 255) * (1.0f / 255.0f) * st) + fbl * (1.0f - st);
			for (k = 0; k < 3; k++)
				o[k] = o[k] < 0.0f ? 0.0f : o[k] > 1.0f ? 1.0f : o[k];
			ccr = (u32)(o[0] * 255.0f + 0.5f);
			ccg = (u32)(o[1] * 255.0f + 0.5f);
			ccb = (u32)(o[2] * 255.0f + 0.5f);
			tint_flat = 1;
		}
		(void)tint_flat;
		cvr = textured ? col_lut[ccr] : ccr;
		cvg = textured ? col_lut[ccg] : ccg;
		cvb = textured ? col_lut[ccb] : ccb;

		ps = &P.pass[0];
		pass_flat(ps);
		ps->tex = (u8)textured;
		ps->zwrite = (u8)occlude;
		ps->col = cvr | (cvg << 8) | (cvb << 16) | (cva << 24);
		ps->test = atest_trans;
		ps->alpha = ALPHA_TRANS;
		ps->abe = 1;
		P.npass = 1;
		if (textured && (r->clut == 1 || r->clut == 2) && lit && (P.rs.tint >> 24) != 0)
		{
			ps->clut_kind = CK_TINT; // the tint equation per palette entry
			ps->clut_param = P.rs.tint;
			ps->clut_keyed = r->clut == 2;
		}
		else if (textured && lit && (P.rs.tint >> 24) != 0 && blend != PF_Fog)
		{
			hw_limit(HW_SHADER, "colormap tint on a direct colour texture is not applied");
		}

		if (blend == (PF_Translucent & PF_Blending))
		{
			/* defaults */
		}
		else if (blend == (PF_Additive & PF_Blending))
		{
			ps->alpha = ALPHA_ADD;
		}
		else if (blend == (PF_ReverseSubtract & PF_Blending))
		{
			ps->alpha = ALPHA_RSUB;
		}
		else if (blend == (PF_Subtractive & PF_Blending))
		{
			// Cs * As - Cd: the source colour is scaled by As beforehand, then (Cs - Cd) clamps at 0
			u32 s = (u32)av;

			cvr = textured ? col_lut[(ccr * s + 127) / 255] : (ccr * s + 127) / 255;
			cvg = textured ? col_lut[(ccg * s + 127) / 255] : (ccg * s + 127) / 255;
			cvb = textured ? col_lut[(ccb * s + 127) / 255] : (ccb * s + 127) / 255;
			ps->col = cvr | (cvg << 8) | (cvb << 16) | (0x80u << 24);
			ps->alpha = ALPHA_SRCMINUS;
			if (partial_tex)
				hw_limit(HW_SUBTRACT, "subtractive blend of a partial alpha texture ignores the texel alpha factor");
		}
		else if (blend == (PF_Environment & PF_Blending))
		{
			// Cs + Cd * (1 - As): the destination is scaled first, then the source is added
			pass_t *p2 = &P.pass[1];

			*p2 = *ps;
			ps->alpha = ALPHA_KEEPDST;
			ps->zwrite = 0;
			ps->clut_kind = 0;
			p2->alpha = ALPHA_SRCPLUS;
			p2->col = (cvr | (cvg << 8) | (cvb << 16) | (0x80u << 24));
			P.npass = 2;
		}
		else if (blend == (PF_Multiplicative & PF_Blending))
		{
			// Cs * Cd: one pass per colour channel, the channel of the source colour is the destination's factor (alpha)
			int ch;

			if (textured && r->clut == 0)
				hw_limit(HW_MULTIPLY, "multiplicative blend of a direct colour texture is approximated");
			for (ch = 0; ch < 3; ch++)
			{
				pass_t *q = &P.pass[ch];
				u32 chan = ch == 0 ? ccr : ch == 1 ? ccg : ccb;

				*q = *ps;
				q->alpha = ALPHA_DSTSCALE;
				q->test = TEST_BITS(0, ATST_ALWAYS, 0);
				q->fbmsk = ~(0xFFu << (8 * ch));
				q->zwrite = (u8)(occlude && ch == 2);
				if (textured && r->clut != 0)
				{
					q->clut_kind = CK_MULT;
					q->clut_param = (u32)ch | (chan << 8);
					q->clut_keyed = r->clut == 2;
					q->col = 0x80808080u;
				}
				else if (textured)
				{
					q->alpha = ALPHA_TRANS;
					q->fbmsk = 0;
					q->col = cvr | (cvg << 8) | (cvb << 16) | (cva << 24);
					q->test = atest_trans;
					P.npass = 1;
					break;
				}
				else
				{
					q->col = 0x80u | (0x80u << 8) | (0x80u << 16) | ((u32)col_lut[chan] << 24);
				}
				P.npass = ch + 1;
			}
		}
		else if (blend == PF_Fog)
		{
			// Cs * As + Cd * Cs (SRC_ALPHA, SRC_COLOR): destination *= Cs per channel, then += Cs * As
			int ch;
			u32 fogc[3] = {ccr, ccg, ccb};

			if (!lit)
			{
				for (ch = 0; ch < 3; ch++)
				{
					pass_t *q = &P.pass[ch];

					pass_flat(q);
					q->tex = 0;
					q->alpha = ALPHA_DSTSCALE;
					q->test = TEST_BITS(0, ATST_ALWAYS, 0);
					q->fbmsk = ~(0xFFu << (8 * ch));
					q->col = 0x80u | (0x80u << 8) | (0x80u << 16) | ((u32)col_lut[fogc[ch]] << 24);
					q->abe = 1;
				}
				ps = &P.pass[3];
				pass_flat(ps);
				ps->alpha = ALPHA_ADD;
				ps->test = TEST_BITS(0, ATST_ALWAYS, 0);
				ps->abe = 1;
				ps->col = ccr | (ccg << 8) | (ccb << 16) | (cva << 24);
				ps->zwrite = (u8)occlude;
				P.npass = 4;
			}
			else
			{
				// per-pixel: ramp CLUTs (colour and alpha of the shader's output at the pixel's depth)
				rampspec_t rs = P.rs;

				for (ch = 0; ch < 4; ch++)
				{
					pass_t *q = &P.pass[ch];

					pass_flat(q);
					q->tex = 1;
					q->ramp = 1;
					q->decal = 1;
					q->abe = 1;
					q->test = TEST_BITS(0, ATST_ALWAYS, 0);
					q->col = 0x80u | (0x80u << 8) | (0x80u << 16) | (0x80u << 24);
					if (ch < 3)
					{
						q->alpha = ALPHA_DSTSCALE;
						q->fbmsk = ~(0xFFu << (8 * ch));
						q->clut_param = (u32)(RV_FOG_R + ch);
					}
					else
					{
						q->alpha = ALPHA_ADD;
						q->clut_param = RV_FOG_ADD;
						q->zwrite = (u8)occlude;
					}
				}
				(void)rs;
				P.npass = 4;
				P.lmode = LM_RAMP;
			}
		}
		else
		{
			// opaque / masked: out = Cs * As, alpha test As > 0.5
			ps->test = atest_mask;
			if (av == 128)
				ps->test = TEST_BITS(1, ATST_GREATER, 0x3F); // PC 128/255 is above 0.5, although GS rounds it to 64/128
			ps->alpha = ALPHA_SRCSCALE;
			ps->abe = (u8)(av != 255 || partial_tex);
			if (!partial_tex && !lit && av != 255)
			{
				u32 sr = (ccr * (u32)av + 127u) / 255u, sg = (ccg * (u32)av + 127u) / 255u, sb = (ccb * (u32)av + 127u) / 255u;
				ps->col = (textured ? col_lut[sr] : sr) | ((textured ? (u32)col_lut[sg] : sg) << 8)
					| ((textured ? (u32)col_lut[sb] : sb) << 16) | (cva << 24);
				ps->abe = 0; // source alpha multiplication is folded into modulation, eliminating a second GS rounding
			}
			if (!ps->abe)
				ps->alpha = ALPHA_TRANS;
		}
		if (dbg & 2)
			for (i = 0; i < P.npass; i++)
				P.pass[i].test &= ~1ull;
		if (flags & PF_NoAlphaTest)
			for (i = 0; i < P.npass; i++)
				P.pass[i].test &= ~1ull;
		if (dbg & 4)
			for (i = 0; i < P.npass; i++)
				P.pass[i].abe = 0;
		if (invisible)
			for (i = 0; i < P.npass; i++)
				P.pass[i].fbmsk = 0xFFFFFFFFu;
	}

	// ---- lighting mode ----
	if (lit && blend != PF_Fog && blend != (PF_Multiplicative & PF_Blending))
	{
		int solid = occlude && !(flags & PF_NoDepthTest) && !invisible && P.npass == 1 && P.pass[0].abe == 0
			&& (!textured || (r->psm == PSM_T8 && r->clut == 1)) && !P.water && !(flags & PF_Ripple);

		(void)solid;
		// Use actual staircase boundaries: a sampled depth ramp moves transitions by up to one ramp texel.
		P.lmode = LM_BANDS;
		P.fogcol = P.rs.fade;
	}
	if (P.lmode == LM_BANDS)
	{
		for (i = 0; i < P.npass; i++)
			if (P.pass[i].tex || P.pass[i].col)
				P.pass[i].fog = 1;
		// the Subtractive source colour was scaled by As: so is the fade colour
		if (blend == (PF_Subtractive & PF_Blending))
		{
			u32 s = (u32)av;

			P.fogcol = (((P.rs.fade & 255) * s + 127) / 255) | (((((P.rs.fade >> 8) & 255) * s + 127) / 255) << 8) | (((((P.rs.fade >> 16) & 255) * s + 127) / 255) << 16);
		}
		if (blend == (PF_Environment & PF_Blending))
			P.pass[0].fog = 0; // the destination scale pass has no colour
	}
	if (dbg & 8)
		for (i = 0; i < P.npass; i++)
			P.pass[i].zwrite = 0;

	P.fast = P.npass == 1 && P.lmode == LM_NONE && !P.cutx && !P.cuty && !P.water && !P.pass[0].ramp && !(flags & PF_Ripple)
		&& P.pass[0].tex == (u8)textured && !(ps2hwd_dbg_flags & 64);
	P.litfast = P.npass == 1 && P.lmode == LM_BANDS && !P.cutx && !P.cuty && !P.water && !P.pass[0].ramp && !(flags & PF_Ripple)
		&& P.pass[0].tex == (u8)textured && !(ps2hwd_dbg_flags & 64);
	if (P.litfast)
		lit_fast_plan(); // PS2-HW-43
	P.vu = 0;
	P.vuok = V.on && V.ready && (P.fast || P.litfast) && P.pass[0].tex && textured && !P.vcol && !(flags & PF_Invisible);
	P.serial = H.serial - 1; // the first polygon sets the state up
	xf_update();
	P.zmax = PS2HWD_ZMAX;
	P.zo = P.zmax * 0.5f + 0.5f;
	P.zk = -P.zmax * 0.5f;
	P.kx = H.vpw * 0.5f * 16.0f;
	P.ky = -H.vph * 0.5f * 16.0f;
	P.ox = (H.vpx + H.vpw * 0.5f - 0.5f + 2048.0f) * 16.0f + 0.5f;
	P.oy = (H.vpy + H.vph * 0.5f - 0.5f + 2048.0f) * 16.0f + 0.5f;
#ifdef HW_VU0
	P.vu0 = !(ps2hwd_dbg_flags & 128);
	if (P.vu0)
		vu0_batch_load(H.mvp, (float)H.guard_x * (1.0f / 1024.0f), (float)H.guard_y * (1.0f / 1024.0f), P.kx, P.ky, P.zk, P.ox, P.oy, P.zo + P.zbias, P.zmax);
#else
	P.vu0 = 0;
#endif
	if (P.lmode == LM_RAMP)
	{
		float kf = FAR_CLIPPING_PLANE / (FAR_CLIPPING_PLANE - H.near_plane);
		int source = textured ? H.cur_tex : NOREC;
		u8 pinned = source != NOREC ? H.rec[source].pin : 0;

		hw_limit(HW_SHADER, "experimental lighting depth ramp quantizes staircase boundaries; not fragment shader parity");
		if (source != NOREC)
			H.rec[source].pin = 1;
		if (ramp_get() == NOREC)
		{
			if (source != NOREC)
				H.rec[source].pin = pinned;
			hw_failure("light ramp texture could not be uploaded");
			return 0;
		}
		if (source != NOREC)
		{
			H.rec[source].pin = pinned;
			P.r = &H.rec[source]; // ramp upload may realloc the record table
		}
		P.ramp_scale = 2;
		P.ra = kf / ramp_zmax[P.ramp_scale];
		P.rb = -H.near_plane * kf / ramp_zmax[P.ramp_scale];
		if (!P.pass[0].ramp)
		{
			// the overlay of a solid polygon: queued in the state of this lighting
			rampspec_t rs = P.rs;
			u32 clut;

			rs.variant = RV_FADE;
			rs.scale = (u8)P.ramp_scale;
			clut = ramp_clut(&rs);
			if (OV.n && OV.clut != clut)
				ov_flush();
			OV.clut = clut;
		}
		else if (OV.n)
		{
			ov_flush(); // a fog block blends over the lit picture
		}
	}
	else if (OV.n)
	{
		ov_flush(); // anything else sees the picture with the queued light applied
	}
	if ((dbg & 4096) && H.frame_no == 150 && P.textured)
		CONS_Printf("HWT view proj=%.3f/%.3f mv5=%.3f vp=%.1fx%.1f at %.1f,%.1f s=%.3f/%.3f scr=%dx%d v=%dx%d near=%.2f\n", H.proj[0], H.proj[5], H.mv[5], H.vpw, H.vph, H.vpx, H.vpy, H.sx, H.sy, H.scr_w, H.scr_h, H.vw, H.vh, H.near_plane);
	H.cur_flags = flags;
	return 1;
}

// ---- vertex transform and packing ----

static cv_t vbuf_a[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA], vbuf_b[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA];

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

// object -> clip space of one engine vertex; vc = optional colour (r, g, b, a in 0..255)
static inline void xform_vertex(const FOutVector *s, const float *vc, cv_t *d)
{
	const float *m = H.mvp;
	float x = s->x, y = s->y, z = s->z;

	d->x = m[0] * x + m[4] * y + m[8] * z + m[12];
	d->y = m[1] * x + m[5] * y + m[9] * z + m[13];
	d->z = m[2] * x + m[6] * y + m[10] * z + m[14];
	d->w = m[3] * x + m[7] * y + m[11] * z + m[15];
	d->a[0] = s->s;
	d->a[1] = s->t;
	if (vc)
	{
		d->a[2] = vc[0];
		d->a[3] = vc[1];
		d->a[4] = vc[2];
		d->a[5] = vc[3];
	}
}

// packing of one pass
typedef struct
{
	int fmt; // 0 ST/RGBAQ/XYZ2; 1 ST/RGBAQ/XYZF2; 2 XYZ2; 3 XYZF2; 4 vertex colours; 5 ST/XYZ2 (Q=1 only)
	int nreg;
	int ramp; // ST from the depth ramp
	float sb, tb; // texture coordinate base subtracted (whole repeats)
	u32 fog; // F of the vertices (fog formats)
	u32 col; // flat RGBA for formats 0/1 (PACKED RGBAQ also commits the Q latched by GIF ST)
} pack_t;

static inline u32 fog_of_class(const rampspec_t *rs, int cls, int *on)
{
	int d = doom_darkness128(&rs->lp, cls); // 0..128

	if (d <= 0)
	{
		*on = 0;
		return 255;
	}
	*on = 1;
	{
		int f = (128 - d) * 2; // F = 256 * (1 - d / 128), 0..255
		return (u32)(f > 255 ? 255 : f);
	}
}

// one vertex as GIF data; returns the next free word
static inline qw_t *put_vertex(qw_t *o, const cv_t *s, const pack_t *pk)
{
	float q = 1.0f / s->w;
	float zf = P.zo + s->z * q * P.zk + P.zbias;
	int xi = (int)(P.ox + s->x * q * P.kx);
	int yi = (int)(P.oy + s->y * q * P.ky);
	u32 zi;

	zf = clampf(zf, 0.0f, P.zmax);
	zi = (u32)zf;
	if (pk->fmt == 0 || pk->fmt == 1 || pk->fmt == 4 || pk->fmt == 5)
	{
		if (pk->ramp)
		{
			o->f[0] = P.ra + P.rb * q; // s = ra * w + rb, over w
			o->f[1] = 0.0f;
		}
		else
		{
			o->f[0] = (s->a[0] - pk->sb) * P.tsx * q;
			o->f[1] = (s->a[1] - pk->tb) * P.tsy * q;
		}
		o->f[2] = q;
		o->w[3] = 0;
		o++;
	}
	if (pk->fmt == 0 || pk->fmt == 1)
	{
		// PACKED ST only latches Q inside GIF. RGBAQ must follow it to commit Q to the GS before XYZ kicks the vertex.
		o->w[0] = pk->col & 255;
		o->w[1] = (pk->col >> 8) & 255;
		o->w[2] = (pk->col >> 16) & 255;
		o->w[3] = pk->col >> 24;
		o++;
	}
	else if (pk->fmt == 4)
	{
		o->w[0] = col_lut[(int)clampf(s->a[2], 0.0f, 255.0f)];
		o->w[1] = col_lut[(int)clampf(s->a[3], 0.0f, 255.0f)];
		o->w[2] = col_lut[(int)clampf(s->a[4], 0.0f, 255.0f)];
		o->w[3] = col_lut[(int)clampf(s->a[5], 0.0f, 255.0f)];
		o++;
	}
	if (pk->fmt == 0 || pk->fmt == 1 || pk->fmt == 4)
		H.gsr.valid &= ~(u32)RV_RGBAQ; // packed vertices changed Q (and sky colours) behind the A+D state cache
	o->w[0] = (u32)xi;
	o->w[1] = (u32)yi;
	if (pk->fmt == 1 || pk->fmt == 3)
	{
		o->w[2] = zi << 4;
		o->w[3] = pk->fog << 4;
	}
	else
	{
		o->w[2] = zi;
		o->w[3] = 0;
	}
	return o + 1;
}

static inline int pack_regs(int fmt)
{
	return (fmt == 0 || fmt == 1 || fmt == 4) ? 3 : fmt == 5 ? 2 : 1;
}

static inline u64 pack_regdesc(int fmt)
{
	switch (fmt)
	{
		case 0: return GIF_REGS_STQ_RGBA_XYZ;
		case 1: return GIF_REGS_STQ_RGBA_XYZF;
		case 2: return GIF_REGS_XYZ;
		case 3: return GIF_REGS_XYZF;
		case 5: return GIF_REGS_STQ_XYZ;
		default: return GIF_REGS_STQ_RGBA_XYZ;
	}
}

// REGLIST sends native 64-bit GS registers. Include PRIM explicitly: PRE is ignored in this format.
// A short fan fits one descriptor list; longer fans and vertex-coloured sky retain PACKED.
static const u64 fan_reglist_desc[6][16] = {
	{0x0000000000000000ull, 0x0000000000005120ull, 0x0000000005125120ull, 0x0000005125125120ull, 0x0005125125125120ull, 0x5125125125125120ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
	{0x0000000000000000ull, 0x0000000000004120ull, 0x0000000004124120ull, 0x0000004124124120ull, 0x0004124124124120ull, 0x4124124124124120ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
	{0x0000000000000000ull, 0x0000000000000050ull, 0x0000000000000550ull, 0x0000000000005550ull, 0x0000000000055550ull, 0x0000000000555550ull, 0x0000000005555550ull, 0x0000000055555550ull, 0x0000000555555550ull, 0x0000005555555550ull, 0x0000055555555550ull, 0x0000555555555550ull, 0x0005555555555550ull, 0x0055555555555550ull, 0x0555555555555550ull, 0x5555555555555550ull},
	{0x0000000000000000ull, 0x0000000000000040ull, 0x0000000000000440ull, 0x0000000000004440ull, 0x0000000000044440ull, 0x0000000000444440ull, 0x0000000004444440ull, 0x0000000044444440ull, 0x0000000444444440ull, 0x0000004444444440ull, 0x0000044444444440ull, 0x0000444444444440ull, 0x0004444444444440ull, 0x0044444444444440ull, 0x0444444444444440ull, 0x4444444444444440ull},
	{0x0000000000000000ull, 0x0000000000005120ull, 0x0000000005125120ull, 0x0000005125125120ull, 0x0005125125125120ull, 0x5125125125125120ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
	{0x0000000000000000ull, 0x0000000000000520ull, 0x0000000000052520ull, 0x0000000005252520ull, 0x0000000525252520ull, 0x0000052525252520ull, 0x0005252525252520ull, 0x0525252525252520ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull}
};

static void emit_reglist_fan(const cv_t *v, int cnt, const pack_t *pk, u32 prim)
{
	const int nr = 1 + cnt * pk->nreg;
	qw_t *p = pk_alloc((u32)(1 + (nr + 1) / 2));
	u64 *o = (u64 *)(p + 1);
	int i;

	p[0].d[0] = HWGIF_TAG(1, 1, 0, 0, 1, nr & 15);
	p[0].d[1] = fan_reglist_desc[pk->fmt][cnt];
	*o++ = prim;
	for (i = 0; i < cnt; i++)
	{
		const cv_t *s = &v[i];
		const float q = pk->fmt == 5 ? 1.0f : 1.0f / s->w;
		float zf = P.zo + s->z * q * P.zk + P.zbias;
		const int xi = (int)(P.ox + s->x * q * P.kx);
		const int yi = (int)(P.oy + s->y * q * P.ky);
		u32 zi;

		zf = clampf(zf, 0.0f, P.zmax);
		zi = (u32)zf;
		if (pk->fmt == 0 || pk->fmt == 1 || pk->fmt == 5)
		{
			qw_t st;
			st.f[0] = (s->a[0] - pk->sb) * P.tsx * q;
			st.f[1] = (s->a[1] - pk->tb) * P.tsy * q;
			*o++ = st.d[0];
			if (pk->fmt != 5)
			{
				st.f[0] = q;
				*o++ = (u64)pk->col | ((u64)st.w[0] << 32);
			}
		}
		if (pk->fmt == 1 || pk->fmt == 3)
			*o++ = (u64)(xi & 0xFFFF) | ((u64)(yi & 0xFFFF) << 16) | ((u64)zi << 32) | ((u64)pk->fog << 56);
		else
			*o++ = (u64)(xi & 0xFFFF) | ((u64)(yi & 0xFFFF) << 16) | ((u64)zi << 32);
	}
	if (nr & 1)
		*o = 0; // ignored half of the final DMA quadword
	if (pk->fmt == 0 || pk->fmt == 1)
		H.gsr.valid &= ~(u32)RV_RGBAQ;
}

// the GS state of pass `k` for the current plan
static void pass_setup(int k, int force_tex)
{
	const pass_t *ps = &P.pass[k];
	gsstate_t s;
	int use_tex = ps->tex || force_tex;

	memset(&s, 0, sizeof s);
	s.test = ps->test | (1ull << 16) | ((u64)((P.flags & PF_NoDepthTest) ? 1 : ((ps2hwd_dbg_flags & 1) ? 1 : 2)) << 17);
	s.alpha = ps->alpha;
	s.zbuf = zbuf_reg(ps->zwrite ? 0 : 1);
	s.frame = frame_reg(H.target, ps->fbmsk);
	s.scissor = scissor_current();
	s.fogcol = P.fogcol;
	s.use_fog = ps->fog;
	s.rgbaq = rgbaq_of(ps->col);
	s.use_tex = use_tex;
	if (use_tex && P.textured && !ps->ramp)
	{
		const texrec_t *r = P.r;
		u32 cbp = 0;

		if (r->psm == PSM_T8)
		{
			if (ps->clut_kind)
				cbp = clut_get(ps->clut_kind, ps->clut_param, ps->clut_keyed);
			else
				cbp = clut_get(r->clut == 3 ? CK_ALPHA : (r->clut == 2 ? CK_KEYED : CK_OPAQUE), 0, 0);
		}
		tex_regs(r, 0, 0, P.linear, cbp, ps->decal, &s.tex0, &s.tex1, &s.clamp);
		s.clamp = P.clamp;
	}
	state_flush(&s);
}

static inline u32 pass_prim(const pass_t *ps)
{
	return PRIM_IIP | (ps->tex ? PRIM_TME : 0) | (ps->fog ? PRIM_FGE : 0) | (ps->abe ? PRIM_ABE : 0);
}

// ---- cutting a convex polygon along an attribute ----

static inline float attr_of(const cv_t *v, int which)
{
	return which == 6 ? v->w : which == 7 ? v->a[0] : v->a[1];
}

// the part of `in` with attribute < cut (side 0) or >= cut (side 1); the point on an edge is interpolated from the end with
// the smaller value, so that both sides of a cut get bit-identical vertices
static int clip_attr(const cv_t *in, int n, cv_t *out, int which, float cut, int side, int na)
{
	int i, k, m = 0;

	for (i = 0; i < n; i++)
	{
		const cv_t *v0 = &in[i], *v1 = &in[i + 1 == n ? 0 : i + 1];
		float f0 = attr_of(v0, which), f1 = attr_of(v1, which);
		int in0 = side ? f0 >= cut : f0 < cut, in1 = side ? f1 >= cut : f1 < cut;

		if (in0)
			out[m++] = *v0;
		if (in0 != in1)
		{
			const cv_t *lo = f0 < f1 ? v0 : v1, *hi = f0 < f1 ? v1 : v0;
			float flo = f0 < f1 ? f0 : f1, fhi = f0 < f1 ? f1 : f0, t = (cut - flo) / (fhi - flo);
			cv_t *o = &out[m++];

			o->x = lo->x + (hi->x - lo->x) * t;
			o->y = lo->y + (hi->y - lo->y) * t;
			o->z = lo->z + (hi->z - lo->z) * t;
			o->w = lo->w + (hi->w - lo->w) * t;
			for (k = 0; k < na; k++)
				o->a[k] = lo->a[k] + (hi->a[k] - lo->a[k]) * t;
			if (which == 6)
				o->w = cut;
			else if (which == 7)
				o->a[0] = cut;
			else
				o->a[1] = cut;
		}
	}
	return m;
}

#define CUTBUF (PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA + 16)
static cv_t cutbuf[3][3][CUTBUF];
#define MAXCUTS 40
#define UV_EXTENT 4096.0f // texels of s or t one GS primitive may span (14.4 fixed point U/V; the rest is cut at whole repeats)

// ---- emitting pieces ----

typedef struct
{
	float sb, tb; // whole repeats removed from s, t
	int cls; // darkness class of a band piece, -1 = none
	int ripple; // water: displace the texture coordinates by the vertex depth
} piece_t;

static inline int ov_room(int cnt)
{
	return OV.n + 1 + (u32)cnt * 3u <= OVQ_QW;
}

// the caller made room with ov_room()/ov_flush()
static void emit_overlay(const cv_t *v, int cnt)
{
	pack_t pk;
	qw_t *o;
	int i, nreg = 3;
	u64 prim = PRIM_TRIFAN | PRIM_IIP | PRIM_TME | PRIM_ABE;

	memset(&pk, 0, sizeof pk);
	pk.fmt = 0;
	pk.ramp = 1;
	o = &ovq[OV.n];
	o[0].d[0] = HWGIF_TAG(cnt, 1, 1, prim, 0, nreg);
	o[0].d[1] = GIF_REGS_STQ_RGBA_XYZ;
	o++;
	for (i = 0; i < cnt; i++)
		o = put_vertex(o, &v[i], &pk);
	OV.n = (u32)(o - ovq);
	H.st.verts_out += (u32)cnt;
}

// all passes of one piece
static void emit_piece(const cv_t *v, int cnt, const piece_t *pc)
{
	int k, i;
	pack_t pk;
	qw_t *o;
	u32 fogF = 255;
	int fog_on = 0;

	if (cnt < 3)
		return;
	if (pc->cls >= 0)
		fogF = fog_of_class(&P.rs, pc->cls, &fog_on);
	for (k = 0; k < P.npass; k++)
	{
		const pass_t *ps = &P.pass[k];
		int usefog = ps->fog && fog_on;
		cv_t tmp[1];

		(void)tmp;
		if (ps->ramp)
			continue; // light overlays and fog ramps are handled below
		if (P.npass == 1)
		{
			// PS2-HW-65: the pieces of one polygon (light bands, repeats) and consecutive polygons of a plan share the pass 0 state: set it once
			if (P.serial != H.serial)
			{
				pass_setup(0, 0);
				P.serial = H.serial;
			}
		}
		else
		{
			pass_setup(k, 0);
		}
		// a pass with fog but a band without darkness: PRIM.FGE off
		memset(&pk, 0, sizeof pk);
		pk.sb = pc->sb;
		pk.tb = pc->tb;
		pk.fog = fogF;
		pk.fmt = P.vcol ? 4 : (ps->tex ? (usefog ? 1 : 0) : (usefog ? 3 : 2));
		pk.col = ps->col;
		if (pk.fmt == 0)
		{
			int unitq = 1;
			for (i = 0; i < cnt; i++) unitq &= v[i].w == 1.0f;
			if (unitq) pk.fmt = 5; // retain the compact affine HUD/screen path
		}
		pk.nreg = pack_regs(pk.fmt);
		if (pk.fmt != 4 && 1 + cnt * pk.nreg <= 16)
		{
			emit_reglist_fan(v, cnt, &pk, PRIM_TRIFAN | (pass_prim(ps) & ~(usefog ? 0u : (u32)PRIM_FGE)));
			H.st.verts_out += (u32)cnt;
			if (k)
				H.st.passes++;
			continue;
		}
		o = pk_alloc((u32)(1 + cnt * pk.nreg));
		o[0].d[0] = HWGIF_TAG(cnt, 1, 1, PRIM_TRIFAN | (pass_prim(ps) & ~(usefog ? 0u : (u32)PRIM_FGE)), 0, pk.nreg);
		o[0].d[1] = pack_regdesc(pk.fmt);
		o++;
		for (i = 0; i < cnt; i++)
			o = put_vertex(o, &v[i], &pk);
		H.st.verts_out += (u32)cnt;
		if (k)
			H.st.passes++;
	}
	if (P.lmode == LM_RAMP && P.npass >= 1 && !P.pass[0].ramp)
	{
		// the light overlay of a solid polygon
		if (!ov_room(cnt))
			ov_flush();
		emit_overlay(v, cnt);
	}
	else if (P.lmode == LM_RAMP)
	{
		// fog block: passes with ramp
		for (k = 0; k < P.npass; k++)
		{
			const pass_t *ps = &P.pass[k];
			gsstate_t s;
			rampspec_t rs = P.rs;
			const texrec_t *rt;
			pack_t pr;

			if (!ps->ramp)
				continue;
			rs.variant = (u8)ps->clut_param;
			rs.scale = (u8)P.ramp_scale;
			memset(&s, 0, sizeof s);
			rt = &H.rec[ramp_tex];
			tex_regs(rt, 1, 1, 0, ramp_clut(&rs), 1, &s.tex0, &s.tex1, &s.clamp);
			s.clamp = 1ull | (1ull << 2);
			s.test = ps->test | (1ull << 16) | ((u64)((P.flags & PF_NoDepthTest) ? 1 : 2) << 17);
			s.alpha = ps->alpha;
			s.zbuf = zbuf_reg(ps->zwrite ? 0 : 1);
			s.frame = frame_reg(H.target, ps->fbmsk);
			s.scissor = scissor_current();
			s.rgbaq = rgbaq_of(ps->col);
			s.use_tex = 1;
			state_flush(&s);
			memset(&pr, 0, sizeof pr);
			pr.fmt = 0;
			pr.ramp = 1;
			pr.col = ps->col;
			o = pk_alloc((u32)(1 + cnt * 3));
			o[0].d[0] = HWGIF_TAG(cnt, 1, 1, PRIM_TRIFAN | PRIM_IIP | PRIM_TME | PRIM_ABE, 0, 3);
			o[0].d[1] = GIF_REGS_STQ_RGBA_XYZ;
			o++;
			for (i = 0; i < cnt; i++)
				o = put_vertex(o, &v[i], &pr);
			H.st.verts_out += (u32)cnt;
			H.st.passes++;
		}
	}
}

// Cut a convex polygon along one attribute at ascending values cuts[0..n); the piece before cuts[0] gets index 0, ...
static void cut_and_emit(const cv_t *v, int cnt, int stage, piece_t pc, int na);

typedef struct
{
	int which;
	float cuts[MAXCUTS];
	int cls[MAXCUTS + 1];
	int n;
	float step; // repeats between two cuts of s / t (1 = every repeat)
} cutlist_t;

static void emit_cuts(const cv_t *v, int cnt, int stage, const piece_t *pc, int na, const cutlist_t *cl, int next_stage)
{
	const cv_t *rem = v;
	int nrem = cnt, k, which = rem == cutbuf[stage][1] ? 1 : 0;
	cv_t *piece = cutbuf[stage][0];
	cutlist_t tail;

	for (;;)
	{
		for (k = 0; k < cl->n; k++)
		{
			int np = clip_attr(rem, nrem, piece, cl->which, cl->cuts[k], 0, na);
			cv_t *rb = cutbuf[stage][1 + which];

			if (np >= 3)
			{
				piece_t p2 = *pc;

				if (cl->which == 7)
					p2.sb = floorf(cl->cuts[k]) - cl->step; // each piece starts at the previous cut
				else if (cl->which == 8)
					p2.tb = floorf(cl->cuts[k]) - cl->step;
				else
					p2.cls = cl->cls[k];
				cut_and_emit(piece, np, next_stage, p2, na);
			}
			nrem = clip_attr(rem, nrem, rb, cl->which, cl->cuts[k], 1, na);
			rem = rb;
			which ^= 1;
			if (nrem < 3)
				return;
		}
		// MAXCUTS bounds staging, not valid repeats. Continue iteratively in bounded storage.
		if ((cl->which == 7 || cl->which == 8) && cl->n == MAXCUTS)
		{
			float hi = attr_of(&rem[0], cl->which), cut = cl->cuts[cl->n - 1] + cl->step;
			int i, n = 0, attr = cl->which;
			for (i = 1; i < nrem; i++)
				if (attr_of(&rem[i], attr) > hi)
					hi = attr_of(&rem[i], attr);
			while (cut < hi && n < MAXCUTS)
			{
				tail.cuts[n++] = cut;
				cut += cl->step;
			}
			if (n)
			{
				tail.which = attr;
				tail.n = n;
				tail.step = cl->step;
				cl = &tail;
				H.st.passes += (u32)n;
				which = rem == cutbuf[stage][1] ? 1 : 0;
				continue;
			}
		}
		{
			piece_t p2 = *pc;

			if (cl->which == 7)
				p2.sb = floorf(cl->cuts[cl->n - 1]);
			else if (cl->which == 8)
				p2.tb = floorf(cl->cuts[cl->n - 1]);
			else
				p2.cls = cl->cls[cl->n];
			cut_and_emit(rem, nrem, next_stage, p2, na);
		}
		return;
	}
}

static void cut_and_emit(const cv_t *v, int cnt, int stage, piece_t pc, int na)
{
	static cutlist_t cl[3];
	cutlist_t *c = &cl[stage < 3 ? stage : 2];
	int i;
	float lo, hi;

	if (cnt < 3)
		return;
	switch (stage)
	{
		case 0: // repeats of a texture whose width is not a power of two; PS2-HW-64: and of a power of two texture whose polygon covers more than UV_EXTENT texels
			if (P.cutx || (P.wrapx && P.r))
			{
				float stp = 1.0f;

				lo = hi = v[0].a[0];
				for (i = 1; i < cnt; i++)
				{
					if (v[i].a[0] < lo)
						lo = v[i].a[0];
					if (v[i].a[0] > hi)
						hi = v[i].a[0];
				}
				if (!P.cutx)
				{
					if ((hi - lo) * (float)P.r->w > UV_EXTENT)
					{
						stp = floorf(UV_EXTENT / (float)P.r->w);
						if (stp < 1.0f)
							stp = 1.0f;
					}
					else
					{
						cut_and_emit(v, cnt, 1, pc, na);
						return;
					}
				}
				pc.sb = floorf(lo);
				if (hi - lo > 0.0f && floorf(hi) > floorf(lo))
				{
					int n = 0;
					float kk;

					for (kk = floorf(lo) + stp; kk < hi && n < MAXCUTS; kk += stp)
						c->cuts[n++] = kk;
					if (n)
					{
						c->which = 7;
						c->n = n;
						c->step = stp;
						H.st.passes += (u32)n;
						emit_cuts(v, cnt, stage, &pc, na, c, 1);
						return;
					}
				}
			}
			cut_and_emit(v, cnt, 1, pc, na);
			return;
		case 1:
			if (P.cuty || (P.wrapy && P.r))
			{
				float stp = 1.0f;

				lo = hi = v[0].a[1];
				for (i = 1; i < cnt; i++)
				{
					if (v[i].a[1] < lo)
						lo = v[i].a[1];
					if (v[i].a[1] > hi)
						hi = v[i].a[1];
				}
				if (!P.cuty)
				{
					if ((hi - lo) * (float)P.r->h > UV_EXTENT)
					{
						stp = floorf(UV_EXTENT / (float)P.r->h);
						if (stp < 1.0f)
							stp = 1.0f;
					}
					else
					{
						cut_and_emit(v, cnt, 2, pc, na);
						return;
					}
				}
				pc.tb = floorf(lo);
				if (floorf(hi) > floorf(lo))
				{
					int n = 0;
					float kk;

					for (kk = floorf(lo) + stp; kk < hi && n < MAXCUTS; kk += stp)
						c->cuts[n++] = kk;
					if (n)
					{
						c->which = 8;
						c->n = n;
						c->step = stp;
						H.st.passes += (u32)n;
						emit_cuts(v, cnt, stage, &pc, na, c, 2);
						return;
					}
				}
			}
			cut_and_emit(v, cnt, 2, pc, na);
			return;
		default:
			break;
	}
	// stage 2: depth bands (light staircase)
	if (stage == 2 && P.lmode == LM_BANDS)
	{
		float wlo = v[0].w, whi = v[0].w, z0, z1, th[MAXCUTS];
		int cls0, n, cls[MAXCUTS];

		for (i = 1; i < cnt; i++)
		{
			if (v[i].w < wlo)
				wlo = v[i].w;
			if (v[i].w > whi)
				whi = v[i].w;
		}
		z0 = zfrag_of_w(wlo);
		z1 = zfrag_of_w(whi);
		n = doom_thresholds(&P.rs.lp, z0, z1, th, cls, MAXCUTS, &cls0);
		if (n)
		{
			for (i = 0; i < n; i++)
			{
				c->cuts[i] = w_of_zfrag(th[i]);
				c->cls[i + 1] = cls[i];
			}
			c->cls[0] = cls0;
			c->which = 6;
			c->n = n;
			H.st.bands += (u32)n + 1;
			emit_cuts(v, cnt, 2, &pc, na, c, 3);
			return;
		}
		pc.cls = cls0;
	}
	emit_piece(v, cnt, &pc);
}

// The common polygon (one pass, no light bands, no texture repeat cuts): what emit_piece does for it without the dispatch.
// The GS state of pass 0 is written when the plan changed (P.serial != H.serial), not for every polygon.
#ifdef HW_VU0
static pv_t pvbuf[PS2HWD_MAXPOLY];

// emit_reglist_fan with the GS integers and 1/w that vu0_xform made
static void emit_reglist_fan_pv(const cv_t *v, const pv_t *pv, int cnt, const pack_t *pk, u32 prim)
{
	const int nr = 1 + cnt * pk->nreg;
	qw_t *p = pk_alloc((u32)(1 + (nr + 1) / 2));
	u64 *o = (u64 *)(p + 1);
	int i;

	p[0].d[0] = HWGIF_TAG(1, 1, 0, 0, 1, nr & 15);
	p[0].d[1] = fan_reglist_desc[pk->fmt][cnt];
	*o++ = prim;
	for (i = 0; i < cnt; i++)
	{
		const float q = pk->fmt == 5 ? 1.0f : pv[i].q;
		const u64 xyz = (u64)((u32)pv[i].xi & 0xFFFF) | ((u64)((u32)pv[i].yi & 0xFFFF) << 16) | ((u64)(u32)pv[i].zi << 32);

		if (pk->fmt == 0 || pk->fmt == 1 || pk->fmt == 5)
		{
			qw_t st;
			st.f[0] = (v[i].a[0] - pk->sb) * P.tsx * q;
			st.f[1] = (v[i].a[1] - pk->tb) * P.tsy * q;
			*o++ = st.d[0];
			if (pk->fmt != 5)
			{
				st.f[0] = q;
				*o++ = (u64)pk->col | ((u64)st.w[0] << 32);
			}
		}
		*o++ = (pk->fmt == 1 || pk->fmt == 3) ? (xyz | ((u64)pk->fog << 56)) : xyz;
	}
	if (nr & 1)
		*o = 0;
	if (pk->fmt == 0 || pk->fmt == 1)
		H.gsr.valid &= ~(u32)RV_RGBAQ;
}
#endif

static void emit_fast_fan(const cv_t *v, const void *pvv, int cnt, float sb, float tb)
{
#ifdef HW_VU0
	const pv_t *pv = (const pv_t *)pvv;
#else
	(void)pvv;
#endif
	const pass_t *ps = &P.pass[0];
	pack_t pk;
	int i;

	if (cnt < 3)
		return;
	if (P.serial != H.serial)
	{
		pass_setup(0, 0);
		P.serial = H.serial;
	}
	pk.sb = sb;
	pk.tb = tb;
	pk.fog = 255;
	pk.ramp = 0;
	pk.col = ps->col;
	pk.fmt = ps->tex ? 0 : 2;
	if (pk.fmt == 0)
	{
		int unitq = 1;

		for (i = 0; i < cnt; i++)
			unitq &= v[i].w == 1.0f;
		if (unitq)
			pk.fmt = 5; // the compact affine HUD/screen path
	}
	pk.nreg = pack_regs(pk.fmt);
	if (1 + cnt * pk.nreg <= 16)
	{
#ifdef HW_VU0
		if (pv)
			emit_reglist_fan_pv(v, pv, cnt, &pk, PRIM_TRIFAN | (pass_prim(ps) & ~(u32)PRIM_FGE));
		else
#endif
		emit_reglist_fan(v, cnt, &pk, PRIM_TRIFAN | (pass_prim(ps) & ~(u32)PRIM_FGE));
	}
	else
	{
		qw_t *o = pk_alloc((u32)(1 + cnt * pk.nreg));

		o[0].d[0] = HWGIF_TAG(cnt, 1, 1, PRIM_TRIFAN | (pass_prim(ps) & ~(u32)PRIM_FGE), 0, pk.nreg);
		o[0].d[1] = pack_regdesc(pk.fmt);
		o++;
		for (i = 0; i < cnt; i++)
			o = put_vertex(o, &v[i], &pk);
	}
	H.st.verts_out += (u32)cnt;
}

// ---- water ripple (PS2-HW-63) ----
// GLSL_WATER_FRAGMENT_SHADER: texel = tex(s - sin(a) * amp, t - cos(a) * amp), a = -pi * (z / 2 * freq) + leveltime * speed, z = eye depth
// of the fragment (zfrag), freq = amp = 0.025, speed = 2. The GS has no fragment program: the polygon is cut into bands of RIPPLE_STEP of
// eye depth, every band takes the offset of its middle (a constant shift of the texture coordinates). Beyond RIPPLE_ZMAX the offset
// (1.6 texels at most) is below one pixel: no ripple there. Error: the offset steps by up to 2 pi * 0.025 * 16 / 160 texel-widths
// per band (about a quarter texel) against the smooth shader.
#define RIPPLE_STEP 16.0f
#define RIPPLE_ZMAX 640.0f
#define RIPPLE_AMP 0.025f

static float ripple_sin[256 + 1];
static int ripple_ready;

static inline float ripple_sinf(float a) // sin() of any angle from a 256 entry table, linear interpolation
{
	float x = a * (256.0f / 6.28318530718f), fl;
	int i;

	fl = floorf(x);
	i = (int)fl & 255;
	x -= fl;
	return ripple_sin[i] + (ripple_sin[i + 1] - ripple_sin[i]) * x;
}

static void ripple_offset(float zc, float *ds, float *dt)
{
	float a = -3.14159265f * (zc * 0.5f * 0.025f) + (float)H.leveltime * 2.0f;

	*ds = ripple_sinf(a) * RIPPLE_AMP;
	*dt = ripple_sinf(a + 1.57079633f) * RIPPLE_AMP; // cos
}

static cv_t wpiece[CUTBUF], wrem[2][CUTBUF];

static void emit_water_poly(const cv_t *v, int cnt, piece_t pc, int na)
{
	float wlo = v[0].w, whi = v[0].w, z0, z1, ds, dt, step = RIPPLE_STEP;
	const cv_t *rem = v;
	int nrem = cnt, i, k, which = 0;
	piece_t p2;

	if (!ripple_ready)
	{
		for (i = 0; i <= 256; i++)
			ripple_sin[i] = sinf((float)i * (6.28318530718f / 256.0f));
		ripple_ready = 1;
	}
	for (i = 1; i < cnt; i++)
	{
		if (v[i].w < wlo)
			wlo = v[i].w;
		if (v[i].w > whi)
			whi = v[i].w;
	}
	z0 = zfrag_of_w(wlo);
	z1 = zfrag_of_w(whi);
	if (z0 >= RIPPLE_ZMAX)
	{
		cut_and_emit(v, cnt, 0, pc, na);
		return;
	}
	k = z0 > 0.0f ? (int)(z0 / step) : 0;
	for (;; k++)
	{
		float cutz = (float)(k + 1) * step;

		if (cutz >= z1 || cutz >= RIPPLE_ZMAX)
		{
			// the last piece: the rest of the polygon
			p2 = pc;
			if (cutz >= RIPPLE_ZMAX && (float)k * step >= RIPPLE_ZMAX)
			{
				ds = dt = 0.0f;
			}
			else
			{
				float lo = (float)k * step, hi = z1 < RIPPLE_ZMAX ? z1 : RIPPLE_ZMAX;

				if (lo < z0)
					lo = z0;
				ripple_offset(0.5f * (lo + hi), &ds, &dt);
			}
			p2.sb += ds;
			p2.tb += dt;
			cut_and_emit(rem, nrem, 0, p2, na);
			return;
		}
		{
			float cutw = w_of_zfrag(cutz);
			int np = clip_attr(rem, nrem, wpiece, 6, cutw, 0, na);
			cv_t *rb = wrem[which];
			int nn = clip_attr(rem, nrem, rb, 6, cutw, 1, na);

			if (np >= 3)
			{
				float lo = (float)k * step;

				if (lo < z0)
					lo = z0;
				ripple_offset(0.5f * (lo + cutz), &ds, &dt);
				p2 = pc;
				p2.sb += ds;
				p2.tb += dt;
				cut_and_emit(wpiece, np, 0, p2, na);
				H.st.bands++;
			}
			if (nn < 3)
				return;
			rem = rb;
			nrem = nn;
			which ^= 1;
		}
	}
}

#include "ps2_hw_fast.inc" // PS2-HW-43: lit_fast_poly / lit_fast_plan
#include "ps2_hw_vu1.inc" // PS2-HW-45: vu_poly / vu_sync (the VU1 program draws the polygons of a batch)

// Transform, clip and pack one convex polygon.
// Vertices: base[idx[i]] (or base[i] when idx is NULL); with `vcol` (4 floats per vertex index) they carry colours.
static void emit_poly(const FOutVector *base, const u32 *idx, int n, const float *vcol)
{
	const float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);
	int i, cnt, and_ = 0x3F, or_ = 0, na = P.vcol ? 6 : 2, big = 0;
	cv_t *v;
	u32 t0 = cyc();
	piece_t pc;
#ifdef HW_VU0
	const int vu0 = P.vu0 && !vcol && !P.vcol;
#endif

	if (n < 3)
		return;
	if (P.vu)
	{
		if (!idx && !vcol && vu_poly(base, n))
			return; // PS2-HW-45: the VU1 program transforms, clips and packs it
		vu_sync();
	}
	for (i = 0; i < n; i++)
	{
		u32 vi = idx ? idx[i] : (u32)i;
		int oc;

#ifdef HW_VU0
		if (vu0)
		{
			oc = vu0_xform(&base[vi], &vbuf_a[i], &pvbuf[i]);
			and_ &= oc;
			or_ |= oc;
			continue;
		}
#endif
		xform_vertex(&base[vi], vcol ? &vcol[vi * 4] : NULL, &vbuf_a[i]);
		oc = outcode(&vbuf_a[i], gx, gy);
		and_ &= oc;
		or_ |= oc;
	}
	H.st.verts_in += (u32)n;
	H.st.polys++;
	if (and_)
	{
		H.st.rejected++;
		H.st.cyc_draw += cyc() - t0;
		return;
	}
	v = vbuf_a;
	cnt = n;
	if ((ps2hwd_dbg_flags & HWDBG_TEX) && P.textured && H.cur_tex != NOREC && H.rec[H.cur_tex].sw && !vcol && !P.vcol)
		poly_plane(vbuf_a, n, (float)H.rec[H.cur_tex].sw, (float)H.rec[H.cur_tex].sh); // before clipping reuses the buffers
	if (or_)
	{
		cnt = clip_poly(vbuf_a, vbuf_b, n, na, or_, &v);
		H.st.clipped++;
		if (cnt < 3)
		{
			H.st.rejected++;
			H.st.cyc_draw += cyc() - t0;
			return;
		}
	}
	if (P.textured && (P.wrapx || P.wrapy) && !P.cutx && !P.cuty)
	{
		// whole repeats of a power of two texture can be dropped from the coordinates (keeps S/Q and T/Q small)
		float smin = 1e30f, tmin = 1e30f, smax = -1e30f, tmax = -1e30f;

		for (i = 0; i < cnt; i++)
		{
			if (v[i].a[0] < smin)
				smin = v[i].a[0];
			if (v[i].a[1] < tmin)
				tmin = v[i].a[1];
			if (v[i].a[0] > smax)
				smax = v[i].a[0];
			if (v[i].a[1] > tmax)
				tmax = v[i].a[1];
		}
		pc.sb = (P.wrapx && fabsf(smin) > 4.0f) ? floorf(smin) : 0.0f;
		pc.tb = (P.wrapy && fabsf(tmin) > 4.0f) ? floorf(tmin) : 0.0f;
		big = (P.wrapx && (smax - smin) * (float)P.r->w > UV_EXTENT) || (P.wrapy && (tmax - tmin) * (float)P.r->h > UV_EXTENT); // PS2-HW-64: too many texels for one primitive: cut at whole repeats (cut_and_emit)
	}
	else
	{
		pc.sb = pc.tb = 0.0f;
	}
	pc.cls = -1;
	pc.ripple = 0;
	if ((ps2hwd_dbg_flags & HWDBG_TEX) && P.textured && H.cur_tex != NOREC && H.rec[H.cur_tex].sw && !vcol && !P.vcol)
		lod_note(v, cnt); // OPT9 instrumentation: the mip level this polygon needs
	if (P.fast && !P.vcol && !vcol && !big)
#ifdef HW_VU0
		emit_fast_fan(v, (vu0 && !or_) ? (const void *)pvbuf : NULL, cnt, pc.sb, pc.tb);
#else
		emit_fast_fan(v, NULL, cnt, pc.sb, pc.tb);
#endif
	else if (P.litfast && !big && !vcol && !P.vcol && !(ps2hwd_dbg_flags & 8192) && lit_fast_poly(v, LF_PV(vu0, or_), cnt, pc.sb, pc.tb))
		; // PS2-HW-43: drawn
	else if (P.water && P.textured && !P.vcol && !vcol && !big)
		emit_water_poly(v, cnt, pc, na);
	else
		cut_and_emit(v, cnt, 0, pc, na);
	H.st.cyc_draw += cyc() - t0;
}

// fans longer than the scratch buffers are cut into pieces that share the first vertex
static void emit_fan(const FOutVector *base, const u32 *idx, int n, const float *vcol)
{
	if (n <= PS2HWD_MAXPOLY)
	{
		emit_poly(base, idx, n, vcol);
	}
	else
	{
		u32 tmp[PS2HWD_MAXPOLY];
		int start = 1, i;

		while (start < n - 1)
		{
			int m = n - start < PS2HWD_MAXPOLY - 1 ? n - start : PS2HWD_MAXPOLY - 1;

			tmp[0] = idx ? idx[0] : 0;
			for (i = 0; i < m; i++)
				tmp[1 + i] = idx ? idx[start + i] : (u32)(start + i);
			emit_poly(base, tmp, m + 1, vcol);
			start += m - 1;
		}
	}
}

// ---- triangle lists (DrawIndexedTriangles, sky dome, models) ----
// A triangle that needs no cutting goes straight into the open GIF run of pass 0; everything else takes the polygon path.

static struct
{
	qw_t *tag;
	u32 n; // vertices written
	int open;
	int nreg;
	int fmt;
} LS;

static void ls_close(void)
{
	if (!LS.open)
		return;
	LS.tag->d[0] = HWGIF_TAG(LS.n, 1, 1, PRIM_TRI | pass_prim(&P.pass[0]), 0, LS.nreg);
	LS.tag->d[1] = pack_regdesc(LS.fmt);
	LS.open = 0;
	H.st.verts_out += LS.n;
}

// room for `nqw` more words of vertex data in the open list (the buffer is flushed and the list reopened when needed)
static qw_t *ls_reserve(u32 nqw)
{
	if (!LS.open || H.wr + nqw > BUF_QW)
	{
		if (LS.open)
		{
			ls_close();
			pk_flush();
		}
		LS.tag = pk_alloc(1 + nqw); // the tag, then the vertices of this reservation
		LS.n = 0;
		LS.open = 1;
		return LS.tag + 1;
	}
	return pk_alloc(nqw);
}

typedef struct
{
	u32 tag; // vertex index + 1, 0 = empty
	u32 gen;
	int oc;
	cv_t v;
} xv_t;

#define XCACHE 256
static xv_t xcache[XCACHE];
static u32 xcache_gen;

static inline xv_t *xv_get(const FOutVector *base, const float *vcol, u32 vi)
{
	xv_t *e = &xcache[vi & (XCACHE - 1)];

	if (e->tag != vi + 1 || e->gen != xcache_gen)
	{
		const float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);

#ifdef HW_VU0
		if (P.vu0 && !vcol)
		{
			pv_t unused;

			e->oc = vu0_xform(&base[vi], &e->v, &unused);
		}
		else
#endif
		{
			xform_vertex(&base[vi], vcol ? &vcol[vi * 4] : NULL, &e->v);
			e->oc = outcode(&e->v, gx, gy);
		}
		e->tag = vi + 1;
		e->gen = xcache_gen;
		H.st.verts_in++;
	}
	return e;
}

// idx: 3 indices per triangle
static void emit_tris(const FOutVector *base, const u32 *idx, u32 nidx, const float *vcol)
{
	u32 i, t0 = cyc();
	int na = P.vcol ? 6 : 2;
	int simple_mode = P.npass == 1 && (P.lmode == LM_NONE || (P.lmode == LM_RAMP && !P.pass[0].ramp)) && !P.cutx && !P.cuty && !P.water && P.pass[0].tex == (u8)P.textured;
	int fmt = P.vcol ? 4 : (P.textured ? 0 : 2);
	int nreg = pack_regs(fmt);
	pack_t pk;

	xcache_gen++;
	LS.open = 0;
	LS.nreg = nreg;
	LS.fmt = fmt;
	memset(&pk, 0, sizeof pk);
	pk.fmt = fmt;
	pk.nreg = nreg;
	pk.col = P.pass[0].col;
	if (simple_mode)
		pass_setup(0, 0);
	for (i = 0; i + 2 < nidx; i += 3)
	{
		// Indices in one triangle can collide in the direct-mapped cache.
		xv_t av = *xv_get(base, vcol, idx[i]);
		xv_t bv = *xv_get(base, vcol, idx[i + 1]);
		xv_t cv = *xv_get(base, vcol, idx[i + 2]);
		const xv_t *a = &av, *b = &bv, *c = &cv;
		int and_ = a->oc & b->oc & c->oc, or_ = a->oc | b->oc | c->oc;
		cv_t *cl;
		int cnt = 3, k;
		qw_t *o;

		H.st.polys++;
		if (P.cull)
		{
			float area = a->v.x * (b->v.y * c->v.w - c->v.y * b->v.w)
				+ b->v.x * (c->v.y * a->v.w - a->v.y * c->v.w)
				+ c->v.x * (a->v.y * b->v.w - b->v.y * a->v.w);
			if (area * (float)P.cull <= 0.0f)
			{
				H.st.rejected++;
				continue;
			}
		}
		if (and_)
		{
			H.st.rejected++;
			continue;
		}
		if (or_)
		{
			vbuf_a[0] = a->v;
			vbuf_a[1] = b->v;
			vbuf_a[2] = c->v;
			cnt = clip_poly(vbuf_a, vbuf_b, 3, na, or_, &cl);
			H.st.clipped++;
			if (cnt < 3)
			{
				H.st.rejected++;
				continue;
			}
		}
		else
		{
			vbuf_a[0] = a->v;
			vbuf_a[1] = b->v;
			vbuf_a[2] = c->v;
			cl = vbuf_a;
		}
		if (simple_mode)
		{
			// wrapping power of two textures: drop whole repeats
			if (P.textured && (P.wrapx || P.wrapy))
			{
				float smin = cl[0].a[0], tmin = cl[0].a[1];

				for (k = 1; k < cnt; k++)
				{
					if (cl[k].a[0] < smin)
						smin = cl[k].a[0];
					if (cl[k].a[1] < tmin)
						tmin = cl[k].a[1];
				}
				pk.sb = (P.wrapx && fabsf(smin) > 4.0f) ? floorf(smin) : 0.0f;
				pk.tb = (P.wrapy && fabsf(tmin) > 4.0f) ? floorf(tmin) : 0.0f;
			}
			o = ls_reserve((u32)((cnt - 2) * 3 * nreg));
			for (k = 1; k + 1 < cnt; k++)
			{
				o = put_vertex(o, &cl[0], &pk);
				o = put_vertex(o, &cl[k], &pk);
				o = put_vertex(o, &cl[k + 1], &pk);
				LS.n += 3;
			}
			if (P.lmode == LM_RAMP)
			{
				if (!ov_room(cnt))
				{
					ls_close();
					ov_flush();
					pass_setup(0, 0); // the flush changed the GS state
				}
				emit_overlay(cl, cnt);
			}
		}
		else
		{
			piece_t pc;

			// the general path: cuts and passes (state changes between triangles are paid for here)
			ls_close();
			pc.sb = pc.tb = 0.0f;
			pc.cls = -1;
			pc.ripple = 0;
			if (P.textured && (P.wrapx || P.wrapy) && !P.cutx && !P.cuty)
			{
				float smin = cl[0].a[0], tmin = cl[0].a[1];

				for (k = 1; k < cnt; k++)
				{
					if (cl[k].a[0] < smin)
						smin = cl[k].a[0];
					if (cl[k].a[1] < tmin)
						tmin = cl[k].a[1];
				}
				pc.sb = (P.wrapx && fabsf(smin) > 4.0f) ? floorf(smin) : 0.0f;
				pc.tb = (P.wrapy && fabsf(tmin) > 4.0f) ? floorf(tmin) : 0.0f;
			}
			memcpy(vbuf_b, cl, (size_t)cnt * sizeof(cv_t)); // the cut stages use their own buffers; keep the clipped copy stable
			cut_and_emit(vbuf_b, cnt, 0, pc, na);
			LS.open = 0;
		}
	}
	ls_close();
	H.st.cyc_draw += cyc() - t0;
}

// ---- clear ----

static void do_clear(int colour, int depth, const FRGBAFloat *cc)
{
	u32 rgba = 0x80000000u;

	if (!colour && !depth)
		return;
	if (OV.n)
		ov_flush();
	frame_prepare(colour);
	if (colour && cc)
	{
		u32 r = (u32)(clampf(cc->red, 0.0f, 1.0f) * 255.0f + 0.5f);
		u32 g = (u32)(clampf(cc->green, 0.0f, 1.0f) * 255.0f + 0.5f);
		u32 b = (u32)(clampf(cc->blue, 0.0f, 1.0f) * 255.0f + 0.5f);
		u32 a = (u32)(clampf(cc->alpha, 0.0f, 1.0f) * 128.0f + 0.5f);

		rgba = r | (g << 8) | (b << 16) | (a << 24);
	}
	gs_fill_all(H.target, colour, depth, rgba);
}

// ---- screen textures and full-screen draws ----

// Sprite with texel coordinates from a texture record into the current frame buffer (or any buffer: `frame`).
// x0..y1 are frame buffer pixels, u0..v1 texels. No Z, no alpha test. clut_blk != 0 replaces the texture's own CLUT image.
static void sprite_tex(texrec_t *r, int linear, u64 frame, u64 alpha, int abe, int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1,
	int sciss_w, int sciss_h, u32 clut_blk)
{
	gsstate_t s;
	u32 cbp = clut_blk;
	qw_t *p;

	if (OV.n)
		ov_flush();
	memset(&s, 0, sizeof s);
	if (r->psm == PSM_T8 && !cbp)
		cbp = clut_get(r->clut == 3 ? CK_ALPHA : (r->clut == 2 ? CK_KEYED : CK_OPAQUE), 0, 0);
	tex_regs(r, 1, 1, linear, cbp, 1, &s.tex0, &s.tex1, &s.clamp);
	s.test = TEST_BITS(0, ATST_ALWAYS, 0) | (1ull << 16) | (1ull << 17);
	s.alpha = alpha;
	s.zbuf = zbuf_reg(1);
	s.frame = frame;
	s.scissor = scissor_reg(0, 0, sciss_w - 1, sciss_h - 1);
	s.use_tex = 1;
	s.rgbaq = rgbaq_of(0x80808080u);
	state_flush(&s);
	p = pk_alloc(1 + 2 * 3);
	p[0].d[0] = HWGIF_TAG(2, 1, 1, PRIM_SPRITE | PRIM_TME | (1u << 8) | (abe ? PRIM_ABE : 0), 0, 3);
	p[0].d[1] = 3ull | (1ull << 4) | (5ull << 8); // UV, RGBAQ, XYZ2
	p[1].d[0] = (u64)(u32)(u0 * 16.0f) | ((u64)(u32)(v0 * 16.0f) << 32); // PACKED UV: U in bits 0..13, V in bits 32..45
	p[1].d[1] = 0;
	p[2].d[0] = 0x80 | (0x80ull << 32);
	p[2].d[1] = 0x80 | (0x80ull << 32);
	p[3].d[0] = (u64)(u32)((2048 + x0) * 16) | ((u64)(u32)((2048 + y0) * 16) << 32);
	p[3].d[1] = 0;
	p[4].d[0] = (u64)(u32)(u1 * 16.0f) | ((u64)(u32)(v1 * 16.0f) << 32);
	p[4].d[1] = 0;
	p[5] = p[2];
	p[6].d[0] = (u64)(u32)((2048 + x1) * 16) | ((u64)(u32)((2048 + y1) * 16) << 32);
	p[6].d[1] = 0;
}
