// OPT11 (GEOM, GEOM2), PS2-HW-80, 201..204: the geometry cache of the BSP walk. Included by hw_main.c (PS2 profile only), after HWR_CeilingLightLevel.
//
// What the BSP walk makes of a seg (HWR_ProcessSeg: the wall polygons, the sky walls, the translucent walls, the lighting of each) and of a floor or
// ceiling (HWR_RenderPlane) is a function of the map state (heights, flats, light, colormaps, side textures and offsets, 3D floors, slopes ...) and of
// some settings, and NOT of the view: on a map that is standing still the same polygons come out of the same functions at 1000 .. 2000 cycles each,
// frame after frame. The cache keeps the last result of every seg (and, with -hwgc 1, plane):
//  * the KEY of a record is a list of plain words, the inputs of the function: for a sector of the map ONE word, its version (a number that changes when the
//    fingerprint of the sector - the fields the builders read, its 3D floors, its light list - changes; the fingerprint is made once per view), for a sector the
//    engine made up for the frame (R_FakeFlat: water) its fingerprint, the side and line words, the translated texture numbers (texture animation), the TF_TRANSPARENT
//    state of the textures; for a plane the slope and flat offsets it reads and its arguments. One function writes the key when the record is made and compares it
//    word by word when it is looked up (gc_seg_words, gc_plane_words), so they cannot disagree, and a compare is exact (no hash collisions).
//  * the OUTPUT is the sequence of calls that the function made to the three sinks of the walk (HWR_SetCurrentTexture, HWR_ProcessPolygon,
//    HWR_AddTransparentWall): recorded while the function runs (hwr_grec_on, hw_batching.c) into an arena, and made again by gc_replay when the
//    key of the next call is the same. The batch gets the same polygons in the same order (-hwpolyhash: the same stream, bit for bit).
//  * segs of polyobjects, planes of a subsector with a horizon line (they hold the view position), polygons of more than 250 vertices and records
//    longer than GC_MAXREC are not cached. The records are dropped when the level changes, when a texture record or a light table goes away
//    (HWR_GCacheFlush) and when a setting of the lighting changes (gc.epoch).
//  * the arena is two halves: the current one is written; when it is full the one before is dropped and the full one becomes the old one; a hit in the old half copies the
//    record to the current one (gc_promote), so what is used survives and what is not goes (the first version dropped all at once: a miss of the whole frame every few frames).
//  * the memory is one block of the zone (PU_HWRPLANE), asked with Z_TryMallocAlign when the zone has room for it and for the renderer; the zone can take it back
//    (Z_AddReclaimHook -> HWR_GCacheReclaim: the cache is gone until the next level, what ran is not affected: gc_rec_end writes nothing without the block, and the room a replay needs
//    in the batch arrays is made BEFORE it starts, HWR_GCReserve).
// -hwgc N: 0 off, 1 on with the planes, 5 (default) on, walls only, 2 every hit is calculated again and the two records are compared (HWGC lines: the number of differences must be
// 0), 3 on, and one hit in 16 is checked that way (on a difference the entry is dropped). +4 no planes, +8 no walls, +16 report the sectors that change. -hwgcmem KB: both halves of the arena
// (default GC_DEFAULT_KB), -hwgcsets N: the number of sets of the table (2 ways), -hwgctest R,S: the zone takes the memory back at the R-th polygon recorded / the S-th reserve (a test).
// HWPROF32 (counts and cycles of the window), HWPROF39 (misses of segs never seen / seen with another key, cycles of the replay), HWPROF50 (which word of the key differed), HWPROF51 (misses of segs by the cycles they took).

#define GC_WAYS 2u
#define GC_WINDOW 24u // views of the window in which the hit rate is looked at
#define GC_BYPASS_MIN 16u
#define GC_BYPASS_MAX 512u
#define GC_DEFAULT_KB 640u // both halves of the arena
#define GC_SCRATCH (24u << 10) // the record of a hit that is checked (mode 2, 3)
#define GC_MAXREC (12u << 10) // longest record that is kept

extern unsigned int PS2HWD_ScanDirection(void); // ps2_hw_tex.inc: 0 / 1 alternating with the frames

enum { GCOP_POLY = 2, GCOP_TWALL = 3 }; // (the texture ops of the first versions are gone: the entry of a polygon holds its texture, the header of the record the texture the function ended with)

typedef struct { UINT8 op, fog; UINT16 size; INT32 texnum; FBITFIELD blend; INT32 lightlevel; extracolormap_t *cmap; FOutVector v[4]; FSurfaceInfo surf; } gctw_t;

typedef struct { UINT32 a, b; } gcfp_t;

typedef struct
{
	UINT32 off;
	UINT16 len, gen;
} gcent_t; // 8 bytes: where the record of a seg or plane is (an offset in the arena), how long it is, the generation of the half it was made in (0: none)

typedef struct
{
	UINT32 id, id2;
	gcent_t e;
} gchent_t; // the hashed table (the planes of 3D floors: their id is the subsector, the side of the plane and the control sector); segs and the planes of the sectors themselves have a place of their own

// a record in the arena: this header, keyw words of key (the inputs the result was made from; compared word by word with the live ones), then the ops
typedef struct { UINT16 keyw, opsoff, npoly, nvert, nwall, flags; GLMipmap_t *last; } gcrh_t; // (npoly, nvert, nwall: what the replay adds to the batch arrays and the list of transparent walls; flags bit 0: the function set the texture, `last` is the one it ended with = current_texture after the replay)
#define GC_KEYMAX 80u

static struct
{
	int mode; // -hwgc: (mode & 3) 0 off (default), 1 on, 2 every hit is replayed AND recomputed and compared (check), 3 every 16th hit checked; +4 no plane cache, +8 no wall cache
	boolean mode_set, on, dead;
	UINT32 kb;
	void *blk; // the zone block that holds everything below
	size_t blk_bytes;
	gchent_t *tab; // hashed (planes of 3D floors)
	gcent_t *segent, *plent; // by the number of the seg / by 2 * the number of the subsector + (ceiling ? 1 : 0)
	UINT32 nsegent, nplent;
	gcfp_t *bfp, *ffp; // per sector: the fingerprint of the sector alone / with its 3D floors and light list
	UINT32 *bst, *fst; // ... and the view they were made in
	angle_t *vang; // per vertex of the map: R_PointToAngle64 of it in the view vst says (HWR_VertAngle: a vertex belongs to the segs of several subsectors and to both sides of its lines)
	UINT32 *vst;
	size_t nvert;
	UINT32 *bver, *fver; // ... and a number that changes when the fingerprint does (what the keys of segs and planes hold: one word for a sector)
	UINT32 vctr;
	UINT32 nsec, view, scan;
	UINT8 *ar;
	UINT32 ar_size, ar_used; // ar_size: both halves; ar_used: the next free byte of the current half (an offset from ar)
	UINT32 hsize, hbase; // the arena is two halves: the current one is written, the one before is still read (and what is read there is copied to the current one: gc_promote)
	UINT16 gen;
	UINT32 epoch;
	UINT32 rec_start, rec_pos, rec_limit, rec_np, rec_nv, rec_nw;
	boolean rec_fail, rec_full, rec_lastset;
	GLMipmap_t *rec_last;
	// statistics of the window (HWPROF32)
	UINT32 s_seg_hit, s_seg_miss, s_seg_skip, s_pl_hit, s_pl_miss, s_pl_skip, s_flush, s_full, s_chk, s_bad, s_rep_ops, s_rec_ops;
	UINT32 c_hit, c_miss, c_key, c_fp, n_fpb, n_fpf; // cycles: replays, calculations with recording, keys, sector fingerprints (inside the keys), their number
	UINT32 ch[10], cl[10]; // misses of segs by the cycles the calculation took (buckets of 512) and the bytes of their records
	UINT32 bypass, bypass_len, w_views, w_q, w_hit, s_bypass; // adaptive: a window of views in which fewer than a third of the queries hit puts the cache out of use for a while (bypass views), longer every time
	UINT32 sdbg, nsets, nsets_user, s_reclaim, s_promo, test_rec, test_res, test_n1, test_n2; // -hwgctest R,S: the zone takes the memory back at the R-th polygon recorded / the S-th HWR_GCReserve (tests of the reclaim)
	UINT32 mmpos, mm[34], mmp[34]; // the position of the first word of a key that differed (1 = the first), histograms of the segs and of the planes
	UINT32 s_new, s_stale, c_rpoly, c_rtex, n_rpoly, n_rtex; // misses of a seg or plane never seen / seen with another key; cycles and number of the polygon and texture ops of the replays
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
	// (not the flat offsets, scales and angles: the wall builder does not read them, and scrolling flats would make every wall of the sector miss;
	// the plane key has the ones its plane uses, gc_plane_words)
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

// the fingerprint of a sector of the map is made once per view (a stamp per sector); the version of the sector is a number that changes when the fingerprint does:
// the keys of the segs and planes hold the version (one word) and not the fingerprint. A sector the engine fakes (R_FakeFlat: a copy) is done every time.
static void gc_refresh(size_t n, const sector_t *s, boolean full)
{
	gcfp_t h;
	const UINT32 t0 = ps2hwp_now();

	gc_init_fp(&h);
	if (full)
	{
		gc_sector_full(&h, s);
		if (h.a != gc.ffp[n].a || h.b != gc.ffp[n].b)
		{
			if ((gc.mode & 16) && gc.fver[n] && gc.sdbg++ < 60)
			{
				gcfp_t b;

				gc_init_fp(&b);
				gc_sector_base(&b, s);
				I_OutputMsg("HWGC sector %u changed (view %u): base part %s | floor %d ceil %d light %d ffloors %d lights %d floorpic %d ceilpic %d\n", (unsigned)n, (unsigned)gc.view, (b.a != gc.bfp[n].a || b.b != gc.bfp[n].b) ? "CHANGED" : "same",
					(int)(s->floorheight >> 16), (int)(s->ceilingheight >> 16), (int)s->lightlevel, s->ffloors ? 1 : 0, (int)s->numlights, (int)s->floorpic, (int)s->ceilingpic);
			}
			gc.ffp[n] = h;
			gc.fver[n] = ++gc.vctr;
		}
		gc.fst[n] = gc.view;
		gc.n_fpf++;
	}
	else
	{
		gc_sector_base(&h, s);
		if (h.a != gc.bfp[n].a || h.b != gc.bfp[n].b)
		{
			gc.bfp[n] = h;
			gc.bver[n] = ++gc.vctr;
		}
		gc.bst[n] = gc.view;
		gc.n_fpb++;
	}
	gc.c_fp += ps2hwp_now() - t0;
}

static inline UINT32 gc_ver(size_t n, const sector_t *s, boolean full)
{
	if (full)
	{
		if (gc.fst[n] != gc.view)
			gc_refresh(n, s, true);
		return gc.fver[n];
	}
	if (gc.bst[n] != gc.view)
		gc_refresh(n, s, false);
	return gc.bver[n];
}

static void gc_get_fp(const sector_t *s, boolean full, gcfp_t *out)
{
	const size_t n = (size_t)(s - sectors);

	if (n < gc.nsec)
	{
		gc_ver(n, s, full);
		*out = full ? gc.ffp[n] : gc.bfp[n];
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

// The KEY of a record: the words below, in this order. Recording (rec) stores them, checking compares them with the live values and stops at the first difference (-1).
// One function for both, so the two cannot disagree; the input is the same as the fingerprint of round 1 (every field the builders read), but compared as plain words:
// no hashing, no collisions, and a hit costs one load and one compare for each word.
#define GCW(x) do { const UINT32 w_ = (UINT32)(x); if (rec) { if (kp >= kend) return -1; *kp++ = w_; } else if (*kp++ != w_) { gc.mmpos = (UINT32)(kp - kbase); return -1; } } while (0)
// a sector of the map is its version; a sector the engine made up for the frame (a copy) is its fingerprint, made now
#define GC_SECW(sc) do { const sector_t *sc_ = (sc); const size_t n_ = (size_t)(sc_ - sectors); \
		if (n_ < gc.nsec) GCW(gc_ver(n_, sc_, true)); \
		else { gcfp_t t_; gc_init_fp(&t_); gc_sector_full(&t_, sc_); GCW(t_.a); GCW(t_.b); } } while (0)
#define GC_SIDEMIDW(sd_) do { const side_t *sdm_ = (sd_); \
		GCW(sdm_->textureoffset); GCW(sdm_->rowoffset); GCW(R_GetTextureNum(sdm_->midtexture)); \
		if (GC_SIDE_UDMF(sdm_)) { GCW(SIDE_OFFSETX_MID(sdm_)); GCW(SIDE_OFFSETY_MID(sdm_)); GCW(SIDE_SCALEX_MID(sdm_)); GCW(SIDE_SCALEY_MID(sdm_)); \
			GCW((UINT16)SIDE_LIGHT(sdm_) | (SIDE_LIGHTABSOLUTE(sdm_) ? 0x10000u : 0u)); } } while (0)

static inline __attribute__((always_inline)) int gc_seg_words(const seg_t *sg, const sector_t *fs, const sector_t *bs, UINT32 *kbase, const boolean rec)
{
	UINT32 *kp = kbase;
	UINT32 *const kend = kbase + GC_KEYMAX;
	const side_t *sd = sg->sidedef;
	const line_t *ld = sg->linedef;

	GC_SECW(fs);
	if (bs)
		GC_SECW(bs);
	GCW((bothceilingssky ? 1u : 0u) | (bothfloorssky ? 2u : 0u) | ((UINT32)(UINT16)ld->flags << 8) | ((UINT32)(UINT16)ld->special << 16));
	GCW(ld->alpha);
	GCW(ld->blendmode);
	GCW(HWR_PS2_SideTexWord(sd->toptexture));
	GCW(HWR_PS2_SideTexWord(sd->bottomtexture));
	GCW(HWR_PS2_SideTexWord(sd->midtexture));
	GCW(sd->textureoffset);
	GCW(sd->rowoffset);
	GCW(sd->repeatcnt);
	if (GC_SIDE_UDMF(sd))
	{
		GCW(SIDE_OFFSETX_TOP(sd));
		GCW(SIDE_OFFSETX_MID(sd));
		GCW(SIDE_OFFSETX_BOTTOM(sd));
		GCW(SIDE_OFFSETY_TOP(sd));
		GCW(SIDE_OFFSETY_MID(sd));
		GCW(SIDE_OFFSETY_BOTTOM(sd));
		GCW(SIDE_SCALEX_TOP(sd));
		GCW(SIDE_SCALEX_MID(sd));
		GCW(SIDE_SCALEX_BOTTOM(sd));
		GCW(SIDE_SCALEY_TOP(sd));
		GCW(SIDE_SCALEY_MID(sd));
		GCW(SIDE_SCALEY_BOTTOM(sd));
		GCW((UINT16)SIDE_LIGHT(sd) | (SIDE_LIGHTABSOLUTE(sd) ? 0x10000u : 0u));
	}
	if (bs && (bs->ffloors || fs->ffloors))
	{
		// the 3D floor loops of HWR_ProcessSeg read the side of the control linedef of every floor of both sectors; where that depends on the seg's own
		// line (ML_TFERLINE) it is here, the other sides are in the version of the sector
		const ffloor_t *r;
		int pass;

		for (pass = 0; pass < 2; pass++)
			for (r = pass ? fs->ffloors : bs->ffloors; r; r = r->next)
				if (r->master->flags & ML_TFERLINE)
				{
					GC_SIDEMIDW(R_GetFFloorSide(ld, r, bs));
					GCW((UINT16)R_GetFFloorLine(ld, r, bs)->flags);
				}
	}
	return (int)(kp - kbase);
}

// the key of a plane: everything HWR_RenderPlane reads of the sector (the slope of the plane and the offset, scale and angle of its flat; the rest of its input is its
// arguments), the texture record the caller chose (the sort key of the polygon is made with it) and the colormap. No sector version: a light that flickers or a floor that
// moves in the same sector changes the arguments of the planes it concerns, and nothing else.
static inline __attribute__((always_inline)) int gc_plane_words(const pslope_t *slope, const sector_t *src, boolean isceiling, fixed_t fixedheight, FBITFIELD PolyFlags, INT32 lightlevel, UINT8 alpha,
	const levelflat_t *levelflat, const extracolormap_t *cm, UINT32 *kbase, const boolean rec)
{
	UINT32 *kp = kbase;
	UINT32 *const kend = kbase + GC_KEYMAX;

	if (!slope)
		GCW(0x51a9e001u);
	else
	{
		GCW(slope->o.x);
		GCW(slope->o.y);
		GCW(slope->o.z);
		GCW(slope->d.x);
		GCW(slope->d.y);
		GCW(slope->zdelta);
		GCW(slope->normal.x);
		GCW(slope->normal.y);
	}
	if (!isceiling)
	{
		GCW(src->floorxoffset);
		GCW(src->flooryoffset);
		GCW(src->floorxscale);
		GCW(src->flooryscale);
		GCW(src->floorangle);
	}
	else
	{
		GCW(src->ceilingxoffset);
		GCW(src->ceilingyoffset);
		GCW(src->ceilingxscale);
		GCW(src->ceilingyscale);
		GCW(src->ceilingangle);
	}
	GCW(fixedheight);
	GCW(PolyFlags);
	GCW(lightlevel);
	GCW(alpha);
	GCW(levelflat ? (UINT32)R_GetTextureNumForFlat((levelflat_t *)levelflat) : 0xFFFFFFFFu);
	GCW((UINT32)(uintptr_t)current_texture);
	if (!cm)
		GCW(0x9c3a5d01u);
	else
	{
		GCW(cm->rgba);
		GCW(cm->fadergba);
		GCW((UINT32)cm->fadestart | ((UINT32)cm->fadeend << 8) | ((UINT32)cm->flags << 16) | (cm->gl_lighttable.needs_update ? 0x01000000u : 0u));
		GCW(cm->gl_lighttable.id);
	}
	return (int)(kp - kbase);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// tables

// A record is live when it was made in the current half (gen) or in the one before (gen - 1); 0 is a free entry. A swap makes the half that is full the old one and drops the old one:
// what was used in the last half survives it (gc_promote), what was not goes. A flush (the texture records or the lights changed) drops both.
static inline boolean gc_live(UINT16 g)
{
	return g == gc.gen || (g && g == (UINT16)(gc.gen - 1));
}

static void gc_advance(UINT32 n)
{
	UINT32 g = (UINT32)gc.gen + n;

	if (g > 65000u) // the numbers are used up: the table is emptied
	{
		if (gc.tab)
			memset(gc.tab, 0, (size_t)gc.nsets * GC_WAYS * sizeof(gchent_t));
		if (gc.segent)
			memset(gc.segent, 0, (size_t)gc.nsegent * sizeof(gcent_t));
		if (gc.plent)
			memset(gc.plent, 0, (size_t)gc.nplent * sizeof(gcent_t));
		g = 2;
		gc.hbase = 0;
	}
	gc.gen = (UINT16)g;
}

static void gc_flush_arena(void)
{
	gc_advance(2);
	gc.ar_used = gc.hbase;
}

static void gc_swap_arena(void)
{
	gc_advance(1);
	if (gc.gen != 2) // (not after a wrap)
		gc.hbase = gc.hbase ? 0 : gc.hsize;
	gc.ar_used = gc.hbase;
}

void HWR_GCacheFlush(void)
{
	hwr_lt_epoch++; // (the light tables or the textures changed: HWR_Lighting asks for the table of the default colormap again)
	if (gc.blk)
		gc_flush_arena();
	gc.s_flush++;
}

// a new level: the zone has freed the block (PU_HWRPLANE), all pointers into it are gone
static void HWR_GCacheLevel(void)
{
	gc.bypass = gc.bypass_len = gc.w_views = gc.w_q = gc.w_hit = 0;
	gc.dead = false;
	gc.blk = NULL;
	gc.on = false;
}

// -hwgo N: bits of the OPT11 geometry micro-optimisations that are switched OFF (A/B on one ELF): 1 AddLine angle reuse (PS2-HW-81), 4 QuadHidden two-corner exit (PS2-HW-86),
// 8 HWR_Lighting fast path (PS2-HW-83), 16 HWR_CalcWallLight memo (PS2-HW-84), 32 RenderBatches shader/scan-direction cache (PS2-HW-85), 1024 inline R_FakeFlat / R_CheckSectorLightLists (PS2-HW-200),
// 2048 light table of the default colormap once per view (PS2-HW-205), 4096 vertex angle once per view (PS2-HW-206) (64 and 128 are the VU branch's: HWR_RenderBatchesV2)
UINT32 hwr_geo_off;

static void HWR_GCacheParse(void)
{
	if (M_CheckParm("-hwgo") && M_IsNextParm())
		hwr_geo_off = (UINT32)atoi(M_GetNextParm());
	gc.mode_set = true;
	gc.mode = 1 | 4; // ON by default since the round 2, the walls only (v2: a hit is a key of plain words and a lean replay; a plane costs about as much to replay as to make and its record is the biggest); -hwgc 0: off, -hwgc 1: the planes too
	gc.kb = GC_DEFAULT_KB;
	if (M_CheckParm("-hwgc") && M_IsNextParm())
		gc.mode = atoi(M_GetNextParm());
	gc.nsets_user = 0; // (the size follows the level unless -hwgcsets says)
	if (M_CheckParm("-hwgcsets") && M_IsNextParm())
	{
		INT32 n = atoi(M_GetNextParm());
		UINT32 p2 = 256;

		while (p2 < (UINT32)n && p2 < 16384)
			p2 <<= 1;
		gc.nsets_user = p2;
	}
	if (M_CheckParm("-hwgctest") && M_IsNextParm())
	{
		const char *a = M_GetNextParm();

		gc.test_rec = (UINT32)atoi(a);
		while (*a && *a != ',')
			a++;
		gc.test_res = *a == ',' ? (UINT32)atoi(a + 1) : 0;
	}
	if (M_CheckParm("-hwgcmem") && M_IsNextParm())
	{
		INT32 kb = atoi(M_GetNextParm());

		gc.kb = kb < 64 ? 64u : kb > 4096 ? 4096u : (UINT32)kb;
	}
}

// The zone asks for memory back (z_zone.h, Z_AddReclaimHook: an allocation does not fit and no cache block is left): the whole cache goes and stays off for the level.
// A record that is being made notices (gc.blk is NULL: gc_rec_end writes nothing); a replay cannot be running, HWR_GCReserve made the room for it first.
static size_t HWR_GCacheReclaim(size_t want)
{
	size_t got = gc.blk_bytes;

	(void)want;
	if (!gc.blk)
		return 0;
	gc.dead = true;
	gc.on = false;
	gc.rec_fail = true;
	hwr_grec_on = false;
	gc.s_reclaim++;
	Z_Free(gc.blk); // (sets gc.blk to NULL)
	gc.blk = NULL;
	return got;
}

// After an abandoned frame (ps2_hwfb.c): the state the jump left behind
void HWR_GCacheAbandon(void)
{
	hwr_grec_on = false;
	gc.on = false;
	gc.rec_fail = true;
}

// A hit in the old half: the record is copied to the current one (if it has room), so that it survives the next swap. Returns the record to replay.
static const gcrh_t *gc_promote(gcent_t *e, const gcrh_t *rh)
{
	const UINT32 len = e->len;

	if (e->gen == gc.gen)
		return rh;
	if (gc.ar_used + len <= gc.hbase + gc.hsize)
	{
		memcpy(gc.ar + gc.ar_used, rh, len);
		e->off = gc.ar_used;
		e->gen = gc.gen;
		gc.ar_used += (len + 3u) & ~3u;
		gc.s_promo++;
		return (const gcrh_t *)(gc.ar + e->off);
	}
	return rh;
}

static boolean gc_alloc_block(void)
{
	UINT32 want = 1024u;
	size_t tab_b, seg_b, pl_b;
	size_t sec_b = (size_t)numsectors * (2 * sizeof(gcfp_t) + 4 * sizeof(UINT32)) + (size_t)numvertexes * (sizeof(angle_t) + sizeof(UINT32));
	size_t ar_b = (size_t)(gc.kb * 512u & ~15u) * 2u + GC_SCRATCH;
	size_t total;
	UINT8 *p;

	// the places of the records: a seg and a plane of a sector have one of their own (8 bytes: no hashing, no ways, nothing to evict but by the arena), the planes of 3D floors
	// (a few for each subsector with floors) share a small hashed table of 2 ways (16 bytes an entry)
	if (gc.nsets_user)
		want = gc.nsets_user; // (-hwgcsets)
	tab_b = (size_t)want * GC_WAYS * sizeof(gchent_t);
	seg_b = ((size_t)numsegs * sizeof(gcent_t) + 15u) & ~(size_t)15u;
	pl_b = (gc.mode & 4) ? 0 : (((size_t)addsubsector * 2u * sizeof(gcent_t) + 15u) & ~(size_t)15u);
	total = tab_b + seg_b + pl_b + sec_b + ar_b + 64;

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
	gc.blk_bytes = total;
#ifdef PS2
	Z_AddReclaimHook(HWR_GCacheReclaim); // (a second call is ignored)
#endif
	gc.nsets = want;
	gc.tab = (gchent_t *)p;
	p += tab_b;
	gc.segent = (gcent_t *)p;
	gc.nsegent = (UINT32)numsegs;
	p += seg_b;
	gc.plent = pl_b ? (gcent_t *)p : NULL;
	gc.nplent = pl_b ? (UINT32)addsubsector * 2u : 0;
	p += pl_b;
	gc.bfp = (gcfp_t *)p;
	p += (size_t)numsectors * sizeof(gcfp_t);
	gc.ffp = (gcfp_t *)p;
	p += (size_t)numsectors * sizeof(gcfp_t);
	gc.bst = (UINT32 *)p;
	p += (size_t)numsectors * sizeof(UINT32);
	gc.fst = (UINT32 *)p;
	p += (size_t)numsectors * sizeof(UINT32);
	gc.bver = (UINT32 *)p;
	p += (size_t)numsectors * sizeof(UINT32);
	gc.fver = (UINT32 *)p;
	p += (size_t)numsectors * sizeof(UINT32);
	gc.vang = (angle_t *)p;
	p += (size_t)numvertexes * sizeof(angle_t);
	gc.vst = (UINT32 *)p;
	p += (size_t)numvertexes * sizeof(UINT32);
	gc.nvert = numvertexes;
	memset(gc.vst, 0, (size_t)numvertexes * sizeof(UINT32));
	gc.ar = p;
	gc.hsize = gc.kb * 512u & ~15u;
	gc.hbase = 0;
	gc.ar_size = gc.hsize * 2u;
	gc.ar_used = 0;
	gc.nsec = (UINT32)numsectors;
	memset(gc.tab, 0, tab_b);
	memset(gc.segent, 0, seg_b);
	if (pl_b)
		memset(gc.plent, 0, pl_b);
	memset(gc.bst, 0, (size_t)numsectors * sizeof(UINT32));
	memset(gc.fst, 0, (size_t)numsectors * sizeof(UINT32));
	memset(gc.bver, 0, (size_t)numsectors * sizeof(UINT32));
	memset(gc.fver, 0, (size_t)numsectors * sizeof(UINT32));
	memset(gc.bfp, 0, (size_t)numsectors * sizeof(gcfp_t));
	memset(gc.ffp, 0, (size_t)numsectors * sizeof(gcfp_t));
	gc.vctr = 0;
	gc.gen = 2;
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
	// A scene whose walls do not fit in the arena (THZ2: 1400 segs of 5 polygons a frame, nothing in the arena is ever used again) only pays for the records. After a window of views with
	// fewer than a third of the queries hitting, the cache is out of use for a while (the records stay: a key that still fits is a hit when it comes back); a longer while every time.
	if (gc.bypass)
	{
		gc.bypass--;
		gc.s_bypass++;
		return false;
	}
	if (++gc.w_views >= GC_WINDOW)
	{
		if ((gc.mode & 3) == 1 && gc.w_q >= 64u && gc.w_hit * 3u < gc.w_q) // (not while checking: a checked hit is not counted as a hit)
		{
			gc.bypass = gc.bypass_len ? gc.bypass_len : GC_BYPASS_MIN;
			gc.bypass_len = gc.bypass_len ? (gc.bypass_len * 2u > GC_BYPASS_MAX ? GC_BYPASS_MAX : gc.bypass_len * 2u) : GC_BYPASS_MIN * 2u;
		}
		else if (gc.w_hit * 2u > gc.w_q)
			gc.bypass_len = 0;
		gc.w_views = gc.w_q = gc.w_hit = 0;
		if (gc.bypass)
		{
			gc.bypass--;
			gc.s_bypass++;
			return false;
		}
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

// the hashed table (the planes of 3D floors)
static gcent_t *gc_find_h(UINT32 id, UINT32 id2, boolean *hit)
{
	UINT32 h = id * 2654435761u + (id2 >> 3) * 40503u;
	gchent_t *set, *victim;
	UINT32 w;

	h = (h ^ (h >> 15)) * 2246822519u;
	set = &gc.tab[((h >> 8) & (gc.nsets - 1)) * GC_WAYS];
	for (w = 0; w < GC_WAYS; w++)
		if (set[w].id == id && set[w].id2 == id2 && gc_live(set[w].e.gen))
		{
			*hit = true;
			return &set[w].e;
		}
	*hit = false;
	victim = &set[0];
	for (w = 0; w < GC_WAYS; w++)
	{
		if (!gc_live(set[w].e.gen))
			return &set[w].e;
		if (set[w].e.gen != gc.gen)
			victim = &set[w]; // (one of the old half before one of the current)
	}
	return &victim->e;
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

static size_t HWR_GCacheReclaim(size_t want);

void HWR_GCRecPoly(const FSurfaceInfo *surf, const FOutVector *verts, FUINT n, FBITFIELD flags, int shader, boolean horizon, UINT32 hash)
{
	const UINT32 size = (UINT32)(sizeof(gcphdr_t) + sizeof(PolygonArrayEntry) + n * sizeof(FOutVector));
	gcphdr_t *h;
	PolygonArrayEntry *pe;
	UINT32 *d;
	const UINT32 *sp;
	UINT32 k;

	if (gc.test_rec && ++gc.test_n1 == gc.test_rec)
		HWR_GCacheReclaim(0); // (test: the zone takes the cache back in the middle of a record)
	if (n > 250)
	{
		gc.rec_fail = true;
		return;
	}
	if (!(h = gc_rec_alloc(size)))
		return;
	h->op = GCOP_POLY;
	h->n = (UINT8)n;
	h->pad = 0;
	h->size = (UINT16)size;
	h->target = (INT16)shader;
	h->hashed = (!(flags & PF_NoTexture) && !horizon) ? 1 : 0;
	if (h->hashed)
		HWR_GCHashes(current_texture, hash & 0xFFFFu, h->hash); // (the order of the texture depends on the scan direction of the frame: both are kept)
	else
		h->hash[0] = h->hash[1] = 0;
	// the entry of the batch as HWR_ProcessPolygon makes it (word copies: memcpy of newlib takes a byte loop for what is not 8 byte aligned)
	pe = (PolygonArrayEntry *)(h + 1);
	d = (UINT32 *)pe;
	for (k = 0; k < sizeof(PolygonArrayEntry) / 4u; k++)
		d[k] = 0;
	sp = (const UINT32 *)surf;
	for (k = 0; k < sizeof(FSurfaceInfo) / 4u; k++)
		d[k] = sp[k];
	pe->surf.PolyFlags = 0; // (stack garbage in most callers; only the model code of the driver reads it)
	pe->numVerts = n;
	pe->polyFlags = flags;
	pe->texture = current_texture;
	pe->horizonSpecial = horizon;
	d = (UINT32 *)((UINT8 *)(h + 1) + sizeof(PolygonArrayEntry));
	sp = (const UINT32 *)verts;
	for (k = 0; k < n * 5u; k++)
		d[k] = sp[k];
	gc.rec_np++;
	gc.rec_nv += n;
	gc.s_rec_ops++;
}

void HWR_GCRecTex(GLMipmap_t *texture)
{
	if (gc.rec_fail)
		return;
	gc.rec_last = texture; // (no op: the polygons that follow hold their texture, the record the one the function ends with)
	gc.rec_lastset = true;
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
	gc.rec_nw++;
	gc.s_rec_ops++;
}

// the record starts with its header and its key; the ops follow while the function runs
static void gc_rec_begin(boolean scratch, const UINT32 *key, int keyw)
{
	gcrh_t *rh;

	gc.rec_start = gc.rec_pos = scratch ? gc.ar_size : gc.ar_used;
	gc.rec_limit = scratch ? gc.ar_size + GC_SCRATCH : gc.hbase + gc.hsize;
	gc.rec_fail = gc.rec_full = false;
	gc.rec_np = gc.rec_nv = gc.rec_nw = 0;
	gc.rec_lastset = false;
	gc.rec_last = NULL;
	hwr_grec_on = true;
	if ((rh = gc_rec_alloc((UINT32)(sizeof(gcrh_t) + 4u * (UINT32)keyw))) != NULL)
	{
		rh->keyw = (UINT16)keyw;
		rh->opsoff = (UINT16)(sizeof(gcrh_t) + 4u * (UINT32)keyw);
		rh->npoly = rh->nvert = rh->nwall = rh->flags = 0;
		rh->last = NULL;
		{
			UINT32 *kd = (UINT32 *)(rh + 1);
			int j;

			for (j = 0; j < keyw; j++)
				kd[j] = key[j];
		}
	}
}

// the records of what the function did: made again (the polygons go to the batch as HWR_ProcessPolygon would have put them, the transparent walls to their list)
static void gc_replay(const gcrh_t *rh, UINT32 len)
{
	const UINT8 *p = (const UINT8 *)rh + rh->opsoff;
	const UINT8 *const end = (const UINT8 *)rh + len;
	const UINT32 view = gc.view;

	while (p < end)
	{
		const UINT32 op = *p;

		if (op == GCOP_POLY)
		{
			const gcphdr_t *h = (const gcphdr_t *)p;

			HWR_GCReplayPoly(h, view);
			p += h->size;
		}
		else if (op == GCOP_TWALL)
		{
			const gctw_t *w = (const gctw_t *)p;

			HWR_AddTransparentWall((FOutVector *)w->v, (FSurfaceInfo *)&w->surf, w->texnum, w->blend, w->fog != 0, w->lightlevel, w->cmap);
			p += w->size;
		}
		else
			break; // (cannot happen: the records are written by the functions above)
	}
	if (rh->flags & 1)
		current_texture = rh->last;
	gc.n_rpoly += rh->npoly;
	gc.s_rep_ops += (UINT32)rh->npoly + rh->nwall;
}

static void gc_report_diff(const char *kind, UINT32 idx, const UINT8 *a, UINT32 alen, const UINT8 *b, UINT32 blen)
{
	UINT32 i, n = alen < blen ? alen : blen;
	const UINT32 base = alen >= sizeof(gcrh_t) ? ((const gcrh_t *)a)->opsoff : 0; // (the key is checked word by word before a hit; the ops are what is decoded)

	gc.s_bad++;
	if (gc.reports++ >= 24)
		return;
	a += base;
	b += base;
	alen -= base;
	blen -= base;
	n = alen < blen ? alen : blen;
	for (i = 0; i < n && a[i] == b[i]; i++)
		;
	I_OutputMsg("HWGC MISMATCH %s %u: cached %u bytes, calculated %u bytes, first difference at %u (op %u)\n", kind, (unsigned)idx, (unsigned)alen, (unsigned)blen, (unsigned)i, (unsigned)(i < n ? a[i] : 0));
	if (gc.reports <= 6)
	{
		// decode: which op, which vertex, which field differs
		UINT32 o = 0, k;

		while (o < n && o + sizeof(gcphdr_t) <= n)
		{
			const UINT32 sz = a[o] == GCOP_TWALL ? (UINT32)sizeof(gctw_t) : ((const gcphdr_t *)(a + o))->size;

			if (i >= o && i < o + sz)
			{
				const UINT32 rel = i - o;

				if (a[o] == GCOP_POLY && rel >= sizeof(gcphdr_t) + sizeof(PolygonArrayEntry))
				{
					const UINT32 vo = rel - (UINT32)(sizeof(gcphdr_t) + sizeof(PolygonArrayEntry));
					const FOutVector *va = (const FOutVector *)(a + o + sizeof(gcphdr_t) + sizeof(PolygonArrayEntry)), *vb = (const FOutVector *)(b + o + sizeof(gcphdr_t) + sizeof(PolygonArrayEntry));
					const UINT32 nv = ((const gcphdr_t *)(a + o))->n;

					I_OutputMsg("  in op at %u (poly, %u vertices, flags %x): vertex %u field %u (x y z s t)\n", (unsigned)o, (unsigned)nv, (unsigned)((const PolygonArrayEntry *)(a + o + sizeof(gcphdr_t)))->polyFlags, (unsigned)(vo / 20), (unsigned)((vo % 20) / 4));
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
static void gc_rec_end(gcent_t *e, UINT32 id, UINT32 id2, boolean check, const char *kind, UINT32 idx)
{
	hwr_grec_on = false;
	if (!gc.blk)
		return; // (the zone took the memory back while the function ran: HWR_GCacheReclaim)
	if (gc.rec_fail)
	{
		if (!check)
		{
			if (gc.rec_full)
			{
				gc.s_full++;
				gc_swap_arena(); // the half is full: the old half goes, this one becomes the old one; a record that is too long is just not kept
			}
			e->gen = 0; // (not kept)
		}
		return;
	}
	{
		gcrh_t *rh = (gcrh_t *)(gc.ar + gc.rec_start);

		rh->npoly = (UINT16)gc.rec_np;
		rh->nvert = (UINT16)gc.rec_nv;
		rh->nwall = (UINT16)gc.rec_nw;
		rh->flags = gc.rec_lastset ? 1 : 0;
		rh->last = gc.rec_lastset ? gc.rec_last : NULL;
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
	if (id2)
	{
		gchent_t *he = (gchent_t *)((UINT8 *)e - offsetof(gchent_t, e)); // (a place in the hashed table: it holds the ids)

		he->id = id;
		he->id2 = id2;
	}
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
	I_OutputMsg("HWPROF39 gcache replay: new=%u stale=%u | poly ops %u (%u cycles) texture ops %u\n", gc.s_new / frames, gc.s_stale / frames, gc.n_rpoly / frames, gc.c_rpoly / frames, gc.n_rtex / frames);
	{
		unsigned i;

		I_OutputMsg("HWPROF50 stale keys by the first word that differs (seg:");
		for (i = 0; i < 34; i++)
			if (gc.mm[i])
				I_OutputMsg(" %u=%u", i, gc.mm[i] / frames);
		I_OutputMsg(" | plane:");
		for (i = 0; i < 34; i++)
			if (gc.mmp[i])
				I_OutputMsg(" %u=%u", i, gc.mmp[i] / frames);
		I_OutputMsg(")\n");
		memset(gc.mm, 0, sizeof gc.mm);
		memset(gc.mmp, 0, sizeof gc.mmp);
		I_OutputMsg("HWPROF51 seg misses by calculation cycles (n bytes per 512-cycle bucket):");
		for (i = 0; i < 10; i++)
			I_OutputMsg(" %u:%u/%u", i * 512, gc.ch[i] / frames, gc.cl[i] / frames);
		I_OutputMsg("\n");
		memset(gc.ch, 0, sizeof gc.ch);
		memset(gc.cl, 0, sizeof gc.cl);
	}
	gc.s_new = gc.s_stale = gc.c_rpoly = gc.c_rtex = gc.n_rpoly = gc.n_rtex = 0;
	I_OutputMsg("HWPROF32 gcache mode=%d on=%d seg hit=%u miss=%u skip=%u | plane hit=%u miss=%u skip=%u | ops replayed=%u recorded=%u | flush=%u full=%u | check=%u bad=%u | cycles key=%u (sector fp %u: %u base %u full) hit=%u miss=%u | reclaim=%u promote=%u bypass=%u | arena=%u/%u\n",
		gc.mode, gc.blk ? 1 : 0, gc.s_seg_hit / frames, gc.s_seg_miss / frames, gc.s_seg_skip / frames, gc.s_pl_hit / frames, gc.s_pl_miss / frames, gc.s_pl_skip / frames, gc.s_rep_ops / frames, gc.s_rec_ops / frames,
		gc.s_flush, gc.s_full, gc.s_chk, gc.s_bad, gc.c_key / frames, gc.c_fp / frames, gc.n_fpb / frames, gc.n_fpf / frames, gc.c_hit / frames, gc.c_miss / frames, gc.s_reclaim, gc.s_promo / frames, gc.s_bypass / frames, (unsigned)((gc.ar_used - gc.hbase) >> 10), (unsigned)(gc.hsize >> 10));
	gc.s_seg_hit = gc.s_seg_miss = gc.s_seg_skip = gc.s_pl_hit = gc.s_pl_miss = gc.s_pl_skip = gc.s_flush = gc.s_full = gc.s_chk = gc.s_bad = gc.s_rep_ops = gc.s_rec_ops = 0;
	gc.c_hit = gc.c_miss = gc.c_key = gc.c_fp = gc.n_fpb = gc.n_fpf = gc.s_promo = gc.s_bypass = 0;
}
