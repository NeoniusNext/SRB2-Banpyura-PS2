// OPT11 (GEOM), PS2-HW-80: the geometry cache of the BSP walk. Included by hw_main.c (PS2 profile only), after HWR_CeilingLightLevel.
//
// What the BSP walk makes of a seg (HWR_ProcessSeg: the wall polygons, the sky walls, the translucent walls, the lighting of each) and of a floor or
// ceiling (HWR_RenderPlane) is a function of the map state (heights, flats, light, colormaps, side textures and offsets, 3D floors, slopes ...) and of
// some settings, and NOT of the view: on a map that is standing still the same polygons come out of the same functions at 1000 .. 1500 cycles each,
// frame after frame. The cache keeps the last result of every seg and plane:
//  * the INPUT of a result is a 64-bit fingerprint (two 32-bit lanes) of everything the function reads: the sector fields (gc_sector_base), for a
//    sector with 3D floors every floor and the light list (gc_sector_full), the side and line fields, the translated texture numbers (texture animation),
//    the TF_TRANSPARENT state of the textures, for a plane its arguments. The fingerprint of a sector is made once per view (a stamp per sector).
//    A sector the engine fakes (R_FakeFlat: water) is a copy: its fingerprint is made of the copy every time, so the view dependence is in it.
//  * the OUTPUT is the sequence of calls that the function made to the three sinks of the walk (HWR_SetCurrentTexture, HWR_ProcessPolygon,
//    HWR_AddTransparentWall): recorded while the function runs (hwr_grec_on, hw_batching.c) into an arena, and made again by gc_replay when the
//    fingerprint of the next call is the same. The batch gets the same polygons in the same order.
//  * segs of polyobjects, planes of a subsector with a horizon line (they hold the view position), polygons of more than 250 vertices and records
//    longer than GC_MAXREC are not cached. The tables are dropped when the level changes, when a texture record or a light table goes away
//    (HWR_GCacheFlush) and when a setting of the lighting changes (gc_epoch).
// -hwgc N: 0 off (default: see opt11-GEOM.md), 1 on, 2 every hit is calculated again and the two records are compared (HWGC lines: the number of differences must be 0),
// 3 on, and one hit in 16 is checked that way (on a difference the entry is dropped). -hwgcmem KB: the size of the record arena (default GC_DEFAULT_KB).
// The memory is one block of the zone (PU_HWRPLANE), asked with Z_TryMallocAlign: when the zone has no room the cache stays off, nothing else fails.

#define GC_SETS 2048u
#define GC_WAYS 2u
#define GC_DEFAULT_KB 384u
#define GC_SCRATCH (24u << 10) // the record of a hit that is checked (mode 2, 3)
#define GC_MAXREC (12u << 10) // longest record that is kept

extern unsigned int PS2HWD_ScanDirection(void); // ps2_hw_tex.inc: 0 / 1 alternating with the frames

enum { GCOP_TEX = 1, GCOP_POLY = 2, GCOP_TWALL = 3 };

typedef struct { UINT8 op, n, horizon, hashed; UINT16 size, h16; FBITFIELD flags; INT32 shader; UINT32 texid; GLMipmap_t *tex; } gcphdr_t; // 24 bytes, followed by an FSurfaceInfo and n FOutVector
typedef struct { UINT8 op, pad[3]; GLMipmap_t *m; } gctex_t;
typedef struct { UINT8 op, fog; UINT16 size; INT32 texnum; FBITFIELD blend; INT32 lightlevel; extracolormap_t *cmap; FOutVector v[4]; FSurfaceInfo surf; } gctw_t;

typedef struct { UINT32 a, b; } gcfp_t;

typedef struct
{
	UINT32 id, id2, k0, k1, off;
	UINT16 len, gen;
} gcent_t;

static struct
{
	int mode; // -hwgc: (mode & 3) 0 off (default), 1 on, 2 every hit is replayed AND recomputed and compared (check), 3 every 16th hit checked; +4 no plane cache, +8 no wall cache
	boolean mode_set, on, dead;
	UINT32 kb;
	void *blk; // the zone block that holds everything below
	gcent_t *tab;
	gcfp_t *bfp, *ffp; // per sector: the fingerprint of the sector alone / with its 3D floors and light list
	UINT32 *bst, *fst; // ... and the view they were made in
	UINT32 nsec, view, scan;
	UINT8 *ar;
	UINT32 ar_size, ar_used;
	UINT16 gen;
	UINT32 epoch;
	UINT32 rec_start, rec_pos, rec_limit;
	boolean rec_fail, rec_full;
	// statistics of the window (HWPROF32)
	UINT32 s_seg_hit, s_seg_miss, s_seg_skip, s_pl_hit, s_pl_miss, s_pl_skip, s_flush, s_full, s_chk, s_bad, s_rep_ops, s_rec_ops;
	UINT32 c_hit, c_miss, c_key, c_fp, n_fpb, n_fpf; // cycles: replays, calculations with recording, keys, sector fingerprints (inside the keys), their number
	UINT32 reports;
} gc;

static void HWR_AddTransparentWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

// two lanes of six simple operations (no multiplication: ~3 cycles a word): a change of any input changes both lanes with a chance of 1 - 2^-64
static inline void gcw(gcfp_t *h, UINT32 x)
{
	h->a = ((h->a << 5) + h->a) ^ x;
	h->b = (h->b ^ x) + (h->b >> 7) + 0x9e3779b9u;
}

static inline void gcw_fp(gcfp_t *h, const gcfp_t *x)
{
	gcw(h, x->a);
	gcw(h, x->b);
}

static inline void gc_init_fp(gcfp_t *h)
{
	h->a = 0x811c9dc5u;
	h->b = 0x9e3779b9u;
}

static void gc_cmap(gcfp_t *h, const extracolormap_t *c)
{
	if (!c)
	{
		gcw(h, 0x9c3a5d01u);
		return;
	}
	gcw(h, (UINT32)c->rgba);
	gcw(h, (UINT32)c->fadergba);
	gcw(h, (UINT32)c->fadestart | ((UINT32)c->fadeend << 8) | ((UINT32)c->flags << 16) | (c->gl_lighttable.needs_update ? 0x01000000u : 0u));
	gcw(h, c->gl_lighttable.id);
}

static void gc_slope(gcfp_t *h, const pslope_t *s)
{
	if (!s)
	{
		gcw(h, 0x51a9e001u);
		return;
	}
	gcw(h, (UINT32)s->o.x);
	gcw(h, (UINT32)s->o.y);
	gcw(h, (UINT32)s->o.z);
	gcw(h, (UINT32)s->d.x);
	gcw(h, (UINT32)s->d.y);
	gcw(h, (UINT32)s->zdelta);
	gcw(h, (UINT32)s->normal.x);
	gcw(h, (UINT32)s->normal.y);
}

// the fields of a sector the wall and plane builders read (without its 3D floors)
static void gc_sector_base(gcfp_t *h, const sector_t *s)
{
	UINT32 i;

	gcw(h, (UINT32)s->floorheight);
	gcw(h, (UINT32)s->ceilingheight);
	gcw(h, (UINT32)s->floorpic);
	gcw(h, (UINT32)s->ceilingpic);
	gcw(h, (UINT16)s->lightlevel | ((UINT32)(UINT16)s->floorlightlevel << 16));
	gcw(h, (UINT16)s->ceilinglightlevel | (s->floorlightabsolute ? 0x10000u : 0u) | (s->ceilinglightabsolute ? 0x20000u : 0u) | (s->ffloors ? 0x40000u : 0u) | ((UINT32)s->numlights << 20));
	gcw(h, (UINT32)s->floorlightsec);
	gcw(h, (UINT32)s->ceilinglightsec);
	gcw(h, (UINT32)s->floorxoffset);
	gcw(h, (UINT32)s->flooryoffset);
	gcw(h, (UINT32)s->ceilingxoffset);
	gcw(h, (UINT32)s->ceilingyoffset);
	gcw(h, (UINT32)s->floorxscale);
	gcw(h, (UINT32)s->flooryscale);
	gcw(h, (UINT32)s->ceilingxscale);
	gcw(h, (UINT32)s->ceilingyscale);
	gcw(h, (UINT32)s->floorangle);
	gcw(h, (UINT32)s->ceilingangle);
	gcw(h, (UINT32)s->heightsec);
	gcw(h, (UINT32)s->flags);
	gcw(h, s->tags.count);
	for (i = 0; i < s->tags.count; i++)
		gcw(h, (UINT32)s->tags.tags[i]);
	gc_cmap(h, s->extra_colormap);
	gc_slope(h, s->f_slope);
	gc_slope(h, s->c_slope);
}

static void gc_get_fp(const sector_t *s, boolean full, gcfp_t *out);

static void gc_side_mid(gcfp_t *h, const side_t *sd);

static void gc_sector_full(gcfp_t *h, const sector_t *s)
{
	const ffloor_t *r;
	INT32 i;

	gc_sector_base(h, s);
	for (r = s->ffloors; r; r = r->next)
	{
		gcfp_t t;

		// (what the wall and plane builders read of a floor: not the offsets of its planes, those come from the control sector, in `t`)
		gcw(h, (UINT32)r->fofflags);
		gcw(h, (UINT32)*r->topheight);
		gcw(h, (UINT32)*r->bottomheight);
		gcw(h, (UINT32)*r->toppic);
		gcw(h, (UINT32)*r->bottompic);
		gcw(h, (UINT32)(UINT16)*r->toplightlevel);
		gc_slope(h, *r->t_slope);
		gc_slope(h, *r->b_slope);
		gcw(h, (UINT32)r->alpha | ((UINT32)r->blend << 16));
		gcw(h, (UINT32)(uintptr_t)r->master | ((UINT32)(UINT16)r->master->flags << 16));
		gc_get_fp(r->master->frontsector, false, &t); // the control sector: its light and colormap (fog blocks), flat offsets
		gcw_fp(h, &t);
		if (!(r->master->flags & ML_TFERLINE))
			gc_side_mid(h, &sides[r->master->sidenum[0]]); // the side of the control line (R_GetFFloorSide): its texture and offsets
	}
	for (i = 0; i < s->numlights; i++)
	{
		const lightlist_t *l = &s->lightlist[i];

		gcw(h, (UINT32)l->height);
		gcw(h, (UINT16)*l->lightlevel);
		gc_cmap(h, *l->extra_colormap);
		gcw(h, (UINT32)l->flags);
		gcw(h, (UINT32)(uintptr_t)l->caster);
		gc_slope(h, l->slope);
	}
}

// the fingerprint of a sector, once per view for the sectors of the map; a copy (R_FakeFlat) is done every time
static void gc_get_fp(const sector_t *s, boolean full, gcfp_t *out)
{
	const size_t n = (size_t)(s - sectors);

	if (n < gc.nsec)
	{
		if (full)
		{
			if (gc.fst[n] != gc.view)
			{
				gcfp_t h;
				const UINT32 t0 = ps2hwp_now();

				gc_init_fp(&h);
				gc_sector_full(&h, s);
				gc.ffp[n] = h;
				gc.fst[n] = gc.view;
				gc.c_fp += ps2hwp_now() - t0;
				gc.n_fpf++;
			}
			*out = gc.ffp[n];
		}
		else
		{
			if (gc.bst[n] != gc.view)
			{
				gcfp_t h;
				const UINT32 t0 = ps2hwp_now();

				gc_init_fp(&h);
				gc_sector_base(&h, s);
				gc.bfp[n] = h;
				gc.bst[n] = gc.view;
				gc.c_fp += ps2hwp_now() - t0;
				gc.n_fpb++;
			}
			*out = gc.bfp[n];
		}
		return;
	}
	gc_init_fp(out);
	if (full)
		gc_sector_full(out, s);
	else
		gc_sector_base(out, s);
}

#if defined(SIDE_UDMF)
#define GC_SIDE_UDMF(sd) 1
#elif defined(HAS_UDMF) || defined(HAS_LUA)
#define GC_SIDE_UDMF(sd) (sideudmf != NULL) // (the sides have the defaults until a map or a script sets one)
#else
#define GC_SIDE_UDMF(sd) 0
#endif

static void gc_side_mid(gcfp_t *h, const side_t *sd)
{
	gcw(h, (UINT32)sd->textureoffset);
	gcw(h, (UINT32)sd->rowoffset);
	gcw(h, (UINT32)R_GetTextureNum(sd->midtexture));
	if (GC_SIDE_UDMF(sd))
	{
		gcw(h, (UINT32)SIDE_OFFSETX_MID(sd));
		gcw(h, (UINT32)SIDE_OFFSETY_MID(sd));
		gcw(h, (UINT32)SIDE_SCALEX_MID(sd));
		gcw(h, (UINT32)SIDE_SCALEY_MID(sd));
		gcw(h, (UINT16)SIDE_LIGHT(sd) | (SIDE_LIGHTABSOLUTE(sd) ? 0x10000u : 0u));
	}
}

// the fingerprint of the input of HWR_ProcessSeg
static void gc_seg_key(const seg_t *sg, const sector_t *fs, const sector_t *bs, gcfp_t *k)
{
	const side_t *sd = sg->sidedef;
	const line_t *ld = sg->linedef;
	gcfp_t t;
	INT32 tt, bt, mt;

	gc_init_fp(k);
	gc_get_fp(fs, true, &t);
	gcw_fp(k, &t);
	if (bs)
	{
		gc_get_fp(bs, true, &t);
		gcw_fp(k, &t);
	}
	gcw(k, (bothceilingssky ? 1u : 0u) | (bothfloorssky ? 2u : 0u) | ((UINT32)(UINT16)ld->flags << 8) | ((UINT32)(UINT16)ld->special << 16));
	gcw(k, (UINT32)ld->alpha);
	gcw(k, (UINT32)ld->blendmode);
	tt = R_GetTextureNum(sd->toptexture);
	bt = R_GetTextureNum(sd->bottomtexture);
	mt = R_GetTextureNum(sd->midtexture);
	gcw(k, (UINT32)tt | (tt ? HWR_PS2_TexTransparent(tt) << 31 : 0u));
	gcw(k, (UINT32)bt | (bt ? HWR_PS2_TexTransparent(bt) << 31 : 0u));
	gcw(k, (UINT32)mt | (mt ? HWR_PS2_TexTransparent(mt) << 31 : 0u));
	gcw(k, (UINT32)sd->textureoffset);
	gcw(k, (UINT32)sd->rowoffset);
	gcw(k, (UINT32)sd->repeatcnt);
	if (GC_SIDE_UDMF(sd))
	{
		gcw(k, (UINT32)SIDE_OFFSETX_TOP(sd));
		gcw(k, (UINT32)SIDE_OFFSETX_MID(sd));
		gcw(k, (UINT32)SIDE_OFFSETX_BOTTOM(sd));
		gcw(k, (UINT32)SIDE_OFFSETY_TOP(sd));
		gcw(k, (UINT32)SIDE_OFFSETY_MID(sd));
		gcw(k, (UINT32)SIDE_OFFSETY_BOTTOM(sd));
		gcw(k, (UINT32)SIDE_SCALEX_TOP(sd));
		gcw(k, (UINT32)SIDE_SCALEX_MID(sd));
		gcw(k, (UINT32)SIDE_SCALEX_BOTTOM(sd));
		gcw(k, (UINT32)SIDE_SCALEY_TOP(sd));
		gcw(k, (UINT32)SIDE_SCALEY_MID(sd));
		gcw(k, (UINT32)SIDE_SCALEY_BOTTOM(sd));
		gcw(k, (UINT16)SIDE_LIGHT(sd) | (SIDE_LIGHTABSOLUTE(sd) ? 0x10000u : 0u));
	}
	if (bs && (bs->ffloors || fs->ffloors))
	{
		// the 3D floor loops of HWR_ProcessSeg read the side of the control linedef of every floor of both sectors; where that depends on the seg's own
		// line (ML_TFERLINE) it is here, the other sides are in the fingerprint of the sector
		const ffloor_t *r;
		int pass;

		for (pass = 0; pass < 2; pass++)
			for (r = pass ? fs->ffloors : bs->ffloors; r; r = r->next)
				if (r->master->flags & ML_TFERLINE)
				{
					gc_side_mid(k, R_GetFFloorSide(ld, r, bs));
					gcw(k, (UINT16)R_GetFFloorLine(ld, r, bs)->flags);
				}
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// tables

static void gc_flush_arena(void)
{
	gc.ar_used = 0;
	if (++gc.gen == 0) // wrapped: the entries of 65535 generations ago would match again
	{
		if (gc.tab)
			memset(gc.tab, 0, (size_t)GC_SETS * GC_WAYS * sizeof(gcent_t));
		gc.gen = 1;
	}
}

void HWR_GCacheFlush(void)
{
	if (gc.blk)
		gc_flush_arena();
	gc.s_flush++;
}

// a new level: the zone has freed the block (PU_HWRPLANE), all pointers into it are gone
static void HWR_GCacheLevel(void)
{
	gc.dead = false;
	gc.blk = NULL;
	gc.on = false;
}

// -hwgo N: bits of the OPT11 geometry micro-optimisations that are switched OFF (A/B on one ELF): 1 AddLine angle reuse (PS2-HW-81), 4 QuadHidden two-corner exit (PS2-HW-86),
// 8 HWR_Lighting fast path (PS2-HW-83), 16 HWR_CalcWallLight memo (PS2-HW-84), 32 RenderBatches shader/scan-direction cache (PS2-HW-85)
UINT32 hwr_geo_off;

static void HWR_GCacheParse(void)
{
	if (M_CheckParm("-hwgo") && M_IsNextParm())
		hwr_geo_off = (UINT32)atoi(M_GetNextParm());
	gc.mode_set = true;
	gc.mode = 0; // OFF by default: the cache of a hit costs about what the calculation does (measured, docs/GATES/g1/opt11-GEOM.md)
	gc.kb = GC_DEFAULT_KB;
	if (M_CheckParm("-hwgc") && M_IsNextParm())
		gc.mode = atoi(M_GetNextParm());
	if (M_CheckParm("-hwgcmem") && M_IsNextParm())
	{
		INT32 kb = atoi(M_GetNextParm());

		gc.kb = kb < 64 ? 64u : kb > 4096 ? 4096u : (UINT32)kb;
	}
}

static boolean gc_alloc_block(void)
{
	size_t tab_b = (size_t)GC_SETS * GC_WAYS * sizeof(gcent_t);
	size_t sec_b = (size_t)numsectors * (2 * sizeof(gcfp_t) + 2 * sizeof(UINT32));
	size_t ar_b = (size_t)gc.kb * 1024u + GC_SCRATCH;
	size_t total = tab_b + sec_b + ar_b + 64;
	UINT8 *p;

	// the cache is a luxury: only when the arena has room for it and still has what the level needs after it
#ifdef PS2
	if (Z_ArenaFree() < total + Z_RenderHeadroom() + (1u << 20))
		return false;
#endif
	p = Z_TryMallocAlign(total, PU_HWRPLANE, &gc.blk, 4);
	if (!p)
	{
		gc.blk = NULL;
		return false;
	}
	gc.tab = (gcent_t *)p;
	p += tab_b;
	gc.bfp = (gcfp_t *)p;
	p += (size_t)numsectors * sizeof(gcfp_t);
	gc.ffp = (gcfp_t *)p;
	p += (size_t)numsectors * sizeof(gcfp_t);
	gc.bst = (UINT32 *)p;
	p += (size_t)numsectors * sizeof(UINT32);
	gc.fst = (UINT32 *)p;
	p += (size_t)numsectors * sizeof(UINT32);
	gc.ar = p;
	gc.ar_size = gc.kb * 1024u;
	gc.ar_used = 0;
	gc.nsec = (UINT32)numsectors;
	memset(gc.tab, 0, tab_b);
	memset(gc.bst, 0, (size_t)numsectors * sizeof(UINT32));
	memset(gc.fst, 0, (size_t)numsectors * sizeof(UINT32));
	gc.gen = 1;
	gc.view = 0;
	gc.epoch = 0;
	return true;
}

// called once per view before the BSP walk: the settings the cached surfaces depend on, the view stamp; false: no cache
static boolean HWR_GCacheFrame(void)
{
	UINT32 sig;
	const float *cp = &Cubepal[0][0][0][0];
	int i;

	if (!gc.mode_set)
		HWR_GCacheParse();
	gc.on = false;
	if ((gc.mode & 3) == 0 || gc.dead)
		return false;
	if (!gc.blk && !gc_alloc_block())
	{
		gc.dead = true; // no room (until the next level)
		return false;
	}
	sig = (UINT32)cv_secbright.value * 2654435761u;
	sig ^= (UINT32)cv_glshaders.value << 3;
	sig ^= (UINT32)cv_glallowshaders.value << 4;
	sig ^= (UINT32)gl_shadersavailable << 5;
	sig ^= (UINT32)HWR_ShouldUsePaletteRendering() << 7;
	sig ^= (UINT32)Cubeapply << 9;
	sig ^= (UINT32)cv_glfakecontrast.value << 11;
	sig ^= (UINT32)cv_glslopecontrast.value << 14;
	sig ^= (UINT32)r_renderwalls << 17;
	sig ^= (UINT32)r_renderfloors << 18;
	sig ^= (UINT32)skyflatnum << 19;
	sig ^= (UINT32)HWR_UseShader() << 31;
	if (Cubeapply)
		for (i = 0; i < 24; i++)
		{
			union { float f; UINT32 u; } c;

			c.f = cp[i];
			sig = (sig << 5) + sig + c.u;
		}
	if (sig != gc.epoch)
	{
		if (gc.epoch)
			gc_flush_arena();
		gc.epoch = sig;
	}
	gc.view++;
	gc.scan = PS2HWD_ScanDirection();
	gc.on = true;
	return true;
}

static gcent_t *gc_find(UINT32 id, UINT32 id2, boolean *hit)
{
	UINT32 h = id * 2654435761u + (id2 >> 3) * 40503u;
	gcent_t *set, *victim;
	UINT32 w;

	h = (h ^ (h >> 15)) * 2246822519u;
	set = &gc.tab[((h >> 8) & (GC_SETS - 1)) * GC_WAYS];
	for (w = 0; w < GC_WAYS; w++)
		if (set[w].gen == gc.gen && set[w].id == id && set[w].id2 == id2)
		{
			*hit = true;
			return &set[w];
		}
	*hit = false;
	victim = &set[0];
	for (w = 0; w < GC_WAYS; w++)
	{
		if (set[w].gen != gc.gen)
			return &set[w];
		if (set[w].off < victim->off)
			victim = &set[w]; // the older record (the arena is written in order)
	}
	return victim;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// recording (the sinks call these while hwr_grec_on)

static void *gc_rec_alloc(UINT32 bytes)
{
	void *p;

	if (gc.rec_fail)
		return NULL;
	if (gc.rec_pos + bytes > gc.rec_limit)
		gc.rec_full = true; // the arena has no room: it is started again
	if (gc.rec_full || gc.rec_pos + bytes - gc.rec_start > GC_MAXREC)
	{
		gc.rec_fail = true;
		return NULL;
	}
	p = gc.ar + gc.rec_pos;
	gc.rec_pos += bytes;
	return p;
}

void HWR_GCRecPoly(const FSurfaceInfo *surf, const FOutVector *verts, FUINT n, FBITFIELD flags, int shader, boolean horizon)
{
	const UINT32 size = (UINT32)(sizeof(gcphdr_t) + sizeof(FSurfaceInfo) + n * sizeof(FOutVector));
	gcphdr_t *h;

	if (n > 250)
	{
		gc.rec_fail = true;
		return;
	}
	if (!(h = gc_rec_alloc(size)))
		return;
	h->op = GCOP_POLY;
	h->n = (UINT8)n;
	h->horizon = horizon ? 1 : 0;
	h->size = (UINT16)size;
	h->flags = flags;
	h->shader = shader;
	h->tex = current_texture; // (what HWR_ProcessPolygon takes: the sort key depends on it)
	h->texid = HWR_GCTexId(current_texture);
	h->hashed = (!(flags & PF_NoTexture) && !horizon) ? 1 : 0;
	h->h16 = (UINT16)(h->hashed ? HWR_GCPolyHash(current_texture, surf, flags, shader) : 0);
	memcpy(h + 1, surf, sizeof(FSurfaceInfo));
	((FSurfaceInfo *)(h + 1))->PolyFlags = 0; // (stack garbage in most callers; only the model code of the driver reads it)
	memcpy((UINT8 *)(h + 1) + sizeof(FSurfaceInfo), verts, n * sizeof(FOutVector));
	gc.s_rec_ops++;
}

void HWR_GCRecTex(GLMipmap_t *texture)
{
	gctex_t *t = gc_rec_alloc(sizeof(gctex_t));

	if (!t)
		return;
	t->op = GCOP_TEX;
	t->pad[0] = t->pad[1] = t->pad[2] = 0;
	t->m = texture;
	gc.s_rec_ops++;
}

static void HWR_GCRecTWall(const FOutVector *wallVerts, const FSurfaceInfo *pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	gctw_t *w = gc_rec_alloc(sizeof(gctw_t));

	if (!w)
		return;
	w->op = GCOP_TWALL;
	w->fog = fogwall ? 1 : 0;
	w->size = (UINT16)sizeof(gctw_t);
	w->texnum = texnum;
	w->blend = blend;
	w->lightlevel = lightlevel;
	w->cmap = wallcolormap;
	memcpy(w->v, wallVerts, sizeof w->v);
	// the surface of a translucent wall is not lit yet (HWR_RenderWall does it after the sort): only the alpha means something, the rest is stack garbage
	memset(&w->surf, 0, sizeof w->surf);
	w->surf.PolyColor.s.alpha = pSurf->PolyColor.s.alpha;
	gc.s_rec_ops++;
}

static void gc_rec_begin(boolean scratch)
{
	gc.rec_start = gc.rec_pos = scratch ? gc.ar_size : gc.ar_used;
	gc.rec_limit = scratch ? gc.ar_size + GC_SCRATCH : gc.ar_size;
	gc.rec_fail = gc.rec_full = false;
	hwr_grec_on = true;
}

// the records of what the function did: made again
static void gc_replay(const UINT8 *p, UINT32 len)
{
	const UINT8 *end = p + len;

	while (p < end)
	{
		switch (*p)
		{
			case GCOP_TEX:
			{
				GLMipmap_t *m = ((const gctex_t *)p)->m;

				if (m && m->ps2_gcv != gc.view)
				{
					m->ps2_gcv = gc.view; // once a view is enough: the driver only stamps the texture with the frame, the zone the block
					HWR_SetCurrentTexture(m); // as HWR_GetTexture / HWR_GetLevelFlat do (and then:)
					Z_ChangeTag(m->data, PU_HWRCACHE_UNLOCKED);
				}
				else
					current_texture = m;
				p += sizeof(gctex_t);
				break;
			}
			case GCOP_POLY:
			{
				const gcphdr_t *h = (const gcphdr_t *)p;
				FSurfaceInfo *sf = (FSurfaceInfo *)(h + 1);
				FOutVector *vv = (FOutVector *)((UINT8 *)(h + 1) + sizeof(FSurfaceInfo));

				if (current_texture == h->tex)
					HWR_GCReplayPoly(sf, vv, h->n, h->flags, h->shader, h->horizon != 0, h->texid, h->h16, gc.scan);
				else
					HWR_ProcessPolygon(sf, vv, h->n, h->flags, h->shader, h->horizon != 0); // (the texture of the caller is not the one of the record: the long way)
				p += h->size;
				break;
			}
			case GCOP_TWALL:
			{
				const gctw_t *w = (const gctw_t *)p;

				HWR_AddTransparentWall((FOutVector *)w->v, (FSurfaceInfo *)&w->surf, w->texnum, w->blend, w->fog != 0, w->lightlevel, w->cmap);
				p += w->size;
				break;
			}
			default:
				return; // (cannot happen: the records are written by the functions above)
		}
		gc.s_rep_ops++;
	}
}

static void gc_report_diff(const char *kind, UINT32 idx, const UINT8 *a, UINT32 alen, const UINT8 *b, UINT32 blen)
{
	UINT32 i, n = alen < blen ? alen : blen;

	gc.s_bad++;
	if (gc.reports++ >= 24)
		return;
	for (i = 0; i < n && a[i] == b[i]; i++)
		;
	I_OutputMsg("HWGC MISMATCH %s %u: cached %u bytes, calculated %u bytes, first difference at %u (op %u)\n", kind, (unsigned)idx, (unsigned)alen, (unsigned)blen, (unsigned)i, (unsigned)(i < n ? a[i] : 0));
	if (gc.reports <= 6)
	{
		// decode: which op, which vertex, which field differs
		UINT32 o = 0, k;

		while (o < n && o + sizeof(gcphdr_t) <= n)
		{
			const UINT32 sz = a[o] == GCOP_TEX ? (UINT32)sizeof(gctex_t) : a[o] == GCOP_TWALL ? (UINT32)sizeof(gctw_t) : ((const gcphdr_t *)(a + o))->size;

			if (i >= o && i < o + sz)
			{
				const UINT32 rel = i - o;

				if (a[o] == GCOP_POLY && rel >= sizeof(gcphdr_t) + sizeof(FSurfaceInfo))
				{
					const UINT32 vo = rel - (UINT32)(sizeof(gcphdr_t) + sizeof(FSurfaceInfo));
					const FOutVector *va = (const FOutVector *)(a + o + sizeof(gcphdr_t) + sizeof(FSurfaceInfo)), *vb = (const FOutVector *)(b + o + sizeof(gcphdr_t) + sizeof(FSurfaceInfo));
					const UINT32 nv = ((const gcphdr_t *)(a + o))->n;

					I_OutputMsg("  in op at %u (poly, %u vertices, flags %x): vertex %u field %u (x y z s t)\n", (unsigned)o, (unsigned)nv, (unsigned)((const gcphdr_t *)(a + o))->flags, (unsigned)(vo / 20), (unsigned)((vo % 20) / 4));
					for (k = 0; k < nv; k++)
						I_OutputMsg("    v%u cached %.4f %.4f %.4f %.6f %.6f | now %.4f %.4f %.4f %.6f %.6f\n", (unsigned)k, va[k].x, va[k].y, va[k].z, va[k].s, va[k].t, vb[k].x, vb[k].y, vb[k].z, vb[k].s, vb[k].t);
				}
				else
					I_OutputMsg("  in op at %u (type %u) offset %u\n", (unsigned)o, (unsigned)a[o], (unsigned)rel);
				break;
			}
			o += sz;
		}
	}
	if (gc.reports <= 8 && i < n)
	{
		const UINT32 from = i & ~3u;
		UINT32 w;

		for (w = from; w < from + 16 && w + 4 <= n; w += 4)
			I_OutputMsg("  +%u: cached %08x calculated %08x\n", (unsigned)w, (unsigned)*(const UINT32 *)(a + w), (unsigned)*(const UINT32 *)(b + w));
	}
}

// after the function ran with the record on: keep it (miss), or compare it with the entry (check)
static void gc_rec_end(gcent_t *e, UINT32 id, UINT32 id2, const gcfp_t *key, boolean check, const char *kind, UINT32 idx)
{
	hwr_grec_on = false;
	if (gc.rec_fail)
	{
		if (!check)
		{
			if (gc.rec_full)
			{
				gc.s_full++;
				gc_flush_arena(); // the arena is full: what it holds goes (one pass of misses); a record that is too long is just not kept
			}
			e->gen = 0; // (not kept)
		}
		return;
	}
	if (check)
	{
		const UINT32 len = gc.rec_pos - gc.rec_start;

		gc.s_chk++;
		if (len != e->len || memcmp(gc.ar + gc.rec_start, gc.ar + e->off, len))
		{
			gc_report_diff(kind, idx, gc.ar + e->off, e->len, gc.ar + gc.rec_start, len);
			e->gen = 0; // dropped: calculated again next time
		}
		return;
	}
	e->id = id;
	e->id2 = id2;
	e->k0 = key->a;
	e->k1 = key->b;
	e->off = gc.rec_start;
	e->len = (UINT16)(gc.rec_pos - gc.rec_start);
	e->gen = gc.gen;
	gc.ar_used = gc.rec_pos;
}

static inline boolean gc_check_this(void)
{
	static UINT32 ctr;

	if ((gc.mode & 3) == 2)
		return true;
	return (gc.mode & 3) == 3 && (++ctr & 15u) == 0;
}


// the HWPROF32 line of the window (called by PS2HWD_ProfExtra, ps2_hw_hg.inc, which resets the other counters)
void HWR_GCacheProf(unsigned int frames)
{
	if (!frames)
		frames = 1;
	I_OutputMsg("HWPROF32 gcache mode=%d on=%d seg hit=%u miss=%u skip=%u | plane hit=%u miss=%u skip=%u | ops replayed=%u recorded=%u | flush=%u full=%u | check=%u bad=%u | cycles key=%u (sector fp %u: %u base %u full) hit=%u miss=%u | arena=%u/%u\n",
		gc.mode, gc.blk ? 1 : 0, gc.s_seg_hit / frames, gc.s_seg_miss / frames, gc.s_seg_skip / frames, gc.s_pl_hit / frames, gc.s_pl_miss / frames, gc.s_pl_skip / frames, gc.s_rep_ops / frames, gc.s_rec_ops / frames,
		gc.s_flush, gc.s_full, gc.s_chk, gc.s_bad, gc.c_key / frames, gc.c_fp / frames, gc.n_fpb / frames, gc.n_fpf / frames, gc.c_hit / frames, gc.c_miss / frames, (unsigned)(gc.ar_used >> 10), (unsigned)(gc.ar_size >> 10));
	gc.s_seg_hit = gc.s_seg_miss = gc.s_seg_skip = gc.s_pl_hit = gc.s_pl_miss = gc.s_pl_skip = gc.s_flush = gc.s_full = gc.s_chk = gc.s_bad = gc.s_rep_ops = gc.s_rec_ops = 0;
	gc.c_hit = gc.c_miss = gc.c_key = gc.c_fp = gc.n_fpb = gc.n_fpf = 0;
}
