// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_pbatch.inc
/// \brief OPT11 round 2 (VU2, PS2-HW-233..238): the polygons of a batch are collected in the form the VU1 program reads them, into their buckets (states), as they arrive.
///        Included by hw_batching.c (PS2 only).
///
/// Before, a polygon went through five passes before it reached the VU1 chunk (HWR_ProcessPolygon: a 64 byte entry with a copy of the surface, a state digest, PalLit,
/// the vertices copied; HWR_GroupPlan: the key of every polygon through a hash table; the scatter of (first vertex, count, light) triples into the sorted order; the plan
/// pass over the entries; vu_fans: the vertices copied again into the ring), about 1200 cycles a polygon of the profile (sampler: 2000 K cycles of the 7.9 M of a
/// DEMO_001 frame, 3400 K of 12 M of DEMO_004). Now:
///  * the state of a polygon (texture, flags, colour, light, ...) is found among the buckets of the batch at once: a direct mapped cache on the texture, flags and colour is
///    verified by comparing the state itself (not a hash of it: the buckets cannot collide, and the digest, PalLit and the entry copy are made once per state, not per polygon);
///  * the polygon is written once, as the block the VU1 program reads (header qw, texture-scale qw, two qw a vertex: x y z / s t, 16 byte aligned), together with the extent of its s, t
///    (the draw needs it for the texture repeats); the block is the pool of the batch, the polygon is linked to the end of its bucket;
///  * the buckets are ordered (the same order as HWR_GroupPlan made: texture order, first appearance of the texture, first appearance of the state), the lists are walked into
///    the array of blocks of a batch, the driver copies each block into the ring with quadword moves (PS2HWD_DrawBlocks, vu_blocks);
///  * the polygons the texture planner wants (big map textures and flats) are listed as they are collected: the plan pass does not look at the others.
/// -hwgo 65536 switches the new collection off (the previous one: polygon array, HWR_GroupPlan, DrawFans) for A/B on one ELF.

#define HWR_GO_NOPB 0x10000u
#define HWR_PB_END 0xFFFFFFFFu
#define HWR_PB_BMAX 4096u // buckets (states) of one batch: more flush what is collected (a frame with more distinct states than that is not seen)
#define HWR_PB_CSLOTS 512u
#define HWR_PB_TSLOTS 2048u
#define HWR_PB_XSLOTS 1024u

typedef union
{
	UINT32 w[4];
	float f[4];
	unsigned long long d[2];
} __attribute__((aligned(16))) hwr_qw_t;

typedef struct
{
	FSurfaceInfo surf; // of the first polygon of the state
	GLMipmap_t *texture;
	FBITFIELD polyFlags;
	INT32 shader; // HWR_ShaderOfTarget
	INT32 shader_target;
	UINT32 ex; // shader target, horizon, sprite batch mode
	UINT32 lmask; // 0: the light level is not part of the state (the VU1 program lights by palette rows: PalLit); ~0 else
	UINT32 head, tail, cnt, first; // the polygons of the state (indices of pb_ent), the first polygon's index
	UINT32 planwant; // the texture planner measures its polygons
	UINT32 needext; // the texture repeats (the draw tests the extent of s, t against the limit of the texture's size)
} hwr_pbk_t;

typedef struct
{
	UINT32 blk; // pool offset (in quadwords)
	UINT32 next; // the next polygon of the same state
} hwr_pbe_t;

typedef struct
{
	UINT32 blk, bucket;
} hwr_pbp_t;

static hwr_qw_t *pb_pool;
static UINT32 pb_wr, pb_cap; // quadwords
static hwr_pbe_t *pb_ent;
static hwr_pbp_t *pb_plan;
static UINT32 *pb_walk;
static UINT32 pb_n, pb_ecap, pb_nplan;
static hwr_pbk_t *pb_bk;
static unsigned long long *pb_sk;
static UINT32 pb_nb, pb_bcap;
static UINT16 pb_cache[HWR_PB_CSLOTS]; // bucket + 1; never cleared (the entry is verified by comparing the state)
static UINT16 pb_tab[HWR_PB_TSLOTS]; // bucket + 1, open addressing; cleared after a batch that used it
static int pb_tab_dirty;
static UINT32 pb_xk[HWR_PB_XSLOTS][2]; // (hi, texture) of the order of the textures: the first polygon of the texture in the batch
static UINT16 pb_xv[HWR_PB_XSLOTS];
static int pb_x_dirty;
static UINT32 pb_nogrow, pb_nfail; // PS2-HW-445: the pool could not grow (for the rest of the level); the batches cut for want of room (HWPROF50)
static UINT32 pb_ngen, pb_nslow, pb_nfind, pb_ncreate, pb_nbuckets, pb_npolys, pb_nflush, pb_nbatches, pb_nplanned; // HWPROF50 (per window)
static UINT32 pb_serial = 1; // OPT13 IR: the number of the batch being collected (bumped when the buckets are emptied): an interned state remembers the bucket it found in this one

static inline int HWR_PBOn(void)
{
	return !(hwr_geo_off & HWR_GO_NOPB);
}

#ifdef __mips__
static inline float hwr_minf(float a, float b)
{
	float r;

	__asm__("min.s %0,%1,%2" : "=f"(r) : "f"(a), "f"(b));
	return r;
}

static inline float hwr_maxf(float a, float b)
{
	float r;

	__asm__("max.s %0,%1,%2" : "=f"(r) : "f"(a), "f"(b));
	return r;
}
#else
static inline float hwr_minf(float a, float b)
{
	return b < a ? b : a;
}

static inline float hwr_maxf(float a, float b)
{
	return b > a ? b : a;
}
#endif

#ifdef __mips__
static inline float hwr_i2f(UINT32 u) // (the word is in an integer register already: mtc1 costs one cycle, a second load into a floating point register two)
{
	float f;

	__asm__("mtc1 %1,%0" : "=f"(f) : "r"(u));
	return f;
}
#else
static inline float hwr_i2f(UINT32 u)
{
	union { UINT32 u; float f; } c;

	c.u = u;
	return c.f;
}
#endif

static void HWR_PBAlloc(void)
{
	if (pb_pool)
		return;
	pb_cap = 12288;
	pb_ecap = pb_cap / 8; // (a polygon takes 8 quadwords at the least: room in the pool is room in the polygon arrays, one test)
	pb_bcap = 256;
	pb_pool = HWR_BatchAlloc((size_t)pb_cap * sizeof(hwr_qw_t));
	pb_ent = HWR_BatchAlloc((size_t)pb_ecap * sizeof(hwr_pbe_t));
	pb_plan = HWR_BatchAlloc((size_t)pb_ecap * sizeof(hwr_pbp_t));
	pb_walk = HWR_BatchAlloc((size_t)pb_ecap * sizeof(UINT32));
	pb_bk = HWR_BatchAlloc((size_t)pb_bcap * sizeof(hwr_pbk_t));
	pb_sk = HWR_BatchAlloc((size_t)pb_bcap * sizeof(unsigned long long));
	pb_wr = pb_n = pb_nplan = pb_nb = 0;
	pb_nogrow = 0;
	memset(pb_tab, 0, sizeof pb_tab);
	memset(pb_xv, 0, sizeof pb_xv);
	memset(pb_cache, 0, sizeof pb_cache);
	pb_tab_dirty = pb_x_dirty = 0;
}

static void HWR_PBStFree(void);

static void HWR_PBFree(void)
{
	pb_serial++;
	HWR_PBStFree();
	Z_Free(pb_pool);
	Z_Free(pb_ent);
	Z_Free(pb_plan);
	Z_Free(pb_walk);
	Z_Free(pb_bk);
	Z_Free(pb_sk);
	pb_pool = NULL;
	pb_ent = NULL;
	pb_plan = NULL;
	pb_walk = NULL;
	pb_bk = NULL;
	pb_sk = NULL;
	pb_wr = pb_cap = pb_n = pb_ecap = pb_nplan = pb_nb = pb_bcap = 0;
}

// what is collected is dropped (after the draw, or the benchmark)
static void HWR_PBReset(void)
{
	pb_serial++;
	pb_wr = pb_n = pb_nplan = pb_nb = 0;
	if (pb_tab_dirty)
	{
		memset(pb_tab, 0, sizeof pb_tab);
		pb_tab_dirty = 0;
	}
	if (pb_x_dirty)
	{
		memset(pb_xv, 0, sizeof pb_xv);
		pb_x_dirty = 0;
	}
}

// the pool is full (a few times a level): grow it by half; the polygon arrays hold one entry for every 8 quadwords of the pool (a triangle takes 8)
// PS2-HW-445 (OPT12 HWDRV): `soft` = the caller can draw what is collected and start again. The pool is then asked for with Z_TryReallocAlign: at the edge of the zone the arena is in
// pieces (DEMO_003 frame 584: 3.2 MB free, the largest block 456 KB, the pool wanted 663 552 B for its fourth step: "Out of memory allocating 663552 bytes", the run ended), and a bigger
// pool is not worth the run. Returns 0 when it did not grow (the pool then keeps its size for the rest of the level: pb_nogrow, no new attempt, every attempt walks the arena).
static int __attribute__((noinline)) HWR_PBGrow(UINT32 need, int soft)
{
	if (pb_wr + need > pb_cap)
	{
		const UINT32 c = (UINT32)HWR_BatchCapacity((int)pb_cap, (int)(pb_wr + need), sizeof(hwr_qw_t) + (sizeof(hwr_pbe_t) + sizeof(hwr_pbp_t) + sizeof(UINT32)) / 8);
		const UINT32 e = (c + 7) / 8;

		if (soft)
		{
			hwr_qw_t *np;

			if (pb_nogrow || (hwr_geo_off & 0x1000000u)) // (-hwgo 16777216: the pool never grows: the cut is exercised without a shortage of memory)
				return 0;
			np = Z_TryReallocAlign(pb_pool, (size_t)c * sizeof(hwr_qw_t), PU_HWRBATCH, NULL, 4);
			if (!np)
			{
				pb_nogrow = 1;
				return 0;
			}
			pb_pool = np;
		}
		else
			pb_pool = HWR_BatchResize(pb_pool, (size_t)c * sizeof(hwr_qw_t));
		pb_ent = HWR_BatchResize(pb_ent, (size_t)e * sizeof(hwr_pbe_t));
		pb_plan = HWR_BatchResize(pb_plan, (size_t)e * sizeof(hwr_pbp_t));
		pb_walk = HWR_BatchResize(pb_walk, (size_t)e * sizeof(UINT32));
		pb_cap = c;
		pb_ecap = e;
	}
	return 1;
}

static void __attribute__((noinline)) HWR_PBGrowB(void)
{
	const UINT32 c = (UINT32)HWR_BatchCapacity((int)pb_bcap, (int)pb_nb + 1, sizeof(hwr_pbk_t) + sizeof(unsigned long long));

	pb_bk = HWR_BatchResize(pb_bk, (size_t)c * sizeof(hwr_pbk_t));
	pb_sk = HWR_BatchResize(pb_sk, (size_t)c * sizeof(unsigned long long));
	pb_bcap = c;
}

// the state of the polygon is the state of the bucket (the light level only when the VU1 program does not light the polygon by palette rows; the surface fields beyond the colour
// only when the polygon is lit by the shader: the callers of the others leave them as they were on the stack)
static inline int HWR_PBMatch(const hwr_pbk_t *b, const FSurfaceInfo *s, const GLMipmap_t *tex, UINT32 flags, UINT32 ex)
{
	UINT32 d = ((UINT32)(uintptr_t)b->texture ^ (UINT32)(uintptr_t)tex) | (b->polyFlags ^ flags) | (b->ex ^ ex) | (b->surf.PolyColor.rgba ^ s->PolyColor.rgba);

	if (flags & PF_ColorMapped)
		d |= (b->surf.TintColor.rgba ^ s->TintColor.rgba) | (b->surf.FadeColor.rgba ^ s->FadeColor.rgba) | (b->surf.LightTableId ^ s->LightTableId)
			| (b->surf.LightInfo.fade_start ^ s->LightInfo.fade_start) | (b->surf.LightInfo.fade_end ^ s->LightInfo.fade_end)
			| ((b->surf.LightInfo.light_level ^ s->LightInfo.light_level) & b->lmask);
	return d == 0;
}

static inline UINT32 HWR_PBH1(const GLMipmap_t *tex, UINT32 flags, UINT32 pc)
{
	return ((UINT32)(uintptr_t)tex ^ (flags << 9) ^ (flags >> 3) ^ pc ^ (pc >> 16)) * 0x9E3779B1u; // (one multiply: cheaper than three on the machine of the profile; the high bits are the index)
}

// the order of a bucket among the others (the order HWR_GroupPlan made, PS2-HW-231): the buckets without a texture (and the horizon lines) first, in reverse order of their
// polygon; then by texture order, the first polygon of the texture in this batch, the first polygon of the bucket
static unsigned long long HWR_PBOrder(const GLMipmap_t *tex, UINT32 flags, UINT32 spr, int hashed, UINT32 first, UINT32 id)
{
	UINT32 hi;

	if (!hashed)
	{
		const UINT32 k = (UINT32)(-(INT32)first); // key = minus the polygon's index (the one that sorts the horizon lines in reverse)

		if (first)
			return ((unsigned long long)(k & 0x7FFFFFFFu) << 32) | id;
		hi = 0; // (key 0: the first polygon of a batch is taken as a textured one by the old sort, the hash of its texture order being 0)
	}
	else
	{
		(void)flags;
		hi = HWR_PS2_TextureOrder(tex) | (spr ? 0x4000u : 0u);
	}
	{
		UINT32 h = ((hi * 0x9E3779B1u) ^ ((UINT32)(uintptr_t)tex * 2654435761u)) >> 22, tf;

		while (pb_xv[h] && (pb_xk[h][0] != hi || pb_xk[h][1] != (UINT32)(uintptr_t)tex))
			h = (h + 1) & (HWR_PB_XSLOTS - 1);
		if (!pb_xv[h])
		{
			pb_xk[h][0] = hi;
			pb_xk[h][1] = (UINT32)(uintptr_t)tex;
			pb_xv[h] = (UINT16)(first > 0xFFFEu ? 0xFFFFu : first + 1u);
			pb_x_dirty = 1;
		}
		tf = (UINT32)pb_xv[h] - 1u;
		return (1ull << 63) | ((unsigned long long)hi << 47) | ((unsigned long long)tf << 31) | ((unsigned long long)(first > 0xFFFFu ? 0xFFFFu : first) << 15) | (id & 0x7FFFu);
	}
}

// more states than a batch holds: what is collected is drawn, collecting starts again (the order across the cut is lost; a frame this rich in states does not occur: a few hundred)
static void HWR_PBFlushEarly(void)
{
	HWR_RenderBatches();
	HWR_StartBatching();
	PS2HWD_TouchTexture(current_texture); // (the polygon being added is not in a batch yet: its texture must stay resident)
}

// a new bucket for the state of the polygon (the polygon about to be added is polygon number pb_n)
static UINT32 HWR_PBNew(const FSurfaceInfo *s, GLMipmap_t *tex, UINT32 flags, UINT32 ex, int shader_target, UINT32 pal, int hashed, UINT32 spr)
{
	hwr_pbk_t *b;
	UINT32 bi;

	if (pb_nb >= ((hwr_geo_off & 0x200000u) ? 24u : HWR_PB_BMAX)) // (-hwgo 2097152: a batch holds 24 states, to test the cut)
		HWR_PBFlushEarly();
	if (pb_nb >= pb_bcap)
		HWR_PBGrowB();
	bi = pb_nb++;
	pb_ncreate++;
	b = &pb_bk[bi];
	b->surf = *s;
	b->texture = tex;
	b->polyFlags = flags;
	b->shader_target = shader_target;
	b->shader = HWR_ShaderOfTarget(shader_target);
	b->ex = ex;
	b->lmask = pal ? 0u : ~0u;
	b->head = b->tail = HWR_PB_END;
	b->cnt = 0;
	b->first = pb_n;
	b->planwant = tex && !(flags & PF_NoTexture) && (tex->regen_kind == 1 || tex->regen_kind == 2) && tex->format == GL_TEXFMT_P_8 && (UINT32)tex->width * tex->height >= 65536u; // KEEP IN STEP with plan_wants (ps2_hw_plan.inc)
	b->needext = tex && !(flags & PF_NoTexture) && ((tex->flags & TF_WRAPXY) || (flags & (PF_ForceWrapX | PF_ForceWrapY)));
	pb_sk[bi] = HWR_PBOrder(tex, flags, spr, hashed, pb_n, bi);
	return bi;
}

// the cache missed: the state is looked for among all the buckets (open addressing on all of the state), made when it is new
static UINT32 __attribute__((noinline)) HWR_PBSlow(const FSurfaceInfo *s, GLMipmap_t *tex, UINT32 flags, UINT32 ex, int shader_target, UINT32 h1, UINT32 ci, UINT32 spr)
{
	const UINT32 pal = PS2HWD_PalLit(s, flags, tex, shader_target) ? 1u : 0u;
	UINT32 h = h1 ^ (ex * 0x27D4EB2Fu), t, e;

	pb_nslow++;
	if (flags & PF_ColorMapped)
	{
		h ^= (s->TintColor.rgba * 0x165667B1u) ^ (s->FadeColor.rgba * 0xD3A2646Cu) ^ (s->LightTableId * 0xFD7046C5u) ^ ((s->LightInfo.fade_start | (s->LightInfo.fade_end << 8)) * 0xB55A4F09u);
		if (!pal)
			h ^= s->LightInfo.light_level * 0x7FEB352Du;
	}
	t = (h * 0x9E3779B1u) >> 21;
	while ((e = pb_tab[t]) != 0)
	{
		const hwr_pbk_t *b = &pb_bk[e - 1];

		if (HWR_PBMatch(b, s, tex, flags, ex))
		{
			pb_cache[ci] = (UINT16)e;
			pb_nfind++;
			return e - 1;
		}
		t = (t + 1) & (HWR_PB_TSLOTS - 1);
	}
	{
		const UINT32 bi = HWR_PBNew(s, tex, flags, ex, shader_target, pal, 1, spr); // (a flush made inside empties the table: the free slot is looked for again)

		t = (h * 0x9E3779B1u) >> 21;
		while (pb_tab[t])
			t = (t + 1) & (HWR_PB_TSLOTS - 1);
		pb_tab[t] = (UINT16)(bi + 1);
		pb_tab_dirty = 1;
		pb_cache[ci] = (UINT16)(bi + 1);
		return bi;
	}
}

// The polygon into its bucket: the block in the pool (room made by the caller) and the link at the end of the bucket's list
static inline __attribute__((always_inline)) void HWR_PBPut(hwr_pbk_t *b, UINT32 bi, const FSurfaceInfo *s, const FOutVector *v, FUINT n, UINT32 need)
{
	const UINT32 pidx = pb_n, blk = pb_wr;
	hwr_qw_t *const o = pb_pool + blk, *const d = o + 2;
	float smin, smax, tmin, tmax;
	typedef UINT32 __attribute__((may_alias)) hwr_word_t;
	const hwr_word_t *w = (const hwr_word_t *)(const void *)v;

	// the words are moved in integer registers (lw / sw cost one cycle on the machine of the profile, lwc1 / swc1 two); the extent of s, t (floating point) only when the texture repeats.
	// (The barrier after each store: gcc pairs adjacent words into 64 bit stores built with shifts and ors.)
#define HWR_PBBAR __asm__ volatile("" ::: "memory")
#define HWR_PBC(i) do { const hwr_word_t *q_ = w + 5 * (i); hwr_qw_t *e_ = d + 2 * (i); const UINT32 x_ = q_[0], y_ = q_[1], z_ = q_[2], s_ = q_[3], t_ = q_[4]; \
		e_[0].w[0] = x_; HWR_PBBAR; e_[0].w[1] = y_; HWR_PBBAR; e_[0].w[2] = z_; HWR_PBBAR; e_[1].w[0] = s_; HWR_PBBAR; e_[1].w[1] = t_; HWR_PBBAR; } while (0)
	if (b->needext)
	{
#define HWR_PBV(i) do { const hwr_word_t *q_ = w + 5 * (i); hwr_qw_t *e_ = d + 2 * (i); const UINT32 x_ = q_[0], y_ = q_[1], z_ = q_[2], s_ = q_[3], t_ = q_[4]; \
		const float sf_ = hwr_i2f(s_), tf_ = hwr_i2f(t_); \
		e_[0].w[0] = x_; HWR_PBBAR; e_[0].w[1] = y_; HWR_PBBAR; e_[0].w[2] = z_; HWR_PBBAR; e_[1].w[0] = s_; HWR_PBBAR; e_[1].w[1] = t_; HWR_PBBAR; \
		smin = hwr_minf(smin, sf_); smax = hwr_maxf(smax, sf_); tmin = hwr_minf(tmin, tf_); tmax = hwr_maxf(tmax, tf_); } while (0)
		smin = smax = hwr_i2f(w[3]);
		tmin = tmax = hwr_i2f(w[4]);
		if (n == 4)
		{
			HWR_PBV(0);
			HWR_PBV(1);
			HWR_PBV(2);
			HWR_PBV(3);
		}
		else if (n == 3)
		{
			HWR_PBV(0);
			HWR_PBV(1);
			HWR_PBV(2);
		}
		else
		{
			FUINT k;

			for (k = 0; k < n; k++)
				HWR_PBV(k);
		}
#undef HWR_PBV
	}
	else
	{
		smin = smax = tmin = tmax = 0.0f;
		if (n == 4)
		{
			HWR_PBC(0);
			HWR_PBC(1);
			HWR_PBC(2);
			HWR_PBC(3);
		}
		else if (n == 3)
		{
			HWR_PBC(0);
			HWR_PBC(1);
			HWR_PBC(2);
		}
		else
		{
			FUINT k;

			for (k = 0; k < n; k++)
				HWR_PBC(k);
		}
	}
#undef HWR_PBC
#undef HWR_PBBAR
	o[0].w[0] = (UINT32)n;
	o[0].f[1] = (float)(int)s->LightInfo.light_level; // the light level of the polygon (the VU1 program makes the colormap row from it)
	o[1].f[0] = smin;
	o[1].f[1] = tmin;
	o[1].f[2] = smax - smin; // (the draw tests the extent against the texture's repeat limit; it knows the texture size then)
	o[1].f[3] = tmax - tmin;
	pb_ent[pidx].blk = blk;
	pb_ent[pidx].next = HWR_PB_END;
	if (b->cnt)
		pb_ent[b->tail].next = pidx;
	else
		b->head = pidx;
	b->tail = pidx;
	b->cnt++;
	if (b->planwant)
	{
		pb_plan[pb_nplan].blk = blk;
		pb_plan[pb_nplan].bucket = bi;
		pb_nplan++;
	}
	pb_n = pidx + 1;
	pb_wr = blk + need;
}

// HWR_ProcessPolygon of a batched polygon (the general case: the state is looked for, made, the arrays grow)
static void __attribute__((noinline)) HWR_PBAdd(const FSurfaceInfo *s, const FOutVector *v, FUINT n, FBITFIELD flags, int shader_target, boolean horizon)
{
	GLMipmap_t *const tex = current_texture;
#ifdef PS2_PROFILE
	const UINT32 spr = (hwr_sprite_batch && !hwr_sprite_shadow) ? 1u : 0u;
#else
	const UINT32 spr = 0;
#endif
	const UINT32 ex = ((UINT32)shader_target & 0xFFu) | ((UINT32)horizon << 8) | (spr << 9);
	const UINT32 need = 2u + 2u * (UINT32)n;
	UINT32 bi;

	pb_ngen++;
	if (!s)
		I_Error("Got a null FSurfaceInfo in batching");// nulls should not come in the stuff that batching currently applies to
	if (!(flags & PF_NoTexture) && !horizon)
	{
		const UINT32 h1 = HWR_PBH1(tex, flags, s->PolyColor.rgba), ci = h1 >> 23;
		const UINT32 e = pb_cache[ci];

		if (e && e <= pb_nb && HWR_PBMatch(&pb_bk[e - 1], s, tex, flags, ex))
			bi = e - 1;
		else
			bi = HWR_PBSlow(s, tex, flags, ex, shader_target, h1, ci, spr);
	}
	else
		bi = HWR_PBNew(s, tex, flags, ex, shader_target, 0, 0, spr);
	if (pb_wr + need > pb_cap && !HWR_PBGrow(need, pb_wr != 0))
	{
		// PS2-HW-445: no room for a bigger pool: what is collected is drawn, collecting starts again (the order across the cut is lost, as with too many states)
		pb_nfail++;
		HWR_PBFlushEarly();
		HWR_PBAdd(s, v, n, flags, shader_target, horizon); // (the buckets are gone: the state is looked for again; the pool is empty, so the polygon fits or the pool grows the old way)
		return;
	}
	HWR_PBPut(&pb_bk[bi], bi, s, v, n, need);
}

// The same for the common polygon (a state that the cache holds, room in the arrays, no sprite batch): inline in HWR_ProcessPolygon, which then needs no frame. Returns 0 when it
// did not take the polygon.
static inline __attribute__((always_inline)) int HWR_PBFast(const FSurfaceInfo *s, const FOutVector *v, FUINT n, FBITFIELD flags, int shader_target, boolean horizon, UINT32 spr)
{
	GLMipmap_t *const tex = current_texture;
	const UINT32 need = 2u + 2u * (UINT32)n;
	UINT32 h1, e;

	if ((flags & PF_NoTexture) || horizon || !s)
		return 0;
	h1 = HWR_PBH1(tex, flags, s->PolyColor.rgba);
	e = pb_cache[h1 >> 23];
	if (e - 1u >= pb_nb || pb_wr + need > pb_cap)
		return 0;
	{
		hwr_pbk_t *const b = &pb_bk[e - 1];

		if (!HWR_PBMatch(b, s, tex, flags, ((UINT32)shader_target & 0xFFu) | ((UINT32)horizon << 8) | (spr << 9)))
			return 0;
		HWR_PBPut(b, e - 1, s, v, n, need);
	}
	return 1;
}

// the block of a polygon as the engine's vertices (the driver's general path, the texture splitter, the planner's reference arithmetic take those)
// (declared for the driver in ps2_hwd.h as the layout of the block: PS2HWD_DrawBlocks)

// the polygons of the textures the planner measures, in the order they arrived
static void HWR_PBPlanPass(void)
{
	UINT32 k;

#ifdef PS2_PROFILE
	HWP_SPAN_BEGIN(tb_plan);
#endif
	PS2HWD_PlanBegin();
	for (k = 0; k < pb_nplan; k++)
		PS2HWD_PlanBlock(pb_bk[pb_plan[k].bucket].texture, pb_pool + pb_plan[k].blk);
	PS2HWD_PlanEnd();
#ifdef PS2_PROFILE
	HWP_SPAN_END2(tb_plan, HWP_B_PLAN, HWP_KB_PLAN);
#ifdef PS2_HWDETAIL
	if (hwr_sprite_batch)
		ps2hwp_cyc[HWP_SF_PLAN] += (unsigned int)(ps2hwp_now() - tb_plan);
#endif
#endif
}

// -hwgo 256: what the driver is given, one line per batch for the level tics 372..376: the same line for the previous collection (HWR_DrawBatch) and this one, the polygons' content
// hashed in the order of the draw (two runs, or the two collections, are compared by diffing the lines)
static UINT32 HWR_PBTraceSurf(const FSurfaceInfo *s, FBITFIELD flags)
{
	UINT32 hs = (0x811c9dc5u ^ s->PolyColor.rgba) * 16777619u;

	if (flags & PF_ColorMapped)
	{
		hs = (hs ^ s->TintColor.rgba) * 16777619u;
		hs = (hs ^ s->FadeColor.rgba) * 16777619u;
		hs = (hs ^ s->LightTableId) * 16777619u;
		hs = (hs ^ s->LightInfo.light_level) * 16777619u;
		hs = (hs ^ s->LightInfo.fade_start) * 16777619u;
		hs = (hs ^ s->LightInfo.fade_end) * 16777619u;
	}
	return hs;
}

static void HWR_PBDrawBatch(FSurfaceInfo *surf, const UINT32 *blks, int count, FBITFIELD polyFlags)
{
	HWP_SPAN_BEGIN(tdb);
	if ((hwr_geo_off & 256) && leveltime >= 372 && leveltime <= 376)
	{
		UINT32 h = 0x811c9dc5u;
		int j, k;

		for (j = 0; j < count; j++)
		{
			const hwr_qw_t *blk = pb_pool + blks[j];
			const UINT32 nn = blk[0].w[0];

			h = (h ^ nn) * 16777619u;
			h = (h ^ (UINT32)(int)blk[0].f[1]) * 16777619u;
			for (k = 0; k < (int)nn; k++)
			{
				h = (h ^ blk[2 + 2 * k].w[0]) * 16777619u;
				h = (h ^ blk[2 + 2 * k].w[1]) * 16777619u;
				h = (h ^ blk[2 + 2 * k].w[2]) * 16777619u;
				h = (h ^ blk[3 + 2 * k].w[0]) * 16777619u;
				h = (h ^ blk[3 + 2 * k].w[1]) * 16777619u;
			}
		}
		I_OutputMsg("HWDC lt=%u n=%d fl=%x surf=%08x content=%08x\n", (unsigned)leveltime, count, (unsigned)polyFlags, (unsigned)HWR_PBTraceSurf(surf, polyFlags), (unsigned)h);
	}
	pb_nbatches++;
	PS2HWD_DrawBlocks(surf, pb_pool, (unsigned int)count, polyFlags, blks);
	HWP_SPAN_END2(tdb, HWP_B_DB, HWP_KB_DB);
}

static inline int HWR_PBSurfDiffers(const FSurfaceInfo *a, const FSurfaceInfo *b, FBITFIELD flags, int shaders)
{
	UINT32 d = a->PolyColor.rgba ^ b->PolyColor.rgba;

	if (shaders && (flags & PF_ColorMapped))
		d |= (a->TintColor.rgba ^ b->TintColor.rgba) | (a->FadeColor.rgba ^ b->FadeColor.rgba) | (a->LightTableId ^ b->LightTableId) | (a->LightInfo.light_level ^ b->LightInfo.light_level)
			| (a->LightInfo.fade_start ^ b->LightInfo.fade_start) | (a->LightInfo.fade_end ^ b->LightInfo.fade_end);
	return d != 0;
}

// Draws what is collected (nothing: returns 0). The buckets in their order; consecutive buckets in the same state (the tests of HWR_RenderBatchesV2: shader, texture, flags, colour,
// light) are one batch for the driver.
static int HWR_PBRender(void)
{
	const UINT32 n = pb_n, m = pb_nb;
	UINT32 i, gap, pos, bstart;
	int currentShader;
	GLMipmap_t *currentTexture;
	FBITFIELD currentPolyFlags;
	FSurfaceInfo currentSurfaceInfo;
	const hwr_pbk_t *e;
	const int shaders = (cv_glshaders.value && gl_shadersavailable) ? 1 : 0;
	static const UINT32 gaps[] = {701, 301, 132, 57, 23, 10, 4, 1};

	if (!n)
		return 0;
	{
		HWP_SPAN_BEGIN(tb_sort);

		for (gap = 0; gap < sizeof gaps / sizeof gaps[0]; gap++)
		{
			const UINT32 g = gaps[gap];
			UINT32 j;

			if (g >= m)
				continue;
			for (i = g; i < m; i++)
			{
				const unsigned long long v = pb_sk[i];

				for (j = i; j >= g && pb_sk[j - g] > v; j -= g)
					pb_sk[j] = pb_sk[j - g];
				pb_sk[j] = v;
			}
		}
		HWP_SPAN_END2(tb_sort, HWP_BATCHSORT, HWP_KB_SORT);
#ifdef PS2_HWDETAIL
		if (hwr_sprite_batch)
			ps2hwp_cyc[HWP_SF_SORT] += (unsigned int)(ps2hwp_now() - tb_sort);
#endif
	}
	if ((hwr_geo_off & 0x100000u) && leveltime >= 374 && leveltime <= 375)
	{
		// -hwgo 1048576: the states of the batch (one line each), to see what splits them
		for (i = 0; i < m; i++)
		{
			const hwr_pbk_t *q = &pb_bk[i];

			I_OutputMsg("HWPB lt=%u b=%u n=%u first=%u tex=%d/%d fl=%x pc=%08x tint=%08x fade=%08x ltid=%u light=%u fs=%u fe=%u ex=%x pal=%d sh=%d\n", (unsigned)leveltime, (unsigned)i, (unsigned)q->cnt, (unsigned)q->first,
				q->texture ? (int)q->texture->regen_kind : -1, q->texture ? (int)q->texture->regen_id : -1, (unsigned)q->polyFlags, (unsigned)q->surf.PolyColor.rgba, (unsigned)q->surf.TintColor.rgba, (unsigned)q->surf.FadeColor.rgba,
				(unsigned)q->surf.LightTableId, (unsigned)q->surf.LightInfo.light_level, (unsigned)q->surf.LightInfo.fade_start, (unsigned)q->surf.LightInfo.fade_end, (unsigned)q->ex, q->lmask ? 0 : 1, (int)q->shader);
		}
	}
	currently_batching = false; // no longer collecting batches
	pb_npolys += n;
	pb_nplanned += pb_nplan;
	pb_nbuckets += m;
	pb_nflush++;
	ps2hwp_cnt[HWC_PROC] += n; // (the counters of HWPROF4: the collection does not count each polygon)
	ps2hwp_cnt[HWC_PROC_BATCH] += n;
	PS2HWD_BatchDraw(); // the textures are made resident now, as each batch is drawn
	ps_hw_numpolys.value.i = (int)n;
	ps_hw_numcalls.value.i = ps_hw_numverts.value.i = 0;
	ps_hw_numshaders.value.i = ps_hw_numtextures.value.i = ps_hw_numpolyflags.value.i = ps_hw_numcolors.value.i = 1;
	HWR_PBPlanPass();
	{
		HWP_SPAN_BEGIN(tb_draw);
		PS_START_TIMING(ps_hw_batchdrawtime);

#define HWR_PBWALK(bk) do { UINT32 p_; for (p_ = (bk)->head; p_ != HWR_PB_END; p_ = pb_ent[p_].next) pb_walk[pos++] = pb_ent[p_].blk; } while (0)
		e = &pb_bk[pb_sk[0] & 0x7FFFu];
		currentShader = e->shader;
		currentTexture = e->texture;
		currentPolyFlags = e->polyFlags;
		currentSurfaceInfo = e->surf;
		if (shaders)
			HWD.pfnSetShader(currentShader);
		if (currentPolyFlags & PF_NoTexture)
			currentTexture = NULL;
		else
		{
			HWP_SPAN_BEGIN(tst);
			HWD.pfnSetTexture(currentTexture);
			HWP_SPAN_END2(tst, HWP_B_TEX, HWP_KB_TEX);
		}
		bstart = pos = 0;
		HWR_PBWALK(e);
		for (i = 1; i < m; i++)
		{
			GLMipmap_t *nextTexture;
			FBITFIELD nextPolyFlags;
			int nextShader;
			boolean changeState = false, changeShader = false, changeTexture = false, changePolyFlags = false, changeSurfaceInfo = false;

			e = &pb_bk[pb_sk[i] & 0x7FFFu];
			nextShader = e->shader;
			nextTexture = e->texture;
			nextPolyFlags = e->polyFlags;
			if (nextPolyFlags & PF_NoTexture)
				nextTexture = 0;
			if (currentShader != nextShader && shaders)
			{
				changeState = true;
				changeShader = true;
			}
			if (currentTexture != nextTexture)
			{
				changeState = true;
				changeTexture = true;
			}
			if (currentPolyFlags != nextPolyFlags)
			{
				changeState = true;
				changePolyFlags = true;
			}
			if (HWR_PBSurfDiffers(&currentSurfaceInfo, &e->surf, nextPolyFlags, shaders))
			{
				changeState = true;
				changeSurfaceInfo = true;
			}
			if (changeState)
			{
				HWR_PBDrawBatch(&currentSurfaceInfo, pb_walk + bstart, (int)(pos - bstart), currentPolyFlags);
				ps_hw_numcalls.value.i++;
				ps_hw_numverts.value.i += 3 * (int)(pos - bstart);
				bstart = pos;
				if (changeShader)
				{
					HWD.pfnSetShader(nextShader);
					currentShader = nextShader;
					ps_hw_numshaders.value.i++;
				}
				if (changeTexture)
				{
					HWP_SPAN_BEGIN(tst);
					HWD.pfnSetTexture(nextTexture);
					HWP_SPAN_END2(tst, HWP_B_TEX, HWP_KB_TEX);
					currentTexture = nextTexture;
					ps_hw_numtextures.value.i++;
				}
				if (changePolyFlags)
				{
					currentPolyFlags = nextPolyFlags;
					ps_hw_numpolyflags.value.i++;
				}
				if (changeSurfaceInfo)
				{
					currentSurfaceInfo = e->surf;
					ps_hw_numcolors.value.i++;
				}
			}
			HWR_PBWALK(e);
		}
#undef HWR_PBWALK
		HWR_PBDrawBatch(&currentSurfaceInfo, pb_walk + bstart, (int)(pos - bstart), currentPolyFlags);
		ps_hw_numcalls.value.i++;
		ps_hw_numverts.value.i += 3 * (int)(pos - bstart);
		HWR_PBReset();
		HWP_SPAN_END2(tb_draw, HWP_BATCHDRAW, HWP_KB_DRAW);
#ifdef PS2_HWDETAIL
		if (hwr_sprite_batch)
			ps2hwp_cyc[HWP_SF_DRAW] += (unsigned int)(ps2hwp_now() - tb_draw);
#endif
		{
			HWP_SPAN_BEGIN(tb_end);
			PS2HWD_BatchEnd();
			HWP_SPAN_END2(tb_end, HWP_B_END, HWP_KB_END);
		}
		PS_STOP_TIMING(ps_hw_batchdrawtime);
	}
	return 1;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// OPT13 IR (RF-1, stored records v3, PS2-HW-480..): the polygons of the geometry cache (hw_gcache.inc) are stored as what this collection makes of them and put back into it without HWR_ProcessPolygon.
//
// A polygon of a record is (gcbk_t, hw_batching.h): its interned STATE (texture, flags, shader target, colours: everything the bucket of the polygon is found by, except the light level, which is a word
// of the block), the box around its vertices, the words of the two header quadwords of its block, and its vertices as the engine made them (x y z s t). The replay finds the bucket from the state
// (an interned state remembers the bucket it found in this batch: one stamp compare), moves the vertices into the pool as PBPut does, links the block. What that spares of the earlier replay
// (HWR_GCReplayPoly -> HWR_ProcessPolygon -> PBFast: the state compared field by field, the polygon hashed to its bucket, texture and flags looked at again) is the larger part of its 650 cycles.
// The bucket a state resolves to is found with the same functions as for a polygon that comes in whole (HWR_PBSlow and its cache), at the first polygon of the state in a batch, so the buckets, their
// order and the blocks are exactly those the polygons would have made (the check mode of the cache compares the two, hw_gcache.inc). The box is tested against the view volume before anything is
// done (PS2-HW-481): a polygon wholly outside one of its five planes is not collected (the driver would throw it away after the copy, the plan and the VU1 clip).
// -hwgv N (hwr_gv_off): 1 the records are made the earlier way (calls of HWR_ProcessPolygon), 2 no box test, 4 the check mode of the cache replays a hit for real before it calculates it again.

#define HWR_ST_CAP 256u // interned states (64 bytes each: 16 KB; the demos need 25..45, a state is texture + flags + colours, not a polygon; 1024 states cost 72 KB and took D3 over the memory edge)
#define HWR_ST_SLOTS 1024u
#define HWR_ST_NONE 0xFFFFu

typedef struct
{
	FSurfaceInfo surf; // PolyColor, and with PF_ColorMapped the tint, fade, light table and fade range; not the light level (the block has it), not PolyFlags
	GLMipmap_t *texture;
	FBITFIELD polyFlags;
	UINT32 ex; // shader target, horizon, sprite batch mode: as the buckets keep them
	INT32 shader_target;
	UINT32 tview; // the view of the cache in which the texture was last made current through this state
	UINT32 bser, blight; // the batch (pb_serial) the bucket below was found in, and the light level it was found for
	UINT16 bucket, bcare; // bcare: the bucket tells polygons of another light level apart (the VU1 program does not light them by rows)
} hwr_pbs_t;

static void *pb_stblk;
static hwr_pbs_t *pb_st;
static UINT16 *pb_sth;
static UINT32 pb_nst;
static int pb_v3_nocull; // the check mode replays without the box test
static UINT32 pb_v3_cyc, pb_v3_cyc_cull, pb_v3_polys, pb_v3_culled, pb_v3_resolve, pb_v3_rec, pb_v3_recfail, pb_v3_cull[5], pb_v3_vchk, pb_v3_vbad, pb_v3_cullbad, pb_v3_runs, pb_v3_stfull, pb_v3_grow;


static void HWR_PBStFree(void)
{
	if (pb_stblk)
		Z_Free(pb_stblk);
	pb_stblk = NULL;
	pb_st = NULL;
	pb_sth = NULL;
	pb_nst = 0;
}

void HWR_PBStateFlush(void)
{
	HWR_PBStFree();
}

static int HWR_PBStAlloc(void)
{
	UINT8 *p;

	if (!pb_stblk)
	{
		pb_st = NULL;
		pb_sth = NULL;
		pb_nst = 0;
	}
	if (pb_st)
		return 1;
	p = Z_TryMallocAlign((size_t)HWR_ST_CAP * sizeof(hwr_pbs_t) + (size_t)HWR_ST_SLOTS * sizeof(UINT16), PU_HWRPLANE, &pb_stblk, 4);
	if (!p)
	{
		pb_stblk = NULL;
		return 0;
	}
	pb_st = (hwr_pbs_t *)p;
	pb_sth = (UINT16 *)(p + (size_t)HWR_ST_CAP * sizeof(hwr_pbs_t));
	memset(pb_sth, 0, (size_t)HWR_ST_SLOTS * sizeof(UINT16));
	pb_nst = 0;
	return 1;
}

static inline UINT32 HWR_StHash(const GLMipmap_t *tex, UINT32 flags, UINT32 ex, INT32 target, const FSurfaceInfo *s)
{
	UINT32 h = (UINT32)(uintptr_t)tex * 0x9E3779B1u;

	h = (h ^ flags) * 0x85EBCA6Bu;
	h = (h ^ ex ^ ((UINT32)target << 12) ^ (s->PolyColor.rgba * 0x27D4EB2Fu)) * 0xC2B2AE35u;
	if (flags & PF_ColorMapped)
		h ^= (s->TintColor.rgba * 0x165667B1u) ^ (s->FadeColor.rgba * 0xD3A2646Cu) ^ (s->LightTableId * 0xFD7046C5u) ^ ((s->LightInfo.fade_start | (s->LightInfo.fade_end << 8)) * 0xB55A4F09u);
	return (h * 0x9E3779B1u) >> 22; // (1024 slots)
}

static inline int HWR_StEq(const hwr_pbs_t *t, const FSurfaceInfo *s, const GLMipmap_t *tex, UINT32 flags, UINT32 ex, INT32 target)
{
	UINT32 d = ((UINT32)(uintptr_t)t->texture ^ (UINT32)(uintptr_t)tex) | (t->polyFlags ^ flags) | (t->ex ^ ex) | (UINT32)(t->shader_target ^ target) | (t->surf.PolyColor.rgba ^ s->PolyColor.rgba);

	if (flags & PF_ColorMapped)
		d |= (t->surf.TintColor.rgba ^ s->TintColor.rgba) | (t->surf.FadeColor.rgba ^ s->FadeColor.rgba) | (t->surf.LightTableId ^ s->LightTableId)
			| (t->surf.LightInfo.fade_start ^ s->LightInfo.fade_start) | (t->surf.LightInfo.fade_end ^ s->LightInfo.fade_end);
	return d == 0;
}

// the state of a polygon that comes in whole (a world polygon: no sprite batch), found or made; HWR_ST_NONE when the table is full or there is no memory
static UINT32 HWR_PBIntern(const FSurfaceInfo *s, GLMipmap_t *tex, FBITFIELD flags, int shader_target, boolean horizon)
{
	const UINT32 ex = ((UINT32)shader_target & 0xFFu) | ((UINT32)horizon << 8);
	UINT32 h, e;
	hwr_pbs_t *t;

	if (!HWR_PBStAlloc())
		return HWR_ST_NONE;
	h = HWR_StHash(tex, flags, ex, shader_target, s);
	while ((e = pb_sth[h]) != 0)
	{
		if (HWR_StEq(&pb_st[e - 1], s, tex, flags, ex, shader_target))
			return e - 1;
		h = (h + 1) & (HWR_ST_SLOTS - 1);
	}
	if (pb_nst >= HWR_ST_CAP)
	{
		pb_v3_stfull++;
		return HWR_ST_NONE;
	}
	t = &pb_st[pb_nst];
	memset(t, 0, sizeof *t);
	t->surf.PolyColor = s->PolyColor;
	if (flags & PF_ColorMapped)
	{
		t->surf.TintColor = s->TintColor;
		t->surf.FadeColor = s->FadeColor;
		t->surf.LightTableId = s->LightTableId;
		t->surf.LightInfo.fade_start = s->LightInfo.fade_start;
		t->surf.LightInfo.fade_end = s->LightInfo.fade_end;
	}
	t->texture = tex;
	t->polyFlags = flags;
	t->ex = ex;
	t->shader_target = shader_target;
	pb_sth[h] = (UINT16)(++pb_nst);
	return pb_nst - 1;
}

// does the bucket of a polygon with that texture and flags hold the extent of s, t? (HWR_PBNew: b->needext)
static inline UINT32 HWR_PBNeedExt(const GLMipmap_t *tex, FBITFIELD flags)
{
	return (tex && !(flags & PF_NoTexture) && ((tex->flags & TF_WRAPXY) || (flags & (PF_ForceWrapX | PF_ForceWrapY)))) ? 1u : 0u;
}

// Called right after a polygon was collected whole while the cache records: the record gets the block (state, light, extent, vertices) instead of the call.
static void HWR_PBRecLast(const FSurfaceInfo *s, const FOutVector *v, FUINT n, FBITFIELD flags, int shader_target, boolean horizon)
{
	GLMipmap_t *const tex = current_texture;
	UINT32 st;
	const hwr_qw_t *blk;
	UINT32 ext;

	if (!pb_n || n > 250u || (st = HWR_PBIntern(s, tex, flags, shader_target, horizon)) == HWR_ST_NONE)
	{
		pb_v3_recfail++;
		HWR_GCRecPoly(s, v, n, flags, shader_target, horizon);
		return;
	}
	blk = pb_pool + pb_ent[pb_n - 1].blk;
	ext = HWR_PBNeedExt(tex, flags);
	pb_v3_rec++;
	HWR_GCRecBlk(st, (flags & PF_ColorMapped) ? blk[0].w[1] : 0u, &blk[1].f[0], ext, v, n);
}

// The bucket (of this batch) of an interned state for a polygon of the light level `light` (the float of the header word): the lookup HWR_PBAdd makes for a polygon that comes in whole.
static UINT32 __attribute__((noinline)) HWR_PBStBucket(hwr_pbs_t *st, UINT32 light)
{
	FSurfaceInfo s = st->surf;
	GLMipmap_t *const tex = st->texture;
	const UINT32 flags = st->polyFlags, ex = st->ex;
	UINT32 h1, ci, e, bi;
	union { UINT32 u; float f; } c;

	c.u = light;
	s.LightInfo.light_level = (FUINT)(INT32)c.f;
	h1 = HWR_PBH1(tex, flags, s.PolyColor.rgba);
	ci = h1 >> 23;
	e = pb_cache[ci];
	pb_v3_resolve++;
	if (e && e <= pb_nb && HWR_PBMatch(&pb_bk[e - 1], &s, tex, flags, ex))
		bi = e - 1;
	else
		bi = HWR_PBSlow(&s, tex, flags, ex, st->shader_target, h1, ci, 0);
	st->bser = pb_serial; // (after: a flush made inside starts another batch)
	st->bucket = (UINT16)bi;
	st->blight = light;
	st->bcare = pb_bk[bi].lmask ? 1u : 0u;
	return bi;
}

// the pool and the buckets have room for a record of np polygons and nv vertices (grows the pool; false: the replay is not started - an allocation is where the zone may take the cache back)
boolean HWR_PBRoomFor(UINT32 np, UINT32 nv)
{
	const UINT32 need = 2u * (np + nv);

	if (!currently_batching || !pb_pool)
		return false;
	if (pb_nb + np + 2u >= HWR_PB_BMAX)
		return false;
	if (pb_wr + need > pb_cap)
	{
		pb_v3_grow++;
		if (!HWR_PBGrow(need, pb_wr != 0))
			return false;
	}
	return true;
}

UINT32 HWR_PBCount(void)
{
	return pb_n;
}

#define HWR_PBBoxOut HWR_FboxOut // (hw_batching.h)

// the extent of s, t of the n vertices of a record (x y z s t): the words PBPut makes
static void HWR_PBExtent(const UINT32 *w, UINT32 n, float *o)
{
	UINT32 k;
	float smin, smax, tmin, tmax;

	smin = smax = hwr_i2f(w[3]);
	tmin = tmax = hwr_i2f(w[4]);
	for (k = 1; k < n; k++)
	{
		const float sf = hwr_i2f(w[5 * k + 3]), tf = hwr_i2f(w[5 * k + 4]);

		smin = hwr_minf(smin, sf);
		smax = hwr_maxf(smax, sf);
		tmin = hwr_minf(tmin, tf);
		tmax = hwr_maxf(tmax, tf);
	}
	o[0] = smin;
	o[1] = tmin;
	o[2] = smax - smin;
	o[3] = tmax - tmin;
}

// the polygon of a record op as the engine's call (for the hash of -hwpolyhash, and for the exact test of what the box test left out)
static void HWR_PBOpPoly(const gcbk_t *h, const hwr_pbs_t *st, FSurfaceInfo *s, FOutVector *v)
{
	union { UINT32 u; float f; } c;
	UINT32 k;

	*s = st->surf;
	c.u = h->light;
	s->LightInfo.light_level = (FUINT)(INT32)c.f;
	for (k = 0; k < h->n; k++)
	{
		const float *w = (const float *)(const void *)(h + 1);

		v[k].x = w[5 * k];
		v[k].y = w[5 * k + 1];
		v[k].z = w[5 * k + 2];
		v[k].s = w[5 * k + 3];
		v[k].t = w[5 * k + 4];
	}
}

// The polygons of a record that follow each other from p, as HWR_ProcessPolygon would have collected them (see above). Returns the first op that is not one. `view`: the view of the cache.
void HWR_PBNoCull(int on)
{
	pb_v3_nocull = on;
}

// The box test costs about as much as it saves where few polygons are outside (D1: a quarter of them, break-even), and saves much where many are (D4: half): the gate looks at the share of
// the polygons it culled in 16 views and, under a quarter of them, leaves the test out for the next 64 views (then it looks again). -hwgv 8: always on.
static UINT32 pb_cg_tests, pb_cg_culls, pb_cg_views, pb_cg_off;

void HWR_PBCullView(void)
{
	if (pb_cg_off)
	{
		if (!--pb_cg_off)
			pb_cg_views = pb_cg_tests = pb_cg_culls = 0;
		return;
	}
	if (++pb_cg_views >= 16)
	{
		if (pb_cg_tests >= 64 && pb_cg_culls * 100u < pb_cg_tests * 24u && !(hwr_gv_off & 8))
			pb_cg_off = 64;
		pb_cg_views = pb_cg_tests = pb_cg_culls = 0;
	}
}

const UINT8 *HWR_GCReplayRun(const UINT8 *p, const UINT8 *end, UINT32 view, GLMipmap_t **lasttex)
{
	const int hash = hwr_ph_on > 0;
	const int cull = hwr_fbox_on && !(hwr_gv_off & 2) && !(hash && !hwr_ph_vis) && !(hwr_ph_on == 2) && !pb_v3_nocull && (!pb_cg_off || hash);
	GLMipmap_t *last = NULL;
	const UINT32 rt0 = ps2hwp_now();

	pb_v3_runs++;
	while (p < end && *p == GCOP_BLK)
	{
		const gcbk_t *const h = (const gcbk_t *)p;
		hwr_pbs_t *const st = &pb_st[h->state];
		const UINT32 n = h->n, need = 2u + 2u * n;
		const UINT32 lightbits = h->light;
		hwr_pbk_t *b;
		UINT32 bi;

		last = st->texture;
		if (cull && !(h->fl & 1))
		{
			const UINT32 pl = HWR_PBBoxOut(h->bb);

			pb_cg_tests++;
			if (pl)
			{
				pb_cg_culls++;
				pb_v3_culled++;
				pb_v3_cull[pl - 1]++;
				if (hash && hwr_ph_vis)
				{
					FSurfaceInfo fs;
					FOutVector fv[250];

					HWR_PBOpPoly(h, st, &fs, fv);
					if (!HWR_FrPolyOutside(fv, n))
					{
						pb_v3_cullbad++;
						if (pb_v3_cullbad <= 8)
							I_OutputMsg("HWGV CULL BAD: plane %u box %d..%d %d..%d %d..%d is not wholly outside by the exact test\n", (unsigned)pl, (int)h->bb[0], (int)h->bb[1], (int)h->bb[2], (int)h->bb[3], (int)h->bb[4], (int)h->bb[5]);
					}
				}
				p += h->size;
				continue;
			}
		}
		if (st->tview != view)
		{
			GLMipmap_t *const m = st->texture;

			st->tview = view;
			if (m && m->ps2_gcv != view)
			{
				m->ps2_gcv = view;
				PS2HWD_TouchTexture(m);
				Z_ChangeTag(m->data, PU_HWRCACHE_UNLOCKED);
			}
		}
		if (hash)
		{
			FSurfaceInfo fs;
			FOutVector fv[250];

			HWR_PBOpPoly(h, st, &fs, fv);
			current_texture = st->texture;
			HWR_PolyHashAdd(&fs, fv, n, st->polyFlags, st->shader_target, (boolean)((st->ex >> 8) & 1u));
		}
		for (;;)
		{
			if (st->bser == pb_serial && (!st->bcare || st->blight == lightbits))
				bi = st->bucket;
			else
				bi = HWR_PBStBucket(st, lightbits);
			if (pb_wr + need <= pb_cap)
				break;
			pb_nfail++; // (the room was made before the replay started: cannot happen; the batch is cut as HWR_PBAdd cuts it)
			if (!HWR_PBGrow(need, pb_wr != 0))
				HWR_PBFlushEarly();
		}
		b = &pb_bk[bi];
		{
			const UINT32 pidx = pb_n, blk = pb_wr;
			hwr_qw_t *const o = pb_pool + blk, *const d = o + 2;
			typedef UINT32 __attribute__((may_alias)) hwr_word_t;
			const hwr_word_t *const w = (const hwr_word_t *)(const void *)(h + 1);

#define HWR_PBBAR2 __asm__ volatile("" ::: "memory")
#define HWR_PBR(i) do { const hwr_word_t *q_ = w + 5 * (i); hwr_qw_t *e_ = d + 2 * (i); const UINT32 x_ = q_[0], y_ = q_[1], z_ = q_[2], s_ = q_[3], t_ = q_[4]; \
		e_[0].w[0] = x_; HWR_PBBAR2; e_[0].w[1] = y_; HWR_PBBAR2; e_[0].w[2] = z_; HWR_PBBAR2; e_[1].w[0] = s_; HWR_PBBAR2; e_[1].w[1] = t_; HWR_PBBAR2; } while (0)
			if (n == 4)
			{
				HWR_PBR(0);
				HWR_PBR(1);
				HWR_PBR(2);
				HWR_PBR(3);
			}
			else if (n == 3)
			{
				HWR_PBR(0);
				HWR_PBR(1);
				HWR_PBR(2);
			}
			else
			{
				UINT32 k;

				for (k = 0; k < n; k++)
					HWR_PBR(k);
			}
#undef HWR_PBR
#undef HWR_PBBAR2
			o[0].w[0] = n;
			o[0].w[1] = h->light;
			if (b->needext)
			{
				if (h->ext)
				{
					o[1].f[0] = h->ex[0];
					o[1].f[1] = h->ex[1];
					o[1].f[2] = h->ex[2];
					o[1].f[3] = h->ex[3];
				}
				else
					HWR_PBExtent((const UINT32 *)(const void *)w, n, &o[1].f[0]); // (the texture repeats now and did not when the record was made)
			}
			else
				o[1].f[0] = o[1].f[1] = o[1].f[2] = o[1].f[3] = 0.0f;
			pb_ent[pidx].blk = blk;
			pb_ent[pidx].next = HWR_PB_END;
			if (b->cnt)
				pb_ent[b->tail].next = pidx;
			else
				b->head = pidx;
			b->tail = pidx;
			b->cnt++;
			if (b->planwant)
			{
				pb_plan[pb_nplan].blk = blk;
				pb_plan[pb_nplan].bucket = bi;
				pb_nplan++;
			}
			pb_n = pidx + 1;
			pb_wr = blk + need;
		}
		pb_v3_polys++;
		p += h->size;
	}
	if (last)
	{
		current_texture = last;
		*lasttex = last;
	}
	pb_v3_cyc += ps2hwp_now() - rt0;
	return p;
}

// The check mode of the cache (hw_gcache.inc, -hwgv 4): the polygons [a, a + k) were collected by the replay of a record, [a + k, a + 2k) by the calculation of the same seg or plane; they must be the
// same blocks in the same buckets. Returns the number of differences (the first ones are printed).
static UINT32 HWR_PBBucketOf(UINT32 idx)
{
	UINT32 i, p;

	for (i = 0; i < pb_nb; i++)
		for (p = pb_bk[i].head; p != HWR_PB_END; p = pb_ent[p].next)
			if (p == idx)
				return i;
	return 0xFFFFFFFFu;
}

UINT32 HWR_GCVerifyPool(UINT32 a, UINT32 b, UINT32 c, const char *what, UINT32 id)
{
	UINT32 i, bad = 0, k = b - a;

	if (a > b || b > c || c > pb_n)
		return 0; // (the batch was cut in between)
	if (c - b != k)
	{
		pb_v3_vbad++;
		if (pb_v3_vbad <= 16)
			I_OutputMsg("HWGV MISMATCH %s %u: the replay collected %u polygons, the calculation %u\n", what, (unsigned)id, (unsigned)k, (unsigned)(c - b));
		return 1;
	}
	for (i = 0; i < k; i++)
	{
		const hwr_qw_t *x = pb_pool + pb_ent[a + i].blk, *y = pb_pool + pb_ent[b + i].blk;
		const UINT32 nx = x[0].w[0], ny = y[0].w[0];
		UINT32 j, bx, by;
		int diff = 0;

		pb_v3_vchk++;
		if (nx != ny)
			diff = 1;
		else
		{
			bx = HWR_PBBucketOf(a + i);
			by = HWR_PBBucketOf(b + i);
			if (bx == 0xFFFFFFFFu || by == 0xFFFFFFFFu || (bx != by && pb_bk[bx].texture && !(pb_bk[bx].polyFlags & PF_NoTexture))) // (a polygon without a texture is a bucket of its own: the pair is two buckets)
			{
				diff = 2;
				if (pb_v3_vbad < 6 && bx < pb_nb && by < pb_nb)
				{
					const hwr_pbk_t *q1 = &pb_bk[bx], *q2 = &pb_bk[by];

					I_OutputMsg("HWGV buckets %u / %u: tex %d/%d vs %d/%d flags %x vs %x ex %x vs %x pc %08x vs %08x tint %08x/%08x fade %08x/%08x ltid %u/%u light %u/%u fs %u/%u fe %u/%u lmask %x/%x\n", (unsigned)bx, (unsigned)by,
						q1->texture ? (int)q1->texture->regen_kind : -1, q1->texture ? (int)q1->texture->regen_id : -1, q2->texture ? (int)q2->texture->regen_kind : -1, q2->texture ? (int)q2->texture->regen_id : -1,
						(unsigned)q1->polyFlags, (unsigned)q2->polyFlags, (unsigned)q1->ex, (unsigned)q2->ex, (unsigned)q1->surf.PolyColor.rgba, (unsigned)q2->surf.PolyColor.rgba,
						(unsigned)q1->surf.TintColor.rgba, (unsigned)q2->surf.TintColor.rgba, (unsigned)q1->surf.FadeColor.rgba, (unsigned)q2->surf.FadeColor.rgba, (unsigned)q1->surf.LightTableId, (unsigned)q2->surf.LightTableId,
						(unsigned)q1->surf.LightInfo.light_level, (unsigned)q2->surf.LightInfo.light_level, (unsigned)q1->surf.LightInfo.fade_start, (unsigned)q2->surf.LightInfo.fade_start,
						(unsigned)q1->surf.LightInfo.fade_end, (unsigned)q2->surf.LightInfo.fade_end, (unsigned)q1->lmask, (unsigned)q2->lmask);
				}
			}
			else
			{
				if (pb_bk[bx].polyFlags & PF_ColorMapped)
					diff |= (x[0].w[1] != y[0].w[1]) ? 4 : 0; // (the light level of a polygon that is not lit by the shader is stack garbage in the call)
				if (pb_bk[bx].needext)
					for (j = 0; j < 4; j++)
						diff |= (x[1].w[j] != y[1].w[j]) ? 8 : 0;
				for (j = 0; j < nx; j++)
					diff |= (x[2 + 2 * j].w[0] != y[2 + 2 * j].w[0] || x[2 + 2 * j].w[1] != y[2 + 2 * j].w[1] || x[2 + 2 * j].w[2] != y[2 + 2 * j].w[2]
						|| x[3 + 2 * j].w[0] != y[3 + 2 * j].w[0] || x[3 + 2 * j].w[1] != y[3 + 2 * j].w[1]) ? 16 : 0;
			}
		}
		if (diff)
		{
			bad++;
			pb_v3_vbad++;
			if (pb_v3_vbad <= 16)
				I_OutputMsg("HWGV MISMATCH %s %u polygon %u of %u: diff bits %d (n %u / %u)\n", what, (unsigned)id, (unsigned)i, (unsigned)k, diff, (unsigned)nx, (unsigned)ny);
		}
	}
	return bad;
}

// HWPROF50: the collection of a window of frames (called by the driver's profile print)
void HWR_PBProf(unsigned int frames)
{
	if (!frames)
		frames = 1;
	I_OutputMsg("HWPROF50 collection: polygons=%u in %u batches (%u buckets) | general path=%u (cache missed: slow=%u found=%u created=%u) | batches drawn=%u | planned polygons=%u | pool %u quadwords, cut for room=%u\n", pb_npolys / frames, pb_nflush / frames, pb_nbuckets / frames, pb_ngen / frames,
		pb_nslow / frames, pb_nfind / frames, pb_ncreate / frames, pb_nbatches / frames, pb_nplanned / frames, pb_cap, pb_nfail);
	pb_nfail = 0;
	pb_nplanned = pb_ngen = pb_nslow = pb_nfind = pb_ncreate = pb_nbuckets = pb_npolys = pb_nflush = pb_nbatches = 0;
#ifdef IR_CENSUS
	{
		extern UINT32 ir_cen[16];
		I_OutputMsg("HWCEN segs=%u polyseg=%u ffloor=%u slope=%u heightsec=%u lights=%u trans=%u | planes=%u slope=%u ffloor=%u trans=%u\n", ir_cen[0], ir_cen[1], ir_cen[2], ir_cen[3], ir_cen[4], ir_cen[5], ir_cen[6], ir_cen[8], ir_cen[9], ir_cen[10], ir_cen[11]);
	}
#endif
	I_OutputMsg("HWPROF70 stored records v3: blocks=%u culled=%u (planes right=%u left=%u top=%u bottom=%u near=%u gate off %u) resolved=%u runs=%u | recorded=%u not=%u stfull=%u states=%u grew=%u | verify=%u bad=%u cullbad=%u | cycles run=%u cull=%u\n",
		pb_v3_polys / frames, pb_v3_culled / frames, pb_v3_cull[0] / frames, pb_v3_cull[1] / frames, pb_v3_cull[2] / frames, pb_v3_cull[3] / frames, pb_v3_cull[4] / frames, pb_cg_off,
		pb_v3_resolve / frames, pb_v3_runs / frames, pb_v3_rec, pb_v3_recfail, pb_v3_stfull, pb_nst, pb_v3_grow, pb_v3_vchk, pb_v3_vbad, pb_v3_cullbad, pb_v3_cyc / frames, pb_v3_cyc_cull / frames);
	{
		extern UINT32 hwr_plc[4], hwr_plcyc[4], hwr_plkey, hwr_plhit, hwr_plhitn;

		I_OutputMsg("HWPROF71 planes by kind a frame: sector %u (%u cycles) 3dfloor %u (%u) translucent %u (%u) translucent 3dfloor %u (%u)\n", hwr_plc[0] / frames, hwr_plcyc[0] / frames, hwr_plc[1] / frames, hwr_plcyc[1] / frames,
			hwr_plc[2] / frames, hwr_plcyc[2] / frames, hwr_plc[3] / frames, hwr_plcyc[3] / frames);
		I_OutputMsg("HWPROF72 planes served by the cache a frame: %u, key %u cycles, replay %u cycles\n", hwr_plhitn / frames, hwr_plkey / frames, hwr_plhit / frames);
		hwr_plkey = hwr_plhit = hwr_plhitn = 0;
		hwr_plc[0] = hwr_plc[1] = hwr_plc[2] = hwr_plc[3] = hwr_plcyc[0] = hwr_plcyc[1] = hwr_plcyc[2] = hwr_plcyc[3] = 0;
	}
	pb_v3_polys = pb_v3_culled = pb_v3_resolve = pb_v3_runs = pb_v3_grow = pb_v3_cyc = pb_v3_cyc_cull = 0;
	pb_v3_cull[0] = pb_v3_cull[1] = pb_v3_cull[2] = pb_v3_cull[3] = pb_v3_cull[4] = 0;
}
