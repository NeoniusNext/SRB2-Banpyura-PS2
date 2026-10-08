// PS2 GS hardware renderer driver: the sprite stream (included by ps2_hwd.c after ps2_hw_fx2.inc). PS2-HW-255.. (OPT11 round 3, FX3).
//
// The things of the game (billboards) and their drop shadows were polygons like any other: HWR_ProcessPolygon copied the four vertices and the surface into the batch, the batch was
// sorted by texture, planned, drawn with a plan (begin_draw) per texture run, and every polygon was copied once more into a chunk of the polygon program. 5 K cycles per sprite on
// the EE, 95 % of the sprites of a DEMO_001 frame being smaller than 8 pixels. Now the engine hands a sprite polygon (hardware/hw_batching.c, HWR_ProcessPolygon, the sprite batch
// only) to PS2HWD_SprPoly, which keeps what the drawing needs in a record of 96 bytes (the parallelogram of the quad, the texture, the surface) in a list (shadows, sprites); the flush of
// the batch (HWR_RenderBatches) draws the shadows first and the sprites after the polygons of the batch, as the batch did. A flush makes one plan per run of equal surfaces (a frame:
// two), then writes for every sprite 6 quad words (the header and the parallelogram, TEX0 and CLAMP of its texture) into a chunk of the sprite program (vu1/ps2_hw_vu1_spr.vsm), which
// expands the four vertices, tests them against the guard band volume, divides, converts, picks the colormap row of the depth and writes the GIF packet. The EE does not transform,
// clip, cut at the light steps or pack anything: it lets the VU1 do it, as the polygon program does for the batches, for the sprites that need no clipping:
//  * a sprite whose bounding sphere does not lie wholly inside the guard band volume (the near plane included) goes the old way (HWR_ProcessPolygon of the batch);
//  * a polygon that is not a parallelogram with the texture coordinates of a sprite, a texture that is not a patch of palette colours, a blend that is not the sprite's, the same.
// -hwfx 262144 (FX3_NOSPR): the stream is off, everything goes the old way (A/B on one ELF).

#ifdef __mips__

#define SPR_MAXENT 512 // entries of the list of the sprites, SPR_MAXSHD of the shadows; a full list is drawn at once
#define SPR_MAXSHD 160

typedef ps2spr_t spr_ent_t;

#define SPR_KW 8 // words of the surface key of a record

static struct
{
	spr_ent_t *ent[2]; // [0] shadows, [1] sprites (zone blocks, PS2HWD_Init)
	u32 n[2], cap[2];
	int in_flush; // diagnostics: PS2HWD_SprFlush is running
	// the plan the sprite chunk is in
	int valid;
	u32 kw[SPR_KW]; // the surface key of the plan
	u32 vw[16]; // the view key of the plan
	u32 serial, cgen, epoch, palgen;
	u32 alpha_var; // the plan takes the alpha of the vertices from the record (every blend but Subtractive)
	u32 fixed_alpha; // else: the alpha of the plan's colour
	// the view of the flush in progress
	u32 cur_vw[16];
	// statistics
	u32 st_why[8];
	u32 st_kept, st_flush, st_plans, st_fb, st_uploads, st_rj_gate, st_rj_tex, st_rj_para, st_rj_geom, st_rj_full;
} SPR;

static void hw_DrawPolygon(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags); // (below)

static void spr_free(void)
{
	Z_Free(SPR.ent[0]);
	Z_Free(SPR.ent[1]);
	SPR.ent[0] = SPR.ent[1] = NULL;
	SPR.n[0] = SPR.n[1] = SPR.cap[0] = SPR.cap[1] = 0;
	SPR.valid = 0;
}

static void spr_alloc(void)
{
	spr_free();
	SPR.ent[0] = (spr_ent_t *)Z_TryMallocAlign(sizeof(spr_ent_t) * SPR_MAXSHD, PU_STATIC, NULL, 6);
	SPR.ent[1] = (spr_ent_t *)Z_TryMallocAlign(sizeof(spr_ent_t) * SPR_MAXENT, PU_STATIC, NULL, 6);
	if (!SPR.ent[0] || !SPR.ent[1])
	{
		spr_free(); // no room: the stream is off (the polygons go through the batch)
		return;
	}
	SPR.cap[0] = SPR_MAXSHD;
	SPR.cap[1] = SPR_MAXENT;
}

// the view and the shader state a plan of the VU1 programs depends on (the words of vu_key_make that are not about the surface or the texture)
static void spr_view_make(u32 *w)
{
	xf_update();
	w[0] = xf_ver;
	w[1] = f2u_(H.vpx);
	w[2] = f2u_(H.vpy);
	w[3] = f2u_(H.vpw);
	w[4] = f2u_(H.vph);
	w[5] = f2u_(H.near_plane);
	w[6] = (u32)H.guard_x;
	w[7] = (u32)H.guard_y;
	w[8] = (u32)H.shader | ((u32)H.shaders_on << 8) | ((u32)H.tex_filter << 12) | ((u32)(H.imm_tex != NULL) << 20);
	w[9] = H.pal_gen;
	w[10] = H.sciss_x0 | (H.sciss_y0 << 16);
	w[11] = H.sciss_x1 | (H.sciss_y1 << 16);
	w[12] = (u32)H.target | (H.frame_no << 4) | ((u32)palette_mode() << 3);
	w[13] = w[14] = w[15] = 0;
}

// what of a texture decides the plan of the sprite stream: the palette kind (the rows of the CLUT table: with or without a hole at 255) and the filter of TEX1. The rest of what makes the
// plan of the polygon program (the wrap, the size that is not a power of two, the region, the alpha of the texels) is per sprite here: TEX0 and CLAMP of every record
static inline u32 spr_kind(const texrec_t *r)
{
	return (u32)r->psm | ((u32)r->clut << 6) | ((u32)(r->nearest_only != 0) << 16) | (1u << 17);
}

static inline u32 spr_pcol(u32 flags, const FSurfaceInfo *s)
{
	if (!(flags & PF_Modulated))
		return 0xFFFFFFFFu;
	if ((flags & PF_Blending) == (PF_Subtractive & PF_Blending))
		return s->PolyColor.rgba;
	return s->PolyColor.rgba & 0x00FFFFFFu; // the alpha of the polygon is the alpha of the vertices (col_lut), taken from the record
}

static inline void spr_key_make(u32 *k, const spr_ent_t *e, u32 kind)
{
	k[0] = e->flags;
	k[1] = spr_pcol(e->flags, &e->surf);
	k[2] = e->surf.TintColor.rgba;
	k[3] = e->surf.FadeColor.rgba;
	k[4] = e->surf.LightTableId;
	k[5] = (u32)e->surf.LightInfo.fade_start | ((u32)e->surf.LightInfo.fade_end << 8) | ((u32)e->shader << 16);
	k[6] = kind;
	k[7] = 0;
}

// the plan of the stream for this record (begin_draw with its surface and the texture of r, the constants of the VU1 programs, the GS state of the plan). 0: the stream cannot take it
static int spr_plan_make(const spr_ent_t *e, texrec_t *r, const u32 *kw)
{
	pass_t *ps;

	SPR.valid = 0;
	vu_sync();
	H.shader = e->shader;
	H.cur_tex = (int)(r - H.rec);
	H.cur_missing = 0;
	if (!begin_draw(e->flags, &e->surf))
		return -1; // the draw is skipped (a missing texture)
	r = &H.rec[H.cur_tex];
	if (P.npass != 1 || !P.pal || !P.vuok || !P.textured || P.r != r || P.water || r->psm != PSM_T8)
		return 0;
	ps = &P.pass[0];
	VU.consts_ok = 0;
	VU.reuse = 0;
	if (!vu_plan_setup())
		return 0;
	SPR.valid = 1;
	memcpy(SPR.kw, kw, sizeof SPR.kw);
	memcpy(SPR.vw, SPR.cur_vw, sizeof SPR.vw);
	SPR.serial = H.serial;
	SPR.cgen = VU.cgen;
	SPR.epoch = H.clut_epoch;
	SPR.palgen = H.pal_gen;
	SPR.alpha_var = (e->flags & PF_Modulated) && (e->flags & PF_Blending) != (PF_Subtractive & PF_Blending);
	SPR.fixed_alpha = ps->col >> 24;
	SPR.st_plans++;
	return 1;
}

// the record of one sprite into the open chunk of the sprite program
static void spr_record(const spr_ent_t *e, const texrec_t *r)
{
	qw_t *o;
	u32 alpha;
	u64 t0, cl;
	int wms = r->wrapx ? 0 : 1, wmt = r->wrapy ? 0 : 1;
	const float sx = r->sxs, sy = r->sys;

	if (VU.open && (VU.kind != 1 || VU.chunk_n + 6 > VU_CHUNK_QW || H.wr + 6 + 3 > BUF_QW))
		vu_chunk_end();
	if (!VU.open)
	{
		if (H.wr + 6 + 3 + 8 > BUF_QW)
			pk_flush();
		vu_chunk_open(1);
	}
	if (e->flags & PF_ForceWrapX)
		wms = 0;
	if (e->flags & PF_RemoveYWrap)
		wmt = 1;
	if (e->flags & PF_ForceWrapY)
		wmt = 0;
	cl = (u64)((r->region & 1) ? 2 : wms) | ((u64)((r->region & 2) ? 2 : wmt) << 2) | ((u64)(r->uw - 1) << 14) | ((u64)(r->vh - 1) << 34);
	t0 = (u64)r->blk | ((u64)r->tbw << 14) | ((u64)r->psm << 20) | ((u64)r->tw << 26) | ((u64)r->th << 30) | ((u64)(r->notcc ? 0 : 1) << 34); // (the CLUT: loaded by the program; no decal)
	alpha = SPR.alpha_var ? (u32)col_lut[e->surf.PolyColor.s.alpha] : SPR.fixed_alpha;
	o = H.ring[H.cur] + H.wr;
	o[0].w[0] = 1;
	o[0].f[1] = (float)(int)e->surf.LightInfo.light_level;
	o[0].w[2] = alpha;
	o[0].f[3] = e->sl * sx;
	o[1].f[0] = e->p0[0];
	o[1].f[1] = e->p0[1];
	o[1].f[2] = e->p0[2];
	o[1].f[3] = e->tt * sy;
	o[2].f[0] = e->r[0];
	o[2].f[1] = e->r[1];
	o[2].f[2] = e->r[2];
	o[2].f[3] = e->sr * sx;
	o[3].f[0] = e->u[0];
	o[3].f[1] = e->u[1];
	o[3].f[2] = e->u[2];
	o[3].f[3] = e->tb * sy;
	ad_set(&o[4], t0, GSR_TEX0_1);
	ad_set(&o[5], cl, GSR_CLAMP_1);
	H.wr += 6;
	VU.chunk_n += 6;
	VU.npoly++;
	H.st.polys++;
	H.st.verts_in += 4;
}

// a record the stream cannot draw: the way of the single draws (the plan of the stream is gone)
static void spr_fallback(const spr_ent_t *e)
{
	FOutVector v[4];
	FSurfaceInfo surf = e->surf;
	int k;

	for (k = 0; k < 4; k++)
	{
		const float a = (k == 1 || k == 2) ? 1.0f : 0.0f, b = (k >= 2) ? 1.0f : 0.0f;

		v[k].x = e->p0[0] + a * e->r[0] + b * e->u[0];
		v[k].y = e->p0[1] + a * e->r[1] + b * e->u[1];
		v[k].z = e->p0[2] + a * e->r[2] + b * e->u[2];
		v[k].s = (k == 1 || k == 2) ? e->sr : e->sl;
		v[k].t = (k >= 2) ? e->tt : e->tb;
	}
	SPR.valid = 0;
	SPR.st_fb++;
	H.shader = e->shader;
	settex_now(e->tex);
	hw_DrawPolygon(&surf, v, 4, e->flags);
}

// draws a list: the records in the order they came
static void spr_flush_list(int which)
{
	spr_ent_t *e = SPR.ent[which];
	const u32 n = SPR.n[which];
	u32 i;

	if (!n)
		return;
	SPR.n[which] = 0;
	SPR.st_flush += n;
	spr_view_make(SPR.cur_vw);
	if (SPR.valid && memcmp(SPR.vw, SPR.cur_vw, sizeof SPR.vw))
		SPR.valid = 0;
	// the textures first: every one of the list resident before the first record is written. A texture that is still to be drawn weighs 60 against eviction (hw: evict_weight, batch
	// phase 2), the others at most 31: the uploads of this loop take the room of the textures that are not pending, and what the loop below draws is never uploaded again (a patch whose
	// data the zone dropped after its upload cannot be made again here: it would be skipped)
	for (i = 0; i < n; i++)
	{
		if (!rec_of(e[i].tex))
		{
			texrec_t *r;

			SPR.st_uploads++;
			settex_now(e[i].tex);
			r = rec_of(e[i].tex);
			if (r)
			{
				H.rec[img_of((int)(r - H.rec))].done = 0; // (settex_now marked it drawn: it is not yet)
			}
			else
			{
				static unsigned nmsg;

				if (nmsg++ < 12)
					CONS_Printf("HWSPR not resident: list %d entry %u of %u tex=%p dl_now=%u data=%p fmt=%d %ux%u frame=%u phase=%d\n", which, i, n, e[i].tex, (unsigned)((GLMipmap_t *)e[i].tex)->downloaded,
						(void *)((GLMipmap_t *)e[i].tex)->data, (int)((GLMipmap_t *)e[i].tex)->format, (unsigned)((GLMipmap_t *)e[i].tex)->width, (unsigned)((GLMipmap_t *)e[i].tex)->height, (unsigned)H.frame_no, batch_phase);
			}
		}
	}
	for (i = 0; i < n; i++, e++)
	{
		texrec_t *r = rec_of(e->tex);
		u32 kw[SPR_KW];
		int idx;

		if (!r)
			continue; // not resident (no room or no data): the draw is skipped, as the batch skips it
		idx = (int)(r - H.rec);
		if (r->psm != PSM_T8 || (r->clut != 1 && r->clut != 2) || r->screen || r->wrapx || r->wrapy)
		{
			spr_fallback(e);
			continue;
		}
		spr_key_make(kw, e, spr_kind(r));
		if (!SPR.valid || SPR.serial != H.serial || SPR.cgen != VU.cgen || SPR.epoch != H.clut_epoch || SPR.palgen != H.pal_gen || !VU.consts_ok || memcmp(SPR.kw, kw, sizeof kw))
		{
			SPR.st_why[!SPR.valid ? 0 : SPR.serial != H.serial ? 1 : SPR.cgen != VU.cgen ? 2 : SPR.epoch != H.clut_epoch ? 3 : SPR.palgen != H.pal_gen ? 4 : !VU.consts_ok ? 5 : 6]++;
			const int ok = spr_plan_make(e, r, kw);

			if (ok < 0)
				continue;
			if (!ok)
			{
				spr_fallback(e);
				continue;
			}
			r = &H.rec[idx];
		}
		H.cur_tex = idx;
		plan_tex_touch(r);
		H.rec[img_of(idx)].done = H.frame_no + 1;
		spr_record(e, r);
	}
}

// both lists: the shadows, then the sprites (one residency pass over the textures of both)
void PS2HWD_SprFlush(int which)
{
	u32 t0;

	(void)which;
	if (!SPR.n[0] && !SPR.n[1])
		return;
	t0 = cyc();
	drv_in();
	SPR.in_flush = 1;
	spr_flush_list(0);
	spr_flush_list(1);
	SPR.in_flush = 0;
	drv_out();
	G.spr_cyc += cyc() - t0;
}

void PS2HWD_SprReset(void)
{
	SPR.n[0] = SPR.n[1] = 0;
	SPR.valid = 0;
}

// bounding sphere of the parallelogram inside the guard band volume with the near plane away from it?
static inline int spr_geom_ok(const float *p0, const float *r, const float *u)
{
	const ps2cull_t *cs = PS2HWD_CullSetup();
	float cx, cy, cz, cw, R, d0, d1, wlo, gxw, gyw;
	float X, Y, Z;

	if (!cs->valid)
		return 0;
	X = p0[0] + 0.5f * (r[0] + u[0]);
	Y = p0[1] + 0.5f * (r[1] + u[1]);
	Z = p0[2] + 0.5f * (r[2] + u[2]);
	d0 = fabsf(r[0] + u[0]) + fabsf(r[1] + u[1]) + fabsf(r[2] + u[2]); // the L1 length of the diagonals bounds the L2 length
	d1 = fabsf(u[0] - r[0]) + fabsf(u[1] - r[1]) + fabsf(u[2] - r[2]);
	R = 0.5f * (d0 > d1 ? d0 : d1);
	cw = cs->r[3][0] * X + cs->r[3][1] * Y + cs->r[3][2] * Z + cs->r[3][3];
	wlo = cw - cs->nw * R;
	if (wlo < cs->w2)
		return 0; // a corner could be at the near plane
	if (cw + cs->nw * R > 0.98f * FAR_CLIPPING_PLANE)
		return 0;
	gxw = ((float)H.guard_x * (1.0f / 1024.0f)) * 0.9f * wlo;
	gyw = ((float)H.guard_y * (1.0f / 1024.0f)) * 0.9f * wlo;
	cx = cs->r[0][0] * X + cs->r[0][1] * Y + cs->r[0][2] * Z + cs->r[0][3];
	if (fabsf(cx) + cs->nx * R > gxw)
		return 0;
	cy = cs->r[1][0] * X + cs->r[1][1] * Y + cs->r[1][2] * Z + cs->r[1][3];
	(void)cz;
	return fabsf(cy) + cs->ny * R <= gyw;
}

// the slot of the next record of a list (the engine fills it), NULL: the stream is off or the list is full
ps2spr_t *PS2HWD_SprSlot(int is_shadow)
{
	const int which = is_shadow ? 0 : 1;

	if ((ps2hwd_fx2 & FX3_NOSPR) || !H.up || !V.on || !V.ready || !VU.spr_base || !SPR.ent[0])
		return NULL;
	if (SPR.n[which] >= SPR.cap[which])
	{
		SPR.st_rj_full++;
		return NULL;
	}
	return &SPR.ent[which][SPR.n[which]];
}

// keeps the record of the slot (the engine has filled it): the checks the stream needs of it; 0: the engine draws the polygon the old way
int PS2HWD_SprCommit(int is_shadow)
{
	const int which = is_shadow ? 0 : 1;
	const spr_ent_t *e = &SPR.ent[which][SPR.n[which]];
	const GLMipmap_t *m = (const GLMipmap_t *)e->tex;
	const u32 blend = e->flags & PF_Blending;

	if ((m->format != GL_TEXFMT_P_8 && m->format != GL_TEXFMT_AP_88) || m->regen_kind || (blend != PF_Masked && blend != (PF_Translucent & PF_Blending))
		|| (e->flags & ~(u32)(PF_Blending | PF_Occlude | PF_Modulated | PF_ColorMapped)))
	{
		SPR.st_rj_gate++;
		return 0;
	}
	if (!PS2HWD_PalLit(&e->surf, e->flags, m, e->shader))
	{
		SPR.st_rj_tex++;
		return 0;
	}
	if (!spr_geom_ok(e->p0, e->r, e->u))
	{
		SPR.st_rj_geom++;
		return 0;
	}
	SPR.n[which]++;
	SPR.st_kept++;
	return 1;
}

int PS2HWD_SprFromVerts(ps2spr_t *e, const void *vverts)
{
	const FOutVector *v = (const FOutVector *)vverts;
	float r[3], u[3];

	r[0] = v[1].x - v[0].x;
	r[1] = v[1].y - v[0].y;
	r[2] = v[1].z - v[0].z;
	u[0] = v[3].x - v[0].x;
	u[1] = v[3].y - v[0].y;
	u[2] = v[3].z - v[0].z;
	// the polygon must be the parallelogram of a sprite: corners 0, 1, 3 give P0, R, U, corner 2 is P0 + R + U, the texture coordinates are those of a rectangle
	if (fabsf(v[2].x - (v[0].x + r[0] + u[0])) > 0.01f || fabsf(v[2].y - (v[0].y + r[1] + u[1])) > 0.01f || fabsf(v[2].z - (v[0].z + r[2] + u[2])) > 0.01f
		|| v[0].s != v[3].s || v[1].s != v[2].s || v[0].t != v[1].t || v[2].t != v[3].t)
		return 0;
	e->p0[0] = v[0].x;
	e->p0[1] = v[0].y;
	e->p0[2] = v[0].z;
	e->tt = v[2].t;
	e->r[0] = r[0];
	e->r[1] = r[1];
	e->r[2] = r[2];
	e->sr = v[1].s;
	e->u[0] = u[0];
	e->u[1] = u[1];
	e->u[2] = u[2];
	e->tb = v[0].t;
	e->sl = v[0].s;
	return 1;
}

// A sprite or shadow polygon of the sprite batch (hw_batching.c). 1: kept for the stream (it is drawn by the flush of the batch); 0: the caller makes a batch polygon of it.
int PS2HWD_SprPoly(const void *vsurf, const void *vverts, unsigned int flags, int shader, void *vtex, int is_shadow)
{
	ps2spr_t *e = PS2HWD_SprSlot(is_shadow);

	if (!e || !vtex || !vsurf)
		return 0;
	if (!PS2HWD_SprFromVerts(e, vverts))
	{
		SPR.st_rj_para++;
		return 0;
	}
	e->flags = flags;
	e->shader = shader;
	e->tex = vtex;
	e->surf = *(const FSurfaceInfo *)vsurf;
	return PS2HWD_SprCommit(is_shadow);
}

void PS2HWD_SprProf(unsigned int frames)
{
	I_OutputMsg("HWPROF42 sprite stream (FX3): kept=%u flushed=%u plans=%u fallback=%u uploads=%u chunks=%u | rejected: gate=%u texture=%u shape=%u geometry=%u full=%u | spr cyc=%u\n", SPR.st_kept / frames, SPR.st_flush / frames,
		SPR.st_plans / frames, SPR.st_fb / frames, SPR.st_uploads / frames, VU.stat_sprchunks / frames, SPR.st_rj_gate / frames, SPR.st_rj_tex / frames, SPR.st_rj_para / frames, SPR.st_rj_geom / frames, SPR.st_rj_full / frames,
		(unsigned)(G.spr_cyc / frames));
	I_OutputMsg("HWPROF43 plan made because: invalid=%u serial=%u cgen=%u epoch=%u palgen=%u consts=%u key=%u\n", SPR.st_why[0] / frames, SPR.st_why[1] / frames, SPR.st_why[2] / frames, SPR.st_why[3] / frames, SPR.st_why[4] / frames, SPR.st_why[5] / frames, SPR.st_why[6] / frames);
	memset(SPR.st_why, 0, sizeof SPR.st_why);
	SPR.st_kept = SPR.st_flush = SPR.st_plans = SPR.st_fb = SPR.st_uploads = SPR.st_rj_gate = SPR.st_rj_tex = SPR.st_rj_para = SPR.st_rj_geom = SPR.st_rj_full = 0;
	VU.stat_sprchunks = 0;
	G.spr_cyc = 0;
}

#else // host test build

ps2spr_t *PS2HWD_SprSlot(int is_shadow)
{
	(void)is_shadow;
	return NULL;
}

int PS2HWD_SprCommit(int is_shadow)
{
	(void)is_shadow;
	return 0;
}

int PS2HWD_SprFromVerts(ps2spr_t *e, const void *verts)
{
	(void)e; (void)verts;
	return 0;
}

int PS2HWD_SprPoly(const void *vsurf, const void *vverts, unsigned int flags, int shader, void *vtex, int is_shadow)
{
	(void)vsurf; (void)vverts; (void)flags; (void)shader; (void)vtex; (void)is_shadow;
	return 0;
}

void PS2HWD_SprFlush(int which)
{
	(void)which;
}

void PS2HWD_SprReset(void)
{
}

static void spr_free(void)
{
}

static void spr_alloc(void)
{
}

void PS2HWD_SprProf(unsigned int frames)
{
	(void)frames;
}

#endif
