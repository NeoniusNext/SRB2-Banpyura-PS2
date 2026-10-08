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

#define SPR_MAXENT 896 // entries of the list of the sprites, SPR_MAXSHD of the shadows; a sprite that finds the list full takes the old way (DEMO_004: 650 sprites and 190 shadows a frame at most)
#define SPR_MAXSHD 256
#define SPR_NGROUP 16 // plans (groups of equal surface) of a list that are told apart; the entries of the last group may differ

typedef ps2spr_t spr_ent_t;

typedef struct
{
	u64 k[4];
} sprkey_t; // what of a record and of its texture makes the plan of the stream: four doublewords

static struct
{
	spr_ent_t *ent[2]; // [0] shadows, [1] sprites (zone blocks, PS2HWD_Init)
	u32 n[2], cap[2];
	u8 gid[SPR_MAXENT]; // the group of every entry of the list being drawn (0xFF: skipped, 0xFE: the way of the single draws)
	int in_flush; // diagnostics: PS2HWD_SprFlush is running
	// the plan the sprite chunk is in
	int valid;
	sprkey_t kw; // the surface key of the plan
	u32 vw[16]; // the view key of the plan
	u32 serial, cgen, epoch, palgen;
	texrec_t *touched; // the record of the texture the last record of the stream was written for (its LRU touch and draw mark are made)
	u32 alpha_var; // the plan takes the alpha of the vertices from the record (every blend but Subtractive)
	u32 fixed_alpha; // else: the alpha of the plan's colour
	// the view of the flush in progress
	u32 cur_vw[16];
	// statistics
	u32 st_why[8], st_kw[8];
	u32 st_note[8];
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
	u32 ne = SPR_MAXENT, ns = SPR_MAXSHD; // -hwsprent N / -hwsprshd N: the sizes of the lists (the zone is tight on 32 MB: 112 bytes an entry; measurement)

	spr_free();
	if (M_CheckParm("-hwsprent") && M_IsNextParm())
		ne = (u32)atoi(M_GetNextParm());
	if (M_CheckParm("-hwsprshd") && M_IsNextParm())
		ns = (u32)atoi(M_GetNextParm());
	ne = ne < 16 ? 16 : ne > SPR_MAXENT ? SPR_MAXENT : ne;
	ns = ns < 16 ? 16 : ns > SPR_MAXENT ? SPR_MAXENT : ns;
	SPR.ent[0] = (spr_ent_t *)Z_TryMallocAlign(sizeof(spr_ent_t) * ns, PU_STATIC, NULL, 6);
	SPR.ent[1] = (spr_ent_t *)Z_TryMallocAlign(sizeof(spr_ent_t) * ne, PU_STATIC, NULL, 6);
	if (!SPR.ent[0] || !SPR.ent[1])
	{
		spr_free(); // no room: the stream is off (the polygons go through the batch)
		return;
	}
	SPR.cap[0] = ns;
	SPR.cap[1] = ne;
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

static inline void spr_key_make(sprkey_t *k, const spr_ent_t *e, u32 kind)
{
	k->k[0] = (u64)e->flags | ((u64)(u32)e->shader << 32);
	k->k[1] = (u64)spr_pcol(e->flags, &e->surf) | ((u64)e->surf.TintColor.rgba << 32);
	k->k[2] = (u64)e->surf.FadeColor.rgba | ((u64)e->surf.LightTableId << 32);
	k->k[3] = (u64)((u32)e->surf.LightInfo.fade_start | ((u32)e->surf.LightInfo.fade_end << 8)) | ((u64)kind << 32);
}

// the plan of the stream for this record (begin_draw with its surface and the texture of r, the constants of the VU1 programs, the GS state of the plan). 0: the stream cannot take it
static int spr_plan_make(const spr_ent_t *e, texrec_t *r, const sprkey_t *kw)
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
	SPR.touched = NULL;
	SPR.kw = *kw;
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

// the records of a flush are written straight into the ring: the position and the room live in locals of the loop, and are counted into the chunk when the loop leaves it
typedef struct
{
	qw_t *o; // the next record, NULL: no chunk of ours is open
	u32 cnt, cap; // written since the chunk was last counted, room for so many more
} sprw_t;

static inline void spr_w_sync(sprw_t *w)
{
	if (w->cnt)
	{
		H.wr += w->cnt * 6;
		VU.chunk_n += w->cnt * 6;
		VU.npoly += w->cnt;
		H.st.polys += w->cnt;
		H.st.verts_in += w->cnt * 4;
		w->cnt = 0;
	}
	w->o = NULL;
}

static void spr_w_open(sprw_t *w)
{
	u32 a, b;

	if (VU.open && (VU.kind != 1 || VU.chunk_n + 6 > VU_CHUNK_QW || H.wr + 6 + 3 > BUF_QW))
		vu_chunk_end();
	if (!VU.open)
	{
		if (H.wr + 6 + 3 + 8 > BUF_QW)
			pk_flush();
		vu_chunk_open(1);
	}
	w->o = H.ring[H.cur] + H.wr;
	w->cnt = 0;
	a = (VU_CHUNK_QW - VU.chunk_n) / 6;
	b = (BUF_QW - 3 - H.wr) / 6;
	w->cap = a < b ? a : b;
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
	SPR.touched = NULL;
	SPR.st_fb++;
	H.shader = e->shader;
	settex_now(e->tex);
	hw_DrawPolygon(&surf, v, 4, e->flags);
}

// draws a list: the entries are grouped by plan (the surface of the sector: the colours of its colormap, its light table; a frame has a few) and drawn group by group, the
// entries of a group in the order they came: they are opaque sprites or drop shadows, the order of the groups does not matter to the picture (the depth buffer decides) and a plan
// per group is a few thousand cycles where a plan for every change of sector (the list is in the depth order) was two of three entries
static void spr_flush_list(int which)
{
	spr_ent_t *e0 = SPR.ent[which];
	const u32 n = SPR.n[which];
	sprw_t w;
	sprkey_t gk[SPR_NGROUP];
	u8 *gid = SPR.gid;
	u32 ng = 0, g, i;
	texrec_t *lr = NULL; // the record of the texture of the last entry, and what the records of it need
	u64 t0 = 0, cl = 0;
	float sx = 1.0f, sy = 1.0f;

	if (!n)
		return;
	SPR.n[which] = 0;
	SPR.st_flush += n;
	SPR.touched = NULL;
	spr_view_make(SPR.cur_vw);
	if (SPR.valid && memcmp(SPR.vw, SPR.cur_vw, sizeof SPR.vw))
		SPR.valid = 0;
	// the textures first: every one of the list resident before the first record is written. A texture that is still to be drawn weighs 60 against eviction (hw: evict_weight, batch
	// phase 2), the others at most 31: the uploads of this loop take the room of the textures that are not pending, and what the loop below draws is never uploaded again (a patch whose
	// data the zone dropped after its upload cannot be made again here: it would be skipped). The same pass sorts the entries into groups of one plan.
	for (i = 0; i < n; i++)
	{
		const spr_ent_t *e = &e0[i];
		texrec_t *r = rec_of((const GLMipmap_t *)e->tex);
		sprkey_t k;

		if (!r)
		{
			SPR.st_uploads++;
			settex_now((GLMipmap_t *)e->tex);
			r = rec_of((const GLMipmap_t *)e->tex);
			if (r)
				H.rec[img_of((int)(r - H.rec))].done = 0; // (settex_now marked it drawn: it is not yet)
		}
		if (!r)
		{
			gid[i] = 0xFF; // not resident (no room or no data): the draw is skipped, as the batch skips it
			continue;
		}
		if (r->psm != PSM_T8 || (r->clut != 1 && r->clut != 2) || r->screen || r->wrapx || r->wrapy || (e->flags & (PF_ForceWrapX | PF_ForceWrapY | PF_RemoveYWrap)))
		{
			gid[i] = 0xFE; // not for the stream: the way of the single draws (below)
			continue;
		}
		spr_key_make(&k, e, spr_kind(r));
		for (g = 0; g < ng; g++)
			if (!((k.k[0] ^ gk[g].k[0]) | (k.k[1] ^ gk[g].k[1]) | (k.k[2] ^ gk[g].k[2]) | (k.k[3] ^ gk[g].k[3])))
				break;
		if (g == ng)
		{
			if (ng < SPR_NGROUP)
				gk[ng++] = k;
			else
				g = SPR_NGROUP - 1; // (too many plans: the last group changes its plan whenever its entries differ)
		}
		gid[i] = (u8)g;
	}
	w.o = NULL;
	w.cnt = w.cap = 0;
	for (g = 0; g < ng; g++)
	{
		const int multi = g == SPR_NGROUP - 1 && ng == SPR_NGROUP; // the entries of this group may have different plans
		int plan_ok = 0;

		for (i = 0; i < n; i++)
		{
			const spr_ent_t *e = &e0[i];
			texrec_t *r;

			if (gid[i] != g)
				continue;
			r = rec_of((const GLMipmap_t *)e->tex);
			if (!r)
				continue;
			if (!plan_ok || multi)
			{
				sprkey_t k;
				const sprkey_t *pk = &gk[g];

				if (multi)
				{
					spr_key_make(&k, e, spr_kind(r));
					pk = &k;
				}
				if (!(SPR.valid && SPR.serial == H.serial && SPR.cgen == VU.cgen && SPR.epoch == H.clut_epoch && SPR.palgen == H.pal_gen && VU.consts_ok
					&& !((pk->k[0] ^ SPR.kw.k[0]) | (pk->k[1] ^ SPR.kw.k[1]) | (pk->k[2] ^ SPR.kw.k[2]) | (pk->k[3] ^ SPR.kw.k[3]))))
				{
					const int idx = (int)(r - H.rec);
					int ok;

					SPR.st_why[!SPR.valid ? 0 : SPR.serial != H.serial ? 1 : SPR.cgen != VU.cgen ? 2 : SPR.epoch != H.clut_epoch ? 3 : SPR.palgen != H.pal_gen ? 4 : !VU.consts_ok ? 5 : 6]++;
					spr_w_sync(&w);
					ok = spr_plan_make(e, r, pk);
					lr = NULL; // (the plan may have written to the ring and the record table)
					if (ok < 0)
						continue;
					if (!ok)
					{
						spr_fallback(e);
						continue;
					}
					r = &H.rec[idx];
					SPR.touched = NULL;
				}
				plan_ok = 1;
			}
			if (r != lr)
			{
				// another texture than the last entry's: what its records carry
				const int idx = (int)(r - H.rec);

				sx = r->sxs;
				sy = r->sys;
				cl = (u64)((r->region & 1) ? 2 : 1) | ((u64)((r->region & 2) ? 2 : 1) << 2) | ((u64)(r->uw - 1) << 14) | ((u64)(r->vh - 1) << 34); // (a patch is clamped)
				t0 = (u64)r->blk | ((u64)r->tbw << 14) | ((u64)r->psm << 20) | ((u64)r->tw << 26) | ((u64)r->th << 30) | ((u64)(r->notcc ? 0 : 1) << 34); // (the CLUT: loaded by the program; no decal)
				lr = r;
				if (r != SPR.touched)
				{
					H.cur_tex = idx;
					plan_tex_touch(r);
					H.rec[img_of(idx)].done = H.frame_no + 1;
					SPR.touched = r;
				}
			}
			if (!w.o || w.cnt >= w.cap)
			{
				spr_w_sync(&w);
				spr_w_open(&w);
			}
			{
				qw_t *o = w.o;
				const u32 alpha = SPR.alpha_var ? (u32)col_lut[e->surf.PolyColor.s.alpha] : SPR.fixed_alpha;

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
				w.o = o + 6;
				w.cnt++;
			}
		}
	}
	spr_w_sync(&w);
	for (i = 0; i < n; i++)
	{
		if (gid[i] == 0xFE)
		{
			spr_fallback(&e0[i]);
			SPR.touched = NULL;
		}
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

int PS2HWD_SprNote(int what)
{
	SPR.st_note[what & 7]++;
	return 0;
}

// the slot of the next record of a list (the engine fills it), NULL: the stream is off or the list is full
ps2spr_t *PS2HWD_SprSlot(int is_shadow)
{
	const int which = is_shadow ? 0 : 1;

	if ((ps2hwd_fx2 & (FX3_NOSPR | FX3_NOSTREAM)) || !H.up || !V.on || !V.ready || !VU.spr_base || !SPR.ent[0])
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
	I_OutputMsg("HWPROF44 key words that differ: flags=%u shader=%u colour=%u tint=%u fade+table=%u fade-range=%u kind=%u\n", SPR.st_kw[0] / frames, SPR.st_kw[1] / frames, SPR.st_kw[2] / frames, SPR.st_kw[3] / frames, SPR.st_kw[4] / frames, SPR.st_kw[5] / frames, SPR.st_kw[6] / frames);
	I_OutputMsg("HWPROF45 shadow builder: built=%u not_for_it=%u patch=%u colormap=%u heightsec=%u slope=%u far=%u hidden=%u\n", SPR.st_note[0] / frames, SPR.st_note[1] / frames, SPR.st_note[2] / frames, SPR.st_note[3] / frames,
		SPR.st_note[4] / frames, SPR.st_note[5] / frames, SPR.st_note[6] / frames, SPR.st_note[7] / frames);
	memset(SPR.st_note, 0, sizeof SPR.st_note);
	memset(SPR.st_kw, 0, sizeof SPR.st_kw);
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

int PS2HWD_SprNote(int what)
{
	(void)what;
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
