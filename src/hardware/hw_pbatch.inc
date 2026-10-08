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
static UINT32 pb_ngen, pb_nslow, pb_nfind, pb_ncreate, pb_nbuckets, pb_npolys, pb_nflush, pb_nbatches, pb_nplanned; // HWPROF50 (per window)

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
	memset(pb_tab, 0, sizeof pb_tab);
	memset(pb_xv, 0, sizeof pb_xv);
	memset(pb_cache, 0, sizeof pb_cache);
	pb_tab_dirty = pb_x_dirty = 0;
}

static void HWR_PBFree(void)
{
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
static void __attribute__((noinline)) HWR_PBGrow(UINT32 need)
{
	if (pb_wr + need > pb_cap)
	{
		const UINT32 c = (UINT32)HWR_BatchCapacity((int)pb_cap, (int)(pb_wr + need), sizeof(hwr_qw_t) + (sizeof(hwr_pbe_t) + sizeof(hwr_pbp_t) + sizeof(UINT32)) / 8);
		const UINT32 e = (c + 7) / 8;

		pb_pool = HWR_BatchResize(pb_pool, (size_t)c * sizeof(hwr_qw_t));
		pb_ent = HWR_BatchResize(pb_ent, (size_t)e * sizeof(hwr_pbe_t));
		pb_plan = HWR_BatchResize(pb_plan, (size_t)e * sizeof(hwr_pbp_t));
		pb_walk = HWR_BatchResize(pb_walk, (size_t)e * sizeof(UINT32));
		pb_cap = c;
		pb_ecap = e;
	}
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
	if (pb_wr + need > pb_cap)
		HWR_PBGrow(need);
	HWR_PBPut(&pb_bk[bi], bi, s, v, n, need);
}

// The same for the common polygon (a state that the cache holds, room in the arrays, no sprite batch): inline in HWR_ProcessPolygon, which then needs no frame. Returns 0 when it
// did not take the polygon.
static inline __attribute__((always_inline)) int HWR_PBFast(const FSurfaceInfo *s, const FOutVector *v, FUINT n, FBITFIELD flags, int shader_target, boolean horizon)
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

		if (!HWR_PBMatch(b, s, tex, flags, ((UINT32)shader_target & 0xFFu) | ((UINT32)horizon << 8)))
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

// HWPROF50: the collection of a window of frames (called by the driver's profile print)
void HWR_PBProf(unsigned int frames)
{
	if (!frames)
		frames = 1;
	I_OutputMsg("HWPROF50 collection: polygons=%u in %u batches (%u buckets) | general path=%u (cache missed: slow=%u found=%u created=%u) | batches drawn=%u | planned polygons=%u\n", pb_npolys / frames, pb_nflush / frames, pb_nbuckets / frames, pb_ngen / frames,
		pb_nslow / frames, pb_nfind / frames, pb_ncreate / frames, pb_nbatches / frames, pb_nplanned / frames);
	pb_nplanned = pb_ngen = pb_nslow = pb_nfind = pb_ncreate = pb_nbuckets = pb_npolys = pb_nflush = pb_nbatches = 0;
}
