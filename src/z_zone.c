// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2006      by Graue.
// Copyright (C) 2006-2023 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  z_zone.c
/// \brief Zone memory allocation.
///        This file does zone memory allocation. Each allocation is done with a
///        tag, and this file keeps track of all the allocations made. Later, you
///        can purge everything with a given tag.
///
///        Some tags (PU_CACHE, for example) may be automatically purged whenever
///        the space is needed, so memory allocated with these tags is no longer
///        guaranteed to be valid after another call to Z_Malloc().
///
///        The original implementation allocated a large block (48 MB, as of the
///        last version of SRB2 that did this) upfront, and Z_Malloc() carved
///        pieces out of that. Unfortunately, this had the effect of masking a
///        lot of read/write past end of buffer type bugs which we have since
///        caught with this direct-malloc version. We also suspected that SRB2's
///        allocator was fragmenting badly. Finally, this version is a bit
///        simpler (about half the lines of code).

#include "doomdef.h"
#include "doomstat.h"
#include "r_patch.h"
#include "r_picformats.h"
#include "i_system.h" // I_GetFreeMem
#include "i_video.h" // rendermode
#include "z_zone.h"
#include "m_misc.h" // M_Memcpy
#include "lua_script.h"

#ifdef HWRENDER
#include "hardware/hw_main.h" // For hardware memory info
#endif

#ifdef HAVE_VALGRIND
#include "valgrind.h"
static boolean Z_calloc = false;
#include "memcheck.h"
#endif

#ifdef PS2
#include "ps2/ps2_mem.h"
#include "m_argv.h"
#endif

#define ZONEID 0xa441d13d

#ifdef ZDEBUG
//#define ZDEBUG2
#endif

#ifdef PS2
// Zone on the PS2: one arena (ps2/ps2_mem.c). PU_CACHE blocks that have an owner are evicted least-recently-used
// first when an allocation does not fit. A block's stamp is the frame it was allocated or touched in
// (Z_ChangeTag, Z_SetUser, Z_Touch); Z_NextFrame, called once per displayed frame, advances the frame, and a block
// stamped with the current frame is never evicted. While the 3D view renders (Z_PurgeLock) only untouched caches
// from earlier frames may go: the renderer must touch roots before holding pointers. The lock first tries to make one free block of
// Z_HEADROOM_DEFAULT bytes (cache of any age may go then: the view holds nothing yet).
// RAM profiles (PS2-OPT-01): 32 MB retail keeps the arena compact, a 128 MB Dev/TOOL console gets large reserves
// and a large headroom so that the frame is never short of one contiguous block; everything else follows from the arena size.
#define Z_RESERVE_DEFAULT (1536u << 10) // C heap left for libc malloc: stdio, GS/IOP buffers, per-frame malloc. PS2-77: was 2 MiB; MAP11 + music peak 911 KB (-zck libcpeak), the sweep of all maps <= 0.9 MB
#define Z_HEADROOM_DEFAULT (4u << 20) // one free block of this size is ensured when purging gets locked
#define Z_RESERVE_128 (8u << 20)
#define Z_HEADROOM_128 (16u << 20)
#define Z_EVICT_ALIGN_PAD 256u        // room for the 64-byte alignment and a header on top of the headroom
#define Z_EVICT_SLACK_DEFAULT (256u << 10) // evict this much beyond the request, so the next few allocations need none
#define Z_EVICT_SLACK_128 (2u << 20)
#define Z_EVICT_SLACK zslack
#define Z_MAKEROOM_MIN (16u << 10)   // from this size on an allocation that does not fit first looks for the cheapest run of neighbours to free
#define Z_AGE_BUCKETS 64
#define Z_FRAME_MASK 0xFFFFFFu

static UINT32 zframe;               // frame stamp, 24 bits
static boolean zframe_explicit;     // the platform layer calls Z_NextFrame; otherwise the 3D view lock does
static INT32 zpurgelock;            // >0 while the 3D view renders
static const zablock_t *zpinned;    // the block Z_ReallocAlign is copying from: neither evicted nor purged
static size_t zreserve = Z_RESERVE_DEFAULT;
static size_t zheadroom = Z_HEADROOM_DEFAULT;
static size_t zslack = Z_EVICT_SLACK_DEFAULT;
static boolean zheadroom_flush;     // the headroom may flush the cache when no run of neighbours makes it (large arenas only)
static UINT32 zflush_period, zflush_count;     // -zflush N: every N-th allocation first drops all cache older than this frame (test)
static UINT32 zflush_total;
// PS2-73: what the policy costs (EE cycles, COP0 Count deltas): Z_EnsureFree at the start of a 3D view, Z_EvictLRU / Z_MakeRoom when an allocation fails
typedef struct { UINT32 calls; uint64_t cycles; } zcost_t;
static zcost_t zcost_ensure, zcost_evict, zcost_room;
#define ZCOST_BEGIN() const UINT32 zcost_t0 = PS2Mem_Cycles()
#define ZCOST_END(c) do { (c).calls++; (c).cycles += (UINT32)(PS2Mem_Cycles() - zcost_t0); } while (0)
static UINT32 zreport_interval, zreport_count; // -zreport N: memory report every N frames, level exits log a line
#if defined(_EE) && defined(__GNUC__)
static size_t ztracemin; // -zcaller [bytes]: opt-in release diagnosis without a debug-sized arena; allocations and frees of at least that size (default 64 KiB)
#define Z_TRACE_CALLER(kind) do { if (ztracemin && size >= ztracemin) \
	I_OutputMsg("[zcaller] %s %lu tag %d caller %p\n", kind, (unsigned long)size, tag, __builtin_return_address(0)); } while (0)
#define Z_TRACE_FREE(block) do { if (ztracemin && !ZA_ISFREE(block) && ZA_SIZE(block) >= ztracemin) \
	I_OutputMsg("[zcaller] free %lu tag %d caller %p\n", (unsigned long)ZA_SIZE(block), ZA_TAG(block), __builtin_return_address(0)); } while (0)
#else
#define Z_TRACE_CALLER(kind) ((void)0)
#define Z_TRACE_FREE(block) ((void)0)
#endif
#ifdef ZDEBUG
static const char *zreqfile; // the allocation being served, for the OOM report
static INT32 zreqline;
#endif

#ifdef ZDEBUG
// -zfreetrace [min]: "[zalloc]"/"[zfreed]" lines for blocks of at least `min` bytes (default 4096)
static size_t Z_TraceMin(void)
{
	static long v = -1;

	if (v < 0)
	{
		v = 0;
		if (M_CheckParm("-zfreetrace"))
		{
			v = M_IsNextParm() ? atol(M_GetNextParm()) : 4096;
			if (v < 1)
				v = 1;
		}
	}
	return (size_t)v;
}
#endif

static UINT32 Z_Age(const zablock_t *block)
{
	return (zframe - ZA_STAMP(block)) & Z_FRAME_MASK;
}

// A cache block with an owner can go: the owner sees NULL and rebuilds. Blocks of the current frame are somebody's
// working set (a pointer held by the code that is running now) unless `current` says that nothing is held (3D view start).
// PS2-OPT-03: besides PU_CACHE, a sprite patch (PU_SPRITE) that is one self-contained block (r_patch.c) can go: every
// user of a sprite patch fetches it again with W_CachePatchNum, which also stamps it for the current frame.
boolean Patch_IsEvictable(const void *patch);
static boolean Z_Evictable(const zablock_t *block, boolean current)
{
	if (ZA_ISFREE(block) || block->user == NULL || block == zpinned || !(current || Z_Age(block) != 0))
		return false;
	return ZA_TAG(block) == PU_CACHE || (ZA_TAG(block) == PU_SPRITE && Patch_IsEvictable(ZA_PAYLOAD(block)));
}

// Long-lived data is carved from the top of the arena, what comes and goes with levels and caches from the bottom.
// PS2-63: the bulk of a level (arrays, BSP, things) is bottom, but once the level is loaded (Z_LevelPhase) the level blocks that
// come and go during play (mobjs, nodes, thinker data) are carved from the top with the other long-lived data. At the bottom they sat
// between cache blocks and temporaries at the frontier, and every cache block that went left a hole between two level blocks
// (MAP11 frame 49: 27 holes of 16..72 KB, largest free block 396 KB of 1.5 MB free): cache and long-lived data now grow from opposite ends.
static boolean zlevel_play;
// PS2-75: the frontier. Once the level is built (play phase) the free middle of the arena is split in two zones by an address: the short-lived
// side (caches, renderer work: they come and go, evicted ones leave holes) below it, the long-lived side (mobjs, thinker data, statics) above.
// A long-lived block that was carved from the highest hole that fitted ended up inside the cache zone as soon as the free middle was used up
// (MAP11: vissprite chunks and 48-byte nodes among the holes of evicted caches: free space 1.6 MB, largest block 465 KB, no 512 KB flat).
// Each zone grows into the other by moving the frontier (Z_MoveFrontier), evicting the caches next to it when the long-lived side needs room.
#define Z_FRONT_MIN_MIDDLE (1u << 20)   // a free middle below this gets no zones
#define Z_FRONT_TOP_MIN (256u << 10)    // the long-lived zone starts with 1/8 of the middle, 256 KB..1 MB
#define Z_FRONT_TOP_MAX (1u << 20)
#define Z_FRONT_STEP (96u << 10)        // the frontier moves this much beyond the request, so that the next few requests need no move
static UINT32 zfront_moves_down, zfront_moves_up, zfront_fallbacks;

static void Z_PlaceFrontier(void)
{
	zablock_t *middle = ZA_LargestFreeBlock();
	size_t top;

	if (!middle || ZA_SIZE(middle) < Z_FRONT_MIN_MIDDLE)
	{
		ZA_SetFrontier(NULL);
		return;
	}
	top = ZA_SIZE(middle) / 8;
	if (top < Z_FRONT_TOP_MIN)
		top = Z_FRONT_TOP_MIN;
	if (top > Z_FRONT_TOP_MAX)
		top = Z_FRONT_TOP_MAX;
	ZA_SetFrontier((uint8_t *)middle + ZA_SIZE(middle) - top);
}

void Z_LevelPhase(boolean playing)
{
	zlevel_play = playing;
	if (playing)
		Z_PlaceFrontier();
	else
		ZA_SetFrontier(NULL);
}

static int Z_SideForTag(INT32 tag)
{
	switch (tag)
	{
		case PU_LEVEL: case PU_LEVSPEC:
			return zlevel_play ? ZA_TOP : ZA_BOTTOM;
		case PU_CACHE: case PU_RENDERWORK: case PU_SPRITE:
			// the transient blocks take the end the level does not grow from. PS2-72: sprite patches are evictable like the caches (Z_Evictable);
			// among the long-lived blocks at the other end every one that was evicted left a hole that small long-lived blocks then pinned
			// (MAP11: 12 holes of 16..72 KB, each with a 48-byte level block behind it, 550 KB that no 512 KB texture flat could use)
			return zlevel_play ? ZA_BOTTOM : ZA_TOP;
		case PU_STATIC: case PU_LUA: case PU_SOUND: case PU_MUSIC:
		case PU_PATCH: case PU_PATCH_DATA: case PU_HUDGFX:
			return ZA_TOP;
		default:
			return ZA_BOTTOM;
	}
}

// PS2-76: the cache registry. Every walk of the whole arena touches each block header (a cache miss each: MAP11 has 43 000 blocks, 2 M cycles a walk),
// and the per-frame headroom check, every eviction and every make-room did one or two of them: 10..60% of a frame under memory pressure.
// The candidates of the eviction (PU_CACHE and PU_SPRITE blocks, Z_Evictable decides) are kept in an address-sorted array instead, so the walks visit
// only them, in the same order as the arena walk did (the age ties and the windows of Z_MakeRoom come out the same). When the array overflows the walks
// fall back to the arena (zreg_ok false until Z_RegRebuild).
#define Z_REG_MAX 6144
static zablock_t *zreg[Z_REG_MAX];
static UINT32 zreg_n;
static boolean zreg_ok = true;
static UINT32 zreg_overflows;
static boolean zpurge_maybe; // some block may carry a tag >= PU_PURGELEVEL (set when one is tagged; the purge walk clears it)

static boolean Z_RegTag(INT32 tag)
{
	return tag == PU_CACHE || tag == PU_SPRITE;
}

static UINT32 Z_RegLower(const zablock_t *block) // first index whose address is >= block
{
	UINT32 lo = 0, hi = zreg_n;

	while (lo < hi)
	{
		const UINT32 mid = (lo + hi) >> 1;

		if ((uintptr_t)zreg[mid] < (uintptr_t)block)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void Z_RegAdd(zablock_t *block)
{
	UINT32 i;

	if (!zreg_ok)
		return;
	i = Z_RegLower(block);
	if (i < zreg_n && zreg[i] == block)
		return;
	if (zreg_n == Z_REG_MAX)
	{
		zreg_ok = false;
		zreg_overflows++;
		return;
	}
	memmove(&zreg[i + 1], &zreg[i], (zreg_n - i) * sizeof zreg[0]);
	zreg[i] = block;
	zreg_n++;
}

static void Z_RegRemove(zablock_t *block)
{
	UINT32 i;

	if (!zreg_ok)
		return;
	i = Z_RegLower(block);
	if (i < zreg_n && zreg[i] == block)
	{
		zreg_n--;
		memmove(&zreg[i], &zreg[i + 1], (zreg_n - i) * sizeof zreg[0]);
	}
}

// One walk of the arena (level changes only): the array is right again whatever happened to it.
static void Z_RegRebuild(void)
{
	zablock_t *b;

	zreg_n = 0;
	zreg_ok = true;
	for (b = ZA_First(); b && zreg_ok; b = ZA_Next(b))
		if (!ZA_ISFREE(b) && Z_RegTag(ZA_TAG(b)))
			Z_RegAdd(b);
}

// Every place that gives a used block a tag: keeps the array and the purge flag right.
static void Z_NoteTag(zablock_t *block, INT32 oldtag, INT32 newtag)
{
	if (oldtag != newtag)
	{
		if (Z_RegTag(oldtag))
			Z_RegRemove(block);
		if (Z_RegTag(newtag))
			Z_RegAdd(block);
	}
	if (newtag >= PU_PURGELEVEL)
		zpurge_maybe = true;
}

// Frees one block; returns the address just past the free block it became part of.
static void *Z_FreeBlock(zablock_t *block)
{
	void *ptr = ZA_PAYLOAD(block);

	// anything that isn't by lua gets passed to lua just in case.
	if (ZA_TAG(block) != PU_LUA)
		LUA_InvalidateUserdata(ptr);

	// Clear the user's mark.
	if (block->user != NULL)
		*block->user = NULL;

	if (Z_RegTag(ZA_TAG(block)))
		Z_RegRemove(block);
	return ZA_Free(ptr);
}

// Frees every block with a tag in range. Never checks the heap, so allocation can call it under pressure.
static void Z_FreeTagRange(INT32 lowtag, INT32 hightag)
{
	zablock_t *block;
	const boolean purge = lowtag >= PU_PURGELEVEL && hightag >= ZA_MAXTAG;

	if (purge && !zpurge_maybe)
		return; // PS2-76: no block was ever tagged purgable since the last walk
	block = ZA_First();
	while (block)
	{
		uint8_t *after = (uint8_t *)block + ZA_SIZE(block); // taken before the block goes away

		if (!ZA_ISFREE(block) && ZA_TAG(block) >= lowtag && ZA_TAG(block) <= hightag && block != zpinned)
			block = ZA_BlockAt(Z_FreeBlock(block));
		else
			block = ZA_BlockAt(after);
	}
	if (purge)
		zpurge_maybe = zpinned && ZA_TAG(zpinned) >= PU_PURGELEVEL; // (only the pinned realloc source can have survived)
}

// The eviction candidates in address order: the registry, or every block of the arena when the registry overflowed.
static zablock_t *Z_CandFirst(UINT32 *ci)
{
	*ci = 0;
	if (!zreg_ok)
		return ZA_First();
	return zreg_n ? zreg[0] : NULL;
}

static zablock_t *Z_CandNext(zablock_t *b, UINT32 *ci)
{
	if (!zreg_ok)
		return ZA_Next(b);
	return ++*ci < zreg_n ? zreg[*ci] : NULL;
}

// Evicts the oldest PU_CACHE blocks that have an owner (it sees NULL and rebuilds on demand), at least `want`
// bytes when that much is evictable. Blocks of the current frame stay, unless `current`. False when nothing was evictable.
static boolean Z_EvictLRUWalk(size_t want, boolean current)
{
	size_t hist[Z_AGE_BUCKETS] = {0}, have = 0, atlimit = SIZE_MAX;
	INT32 age, limit = -1;
	const INT32 minage = current ? 0 : 1;
	zablock_t *block;
	boolean any = false;
	UINT32 ci;

	for (block = Z_CandFirst(&ci); block; block = Z_CandNext(block, &ci))
		if (Z_Evictable(block, current))
		{
			age = (INT32)Z_Age(block);
			hist[age < Z_AGE_BUCKETS ? age : Z_AGE_BUCKETS - 1] += ZA_SIZE(block);
			any = true;
		}
	if (!any)
		return false;

	// the age from which on (older) the blocks add up to `want`: all older ones go, and as many of this age as needed;
	// when they never add up, everything evictable goes
	for (age = Z_AGE_BUCKETS - 1; age >= minage; age--)
	{
		have += hist[age];
		if (have >= want)
		{
			limit = age;
			atlimit = want - (have - hist[age]);
			break;
		}
	}
	// Resolve the overflow bucket by exact age, rather than treating everything older than 62 frames as a tie.
	if (limit == Z_AGE_BUCKETS - 1)
	{
		UINT32 prefix = 0, mask = 0;
		INT32 shift;
		atlimit = want;
		// Four six-bit radix passes reuse the histogram: bounded scans, no allocation or per-victim search.
		for (shift = 18; shift >= 0; shift -= 6)
		{
			memset(hist, 0, sizeof hist);
			for (block = Z_CandFirst(&ci); block; block = Z_CandNext(block, &ci))
				if (Z_Evictable(block, current) && Z_Age(block) >= Z_AGE_BUCKETS - 1 && (Z_Age(block) & mask) == prefix)
					hist[(Z_Age(block) >> shift) & (Z_AGE_BUCKETS - 1)] += ZA_SIZE(block);
			for (age = Z_AGE_BUCKETS - 1; age > 0; age--)
			{
				if (hist[age] >= atlimit)
					break;
				atlimit -= hist[age];
			}
			prefix |= (UINT32)age << shift;
			mask |= (Z_AGE_BUCKETS - 1u) << shift;
		}
		limit = (INT32)prefix;
	}

	if (zreg_ok)
	{
		// the registry is address-sorted like the arena walk: the same blocks of the limit age go first; a freed block leaves the array, so the index stays
		for (ci = 0; ci < zreg_n;)
		{
			boolean victim = false;

			block = zreg[ci];
			if (Z_Evictable(block, current))
			{
				age = (INT32)Z_Age(block);
				if (limit < 0 || age > limit)
					victim = true;
				else if (age == limit && atlimit)
				{
					victim = true;
					atlimit = ZA_SIZE(block) < atlimit ? atlimit - ZA_SIZE(block) : 0;
				}
			}
			if (victim)
			{
				ZA_NoteEvict(ZA_SIZE(block));
				Z_FreeBlock(block);
			}
			else
				ci++;
		}
		return true;
	}
	for (block = ZA_First(); block;)
	{
		uint8_t *after = (uint8_t *)block + ZA_SIZE(block);
		boolean victim = false;

		if (Z_Evictable(block, current))
		{
			age = (INT32)Z_Age(block);
			if (limit < 0 || age > limit)
				victim = true;
			else if (age == limit && atlimit)
			{
				victim = true;
				atlimit = ZA_SIZE(block) < atlimit ? atlimit - ZA_SIZE(block) : 0;
			}
		}
		if (victim)
		{
			ZA_NoteEvict(ZA_SIZE(block));
			block = ZA_BlockAt(Z_FreeBlock(block));
		}
		else
			block = ZA_BlockAt(after);
	}
	return true;
}

static boolean Z_MakeRoom(size_t bytes, boolean current);
static boolean Z_EvictLRUWalk(size_t want, boolean current);

static boolean Z_EvictLRU(size_t want, boolean current)
{
	ZCOST_BEGIN();
	const boolean any = Z_EvictLRUWalk(want, current);

	ZCOST_END(zcost_evict);
	return any;
}

// PS2-71: see Z_SetReclaimHook (z_zone.h). Not reentrant: the hook only frees.
static z_reclaim_fn zreclaim_hook;
static boolean zreclaiming;
void Z_SetReclaimHook(z_reclaim_fn fn)
{
	zreclaim_hook = fn;
}

static boolean Z_Reclaim(size_t want)
{
	size_t got;

	if (!zreclaim_hook || zreclaiming)
		return false;
	zreclaiming = true;
	got = zreclaim_hook(want == SIZE_MAX ? (size_t)1 << 30 : want);
	zreclaiming = false;
	return got != 0;
}

// PS2-75: moves the frontier so that a request that does not fit in its zone may fit. Returns true when it moved.
//  long-lived side (ZA_TOP): the span of free blocks and evictable caches that reaches the frontier gives its upper end to the long-lived zone;
//    the caches in it are evicted (they rebuild), a pinned block or a cache of this frame ends the span.
//  short-lived side (ZA_BOTTOM): the free block that holds the frontier gives the part the request needs to the cache zone.
static boolean Z_MoveFrontier(size_t size, size_t align, int side)
{
	uint8_t *front = ZA_Frontier(), *as = NULL, *ae = NULL;
	const size_t need = (size + ZA_HDR + 32 + align + 15) & ~(size_t)15;
	zablock_t *b;

	if (!front || !za_twosided)
		return false;
	if (side == ZA_BOTTOM)
	{
		for (b = ZA_First(); b; b = ZA_Next(b))
		{
			uint8_t *bs = (uint8_t *)b, *be = bs + ZA_SIZE(b), *nf;

			if (be <= front)
				continue;
			if (!ZA_ISFREE(b))
				return false; // a long-lived block holds the frontier: nothing to give
			nf = (uint8_t *)(((uintptr_t)bs + need + 15) & ~(uintptr_t)15);
			if (nf <= front || nf > be)
				return false;
			ZA_SetFrontier(nf);
			zfront_moves_up++;
			return true;
		}
		return false;
	}
	for (b = ZA_First(); b; b = ZA_Next(b))
	{
		uint8_t *bs = (uint8_t *)b;

		if (!(ZA_ISFREE(b) || Z_Evictable(b, false)))
		{
			if (as && ae >= front)
				break; // the span that reaches the frontier ends here
			as = NULL;
			continue;
		}
		if (!as)
			as = bs;
		ae = bs + ZA_SIZE(b);
	}
	if (!as || ae < front || (size_t)(ae - as) < need)
		return false;
	{
		// the zone gets [x, ae): the request plus a step, but never more than the span has
		uint8_t *x = (uint8_t *)(((uintptr_t)ae - need) & ~(uintptr_t)15);

		if ((size_t)(x - as) >= Z_FRONT_STEP)
			x -= Z_FRONT_STEP;
		for (b = ZA_First(); b;)
		{
			uint8_t *bs = (uint8_t *)b;

			if (bs + ZA_SIZE(b) <= x || bs >= ae)
				b = ZA_Next(b);
			else if (!ZA_ISFREE(b))
			{
				ZA_NoteEvict(ZA_SIZE(b));
				b = ZA_BlockAt(Z_FreeBlock(b));
			}
			else
				b = ZA_Next(b);
		}
		if (x < front)
		{
			ZA_SetFrontier(x);
			zfront_moves_down++;
		}
	}
	return true;
}

// Gets `size` bytes from the arena, purging as the tags allow; NULL means out of memory.
static void *Z_AllocBlock(size_t size, INT32 tag, size_t align)
{
	const int side = Z_SideForTag(tag);
	void *p, *frontier = NULL;
	size_t want = size > SIZE_MAX - Z_EVICT_SLACK ? SIZE_MAX : size + Z_EVICT_SLACK;

	if (zflush_period && ++zflush_count >= zflush_period)
	{
		// test injection: pretend that memory ran out right now. Whatever the code holds on to without owning
		// (a cache pointer kept across an allocation) is then gone, exactly as under real pressure.
		zflush_count = 0;
		zflush_total++;
		Z_EvictLRU((size_t)-1, false);
	}
	p = ZA_Alloc(size, align, side);
	if (p)
		return p;
	if (ZA_Frontier())
	{
		if (Z_MoveFrontier(size, align, side) && (p = ZA_Alloc(size, align, side)) != NULL)
			return p;
		// the zones could not make room: from here on space is short and a fit anywhere beats an out-of-memory error
		zfront_fallbacks++;
		frontier = ZA_Frontier();
		ZA_SetFrontier(NULL);
	}

	// While the 3D view renders (zpurgelock) only blocks of earlier frames may go: everything the view uses is stamped
	// with the current frame by the cache lookups (Z_Touch), and the view does not keep pointers from earlier frames.
	// (The lock used to forbid eviction altogether: a view that needed more than the headroom then died with an OOM.)
	if (!zpurgelock)
		Z_FreeTagRange(PU_PURGELEVEL, INT32_MAX);
	p = ZA_Alloc(size, align, side);
	while (!p)
	{
		// the caches go first; when none is left, the subsystem hook (audio effects) gives back what it can rebuild
		if (!Z_EvictLRU(want, false) && !Z_Reclaim(want))
			break;
		p = ZA_Alloc(size, align, side);
		// PS2-61: a large request needs one contiguous block, and the oldest caches by age are scattered pieces. When the oldest
		// bytes did not make it, the cheapest run of free and evictable neighbours is freed (the rest of the cache stays).
		if (!p && size >= Z_MAKEROOM_MIN && Z_MakeRoom(size + Z_EVICT_ALIGN_PAD, false))
			p = ZA_Alloc(size, align, side);
		want = want > SIZE_MAX / 2 ? SIZE_MAX : want * 2; // fragmented space: bounded, saturating retries
	}
	if (frontier)
		ZA_SetFrontier(frontier); // (a zone-less placement is not worth moving the zones for)
	return p;
}

// Frees the cheapest run of neighbouring blocks that makes one free block of `bytes`: free blocks and cache blocks with
// an owner are eligible, what goes costs its size times its recency (halving with every frame of age, 12 frames and older all equal).
// Evicting by age alone frees pieces that are scattered, and a single large allocation then still does not fit.
static size_t Z_RoomCost(const zablock_t *b)
{
	UINT32 age;

	if (ZA_ISFREE(b))
		return 0;
	age = Z_Age(b) < 12 ? Z_Age(b) : 12;
	return (ZA_SIZE(b) >> 8) * (4096u >> age) + 1; // halves with every frame of age: what was used lately is dear
}

static boolean Z_MakeRoomWalk(size_t bytes, boolean current)
{
	zablock_t *b, *lo = NULL, *bestlo = NULL, *bestend = NULL, *first = ZA_First(), *last = NULL;
	size_t sum = 0, bestcost = SIZE_MAX, cost = 0;

	if (zreg_ok && zreg_n)
	{
		// PS2-76: a window that is worth anything holds an eviction candidate: from the free block before the first one (if there is one)
		// to the run of free blocks after the last one
		zablock_t *prev = ZA_PrevFree(zreg[0]);

		first = prev ? prev : zreg[0];
		last = zreg[zreg_n - 1];
	}
	for (b = first; b; b = ZA_Next(b))
	{
		if (!ZA_ISFREE(b) && !Z_Evictable(b, current))
		{
			lo = NULL; // the window cannot span a block that stays
			sum = cost = 0;
			if (last && (uintptr_t)b > (uintptr_t)last)
				break; // no candidate behind this block
			continue;
		}
		if (!lo)
			lo = b;
		sum += ZA_SIZE(b);
		cost += Z_RoomCost(b);
		while (lo != b && sum - ZA_SIZE(lo) >= bytes) // drop blocks from the left while the window stays large enough
		{
			sum -= ZA_SIZE(lo);
			cost -= Z_RoomCost(lo);
			lo = ZA_Next(lo);
		}
		if (sum >= bytes && cost < bestcost)
		{
			bestcost = cost;
			bestlo = lo;
			bestend = b;
		}
	}
	if (!bestlo)
		return false;

	for (b = bestlo; b;)
	{
		uint8_t *after = (uint8_t *)b + ZA_SIZE(b);
		if (!ZA_ISFREE(b))
		{
			ZA_NoteEvict(ZA_SIZE(b));
			Z_FreeBlock(b);
		}
		if (b == bestend)
			break;
		b = ZA_BlockAt(after);
	}
	return true;
}

static boolean Z_MakeRoom(size_t bytes, boolean current)
{
	ZCOST_BEGIN();
	const boolean made = Z_MakeRoomWalk(bytes, current);

	ZCOST_END(zcost_room);
	return made;
}

// Called when the 3D view starts: nothing is held yet, so any cache block with an owner may go. Tries to make one free
// block of `bytes` (the view's own allocations, such as the draw segment array doubling, are large).
static void Z_EnsureFreeWork(size_t bytes)
{
	Z_FreeTagRange(PU_PURGELEVEL, INT32_MAX);
	if (ZA_LargestFree() >= bytes + Z_EVICT_ALIGN_PAD)
		return;
	// the cheapest run of neighbours first. Where blocks that stay split the arena into pieces that are all too small and the
	// arena is large, the oldest cache goes by bytes at least (smaller requests then still fit). On the retail 32 MB arena that
	// fallback is wrong (PS2-61): the headroom is more than everything the cache holds, so it flushed the whole cache at the
	// start of every frame (MAP11: 124 MB evicted in 35 frames); there a failed run just leaves the cache alone.
	if (!Z_MakeRoom(bytes + Z_EVICT_ALIGN_PAD, true) && zheadroom_flush && ZA_FreeBytes() < bytes)
		Z_EvictLRU(bytes - ZA_FreeBytes() + Z_EVICT_SLACK, true);
}

static void Z_EnsureFree(size_t bytes)
{
	ZCOST_BEGIN();

	Z_EnsureFreeWork(bytes);
	ZCOST_END(zcost_ensure);
}

// PS2-73: "[zcost]" line of the report: how many times the eviction machinery ran and what it cost in EE cycles (each run walks the arena's blocks)
void Z_ReportCosts(void)
{
	I_OutputMsg("[zcost] headroom calls %lu cycles %lu | evict-walks %lu cycles %lu | make-room %lu cycles %lu | reclaim hook %s\n",
		(unsigned long)zcost_ensure.calls, (unsigned long)(zcost_ensure.cycles >> 10) << 10, (unsigned long)zcost_evict.calls,
		(unsigned long)(zcost_evict.cycles >> 10) << 10, (unsigned long)zcost_room.calls, (unsigned long)(zcost_room.cycles >> 10) << 10,
		zreclaim_hook ? "set" : "off");
	I_OutputMsg("[zreg] cache registry %s: %lu candidates now (capacity %u), overflows %lu\n", zreg_ok ? "on" : "OFF (overflowed: arena walks)", (unsigned long)zreg_n,
		(unsigned)Z_REG_MAX, (unsigned long)zreg_overflows);
	{
		const uint8_t *front = ZA_Frontier();
		size_t below = 0, above = 0;
		zablock_t *b;

		for (b = ZA_First(); b && front; b = ZA_Next(b))
			if (ZA_ISFREE(b))
				((const uint8_t *)b < front ? &below : &above)[0] += ZA_SIZE(b);
		I_OutputMsg("[zfront] frontier %s +%lx, free below (caches) %lu above (long-lived) %lu; moves down %lu up %lu, zone-less fallbacks %lu\n",
			front ? "at" : "off", front ? (unsigned long)(front - (const uint8_t *)ZA_First()) : 0ul, (unsigned long)below, (unsigned long)above,
			(unsigned long)zfront_moves_down, (unsigned long)zfront_moves_up, (unsigned long)zfront_fallbacks);
	}
}

static void Z_OutOfMemory(size_t size, INT32 tag, size_t align)
{
	zpinned = NULL;
	CONS_Printf("OOM: request %lu B tag %d (%s) align %lu, purge lock %d, frame %lu\n", (unsigned long)size, (int)tag,
		PS2Mem_TagName(tag), (unsigned long)align, (int)zpurgelock, (unsigned long)zframe);
#ifdef ZDEBUG
	CONS_Printf("OOM: failing request from %s:%d\n", zreqfile ? zreqfile : "?", (int)zreqline);
#endif
	PS2Mem_Report(24);
	PS2Mem_FreeList(8192);
	PS2Mem_Map(400);
	I_Error("Out of memory allocating %s bytes", sizeu1(size));
}

static size_t Z_KiBParm(const char *name, size_t fallback)
{
	const char *text;
	char *end;
	unsigned long kib;
	if (!M_CheckParm(name) || !M_IsNextParm())
		return fallback;
	text = M_GetNextParm();
	kib = strtoul(text, &end, 10);
	if (!*text || *text == '-' || *end || kib > (0x7FFFFFF0u >> 10))
		I_Error("%s: expected a nonnegative KiB budget within the EE address range", name);
	return (size_t)kib << 10;
}

static void Z_ArenaStart(void)
{
	size_t cap = 0, size;

	if (ZA_Ready())
		return;
	zreg_n = 0;
	zreg_ok = true;
	zpurge_maybe = false;
#if defined(_EE) && defined(__GNUC__)
	if (M_CheckParm("-zcaller"))
		ztracemin = M_IsNextParm() ? (size_t)atol(M_GetNextParm()) : 65536;
#endif
	if (PS2Mem_RamClass() >= 128)
	{
		zreserve = Z_RESERVE_128;
		zheadroom = Z_HEADROOM_128;
		zslack = Z_EVICT_SLACK_128;
		zheadroom_flush = true;
	}
	if (M_CheckParm("-zflush") && M_IsNextParm())
		zflush_period = (UINT32)atoi(M_GetNextParm());
	zreserve = Z_KiBParm("-zreserve", zreserve);
	cap = Z_KiBParm("-zarena", cap);
	zheadroom = Z_KiBParm("-zheadroom", zheadroom);
	if (M_CheckParm("-zredzone"))
		za_redzone = 1;
	if (M_CheckParm("-zsides") && M_IsNextParm())
		za_twosided = atoi(M_GetNextParm()) >= 2;
	if (M_CheckParm("-zprefer") && M_IsNextParm())
		za_prefer = atoi(M_GetNextParm());
	if (M_CheckParm("-zsmall") && M_IsNextParm())
		za_smallfit = atoi(M_GetNextParm());
	if (M_CheckParm("-zreport"))
		zreport_interval = M_IsNextParm() ? (UINT32)atoi(M_GetNextParm()) : 2100;

	size = ZA_InitHeap(zreserve, cap);
	if (!size)
		I_Error("Z_Init: cannot take the zone arena from the C heap (reserve %lu KiB)", (unsigned long)(zreserve >> 10));
	CONS_Printf("Zone arena: %lu KiB (RAM %lu MiB = %d MB profile, C heap reserve %lu KiB, headroom %lu KiB, slack %lu KiB, %s, red zones %s), C heap left: %lu KiB\n",
		(unsigned long)(size >> 10), (unsigned long)(PS2Mem_RamBytes() >> 20), PS2Mem_RamClass(), (unsigned long)(zreserve >> 10), (unsigned long)(zheadroom >> 10),
		(unsigned long)(zslack >> 10), za_twosided ? "two-sided" : "one-sided", za_redzone ? "on" : "off", (unsigned long)(PS2Mem_LibcFree() >> 10));
	if (zreport_interval)
		PS2Mem_Report(0);
}

static void Z_AdvanceFrame(void)
{
	zframe = (zframe + 1) & Z_FRAME_MASK;
	if (zreport_interval && ++zreport_count >= zreport_interval)
	{
		zreport_count = 0;
		PS2Mem_Report(0);
	}
}

/** Marks a frame boundary (call once per displayed frame, after the frame was drawn): blocks allocated or touched
  * since the previous call are no longer "current" and become eviction candidates.
  */
// PS2-72: drops every cache block that has an owner (they rebuild on demand), whatever its age: the level load starts from an arena
// without the previous level's cache between its blocks, so the level's arrays are one run from the bottom and the caches regrow above them.
// Only legal when no cache pointer is held (P_LoadLevel).
void Z_FlushCache(void)
{
	Z_FreeTagRange(PU_PURGELEVEL, INT32_MAX);
	Z_EvictLRU((size_t)-1, true);
	Z_RegRebuild(); // PS2-76: one arena walk per level; the registry is right again even after an overflow
}

void Z_NextFrame(void)
{
	zframe_explicit = true;
	Z_AdvanceFrame();
	PS2Mem_Frame();
}

/** Marks a cache block as used in the current frame, so it is not evicted before the frame ends. */
void Z_Touch(void *ptr)
{
	if (ptr)
		ZA_SetStamp(ZA_BLOCK(ptr), zframe);
}

// The caller has consumed every alias; leave the owner-backed cache resident until pressure needs it.
void Z_ReleaseCache(void *ptr)
{
	if (ptr)
	{
		zablock_t *block = ZA_BLOCK(ptr);
		if (ZA_TAG(block) == PU_CACHE && block->user && block != zpinned && Z_Age(block) == 0)
			ZA_SetStamp(block, (zframe - 1) & Z_FRAME_MASK);
	}
}

UINT32 Z_FrameCount(void)
{
	return zframe;
}

size_t Z_ArenaFree(void)
{
	return ZA_FreeBytes();
}

size_t Z_RenderHeadroom(void)
{
	return zheadroom;
}

unsigned Z_TestFlushes(void)
{
	return zflush_total;
}

/** Locks/unlocks purging of PU_CACHE blocks (nestable). The outermost lock first tries to make one free block of zheadroom bytes
  * (evicting cache, the cheapest run of neighbours: nothing is held yet, so blocks of this frame may go too); while
  * locked, current-frame cache roots remain protected and older untouched roots may still be evicted.
  */
void Z_PurgeLock(boolean lock)
{
	if ((lock && zpurgelock == INT32_MAX) || (!lock && zpurgelock == 0))
		I_Error("Z_PurgeLock: unbalanced lock/unlock");
	if (lock && zpurgelock == 0)
	{
		if (!zframe_explicit)
			Z_AdvanceFrame(); // nobody marks frame boundaries, so the start of a 3D view does
		if (zheadroom)
			Z_EnsureFree(zheadroom);
	}
	zpurgelock += lock ? 1 : -1;
}
#else
typedef struct memblock_s
{
	void **user;
	INT32 tag; // purgelevel
	UINT32 id; // Should be ZONEID

	size_t size; // including the header and blocks
	size_t realsize; // size of real data only

#ifdef ZDEBUG
	const char *ownerfile;
	INT32 ownerline;
#endif

	struct memblock_s *next, *prev;
} memblock_t;

#define MEMORY(x) (void *)((uintptr_t)(x) + sizeof(memblock_t))
#define MEMBLOCK(x) (memblock_t *)((uintptr_t)(x) - sizeof(memblock_t))

// both the head and tail of the zone memory block list
static memblock_t head;
#endif

//
// Function prototypes
//
static void Command_Memfree_f(void);
#ifdef ZDEBUG
static void Command_Memdump_f(void);
#endif

// --------------------------
// Zone memory initialisation
// --------------------------

/** Initialises zone memory.
  * Used at game startup.
  *
  * \sa I_GetFreeMem, Command_Memfree_f, Command_Memdump_f
  */
#ifdef PS2
void Z_Init(void)
{
	size_t total, memfree;

	Z_ArenaStart();

	memfree = I_GetFreeMem(&total)>>20;
	CONS_Printf("System memory: %sMB - Free: %sMB\n", sizeu1(total>>20), sizeu2(memfree));

	// Note: This allocates memory. Watch out.
	COM_AddCommand("memfree", Command_Memfree_f, COM_LUA);
	PS2Mem_Init();

#ifdef ZDEBUG
	COM_AddCommand("memdump", Command_Memdump_f, COM_LUA);
#endif
}
#else
void Z_Init(void)
{
	size_t total, memfree;

	memset(&head, 0x00, sizeof(head));

	head.next = head.prev = &head;

	memfree = I_GetFreeMem(&total)>>20;
	CONS_Printf("System memory: %sMB - Free: %sMB\n", sizeu1(total>>20), sizeu2(memfree));

	// Note: This allocates memory. Watch out.
	COM_AddCommand("memfree", Command_Memfree_f, COM_LUA);

#ifdef ZDEBUG
	COM_AddCommand("memdump", Command_Memdump_f, COM_LUA);
#endif
}
#endif


// ----------------------
// Zone memory allocation
// ----------------------

/** Frees allocated memory.
  *
  * \param ptr A pointer to allocated memory,
  *             assumed to have been allocated with Z_Malloc/Z_Calloc.
  * \sa Z_FreeTags
  */
#ifdef PS2
#ifdef ZDEBUG
void Z_Free2(void *ptr, const char *file, INT32 line)
#else
void Z_Free(void *ptr)
#endif
{
	zablock_t *block;

	if (ptr == NULL)
		return;

#ifdef ZDEBUG2
	CONS_Debug(DBG_MEMORY, "Z_Free %s:%d\n", file, line);
#endif

	block = ZA_BLOCK(ptr);
#ifdef PARANOIA
	if (!(block->sf & ZAF_VALID) || !(block->sf & ZAF_USED))
#ifdef ZDEBUG
		I_Error("Z_Free at %s:%d: wrong id", file, line);
#else
		I_Error("Z_Free: wrong id");
#endif
#endif

#ifdef ZDEBUG
	// Write every Z_Free call to a debug file.
	CONS_Debug(DBG_MEMORY, "Z_Free at %s:%d\n", file, line);
	{
		// -zfreetrace: who allocated the freed blocks (temporary buffers carved from the long-lived end leave holes there)
		size_t tmin = Z_TraceMin();
		if (tmin && ZA_SIZE(block) >= tmin && !ZA_ISFREE(block))
			I_OutputMsg("[zfreed] %s:%d tag %d size %lu freed at %s:%d\n", block->ownerfile ? block->ownerfile : "?", (int)block->ownerline,
				ZA_TAG(block), (unsigned long)ZA_SIZE(block), file, (int)line);
	}
#endif

#ifndef ZDEBUG
	Z_TRACE_FREE(block);
#endif
	Z_FreeBlock(block);
}
#else
#ifdef ZDEBUG
void Z_Free2(void *ptr, const char *file, INT32 line)
#else
void Z_Free(void *ptr)
#endif
{
	memblock_t *block;

	if (ptr == NULL)
		return;

#ifdef ZDEBUG2
	CONS_Debug(DBG_MEMORY, "Z_Free %s:%d\n", file, line);
#endif

	block = MEMBLOCK(ptr);
#ifdef PARANOIA
	if (block->id != ZONEID)
#ifdef ZDEBUG
		I_Error("Z_Free at %s:%d: wrong id", file, line);
#else
		I_Error("Z_Free: wrong id");
#endif
#endif

#ifdef ZDEBUG
	// Write every Z_Free call to a debug file.
	CONS_Debug(DBG_MEMORY, "Z_Free at %s:%d\n", file, line);
#endif

	// anything that isn't by lua gets passed to lua just in case.
	if (block->tag != PU_LUA)
		LUA_InvalidateUserdata(ptr);

	// TODO: if zdebugging, make sure no other block has a user
	// that is about to be freed.

	// Clear the user's mark.
	if (block->user != NULL)
		*block->user = NULL;

#ifdef VALGRIND_DESTROY_MEMPOOL
	VALGRIND_DESTROY_MEMPOOL(block);
#endif
	block->prev->next = block->next;
	block->next->prev = block->prev;
	free(block);
}
#endif

#ifndef PS2
/** malloc() that doesn't accept failure.
  *
  * \param size Amount of memory to be allocated, in bytes.
  * \return A pointer to the allocated memory.
  */
static void *xm(size_t size)
{
	const size_t padedsize = size+sizeof (size_t);
	void *p;

	if (padedsize < size)/* overflow check */
		I_Error("You are allocating memory too large!");
	p = malloc(padedsize);

	if (p == NULL)
	{
		// Oh crumbs: we're out of heap. Try purging the cache and reallocating.
		Z_FreeTags(PU_PURGELEVEL, INT32_MAX);
		p = malloc(padedsize);

		if (p == NULL)
		{
			I_Error("Out of memory allocating %s bytes", sizeu1(size));
		}
	}

	return p;
}
#endif
/** The Z_MallocAlign function.
  * Allocates a block of memory, adds it to a linked list so we can keep track of it.
  *
  * \param size Amount of memory to be allocated, in bytes.
  * \param tag Purge tag.
  * \param user The address of a pointer to the memory to be allocated.
  *             When the memory is freed by Z_Free later,
  *             the pointer at this address will then be automatically set to NULL.
  * \param alignbits The alignment of the memory to be allocated, in bits. Can be 0.
  * \note You can pass Z_Malloc() a NULL user if the tag is less than PU_PURGELEVEL.
  * \sa Z_CallocAlign, Z_ReallocAlign
  */
#ifdef PS2
static void *Z_MallocInternal(size_t size, INT32 tag, void *user, INT32 alignbits,
	const char *file, INT32 line, boolean fatal)
{
	zablock_t *block;
	void *ptr;
	size_t align, minimum;
	(void)file;
	(void)line;

#ifdef ZDEBUG2
	CONS_Debug(DBG_MEMORY, "Z_Malloc %s:%d\n", file, line);
#endif

#ifdef ZDEBUG
	zreqfile = file;
	zreqline = line;
#endif
	if (!ZA_Ready())
		Z_ArenaStart();
	if (size > 0x7FFFFFF0u)
		I_Error("You are allocating memory too large!");
	// alignbits is a log2 byte alignment. 2^32 cannot be represented by the EE's size_t.
	if (alignbits < 0 || alignbits >= 32)
		I_Error("Z_MallocAlign: invalid alignment bits %d", alignbits);
	if (tag < 0 || tag > ZA_MAXTAG)
		I_Error("Z_MallocAlign: invalid purge tag %d", tag);
	if (tag >= PU_PURGELEVEL && user == NULL)
		I_Error("Z_Malloc: attempted to allocate purgable block "
			"(size %s) with no user", sizeu1(size));

	// 16-byte payload (the R5900 faults on unaligned ld/sd), 64 from 2 KiB up (DMA), more when asked
	align = (size_t)1 << alignbits;
	minimum = size >= 2048 ? 64 : 16;
	if (align < minimum)
		align = minimum;

	ptr = Z_AllocBlock(size, tag, align);
	if (ptr == NULL)
	{
		if (fatal)
			Z_OutOfMemory(size, tag, align);
		return NULL;
	}

	block = ZA_BLOCK(ptr);
	ZA_SetTag(block, tag);
	Z_NoteTag(block, -1, tag);
	ZA_SetStamp(block, zframe);
#ifdef ZDEBUG
	block->ownerline = line;
	block->ownerfile = file;
	{
		size_t tmin = Z_TraceMin();
		if (tmin && size >= tmin)
			I_OutputMsg("[zalloc] %s:%d tag %d size %lu at +%lx\n", file ? file : "?", (int)line, (int)tag, (unsigned long)ZA_SIZE(block),
				(unsigned long)((uint8_t *)block - (uint8_t *)ZA_First()));
	}
#endif
	I_Assert((intptr_t)ptr % sizeof (void *) == 0);

	if (user != NULL)
	{
		block->user = user;
		*(void **)user = ptr;
	}
	return ptr;
}

#ifdef ZDEBUG
void *Z_Malloc2(size_t size, INT32 tag, void *user, INT32 alignbits, const char *file, INT32 line)
{
	Z_TRACE_CALLER("malloc");
	return Z_MallocInternal(size, tag, user, alignbits, file, line, true);
}
#else
void *Z_MallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits)
{
	Z_TRACE_CALLER("malloc");
	return Z_MallocInternal(size, tag, user, alignbits, NULL, 0, true);
}
#endif

void *Z_TryMallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits)
{
	Z_TRACE_CALLER("trymalloc");
	return Z_MallocInternal(size, tag, user, alignbits, NULL, 0, false);
}
#else
#ifdef ZDEBUG
void *Z_Malloc2(size_t size, INT32 tag, void *user, INT32 alignbits,
	const char *file, INT32 line)
#else
void *Z_MallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits)
#endif
{
	memblock_t *block;
	void *ptr;
	(void)(alignbits); // no longer used, so silence warnings.

#ifdef ZDEBUG2
	CONS_Debug(DBG_MEMORY, "Z_Malloc %s:%d\n", file, line);
#endif

	block = xm(sizeof (memblock_t) + size);
	ptr = MEMORY(block);
	I_Assert((intptr_t)ptr % sizeof (void *) == 0);

#ifdef HAVE_VALGRIND
	Z_calloc = false;
#endif

	block->next = head.next;
	block->prev = &head;
	head.next = block;
	block->next->prev = block;

	block->tag = tag;
	block->user = NULL;
#ifdef ZDEBUG
	block->ownerline = line;
	block->ownerfile = file;
#endif
	block->size = sizeof (memblock_t) + size;
	block->realsize = size;

#ifdef VALGRIND_CREATE_MEMPOOL
	VALGRIND_CREATE_MEMPOOL(block, size, Z_calloc);
#endif

	block->id = ZONEID;

	if (user != NULL)
	{
		block->user = user;
		*(void **)user = ptr;
	}
	else if (tag >= PU_PURGELEVEL)
		I_Error("Z_Malloc: attempted to allocate purgable block "
			"(size %s) with no user", sizeu1(size));

	return ptr;
}
#endif

/** The Z_CallocAlign function.
  * Allocates a block of memory, adds it to a linked list so we can keep track of it.
  * Unlike Z_MallocAlign, this also initialises the bytes to zero.
  *
  * \param size Amount of memory to be allocated, in bytes.
  * \param tag Purge tag.
  * \param user The address of a pointer to the memory to be allocated.
  *             When the memory is freed by Z_Free later,
  *             the pointer at this address will then be automatically set to NULL.
  * \param alignbits The alignment of the memory to be allocated, in bits. Can be 0.
  * \note You can pass Z_Calloc() a NULL user if the tag is less than PU_PURGELEVEL.
  * \sa Z_MallocAlign, Z_ReallocAlign
  */
#ifdef ZDEBUG
void *Z_Calloc2(size_t size, INT32 tag, void *user, INT32 alignbits, const char *file, INT32 line)
#else
void *Z_CallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits)
#endif
{
#ifdef VALGRIND_MEMPOOL_ALLOC
	Z_calloc = true;
#endif
#ifdef PS2
	Z_TRACE_CALLER("calloc");
#endif
#ifdef ZDEBUG
	return memset(Z_Malloc2    (size, tag, user, alignbits, file, line), 0, size);
#else
	return memset(Z_MallocAlign(size, tag, user, alignbits            ), 0, size);
#endif
}

/** The Z_ReallocAlign function.
  * Reallocates a block of memory with a new size.
  *
  * \param ptr A pointer to allocated memory,
  *             assumed to have been allocated with Z_Malloc/Z_Calloc.
  *             If NULL, this function instead acts as a wrapper for Z_CallocAlign.
  * \param size New size of memory block, in bytes.
  *             If zero, then the memory is freed and NULL is returned.
  * \param tag New purge tag.
  * \param user The address of a pointer to the memory to be reallocated.
  *             This can be a different user to the one originally assigned to the memory block.
  * \param alignbits The alignment of the memory to be allocated, in bits. Can be 0.
  * \return A pointer to the reallocated memory. Can be NULL if memory was freed.
  * \note You can pass Z_Realloc() a NULL user if the tag is less than PU_PURGELEVEL.
  * \sa Z_MallocAlign, Z_CallocAlign
  */
#ifdef PS2
#ifdef ZDEBUG
void *Z_Realloc2(void *ptr, size_t size, INT32 tag, void *user, INT32 alignbits, const char *file, INT32 line)
#else
void *Z_ReallocAlign(void *ptr, size_t size, INT32 tag, void *user, INT32 alignbits)
#endif
{
	void *rez;
	zablock_t *block;
	size_t copysize;
	Z_TRACE_CALLER("realloc");

#ifdef ZDEBUG2
	CONS_Debug(DBG_MEMORY, "Z_Realloc %s:%d\n", file, line);
#endif

	if (!size)
	{
		Z_Free(ptr);
		return NULL;
	}

	if (!ptr)
	{
#ifdef ZDEBUG
		return Z_Calloc2(size, tag, user, alignbits, file , line);
#else
		return Z_CallocAlign(size, tag, user, alignbits);
#endif
	}

	block = ZA_BLOCK(ptr);
#ifdef PARANOIA
	if (!(block->sf & ZAF_VALID) || !(block->sf & ZAF_USED))
#ifdef ZDEBUG
		I_Error("Z_ReallocAlign at %s:%d: wrong id", file, line);
#else
		I_Error("Z_ReallocAlign: wrong id");
#endif
#endif

	// Avoid the old+new allocation peak when the existing address meets the new alignment.
	if (size > 0x7FFFFFF0u)
		I_Error("You are allocating memory too large!");
	if (alignbits < 0 || alignbits >= 32)
		I_Error("Z_ReallocAlign: invalid alignment bits %d", alignbits);
	if (tag < 0 || tag > ZA_MAXTAG || (tag >= PU_PURGELEVEL && !user))
		I_Error("Z_ReallocAlign: invalid tag or purgable block without user");
	copysize = block->realsize;
	{
		size_t align = (size_t)1 << alignbits;
		size_t minimum = size >= 2048 ? 64 : 16;
		if (align < minimum)
			align = minimum;
		if (!((uintptr_t)ptr & (align - 1)) && ZA_Resize(ptr, size))
		{
			if (block->user && block->user != user)
				*block->user = NULL;
			block->user = user;
			if (user)
				*(void **)user = ptr;
			Z_NoteTag(block, ZA_TAG(block), tag);
			ZA_SetTag(block, tag);
			ZA_SetStamp(block, zframe);
#ifdef ZDEBUG
			block->ownerfile = file;
			block->ownerline = line;
#endif
			if (size > copysize)
				memset((uint8_t *)ptr + copysize, 0, size - copysize);
			return ptr;
		}
	}

	// the old block is in use right now: it must not be the one evicted or purged to make room for its replacement
	ZA_SetStamp(block, zframe);
	zpinned = block;

#ifdef ZDEBUG
	// Write every Z_Realloc call to a debug file.
	DEBFILE(va("Z_Realloc at %s:%d\n", file, line));
	rez = Z_Malloc2(size, tag, user, alignbits, file, line);
#else
	rez = Z_MallocAlign(size, tag, user, alignbits);
#endif

	if (size < block->realsize)
		copysize = size;
	else
		copysize = block->realsize;

	zpinned = NULL;

	M_Memcpy(rez, ptr, copysize);

#ifdef ZDEBUG
	Z_Free2(ptr, file, line);
#else
	Z_Free(ptr);
#endif

	// Need to set the user in case the old block had the same one, in
	// which case the Z_Free will just have NULLed it out.
	if (user)
		*((void**)user) = rez;

	if (size > copysize)
		memset((char*)rez+copysize, 0x00, size-copysize);

	return rez;
}
#else
#ifdef ZDEBUG
void *Z_Realloc2(void *ptr, size_t size, INT32 tag, void *user, INT32 alignbits, const char *file, INT32 line)
#else
void *Z_ReallocAlign(void *ptr, size_t size, INT32 tag, void *user, INT32 alignbits)
#endif
{
	void *rez;
	memblock_t *block;
	size_t copysize;

#ifdef ZDEBUG2
	CONS_Debug(DBG_MEMORY, "Z_Realloc %s:%d\n", file, line);
#endif

	if (!size)
	{
		Z_Free(ptr);
		return NULL;
	}

	if (!ptr)
	{
#ifdef ZDEBUG
		return Z_Calloc2(size, tag, user, alignbits, file , line);
#else
		return Z_CallocAlign(size, tag, user, alignbits);
#endif
	}

	block = MEMBLOCK(ptr);
#ifdef PARANOIA
	if (block->id != ZONEID)
#ifdef ZDEBUG
		I_Error("Z_ReallocAlign at %s:%d: wrong id", file, line);
#else
		I_Error("Z_ReallocAlign: wrong id");
#endif
#endif

	if (block == NULL)
		return NULL;

#ifdef ZDEBUG
	// Write every Z_Realloc call to a debug file.
	DEBFILE(va("Z_Realloc at %s:%d\n", file, line));
	rez = Z_Malloc2(size, tag, user, alignbits, file, line);
#else
	rez = Z_MallocAlign(size, tag, user, alignbits);
#endif

	if (size < block->realsize)
		copysize = size;
	else
		copysize = block->realsize;

	M_Memcpy(rez, ptr, copysize);

#ifdef ZDEBUG
	Z_Free2(ptr, file, line);
#else
	Z_Free(ptr);
#endif

	// Need to set the user in case the old block had the same one, in
	// which case the Z_Free will just have NULLed it out.
	if (user)
		*((void**)user) = rez;

	if (size > copysize)
		memset((char*)rez+copysize, 0x00, size-copysize);

	return rez;
}
#endif

/** Frees all memory for a given set of tags.
  *
  * \param lowtag The lowest tag to consider.
  * \param hightag The highest tag to consider.
  */
#ifdef PS2
void Z_FreeTags(INT32 lowtag, INT32 hightag)
{
	Z_CheckHeap(420);
	if (zreport_interval && lowtag <= PU_LEVEL && hightag >= PU_LEVEL)
	{
		PS2Mem_Line("level-exit");
		ZA_ResetPeak(); // the next line shows the peak of the next level
	}
	Z_FreeTagRange(lowtag, hightag);
}
#else
void Z_FreeTags(INT32 lowtag, INT32 hightag)
{
	memblock_t *block, *next;

	Z_CheckHeap(420);
	for (block = head.next; block != &head; block = next)
	{
		next = block->next; // get link before freeing
		if (block->tag >= lowtag && block->tag <= hightag)
			Z_Free(MEMORY(block));
	}
}
#endif

/** Iterates through all memory for a given set of tags.
  *
  * \param lowtag The lowest tag to consider.
  * \param hightag The highest tag to consider.
  * \param iterfunc The iterator function.
  */
#ifdef PS2
void Z_IterateTags(INT32 lowtag, INT32 hightag, boolean (*iterfunc)(void *))
{
	zablock_t *block;

	if (!iterfunc)
		I_Error("Z_IterateTags: no iterator function was given");

	for (block = ZA_First(); block;)
	{
		uint8_t *after = (uint8_t *)block + ZA_SIZE(block); // the next block, taken before the callback frees things

		if (!ZA_ISFREE(block) && ZA_TAG(block) >= lowtag && ZA_TAG(block) <= hightag)
		{
			void *mem = ZA_PAYLOAD(block);
			boolean free = iterfunc(mem);
			if (free)
				block = ZA_BlockAt(Z_FreeBlock(ZA_BLOCK(mem)));
			else
				block = ZA_BlockAt(after);
		}
		else
			block = ZA_BlockAt(after);
	}
}
#else
void Z_IterateTags(INT32 lowtag, INT32 hightag, boolean (*iterfunc)(void *))
{
	memblock_t *block, *next;

	if (!iterfunc)
		I_Error("Z_IterateTags: no iterator function was given");

	for (block = head.next; block != &head; block = next)
	{
		next = block->next; // get link before possibly freeing

		if (block->tag >= lowtag && block->tag <= hightag)
		{
			void *mem = MEMORY(block);
			boolean free = iterfunc(mem);
			if (free)
				Z_Free(mem);
		}
	}
}
#endif

// -----------------
// Utility functions
// -----------------

// starting value of nextcleanup
#define CLEANUPCOUNT 2000

// number of function calls left before next cleanup
static INT32 nextcleanup = CLEANUPCOUNT;

/** This was in Z_Malloc, but was freeing data at
  * unsafe times. Now it is only called when it is safe
  * to cleanup memory.
  *
  * \todo Currently blocks >= PU_PURGELEVEL are freed every
  *       CLEANUPCOUNT. It might be better to keep track of
  *       the total size of all purgable memory and free it when the
  *       size exceeds some value.
  */
void Z_CheckMemCleanup(void)
{
#ifdef PS2
	if (zpurgelock)
		return;
#endif
	if (nextcleanup-- == 0)
	{
		nextcleanup = CLEANUPCOUNT;
		Z_FreeTags(PU_PURGELEVEL, INT32_MAX);
	}
}


/** Checks the heap, as well as the memhdr_ts, for any corruption or
  * other problems.
  * \param i Identifies from where in the code Z_CheckHeap was called.
  * \author Graue <graue@oceanbase.org>
  */
#ifdef PS2
void Z_CheckHeap(INT32 i)
{
	char msg[200];
	zablock_t *block;
	UINT32 blocknumon = 0;

	// structure, coalescing, free lists, boundary tags, red zones
	if (ZA_Check(msg, sizeof msg))
		I_Error("Z_CheckHeap %d: %s", i, msg);

	for (block = ZA_First(); block; block = ZA_Next(block))
	{
		blocknumon++;
		if (ZA_ISFREE(block))
			continue;
#ifdef ZDEBUG2
		CONS_Debug(DBG_MEMORY, "block %u owned by %s:%d\n",
			blocknumon, block->ownerfile, block->ownerline);
#endif
		if (block->user != NULL && *(block->user) != ZA_PAYLOAD(block))
		{
			I_Error("Z_CheckHeap %d: block %u"
#ifdef ZDEBUG
				"(owned by %s:%d)"
#endif
				" doesn't have a proper user", i, blocknumon
#ifdef ZDEBUG
				, block->ownerfile, block->ownerline
#endif
				);
		}
	}
}
#else
void Z_CheckHeap(INT32 i)
{
	memblock_t *block;
	UINT32 blocknumon = 0;
	void *given;

	for (block = head.next; block != &head; block = block->next)
	{
		blocknumon++;
		given = MEMORY(block);
#ifdef ZDEBUG2
		CONS_Debug(DBG_MEMORY, "block %u owned by %s:%d\n",
			blocknumon, block->ownerfile, block->ownerline);
#endif
#ifdef VALGRIND_MEMPOOL_EXISTS
		if (!VALGRIND_MEMPOOL_EXISTS(block))
		{
			I_Error("Z_CheckHeap %d: block %u"
#ifdef ZDEBUG
				"(owned by %s:%d)"
#endif
				" should not exist", i, blocknumon
#ifdef ZDEBUG
				, block->ownerfile, block->ownerline
#endif
				);
		}
#endif
		if (block->user != NULL && *(block->user) != given)
		{
			I_Error("Z_CheckHeap %d: block %u"
#ifdef ZDEBUG
				"(owned by %s:%d)"
#endif
				" doesn't have a proper user", i, blocknumon
#ifdef ZDEBUG
				, block->ownerfile, block->ownerline
#endif
				);
		}
		if (block->next->prev != block)
		{
			I_Error("Z_CheckHeap %d: block %u"
#ifdef ZDEBUG
				"(owned by %s:%d)"
#endif
				" lacks proper backlink", i, blocknumon
#ifdef ZDEBUG
				, block->ownerfile, block->ownerline
#endif
				);
		}
		if (block->prev->next != block)
		{
			I_Error("Z_CheckHeap %d: block %u"
#ifdef ZDEBUG
				"(owned by %s:%d)"
#endif
				" lacks proper forward link", i, blocknumon
#ifdef ZDEBUG
				, block->ownerfile, block->ownerline
#endif
				);
		}
		if (block->id != ZONEID)
		{
			I_Error("Z_CheckHeap %d: block %u"
#ifdef ZDEBUG
				"(owned by %s:%d)"
#endif
				" have the wrong ID", i, blocknumon
#ifdef ZDEBUG
				, block->ownerfile, block->ownerline
#endif
				);
		}
	}
}
#endif

// ------------------------
// Zone memory modification
// ------------------------

/** Changes a memory block's purge tag.
  *
  * \param ptr A pointer to allocated memory,
  *             assumed to have been allocated with Z_Malloc/Z_Calloc.
  * \param tag The new tag.
  * \sa Z_SetUser
  */
#ifdef PS2
#ifdef PARANOIA
void Z_ChangeTag2(void *ptr, INT32 tag, const char *file, INT32 line)
#else
void Z_ChangeTag(void *ptr, INT32 tag)
#endif
{
	zablock_t *block;

	if (ptr == NULL)
		return;

	block = ZA_BLOCK(ptr);

#ifdef PARANOIA
	if (!(block->sf & ZAF_VALID) || !(block->sf & ZAF_USED))
		I_Error("Z_ChangeTag at %s:%d: wrong id", file, line);
#endif

	if (tag >= PU_PURGELEVEL && block->user == NULL)
		I_Error("Internal memory management error: "
			"tried to make block purgable but it has no owner");
	if (tag < 0 || tag > ZA_MAXTAG)
		I_Error("Z_ChangeTag: invalid purge tag %d", tag);

	Z_NoteTag(block, ZA_TAG(block), tag);
	ZA_SetTag(block, tag);
	ZA_SetStamp(block, zframe); // somebody wants it right now
}
#else
#ifdef PARANOIA
void Z_ChangeTag2(void *ptr, INT32 tag, const char *file, INT32 line)
#else
void Z_ChangeTag(void *ptr, INT32 tag)
#endif
{
	memblock_t *block;

	if (ptr == NULL)
		return;

	block = MEMBLOCK(ptr);

#ifdef PARANOIA
	if (block->id != ZONEID) I_Error("Z_ChangeTag at %s:%d: wrong id", file, line);
#endif

	if (tag >= PU_PURGELEVEL && block->user == NULL)
		I_Error("Internal memory management error: "
			"tried to make block purgable but it has no owner");

	block->tag = tag;
}
#endif

/** Changes a memory block's user.
  *
  * \param ptr A pointer to allocated memory,
  *             assumed to have been allocated with Z_Malloc/Z_Calloc.
  * \param newuser The new user for the memory block.
  * \sa Z_ChangeTag
  */
#ifdef PS2
#ifdef PARANOIA
void Z_SetUser2(void *ptr, void **newuser, const char *file, INT32 line)
#else
void Z_SetUser(void *ptr, void **newuser)
#endif
{
	zablock_t *block;

	if (ptr == NULL)
		return;

	block = ZA_BLOCK(ptr);

#ifdef PARANOIA
	if (!(block->sf & ZAF_VALID) || !(block->sf & ZAF_USED))
		I_Error("Z_SetUser at %s:%d: wrong id", file, line);
#endif

	if (ZA_TAG(block) >= PU_PURGELEVEL && newuser == NULL)
		I_Error("Internal memory management error: "
			"tried to make block purgable but it has no owner");

	block->user = (void*)newuser;
	if (newuser)
		*newuser = ptr;
	ZA_SetStamp(block, zframe);
}
#else
#ifdef PARANOIA
void Z_SetUser2(void *ptr, void **newuser, const char *file, INT32 line)
#else
void Z_SetUser(void *ptr, void **newuser)
#endif
{
	memblock_t *block;

	if (ptr == NULL)
		return;

	block = MEMBLOCK(ptr);

#ifdef PARANOIA
	if (block->id != ZONEID) I_Error("Z_SetUser at %s:%d: wrong id", file, line);
#endif

	if (block->tag >= PU_PURGELEVEL && newuser == NULL)
		I_Error("Internal memory management error: "
			"tried to make block purgable but it has no owner");

	block->user = (void*)newuser;
	*newuser = ptr;
}
#endif

// -----------------
// Zone memory usage
// -----------------

/** Calculates memory usage for a given set of tags.
  *
  * \param lowtag The lowest tag to consider.
  * \param hightag The highest tag to consider.
  * \return Number of bytes currently allocated in the heap for the
  *         given tags.
  */
#ifdef PS2
size_t Z_TagsUsage(INT32 lowtag, INT32 hightag)
{
	size_t cnt = 0;
	zablock_t *rover;

	// the bytes the blocks take in the arena: header, payload and padding
	for (rover = ZA_First(); rover; rover = ZA_Next(rover))
	{
		if (ZA_ISFREE(rover) || ZA_TAG(rover) < lowtag || ZA_TAG(rover) > hightag)
			continue;
		cnt += ZA_SIZE(rover);
	}

	return cnt;
}
#else
size_t Z_TagsUsage(INT32 lowtag, INT32 hightag)
{
	size_t cnt = 0;
	memblock_t *rover;

	for (rover = head.next; rover != &head; rover = rover->next)
	{
		if (rover->tag < lowtag || rover->tag > hightag)
			continue;
		cnt += rover->size + sizeof *rover;
	}

	return cnt;
}
#endif

// -----------------------
// Miscellaneous functions
// -----------------------

/** The function called by the "memfree" console command.
  * Prints the memory being used by each part of the game to the console.
  */
static void Command_Memfree_f(void)
{
	size_t freebytes, totalbytes;

	Z_CheckHeap(-1);
	CONS_Printf("\x82%s", M_GetText("Memory Info\n"));
	CONS_Printf(M_GetText("Total heap used        : %7s KB\n"), sizeu1(Z_TotalUsage()>>10));
	CONS_Printf(M_GetText("Static                 : %7s KB\n"), sizeu1(Z_TagUsage(PU_STATIC)>>10));
	CONS_Printf(M_GetText("Static (sound)         : %7s KB\n"), sizeu1(Z_TagUsage(PU_SOUND)>>10));
	CONS_Printf(M_GetText("Static (music)         : %7s KB\n"), sizeu1(Z_TagUsage(PU_MUSIC)>>10));
	CONS_Printf(M_GetText("Patches                : %7s KB\n"), sizeu1(Z_TagUsage(PU_PATCH)>>10));
	CONS_Printf(M_GetText("Patches (low priority) : %7s KB\n"), sizeu1(Z_TagUsage(PU_PATCH_LOWPRIORITY)>>10));
	CONS_Printf(M_GetText("Patches (rotated)      : %7s KB\n"), sizeu1(Z_TagUsage(PU_PATCH_ROTATED)>>10));
	CONS_Printf(M_GetText("Sprites                : %7s KB\n"), sizeu1(Z_TagUsage(PU_SPRITE)>>10));
	CONS_Printf(M_GetText("HUD graphics           : %7s KB\n"), sizeu1(Z_TagUsage(PU_HUDGFX)>>10));
	CONS_Printf(M_GetText("Locked cache           : %7s KB\n"), sizeu1(Z_TagUsage(PU_CACHE)>>10));
	CONS_Printf(M_GetText("Level                  : %7s KB\n"), sizeu1(Z_TagUsage(PU_LEVEL)>>10));
	CONS_Printf(M_GetText("Special thinker        : %7s KB\n"), sizeu1(Z_TagUsage(PU_LEVSPEC)>>10));
	CONS_Printf(M_GetText("All purgable           : %7s KB\n"),
		sizeu1(Z_TagsUsage(PU_PURGELEVEL, INT32_MAX)>>10));

#ifdef HWRENDER
	if (rendermode == render_opengl)
	{
		CONS_Printf(M_GetText("Patch info headers     : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRPATCHINFO)>>10));
		CONS_Printf(M_GetText("Cached textures        : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRCACHE)>>10));
		CONS_Printf(M_GetText("Texture colormaps      : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRPATCHCOLMIPMAP)>>10));
		CONS_Printf(M_GetText("Model textures         : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRMODELTEXTURE)>>10));
		CONS_Printf(M_GetText("Light table textures   : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRLIGHTTABLEDATA)>>10));
		CONS_Printf(M_GetText("Plane polygons         : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRPLANE)>>10));
		CONS_Printf(M_GetText("All GPU textures       : %7d KB\n"), HWR_GetTextureUsed()>>10);
	}
#endif

	CONS_Printf("\x82%s", M_GetText("System Memory Info\n"));
	freebytes = I_GetFreeMem(&totalbytes);
	CONS_Printf(M_GetText("    Total physical memory: %s KB\n"), sizeu1(totalbytes>>10));
	CONS_Printf(M_GetText("Available physical memory: %s KB\n"), sizeu1(freebytes>>10));
}

#ifdef ZDEBUG
/** The function called by the "memdump" console command.
  * Prints zone memory debugging information (i.e. tag, size, location in code allocated).
  * Can be all memory allocated in game, or between a set of tags (if -min/-max args used).
  * This command is available only if ZDEBUG is enabled.
  */
#ifdef PS2
static void Command_Memdump_f(void)
{
	zablock_t *block;
	INT32 mintag = 0, maxtag = INT32_MAX;
	INT32 i;

	if ((i = COM_CheckParm("-min")))
		mintag = atoi(COM_Argv(i + 1));

	if ((i = COM_CheckParm("-max")))
		maxtag = atoi(COM_Argv(i + 1));

	for (block = ZA_First(); block; block = ZA_Next(block))
		if (!ZA_ISFREE(block) && ZA_TAG(block) >= mintag && ZA_TAG(block) <= maxtag)
		{
			char *filename = strrchr(block->ownerfile, PATHSEP[0]);
			CONS_Printf("[%3d] %s (%s) bytes @ %s:%d\n", ZA_TAG(block), sizeu1(ZA_SIZE(block)), sizeu2(block->realsize), filename ? filename + 1 : block->ownerfile, block->ownerline);
		}
}
#else
static void Command_Memdump_f(void)
{
	memblock_t *block;
	INT32 mintag = 0, maxtag = INT32_MAX;
	INT32 i;

	if ((i = COM_CheckParm("-min")))
		mintag = atoi(COM_Argv(i + 1));

	if ((i = COM_CheckParm("-max")))
		maxtag = atoi(COM_Argv(i + 1));

	for (block = head.next; block != &head; block = block->next)
		if (block->tag >= mintag && block->tag <= maxtag)
		{
			char *filename = strrchr(block->ownerfile, PATHSEP[0]);
			CONS_Printf("[%3d] %s (%s) bytes @ %s:%d\n", block->tag, sizeu1(block->size), sizeu2(block->realsize), filename ? filename + 1 : block->ownerfile, block->ownerline);
		}
}
#endif
#endif

/** Creates a copy of a string.
  *
  * \param s The string to be copied.
  * \return A copy of the string, allocated in zone memory.
  */
char *Z_StrDup(const char *s)
{
	return strcpy(ZZ_Alloc(strlen(s) + 1), s);
}
