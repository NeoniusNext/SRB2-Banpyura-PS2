/* Execute the real src/z_zone.c and src/ps2/ps2_mem.c (the D4 arena) with only engine/platform services stubbed.
 * Both are included in this translation unit so budget/list invariants can be checked without test APIs.
 * ZONE_HOST_NATIVE instead tests the unchanged host allocator and the PS2_PROFILE no-op calls.
 * The PS2 path runs on a 32-bit host (matching EE pointer/size_t widths; x64 builds only check portability);
 * neither emulates EE DMA/cache behavior.
 *
 * Checks (PS2 path):
 *  - alignment for alignbits 0..16, calloc/realloc semantics, error paths
 *  - randomized torture against an independent shadow model: alignment, no overlap, data intact, accounting,
 *    coalescing (via ZA_Check and the model), eviction legality (only PU_CACHE/purgable blocks with an owner, never
 *    the current frame, never under the purge lock), owner clearing, OOM only when nothing evictable is left
 *  - deterministic LRU / frame-stamp / lock / headroom scenarios, red zones
 *  - synthetic level trace: fragmentation, cache misses, evictions for one-sided vs two-sided arenas
 */
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <malloc.h>
#pragma warning(disable:4505) /* unreferenced stubs in the native build */
#endif

/* Skip unrelated engine headers, but use the actual zone header, tags and allocator source. */
#define __DOOMDEF__
#define __DOOMTYPE__
#define __DOOMSTAT__
#define __R_PATCH__
#define __R_PICFORMATS__
#define __I_SYSTEM__
#define __I_VIDEO__
#define __M_MISC__
#define __COMMAND_H__
#define __M_ARGV__
#define LUA_SCRIPT_H
#define PS2_PROFILE
#ifndef ZONE_HOST_NATIVE
#define PS2
#endif
#ifdef _MSC_VER
/* Map the allocator's sole GNU type attribute to MSVC's equivalent. */
#define __attribute__(attrs) __declspec(align(16))
#endif
typedef int INT32;
typedef unsigned int UINT32;
typedef int boolean;
#define false 0
#define true 1
#define COM_LUA 0
#define M_GetText(s) (s)
#define M_Memcpy memcpy
#define ZZ_Alloc(s) Z_Malloc(s, PU_STATIC, NULL)
#define I_Assert(e) assert(e)
#define PATHSEP "\\"
#define DBG_MEMORY 0
#define CONS_Debug(flag, ...) ((void)(flag))
#define DEBFILE(s) ((void)(s))
#define va(...) ""

#ifdef ZONE_TRACE
#define TRACE(...) (fprintf(stderr, __VA_ARGS__), fputc(10, stderr))
#else
#define TRACE(...) ((void)0)
#endif

static jmp_buf error_jump;
static int expecting_error;
static char error_text[512];
static unsigned int checks;
static const size_t heap_capacity = 11u << 20;

static void I_Error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(error_text, sizeof error_text, fmt, ap);
	va_end(ap);
	if (expecting_error)
		longjmp(error_jump, 1);
	fprintf(stderr, "Unexpected I_Error: %s\n", error_text);
	exit(2);
}
static void check(int condition, const char *what)
{
	checks++;
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", what);
		exit(1);
	}
}
static char cons_log[1 << 16];
static size_t cons_len;
static int cons_echo;
static void CONS_Printf(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	size_t n;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	n = strlen(line);
	if (cons_echo)
		fputs(line, stdout);
	if (cons_len + n < sizeof cons_log)
	{
		memcpy(cons_log + cons_len, line, n + 1);
		cons_len += n;
	}
}
#define I_OutputMsg CONS_Printf
static void cons_clear(void) { cons_len = 0; cons_log[0] = 0; }
static void COM_AddCommand(const char *name, void (*fn)(void), int flags)
{ (void)name; (void)fn; (void)flags; }
static size_t COM_Argc(void) { return 0; }
static const char *COM_Argv(size_t i) { (void)i; return ""; }
static size_t COM_CheckParm(const char *s) { (void)s; return 0; }
static const char *test_parm, *test_value;
static int M_CheckParm(const char *s) { return test_parm && !strcmp(s, test_parm); }
static boolean M_IsNextParm(void) { return test_value != NULL; }
static const char *M_GetNextParm(void) { return test_value; }
static void LUA_InvalidateUserdata(void *p) { (void)p; }
/* Synthetic PU_SPRITE payloads are not patch_t objects; only the real renderer owns that layout. */
static int sprite_evictable; /* the fragmentation trace models evictable sprite patches (PS2-72); every other test keeps them pinned */
boolean Patch_IsEvictable(const void *p) { (void)p; return sprite_evictable != 0; }
static size_t I_GetFreeMem(size_t *total) { *total = heap_capacity; return heap_capacity; }
size_t PS2_HeapCapacity(void) { return heap_capacity; }
static const char *sizeu1(size_t n)
{
	static char s[32];
	snprintf(s, sizeof s, "%zu", n);
	return s;
}
static const char *sizeu2(size_t n) { return sizeu1(n); }

#ifdef _MSC_VER
#define aligned_release(p) _aligned_free(p)
#define host_aligned_alloc(align, size) _aligned_malloc((size), (align))
#else
#define aligned_release(p) free(p)
#define host_aligned_alloc(align, size) aligned_alloc((align), (size))
#endif
static size_t injected_heap_limit;
static void *host_memalign(size_t align, size_t size)
{
	if (injected_heap_limit && size > injected_heap_limit)
		return NULL;
	return host_aligned_alloc(align, size);
}
#define memalign(align, size) host_memalign((align), (size))

#ifdef ZONE_HOST_SOURCE
#include ZONE_HOST_SOURCE
#else
#include "../../src/z_zone.c"
#endif
#ifdef PS2
#ifdef ZONE_MEM_SOURCE
#include ZONE_MEM_SOURCE
#else
#include "../../src/ps2/ps2_mem.c"
#endif
#endif

#ifdef PS2
/* ---------------------------------------------------------------------------------------------- */
/* Test arena                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

static unsigned char *arena_raw;  /* what the C allocator returned */
static unsigned char *arena_mem;  /* the arena base: 64 KiB aligned + arena_skew, so a run does not depend on where malloc put the block */
static int ship_sides, ship_prefer;  /* the policy ps2_mem.c ships with (taken before any test changes it) */
static size_t arena_skew;         /* the traces vary it: the placement of 64 KiB-aligned blocks depends on the base address */

static uint64_t rng_state = 88172645463325252ull;
static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 11);
}
static uint32_t rndn(uint32_t n) { return n ? rnd() % n : 0; }

static void arena_reset(size_t bytes, int twosided, int prefer, int redzone)
{
	if (arena_raw)
		aligned_release(arena_raw);
	arena_raw = memalign(64, bytes + 65536 + arena_skew);
	check(arena_raw != NULL, "test arena memory");
	arena_mem = (unsigned char *)(((uintptr_t)arena_raw + 65535) & ~(uintptr_t)65535) + arena_skew;
	memset(arena_mem, 0xCD, bytes); /* nothing may rely on zeroed memory */
	za_twosided = twosided;
	za_prefer = prefer;
	za_redzone = redzone;
	check(ZA_InitMem(arena_mem, bytes) == 0, "arena init");
	zlevel_play = false; /* the phase of an earlier test must not decide the sides of this one */
	zframe = 0;
	zframe_explicit = false;
	zpurgelock = 0;
	zheadroom = 0;
	zreport_interval = 0;
	zreg_n = 0; /* PS2-76: the registry belongs to the arena */
	zreg_ok = true;
	zpurge_maybe = false;
}

static size_t blk_bytes(const void *p) { return ZA_SIZE(ZA_BLOCK(p)); }

/* Independent walk of the arena: tiling, flags, coalescing, accounting. */
static void consistent(void)
{
	zablock_t *b;
	zastats_t st;
	size_t used = 0, freeb = 0, nused = 0, nfree = 0;
	int prevfree = 0;
	char msg[256];
	Z_CheckHeap(123);
	check(ZA_Check(msg, sizeof msg) == 0, "ZA_Check clean");
	for (b = ZA_First(); b; b = ZA_Next(b))
	{
		check(ZA_SIZE(b) >= ZA_MINBLK && ZA_SIZE(b) % 16 == 0, "block size multiple of 16 and >= 32");
		check((uintptr_t)b % 16 == 0, "header 16-byte aligned");
		if (ZA_ISFREE(b))
		{
			check(!prevfree, "no two adjacent free blocks (coalesced)");
			freeb += ZA_SIZE(b);
			nfree++;
			prevfree = 1;
		}
		else
		{
			check((uintptr_t)ZA_PAYLOAD(b) % 16 == 0, "payload 16-byte aligned");
			check(ZA_HDR + b->realsize <= ZA_SIZE(b), "payload fits in its block");
			used += ZA_SIZE(b);
			nused++;
			prevfree = 0;
		}
	}
	ZA_Stats(&st);
	check(used + freeb == st.arena, "used + free = arena");
	check(used == st.used && freeb == st.freebytes, "accounting equals the walk");
	check(nused == st.usedblocks && nfree == st.freeblocks, "block counts equal the walk");
	check(Z_TotalUsage() == used, "Z_TotalUsage equals used arena bytes");
	if (zreg_ok)
	{
		/* PS2-76: the cache registry is exactly the used PU_CACHE/PU_SPRITE blocks, sorted by address */
		UINT32 n = 0;
		for (b = ZA_First(); b; b = ZA_Next(b))
			if (!ZA_ISFREE(b) && (ZA_TAG(b) == PU_CACHE || ZA_TAG(b) == PU_SPRITE))
			{
				check(n < zreg_n && zreg[n] == b, "registry holds every cache block in address order");
				n++;
			}
		check(n == zreg_n, "registry holds nothing else");
	}
}
static void empty_zone(void)
{
	zastats_t st;
	Z_FreeTags(0, INT32_MAX);
	consistent();
	ZA_Stats(&st);
	check(st.usedblocks == 0 && st.freeblocks == 1 && st.largestfree == st.arena, "empty zone is one free block");
}

static void expect_alloc_error(size_t size, INT32 bits, const char *message)
{
	zastats_t before, after;
	ZA_Stats(&before);
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		(void)Z_MallocAlign(size, PU_STATIC, NULL, bits);
		check(0, "allocation should have refused");
	}
	expecting_error = 0;
	check(strstr(error_text, message) != NULL, "expected error reason");
	ZA_Stats(&after);
	check(before.used == after.used && before.usedblocks == after.usedblocks,
		"refusal leaves live allocations and accounting unchanged");
	consistent();
}

/* ---------------------------------------------------------------------------------------------- */
/* Alignment, calloc/realloc, errors                                                               */
/* ---------------------------------------------------------------------------------------------- */

static void alignment_and_cycles(void)
{
	static const size_t sizes[] = {0, 1, 2047, 2048, 4097};
	size_t i, j;
	INT32 bits;
	arena_reset(8u << 20, ship_sides, ship_prefer, 0);
	for (bits = 0; bits <= 16; bits++)
		for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
			for (j = 0; j < 2; j++)
			{
				unsigned char *owner = NULL;
				unsigned char *p;
				size_t k;
				za_twosided = (int)j;
				TRACE("calloc size %zu bits %d side %zu", sizes[i], (int)bits, j);
				p = Z_CallocAlign(sizes[i], PU_STATIC, &owner, bits);
				check(owner == p, "calloc sets owner");
				check((uintptr_t)p % ((uintptr_t)1 << bits) == 0, "requested alignment up to 65536");
				check((uintptr_t)p % (sizes[i] >= 2048 ? 64 : 16) == 0, "minimum payload alignment");
				check((uintptr_t)ZA_BLOCK(p) % 16 == 0, "header address is 16-byte aligned");
				for (k = 0; k < sizes[i]; k++)
					check(p[k] == 0, "calloc zeros the entire payload");
				memset(p, 0x3C, sizes[i]);
				consistent();
				Z_Free(p);
				check(owner == NULL, "free clears owner");
				consistent();
			}
	za_twosided = 1;
	for (i = 0; i < 20000; i++)
	{
		unsigned char *owner = NULL;
		size_t n = i % 8192;
		unsigned char *p = Z_MallocAlign(n, PU_STATIC, &owner, (INT32)(i % 17));
		memset(p, 0x5A, n);
		Z_Free(p);
		check(owner == NULL, "cycle owner cleared");
		if (i % 97 == 0)
		{
			consistent();
			check(Z_TotalUsage() == 0, "repeated alloc/free does not drift");
		}
	}
	for (i = 0; i < 200; i++)
	{
		unsigned char *owner = NULL;
		unsigned char *p = Z_CallocAlign(257, PU_STATIC, &owner, 8);
		memset(p, 0x62, 257);
		p = Z_ReallocAlign(p, 4097, PU_STATIC, &owner, 16);
		check(p == owner && (uintptr_t)p % 65536 == 0, "realloc restores owner and requested alignment");
		for (j = 0; j < 4097; j++)
			check(p[j] == (j < 257 ? 0x62 : 0), "realloc preserves data and zeros extension");
		consistent();
		p = Z_Realloc(p, 0, PU_STATIC, &owner);
		check(p == NULL && owner == NULL, "zero-size realloc frees and clears owner");
		consistent();
		check(Z_TotalUsage() == 0, "realloc/free does not drift");
	}
	expect_alloc_error(1, -1, "invalid alignment");
	expect_alloc_error(1, 32, "invalid alignment");
	expect_alloc_error(1, INT32_MAX, "invalid alignment");
	expect_alloc_error(SIZE_MAX, 0, "too large");
	expect_alloc_error(SIZE_MAX - ZA_HDR, 16, "too large");
	expect_alloc_error(1, 31, "Out of memory");
	expect_alloc_error(9u << 20, 0, "Out of memory");
	/* regression (R_CreateInterpolator_Polyobj, vanilla r_fps.c): Z_CallocAlign(.., PU_LEVEL, NULL, 32) is not a 32-byte
	 * alignment request but alignbits = 32 = 2^32 bytes. The vanilla zone ignored the argument; the arena refuses it with
	 * a message that names the bits, and the 4 (16 bytes) that the profile passes instead works and is zeroed. */
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		(void)Z_CallocAlign(sizeof(INT32) * 2 * 6, PU_LEVEL, NULL, 32);
		check(0, "alignbits 32 through Z_CallocAlign should be refused");
	}
	expecting_error = 0;
	check(strstr(error_text, "invalid alignment bits 32") != NULL, "alignbits 32 refused with the offending value in the message");
	{
		INT32 *verts = Z_CallocAlign(sizeof(INT32) * 2 * 6, PU_LEVEL, NULL, 4);
		size_t k;

		check(verts != NULL && ((uintptr_t)verts & 15) == 0, "polyobject interpolator allocation with alignbits 4 is 16-byte aligned");
		for (k = 0; verts && k < 12; k++)
			check(verts[k] == 0, "polyobject interpolator allocation is zeroed");
		Z_Free(verts);
	}
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		(void)Z_MallocAlign(4, PU_CACHE_UNLOCKED, NULL, 0);
		check(0, "purgable block without owner should be refused");
	}
	expecting_error = 0;
	check(strstr(error_text, "no user") != NULL, "purgable without owner refused");
	check(Z_TotalUsage() == 0, "ownerless purgable refusal does not allocate or leak");
	for (bits = -1; bits <= 256; bits += 257)
	{
		expecting_error = 1;
		if (!setjmp(error_jump))
		{
			(void)Z_MallocAlign(4, bits, NULL, 0);
			check(0, "invalid purge tag should be refused");
		}
		expecting_error = 0;
		check(strstr(error_text, "invalid purge tag") != NULL && Z_TotalUsage() == 0, "invalid tag leaves accounting unchanged");
	}
	empty_zone();
	printf("PASS alignment 0..16 x both sides, invalid shifts/size/tag errors, 20000 alloc/free cycles, 200 realloc cycles\n");
}

/* ---------------------------------------------------------------------------------------------- */
/* Coalescing                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

static void coalescing(void)
{
	enum { N = 64 };
	void *p[N];
	size_t order[N], i, j, round;
	zastats_t st;
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	for (round = 0; round < 40; round++)
	{
		for (i = 0; i < N; i++)
			p[i] = Z_Malloc(16 + (i * 37) % 700, round & 1 ? PU_STATIC : PU_LEVEL, NULL);
		for (i = 0; i < N; i++)
			order[i] = i;
		for (i = N - 1; i > 0; i--)
		{
			j = rndn((uint32_t)i + 1);
			size_t t = order[i];
			order[i] = order[j];
			order[j] = t;
		}
		if (round == 0)
			for (i = 0; i < N; i++)
				order[i] = i; /* ascending */
		if (round == 1)
			for (i = 0; i < N; i++)
				order[i] = N - 1 - i; /* descending */
		for (i = 0; i < N; i++)
		{
			Z_Free(p[order[i]]);
			consistent();
		}
		ZA_Stats(&st);
		check(st.freeblocks == 1 && st.largestfree == st.arena, "freeing everything in any order leaves one free block");
	}
	/* a hole between two live blocks is reused whole, and merges when its neighbour goes */
	{
		/* No alignment gaps here: Z_Malloc's sizeof(void *) alignbits becomes 8 on x64 (256 bytes). */
		void *a = Z_MallocAlign(100, PU_STATIC, NULL, 4), *b = Z_MallocAlign(1000, PU_STATIC, NULL, 4),
			*c = Z_MallocAlign(100, PU_STATIC, NULL, 4);
		size_t hole = blk_bytes(b);
		Z_Free(b);
		ZA_Stats(&st);
		check(st.freeblocks == 2, "hole between neighbours is its own free block");
		b = Z_MallocAlign(1000, PU_STATIC, NULL, 4);
		check(blk_bytes(b) == hole, "same-size request reuses the hole exactly");
		Z_Free(b);
		Z_Free(a);
		ZA_Stats(&st);
		check(st.freeblocks == 2, "free merges with the following hole");
		Z_Free(c);
		ZA_Stats(&st);
		check(st.freeblocks == 1, "last free merges both sides");
	}
	empty_zone();
	printf("PASS coalescing (ascending, descending, 38 shuffled rounds, hole reuse)\n");
}

/* ---------------------------------------------------------------------------------------------- */
/* Randomized torture against a shadow model                                                       */
/* ---------------------------------------------------------------------------------------------- */

#define MAXLIVE 3072
typedef struct
{
	int live;
	unsigned char *p;
	size_t size, align;
	INT32 tag;
	int hasuser;
	uint32_t seed;
	UINT32 stampframe;
} sh_t;

static sh_t sh[MAXLIVE];
static void *slotmem[MAXLIVE];
static int locked;               /* purge lock depth as the model sees it */
static unsigned long st_alloc, st_free, st_realloc, st_tag, st_evict, st_oom, st_lock_ops, st_iter, st_level, st_frames,
	st_user, st_touch, st_purgetag;

static uint8_t pat(uint32_t seed, size_t i) { return (uint8_t)((seed ^ ((uint32_t)i * 0x9E3779B1u)) >> 13); }
static void fill(unsigned char *p, size_t n, uint32_t seed)
{
	size_t i;
	for (i = 0; i < n; i++)
		p[i] = pat(seed, i);
}
static void verify_pattern(const sh_t *s, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (s->p[i] != pat(s->seed, i))
		{
			fprintf(stderr, "pattern corrupt at +%zu of %zu-byte block\n", i, s->size);
			check(0, "live block data intact (no overlap, no stray writes)");
		}
}

static int locking; /* inside the Z_PurgeLock(true) that starts the 3D view: nothing is held yet */

static int evict_legal(const sh_t *s)
{
	if (s->tag >= PU_PURGELEVEL)
		return !locked;
	if (locking && s->tag == PU_CACHE && s->hasuser)
		return 1; /* the lock makes its headroom from cache of any age */
	if (s->tag == PU_CACHE && s->hasuser && s->stampframe != zframe)
		return 1;
	return 0;
}

/* After any operation: blocks the zone freed on its own (owner slot NULL) must have been legal victims. */
static void reconcile(void)
{
	int id;
	for (id = 0; id < MAXLIVE; id++)
	{
		sh_t *s = &sh[id];
		if (!s->live)
			continue;
		if (s->hasuser)
		{
			if (slotmem[id] == NULL)
			{
				if (!evict_legal(s))
					fprintf(stderr, "illegal eviction: id %d tag %d stamp %u frame %u locked %d\n", id, s->tag,
						(unsigned)s->stampframe, (unsigned)zframe, locked);
				check(evict_legal(s), "zone freed only legal victims (cache/purgable with owner, not this frame, not locked)");
				s->live = 0;
				st_evict++;
			}
			else
				check(slotmem[id] == s->p, "owner slot still points at its block");
		}
	}
}

static void sweep(int full)
{
	int id;
	for (id = 0; id < MAXLIVE; id++)
	{
		sh_t *s = &sh[id];
		zablock_t *b;
		size_t need;
		if (!s->live)
			continue;
		b = ZA_BLOCK(s->p);
		check((b->sf & ZAF_USED) && (b->sf & ZAF_VALID), "live block header intact");
		check(b->realsize == s->size && ZA_TAG(b) == s->tag, "header size and tag match the model");
		check((uintptr_t)s->p % s->align == 0, "payload alignment as requested");
		need = (ZA_HDR + s->size + (za_redzone ? ZA_GUARD_MIN : 0) + 15) & ~(size_t)15;
		if (need < ZA_MINBLK)
			need = ZA_MINBLK;
		check(ZA_SIZE(b) >= need && ZA_SIZE(b) <= need + ZA_MINBLK, "block size within the minimum plus absorbed tail");
		if (full)
			verify_pattern(s, s->size);
	}
}

/* No two live payloads (header included) overlap, all inside the arena. */
static int cmp_range(const void *a, const void *b)
{
	uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
	return x < y ? -1 : x > y;
}
static void overlap_check(void)
{
	static uintptr_t r[MAXLIVE][2];
	int id;
	size_t n = 0, i;
	for (id = 0; id < MAXLIVE; id++)
		if (sh[id].live)
		{
			r[n][0] = (uintptr_t)ZA_BLOCK(sh[id].p);
			r[n][1] = r[n][0] + blk_bytes(sh[id].p);
			n++;
		}
	qsort(r, n, sizeof r[0], cmp_range);
	for (i = 0; i < n; i++)
	{
		check(r[i][0] >= (uintptr_t)arena_mem && r[i][1] <= (uintptr_t)arena_mem + za_size, "block inside the arena");
		if (i)
			check(r[i - 1][1] <= r[i][0], "no two live blocks overlap");
	}
}

static int find_id(const void *p)
{
	int id;
	for (id = 0; id < MAXLIVE; id++)
		if (sh[id].live && sh[id].p == p)
			return id;
	return -1;
}

static size_t pick_size(void)
{
	uint32_t r = rndn(100);
	if (r < 55)
		return rndn(256);
	if (r < 85)
		return rndn(4096);
	if (r < 97)
		return rndn(40000);
	return 40000 + rndn(160000);
}
static INT32 pick_tag(void)
{
	static const INT32 tags[] = {PU_STATIC, PU_LEVEL, PU_CACHE, PU_CACHE, PU_CACHE, PU_LEVSPEC, PU_PATCH, PU_PATCH_DATA,
		PU_SPRITE, PU_HUDGFX, PU_SOUND, PU_CACHE_UNLOCKED, PU_HWRCACHE_UNLOCKED, PU_LUA};
	return tags[rndn(sizeof tags / sizeof tags[0])];
}
static INT32 pick_bits(void)
{
	uint32_t r = rndn(100);
	if (r < 70)
		return (INT32)rndn(5);
	if (r < 90)
		return 6;
	if (r < 97)
		return 8 + (INT32)rndn(5);
	return 16;
}

static int free_id(void)
{
	int tries, id;
	for (tries = 0; tries < 64; tries++)
	{
		id = (int)rndn(MAXLIVE);
		if (!sh[id].live)
			return id;
	}
	for (id = 0; id < MAXLIVE; id++)
		if (!sh[id].live)
			return id;
	return -1;
}
static int live_id(void)
{
	int tries, id;
	for (tries = 0; tries < 64; tries++)
	{
		id = (int)rndn(MAXLIVE);
		if (sh[id].live)
			return id;
	}
	return -1;
}

static void op_alloc(void)
{
	int id = free_id();
	INT32 tag = pick_tag(), bits = pick_bits();
	int hasuser;
	size_t size = pick_size(), align;
	void *volatile got = NULL;
	if (id < 0)
		return;
	hasuser = tag >= PU_PURGELEVEL || (tag == PU_CACHE ? rndn(10) < 8 : rndn(10) < 2);
	slotmem[id] = NULL;
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		got = rndn(4) ? Z_MallocAlign(size, tag, hasuser ? &slotmem[id] : NULL, bits)
		              : Z_CallocAlign(size, tag, hasuser ? &slotmem[id] : NULL, bits);
	}
	else
	{
		/* out of memory is legal only if nothing evictable was left and the request really does not fit */
		zablock_t *b;
		zastats_t st;
		st_oom++;
		check(strstr(error_text, "Out of memory") != NULL, "only OOM may refuse a valid request");
		for (b = ZA_First(); b; b = ZA_Next(b))
				check(!Z_Evictable(b, false) && !( !locked && !ZA_ISFREE(b) && ZA_TAG(b) >= PU_PURGELEVEL),
					"OOM with nothing evictable left");
		ZA_Stats(&st);
		align = (size_t)1 << bits;
		if (!locked && align <= 64)
			check(st.largestfree < ((ZA_HDR + size + (za_redzone ? ZA_GUARD_MIN : 0) + 15) & ~(size_t)15) + 160,
				"OOM only when no free block can hold the request");
		got = NULL;
	}
	expecting_error = 0;
	reconcile();
	if (got)
	{
		sh_t *s = &sh[id];
		unsigned char *p = got;
		align = (size_t)1 << bits;
		if (align < (size >= 2048 ? 64u : 16u))
			align = size >= 2048 ? 64u : 16u;
		s->live = 1;
		s->p = p;
		s->size = size;
		s->align = align;
		s->tag = tag;
		s->hasuser = hasuser;
		s->seed = rnd();
		s->stampframe = zframe;
		if (hasuser)
			check(slotmem[id] == p, "owner set by allocation");
		fill(p, size, s->seed);
		st_alloc++;
	}
}

static void op_free(void)
{
	int id = live_id();
	sh_t *s;
	if (id < 0)
		return;
	s = &sh[id];
	verify_pattern(s, s->size);
	Z_Free(s->p);
	if (s->hasuser)
		check(slotmem[id] == NULL, "free clears owner");
	s->live = 0;
	st_free++;
}

static void op_realloc(void)
{
	int id = live_id();
	sh_t *s;
	size_t n, k;
	void *volatile got = NULL;
	if (id < 0)
		return;
	s = &sh[id];
	n = pick_size();
	if (!n)
		n = 1;
	verify_pattern(s, s->size);
	expecting_error = 1;
	if (!setjmp(error_jump))
		got = Z_ReallocAlign(s->p, n, s->tag, s->hasuser ? &slotmem[id] : NULL, 0);
	else
	{
		st_oom++;
		got = NULL;
	}
	expecting_error = 0;
	if (got)
	{
		unsigned char *p = got;
		size_t keep = n < s->size ? n : s->size;
		/* the old block was stamped this frame by the realloc, so it survived; the new one is the owner now */
		for (k = 0; k < keep; k++)
			check(p[k] == pat(s->seed, k), "realloc preserves the common prefix");
		for (k = keep; k < n; k++)
			check(p[k] == 0, "realloc zeroes the extension");
		s->p = p;
		s->size = n;
		s->stampframe = zframe;
		s->seed = rnd();
		if ((uintptr_t)p % 16 != 0 || (n >= 2048 && (uintptr_t)p % 64 != 0))
			check(0, "realloc result alignment");
		s->align = n >= 2048 ? 64 : 16;
		fill(p, n, s->seed);
		if (s->hasuser)
			check(slotmem[id] == p, "realloc keeps the owner");
		st_realloc++;
	}
	reconcile();
}

static void op_changetag(void)
{
	int id = live_id();
	sh_t *s;
	INT32 tag;
	if (id < 0)
		return;
	s = &sh[id];
	tag = pick_tag();
	if (tag >= PU_PURGELEVEL && !s->hasuser)
		tag = PU_CACHE;
	Z_ChangeTag(s->p, tag);
	s->tag = tag;
	s->stampframe = zframe;
	st_tag++;
}

static void op_setuser(void)
{
	int id = live_id();
	sh_t *s;
	if (id < 0)
		return;
	s = &sh[id];
	if (s->hasuser)
		return;
	Z_SetUser(s->p, &slotmem[id]);
	s->hasuser = 1;
	s->stampframe = zframe;
	st_user++;
}

static void op_touch(void)
{
	int id = live_id();
	if (id < 0)
		return;
	Z_Touch(sh[id].p);
	sh[id].stampframe = zframe;
	st_touch++;
}

static void op_frame(void)
{
	Z_NextFrame();
	st_frames++;
}

static void op_level_exit(void)
{
	int id;
	Z_FreeTags(PU_LEVEL, PU_PURGELEVEL - 1);
	Z_LevelPhase(false);
	for (id = 0; id < MAXLIVE; id++)
		if (sh[id].live && sh[id].tag >= PU_LEVEL && sh[id].tag < PU_PURGELEVEL)
		{
			if (sh[id].hasuser)
				check(slotmem[id] == NULL, "level exit clears owners");
			sh[id].live = 0;
		}
	st_level++;
}

static void op_purgetags(void)
{
	int id;
	Z_FreeTags(PU_PURGELEVEL, INT32_MAX);
	for (id = 0; id < MAXLIVE; id++)
		if (sh[id].live && sh[id].tag >= PU_PURGELEVEL)
		{
			check(slotmem[id] == NULL, "purge clears owners");
			sh[id].live = 0;
		}
	st_purgetag++;
}

static int iter_toggle, iter_killneighbour;
static boolean iter_cb(void *mem)
{
	int id = find_id(mem);
	check(id >= 0, "iterator visits only live blocks");
	iter_toggle++;
	if (iter_killneighbour && iter_toggle % 3 == 0)
	{
		/* the callback frees the block right after this one, as Patch_FreeTagsCallback frees its pixel arrays */
		unsigned char *after = (unsigned char *)ZA_BLOCK(mem) + blk_bytes(mem);
		int nid = -1, k;
		for (k = 0; k < MAXLIVE; k++)
			if (sh[k].live && (unsigned char *)ZA_BLOCK(sh[k].p) == after && sh[k].tag != sh[id].tag)
				nid = k;
		if (nid >= 0)
		{
			Z_Free(sh[nid].p);
			sh[nid].live = 0;
		}
	}
	if (iter_toggle & 1)
	{
		sh[id].live = 0;
		return true;
	}
	return false;
}
static void op_iterate(void)
{
	INT32 lo = pick_tag(), hi = lo;
	iter_toggle = 0;
	iter_killneighbour = (int)rndn(2);
	Z_IterateTags(lo, hi, iter_cb);
	st_iter++;
	/* blocks the callback freed were removed from the model inside; owners must be clear */
	reconcile();
}

static void op_lock(void)
{
	int n;
	if (locked == 0)
	{
		locking = 1;
		Z_PurgeLock(true); /* may evict to make headroom: judged as unlocked, current-frame cache included */
		reconcile();
		locking = 0;
		locked++;
		st_lock_ops++;
		/* inside the lock reconcile() fails on any block the zone frees by itself */
		for (n = 0; n < 40; n++)
		{
			uint32_t r = rndn(10);
			if (r < 6)
				op_alloc();
			else if (r < 8)
				op_realloc();
			else if (r < 9)
				op_touch();
			else
				op_free();
		}
		locked--;
		Z_PurgeLock(false);
	}
}

static void torture(size_t arena, int twosided, int prefer, int redzone, uint64_t seed, int ops, int headroom)
{
	int i, id;
	memset(sh, 0, sizeof sh);
	memset(slotmem, 0, sizeof slotmem);
	locked = 0;
	rng_state = seed;
	arena_reset(arena, twosided, prefer, redzone);
	zheadroom = (size_t)headroom;
	for (i = 0; i < ops; i++)
	{
		uint32_t r = rndn(1000);
		if (r < 330)
			op_alloc();
		else if (r < 560)
			op_free();
		else if (r < 620)
			op_realloc();
		else if (r < 680)
			op_changetag();
		else if (r < 700)
			op_setuser();
		else if (r < 760)
			op_touch();
		else if (r < 800)
			op_frame();
		else if (r < 806)
			op_level_exit();
		else if (r < 812)
			op_purgetags();
		else if (r < 818)
			Z_LevelPhase(rndn(3) != 0); /* PS2-75: the level phases move the frontier */
		else if (r < 830)
			op_iterate();
		else if (r < 850)
			op_lock();
		else
			op_alloc();
		reconcile();
		if (i % 8 == 0)
		{
			consistent();
			sweep(0);
		}
		if (i % 64 == 0)
		{
			sweep(1);
			overlap_check();
		}
	}
	/* settle: model and zone agree on every live block */
	consistent();
	sweep(1);
	overlap_check();
	for (id = 0; id < MAXLIVE; id++)
		if (sh[id].live)
		{
			size_t tagsum = 0;
			int k;
			for (k = 0; k < MAXLIVE; k++)
				if (sh[k].live && sh[k].tag == sh[id].tag)
					tagsum += blk_bytes(sh[k].p);
			check(Z_TagUsage(sh[id].tag) == tagsum, "per-tag usage equals the sum of the model's blocks");
		}
	empty_zone();
	for (id = 0; id < MAXLIVE; id++)
		sh[id].live = 0;
}

static void torture_all(void)
{
	static const struct { size_t arena; int sides, prefer, redzone; int headroom; } cfg[] = {
		{1u << 20, 1, 4, 1, 0},
		{3u << 20, 1, 4, 0, 0},
		{3u << 20, 0, 1, 1, 128 << 10},
		{6u << 20, 1, 1, 0, 256 << 10},
		{6u << 20, 1, 8, 1, 0},
		{16u << 20, 1, 4, 0, 1u << 20},
	};
	size_t c;
	int seed;
	for (c = 0; c < sizeof cfg / sizeof cfg[0]; c++)
		for (seed = 1; seed <= 2; seed++)
			torture(cfg[c].arena, cfg[c].sides, cfg[c].prefer, cfg[c].redzone, 0x9E3779B97F4A7C15ull * (uint64_t)(c * 7 + (size_t)seed),
				60000, cfg[c].headroom);
	printf("PASS torture: %zu configs x 2 seeds x 60000 ops: %lu alloc %lu free %lu realloc %lu retag %lu setuser %lu touch "
		"%lu frames %lu level exits %lu purges %lu iterate %lu lock scopes; %lu evictions, %lu OOM refusals\n",
		sizeof cfg / sizeof cfg[0], st_alloc, st_free, st_realloc, st_tag, st_user, st_touch, st_frames, st_level, st_purgetag,
		st_iter, st_lock_ops, st_evict, st_oom);
	check(st_evict > 100 && st_oom > 0 && st_lock_ops > 100 && st_iter > 100, "torture exercised eviction, OOM, locks and iteration");
}

/* ---------------------------------------------------------------------------------------------- */
/* Deterministic LRU / frame / lock scenarios                                                      */
/* ---------------------------------------------------------------------------------------------- */

static void lru_semantics(void)
{
	void *c[8];
	void *scratch, *fixed, *big;
	int i;
	zastats_t st;
	size_t blk;

	/* arena of ~1 MiB, eight 96 KiB caches that fill it */
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	for (i = 0; i < 8; i++)
	{
		c[i] = NULL;
		Z_Malloc(96u << 10, PU_CACHE, &c[i]);
		Z_NextFrame(); /* each its own frame: c[0] oldest */
	}
	blk = blk_bytes(c[0]);
	scratch = Z_Malloc(30000, PU_CACHE, NULL); /* ownerless cache is scratch: never evicted */
	fixed = Z_Malloc(60000, PU_STATIC, NULL);
	Z_Touch(c[0]); /* c[0] is now the newest */
	big = Z_Malloc(2 * blk + 40000, PU_STATIC, NULL); /* needs two victims */
	check(c[0] != NULL, "touched cache survives (LRU, not FIFO)");
	check(c[1] == NULL && c[2] == NULL, "the least recently used caches are evicted first");
	check(c[7] != NULL, "the newest cache stays");
	check(ZA_BLOCK(scratch)->sf & ZAF_USED && ZA_BLOCK(fixed)->sf & ZAF_USED, "ownerless cache and static blocks stay");
	(void)big;
	consistent();

	/* frame protection: everything allocated this frame is safe, so the request fails with an OOM report */
	empty_zone();
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	for (i = 0; i < 8; i++)
	{
		c[i] = NULL;
		Z_Malloc(96u << 10, PU_CACHE, &c[i]); /* all in frame 0 */
	}
	ZA_Stats(&st);
	cons_clear();
	expect_alloc_error(300000, 0, "Out of memory");
	check(strstr(cons_log, "ps2_mem: tag  49 PU_CACHE") != NULL, "OOM report lists usage by tag");
	check(strstr(cons_log, "OOM: request 300000") != NULL, "OOM report names the failing request");
	for (i = 0; i < 8; i++)
		check(c[i] != NULL, "current-frame blocks are not evicted");
	Z_NextFrame();
	big = Z_Malloc(300000, PU_STATIC, NULL);
	{
		/* equal-age ties go together, in address order (the cache sits at the end the level does not grow from, PS2-63, so which
		   of the blocks go is a matter of the side); the request plus the eviction slack is freed, never the whole cache */
		int gone = 0;
		for (i = 0; i < 8; i++)
			gone += c[i] == NULL;
		check(gone >= 3 && gone < 8, "after the frame ends the same request evicts just enough of the equally old caches");
	}
	consistent();

	/* purge lock: no eviction; the lock itself makes headroom first */
	empty_zone();
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	zheadroom = 300000;
	for (i = 0; i < 8; i++)
	{
		c[i] = NULL;
		Z_Malloc(100000, PU_CACHE, &c[i]);
		Z_NextFrame();
	}
	check(ZA_FreeBytes() < 300000, "arena nearly full before the lock");
	Z_PurgeLock(true);
	check(ZA_FreeBytes() >= 300000, "lock ensures its headroom by evicting old cache");
	check(c[0] == NULL && c[7] != NULL, "headroom evicted the oldest first");
	Z_PurgeLock(true);
	{
		void *x = Z_Malloc(250000, PU_STATIC, NULL); /* inside the headroom */
		int alive_before = 0, alive_after = 0;
		for (i = 0; i < 8; i++)
		{
			alive_before += c[i] != NULL;
			Z_Touch(c[i]); // every held root is current, including roots fetched before the lock
		}
		expect_alloc_error(ZA_FreeBytes() + 1000, 0, "Out of memory");
		for (i = 0; i < 8; i++)
			alive_after += c[i] != NULL;
		check(alive_before == alive_after, "refusal under the lock evicts nothing");
		(void)x;
	}
	Z_PurgeLock(false);
	Z_PurgeLock(false);
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		Z_PurgeLock(false);
		check(0, "unbalanced unlock should fail");
	}
	expecting_error = 0;
	check(zpurgelock == 0, "unbalanced unlock does not corrupt lock state");

	/* the lock makes ONE free block of the headroom, not just free bytes: the 3D view's own allocations (the draw segment
	 * array doubling to 1.7 MB on a mid-size map, measured on the EE) need a contiguous block; and the caches of the
	 * current frame may go, because nothing is held when the view starts (a level load and its first view share a frame) */
	empty_zone();
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	zheadroom = 250000;
	Z_NextFrame(); /* use the real displayed-frame hook: the lock must not advance the stamp */
	{
		void *keep[4] = {NULL, NULL, NULL, NULL}, *cc[3] = {NULL, NULL, NULL}, *hole[2] = {NULL, NULL};
		zastats_t before, after;
		int alive = 0;

		for (i = 0; i < 3; i++)
		{
			keep[i] = Z_Malloc(20000, PU_LEVEL, NULL);
			Z_Malloc(100000, PU_CACHE, &cc[i]);
			if (i < 2)
				hole[i] = Z_Malloc(230000, PU_LEVEL, NULL);
		}
		keep[3] = Z_Malloc(20000, PU_LEVEL, NULL);
		Z_Free(hole[0]);
		Z_Free(hole[1]);
		ZA_Stats(&before);
		check(before.freebytes >= 250000 && before.largestfree < 250000, "fragmented: enough free bytes in total, not in one piece");
		check(zframe == 1, "the caches belong to the current frame");
		Z_PurgeLock(true);
		ZA_Stats(&after);
		check(after.largestfree >= 250000, "lock makes one free block of the headroom");
		for (i = 0; i < 3; i++)
			alive += cc[i] != NULL;
		check(alive == 2, "one cache block of the current frame went, the cheapest run only");
		for (i = 0; i < 4; i++)
			check(keep[i] != NULL && (((zablock_t *)((uint8_t *)keep[i] - ZA_HDR))->sf & ZAF_USED), "blocks that are not cache stay");
		consistent();
		Z_PurgeLock(false);
	}
	/* ... but never a block without an owner or a non-cache block, and only the part of a run that is needed */
	empty_zone();
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	zheadroom = 250000;
	{
		void *cc[4] = {NULL, NULL, NULL, NULL}, *noowner, *level;

		Z_Malloc(100000, PU_CACHE, &cc[0]);
		noowner = Z_Malloc(100000, PU_CACHE, NULL); /* PU_CACHE without an owner is not evictable */
		Z_Malloc(100000, PU_CACHE, &cc[1]);
		level = Z_Malloc(300000, PU_LEVEL, NULL);
		Z_Malloc(100000, PU_CACHE, &cc[2]);
		Z_Malloc(100000, PU_CACHE, &cc[3]);
		ZA_Stats(&st);
		check(st.largestfree < 250000, "arena nearly full");
		Z_PurgeLock(true);
		ZA_Stats(&st);
		check(st.largestfree >= 250000, "lock makes room next to the free space at the top");
		check(cc[3] == NULL && cc[2] != NULL && cc[1] != NULL && cc[0] != NULL, "only the cache block that is needed went");
		check(noowner != NULL && (ZA_BLOCK(noowner)->sf & ZAF_USED), "cache block without owner is never evicted");
		check(level != NULL && (ZA_BLOCK(level)->sf & ZAF_USED), "level data is never evicted");
		consistent();
		Z_PurgeLock(false);
	}
	zheadroom = 0;
	/* purgable tags (>= PU_PURGELEVEL) go before any LRU eviction, in any frame */
	empty_zone();
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	zheadroom = 0;
	{
		void *pur = NULL, *cache = NULL;
		Z_Malloc(400000, PU_CACHE_UNLOCKED, &pur);
		Z_NextFrame();
		Z_Malloc(400000, PU_CACHE, &cache);
		Z_NextFrame();
		Z_Malloc(300000, PU_STATIC, NULL);
		check(pur == NULL && cache != NULL, "purgable tag is purged first, the cache survives");
	}
	/* automatic frame advance when nobody calls Z_NextFrame (the 3D view lock stands in) */
	empty_zone();
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	{
		UINT32 f0 = zframe;
		Z_PurgeLock(true);
		Z_PurgeLock(false);
		check(zframe == f0 + 1, "without an explicit frame call the outermost lock advances the frame");
		Z_NextFrame();
		Z_PurgeLock(true);
		Z_PurgeLock(false);
		check(zframe == f0 + 2, "once Z_NextFrame is used, the lock no longer advances the frame");
	}
	empty_zone();
	printf("PASS LRU order, touch, frame protection, OOM report, lock headroom, purgable-first, automatic frame\n");
}

/* The owner is the only rebuild oracle: column/post/pixel aliases belong to that root's lifetime. */
/* PS2-71: the reclaim hook (the audio effects cache gives back idle samples when an allocation does not fit and no cache is left) */
static void *rc_block[12];
static unsigned rc_calls;
static size_t rc_asked;
static int rc_give;
static size_t reclaim_test_hook(size_t want)
{
	size_t freed = 0;
	int i;

	rc_calls++;
	rc_asked = want;
	for (i = 0; i < 12 && rc_give && freed < want; i++)
		if (rc_block[i])
		{
			freed += blk_bytes(rc_block[i]);
			Z_Free(rc_block[i]);
			rc_block[i] = NULL;
		}
	return freed;
}

static void reclaim_hook_tests(void)
{
	void *keep, *x, *cache = NULL;
	int i, alive;

	/* 1 MiB arena, eight 100 kB PU_SOUND blocks (ownerless, never evictable by the zone) leave ~240 kB */
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	Z_SetReclaimHook(reclaim_test_hook);
	rc_give = 1;
	rc_calls = 0;
	memset(rc_block, 0, sizeof rc_block);
	for (i = 0; i < 8; i++)
	{
		rc_block[i] = Z_Malloc(100000, PU_SOUND, NULL);
		Z_NextFrame();
	}
	check(ZA_FreeBytes() < 300000, "the arena is nearly full of sound blocks");

	/* a request that fits does not call the hook */
	x = Z_Malloc(100000, PU_STATIC, NULL);
	check(rc_calls == 0, "the hook is not called while the request fits");
	Z_Free(x);

	/* a request that does not fit: the hook is called, frees enough, the request succeeds; the other sound blocks stay */
	x = Z_Malloc(400000, PU_STATIC, NULL);
	check(x != NULL && rc_calls >= 1, "the request succeeds after the hook gave memory back");
	check(rc_asked >= 400000, "the hook is asked for at least the request");
	alive = 0;
	for (i = 0; i < 8; i++)
		alive += rc_block[i] != NULL;
	check(alive > 0 && alive < 8, "the hook freed some of its blocks, not all of them");
	consistent();
	Z_Free(x);

	/* caches are evicted first: with an evictable cache big enough the hook stays unused */
	empty_zone();
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	memset(rc_block, 0, sizeof rc_block);
	rc_calls = 0;
	for (i = 0; i < 4; i++)
		rc_block[i] = Z_Malloc(100000, PU_SOUND, NULL);
	Z_Malloc(500000, PU_CACHE, &cache);
	Z_NextFrame();
	keep = Z_Malloc(300000, PU_STATIC, NULL);
	check(cache == NULL && rc_calls == 0, "the cache went first, the hook was not called");
	Z_Free(keep);
	consistent();

	/* a hook that has nothing to give: a normal out-of-memory error, no loop */
	empty_zone();
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	memset(rc_block, 0, sizeof rc_block);
	for (i = 0; i < 8; i++)
		rc_block[i] = Z_Malloc(100000, PU_SOUND, NULL);
	rc_give = 0;
	rc_calls = 0;
	expect_alloc_error(400000, 0, "Out of memory");
	check(rc_calls >= 1, "the hook was asked before the error");
	for (i = 0; i < 8; i++)
		check(rc_block[i] != NULL, "a hook that gives nothing loses nothing");

	/* no hook: an error as before */
	Z_SetReclaimHook(NULL);
	expect_alloc_error(400000, 0, "Out of memory");
	empty_zone();
	memset(rc_block, 0, sizeof rc_block);
	printf("PASS reclaim hook (PS2-71)\n");
}

static void cache_lifetimes(void)
{
	void *young = NULL, *old = NULL;
	unsigned char *src, *dst;
	void *owner = NULL, *other = NULL;
	size_t i;

	/* Physical order must not break LRU when both ages exceed the fast histogram range. */
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	zframe = 100;
	Z_Malloc(100000, PU_CACHE, &young);
	Z_Malloc(100000, PU_CACHE, &old);
	ZA_SetStamp(ZA_BLOCK(old), 0);
	zframe = 200;
	check(Z_EvictLRU(blk_bytes(old), false), "long-age LRU has a victim");
	check(old == NULL && young != NULL, "age 200 is evicted before age 100 despite physical order");
	empty_zone();
	{
		enum { N = 128 };
		void *c[N];
		UINT32 ages[N];
		int order[N], j, k;
		arena_reset(1u << 20, ship_sides, ship_prefer, 0);
		zframe = Z_FRAME_MASK;
		for (j = 0; j < N; j++)
		{
			c[j] = NULL;
			Z_Malloc(100 + (size_t)j * 7, PU_CACHE, &c[j]);
			ages[j] = 63u + ((UINT32)j * 7919u * 13u) % (Z_FRAME_MASK - 63u);
			ZA_SetStamp(ZA_BLOCK(c[j]), zframe - ages[j]);
			order[j] = j;
		}
		/* Independent descending insertion sort, with unequal block sizes and ages across all radix digits. */
		for (j = 1; j < N; j++)
		{
			int id = order[j];
			for (k = j; k > 0 && ages[order[k - 1]] < ages[id]; k--)
				order[k] = order[k - 1];
			order[k] = id;
		}
		for (j = 0; j < N; j += 2)
		{
			size_t want = blk_bytes(c[order[j]]) + blk_bytes(c[order[j + 1]]) / 2;
			check(Z_EvictLRU(want, false), "radix LRU finds old victims");
			for (k = 0; k < N; k++)
				check((c[order[k]] == NULL) == (k <= j + 1), "exact long-age LRU matches independent sorted oracle");
			consistent();
		}
		empty_zone();
	}

	/* Modular stamps still distinguish old/current blocks at the 24-bit rollover. */
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	zframe = Z_FRAME_MASK - 1;
	Z_Malloc(100000, PU_CACHE, &old);
	Z_NextFrame();
	Z_NextFrame();
	Z_Malloc(100000, PU_CACHE, &young);
	check(zframe == 0 && Z_Age(ZA_BLOCK(old)) == 2, "frame rollover preserves recent age");
	Z_EvictLRU(100000, false);
	check(old == NULL && young != NULL, "rollover evicts old cache but protects frame zero");
	empty_zone();

	/* >=100 tags are purged regardless of stamps: realloc must pin the source separately. */
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	src = Z_Malloc(400000, PU_CACHE_UNLOCKED, &owner);
	memset(src, 0x63, 400000);
	Z_Malloc(350000, PU_CACHE_UNLOCKED, &other);
	dst = Z_Realloc(src, 500000, PU_CACHE_UNLOCKED, &owner);
	check(other == NULL && owner == dst && zpinned == NULL, "realloc pins source while purging others, restores owner and unpins");
	for (i = 0; i < 500000; i++)
		check(dst[i] == (i < 400000 ? 0x63 : 0), "pinned realloc preserves source and zeroes extension under pressure");
	consistent();
	/* A failing realloc also preserves its old root and payload. */
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		(void)Z_Realloc(dst, 1100000, PU_CACHE_UNLOCKED, &owner);
		check(0, "pinned source cannot be purged to satisfy its own replacement");
	}
	expecting_error = 0;
	check(strstr(error_text, "Out of memory") != NULL && owner == dst && zpinned == NULL,
		"failed realloc keeps old owner and clears transient pin");
	check(dst[0] == 0x63 && dst[399999] == 0x63 && dst[499999] == 0, "failed realloc keeps old bytes");
	consistent();
	empty_zone();

	/* Two equal-size candidate runs: the recently touched one is more expensive, not the first by address. */
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	zframe = 20;
	Z_Malloc(300000, PU_CACHE, &young);
	Z_Malloc(100000, PU_LEVEL, NULL);
	Z_Malloc(300000, PU_CACHE, &old);
	Z_Malloc(300000, PU_LEVEL, NULL);
	ZA_SetStamp(ZA_BLOCK(old), 10);
	check(Z_MakeRoom(250000, true), "headroom has two candidate runs");
	check(young != NULL && old == NULL, "headroom chooses cheaper old run over first recent run");
	empty_zone();

	/* Mirrors texturecache + interior columns/posts/pixels, using actual arena allocations. */
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	src = Z_Malloc(400000, PU_CACHE, &owner);
	memset(src, 0x72, 400000);
	Z_NextFrame();
	Z_PurgeLock(true);
	Z_PurgeLock(true);
	Z_Touch(src); // nested locks protect fetched current-frame roots, not untouched old cache
	{
		unsigned char *column = src + 64, *post = src + 128, *pixel = src + 399999;
		void *scratch = Z_Malloc(500000, PU_STATIC, NULL);
		expect_alloc_error(200000, 0, "Out of memory");
		check(owner == src && *column == 0x72 && *post == 0x72 && *pixel == 0x72,
			"nested lock protects root and all held interior texture aliases under allocation pressure");
		Z_PurgeLock(false);
		expect_alloc_error(200000, 0, "Out of memory");
		check(zpurgelock == 1 && owner == src && *pixel == 0x72, "inner unlock leaves outer protection intact");
		Z_PurgeLock(false);
		Z_NextFrame(); // held aliases have expired; the root can now be evicted
		Z_Malloc(200000, PU_STATIC, NULL);
		check(owner == NULL, "after outer unlock pressure clears texture root: interior aliases must be discarded");
		/* No read through column/post/pixel after owner became NULL. Rebuild all aliases from a fresh root. */
		Z_Free(scratch);
		src = Z_Malloc(400000, PU_CACHE, &owner);
		memset(src, 0x29, 400000);
		column = src + 64; post = src + 128; pixel = src + 399999;
		check(owner == src && *column == 0x29 && *post == 0x29 && *pixel == 0x29,
			"owner-guarded rebuild refreshes every interior texture alias");
	}
	consistent();
	empty_zone();

	/* Retarget an owner table before discarding the old table, as R_LoadTextures does. */
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	src = Z_Malloc(100000, PU_CACHE, &owner);
	Z_NextFrame();
	Z_SetUser(src, &other);
	owner = NULL; /* the former owner slot is no longer authoritative */
	check(other == src && Z_Age(ZA_BLOCK(src)) == 0, "owner retarget stamps the current frame");
	consistent();
	Z_Free(src);
	check(other == NULL, "free clears the replacement owner");
	src = Z_Malloc(100, PU_STATIC, &owner);
	Z_SetUser(src, NULL); /* non-purgable blocks may detach their owner */
	owner = NULL;
	consistent();
	Z_Free(src);
	empty_zone();

	/* Synchronous wall consumers explicitly relinquish roots, then getters can protect/rebuild them again. */
	for (unsigned rollover = 0; rollover < 2; rollover++)
	{
		void *scratch, *pressure;
		UINT32 frame = rollover ? Z_FRAME_MASK : 0;
		arena_reset(1u << 20, ship_sides, ship_prefer, 0);
		zframe = frame;
		src = Z_Malloc(400000, PU_CACHE, &owner);
		memset(src, 0x56, 400000);
		Z_ReleaseCache(NULL);
		zpinned = ZA_BLOCK(src); Z_ReleaseCache(src);
		check(Z_Age(ZA_BLOCK(src)) == 0, "release cannot demote a realloc-pinned root");
		zpinned = NULL;
		ZA_SetStamp(ZA_BLOCK(src), (zframe - 5) & Z_FRAME_MASK);
		Z_ReleaseCache(src);
		check(Z_Age(ZA_BLOCK(src)) == 5, "release does not rejuvenate a previously unused root");
		Z_Touch(src);
		Z_PurgeLock(true); Z_PurgeLock(true);
		Z_ReleaseCache(src);
		check(owner == src && src[399999] == 0x56 && Z_Age(ZA_BLOCK(src)) == 1,
			"release retains pixels without pressure and works at stamp rollover");
		Z_Touch(src);
		scratch = Z_Malloc(500000, PU_STATIC, NULL);
		pressure = Z_TryMallocAlign(200000, PU_STATIC, NULL, 0);
		check(!pressure && owner == src && Z_Age(ZA_BLOCK(src)) == 0,
			"refetched root is protected inside nested locks");
		Z_ReleaseCache(src);
		pressure = Z_Malloc(200000, PU_STATIC, NULL);
		check(owner == NULL && zpurgelock == 2, "pressure clears released root owner inside current-frame nested lock");
		Z_Free(scratch);
		src = Z_Malloc(400000, PU_CACHE, &owner);
		memset(src, 0x56, 400000);
		check(owner == src && src[64] == 0x56 && src[399999] == 0x56 && Z_Age(ZA_BLOCK(src)) == 0,
			"reloaded root restores bytes and all aliases before use");
		Z_ReleaseCache(pressure);
		check(Z_Age(ZA_BLOCK(pressure)) == 0, "non-cache geometry cannot be demoted");
		Z_PurgeLock(false); Z_PurgeLock(false);
		Z_Free(pressure);
		empty_zone();
	}
	printf("PASS long-age LRU, frame rollover, realloc source pin/OOM, age-cost headroom, nested lock/interior texture lifetime\n");
	printf("PASS synchronous cache release: pressure only, refetch protection, nested locks, owner clearing/reload, mandatory/pinned guards, stamp rollover\n");
}

/* ---------------------------------------------------------------------------------------------- */
/* Red zones                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

static void expect_check_error(const char *message)
{
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		Z_CheckHeap(77);
		check(0, "Z_CheckHeap should have failed");
	}
	expecting_error = 0;
	check(strstr(error_text, message) != NULL, "Z_CheckHeap reports the expected corruption");
}

static void redzones(void)
{
	unsigned char *a, *b, *c;
	void *owner = NULL;
	arena_reset(1u << 20, ship_sides, ship_prefer, 1);
	a = Z_Malloc(100, PU_STATIC, NULL);
	b = Z_Malloc(100, PU_STATIC, NULL);
	c = Z_Malloc(100, PU_CACHE, &owner);
	consistent();
	a[100] ^= 0xFF; /* one byte past the payload */
	expect_check_error("red zone");
	a[100] ^= 0xFF;
	consistent();
	b[ZA_SIZE(ZA_BLOCK(b)) - ZA_HDR - 1] ^= 0x01; /* last guard byte */
	expect_check_error("red zone");
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		Z_Free(b);
		check(0, "freeing a block with a damaged red zone should fail");
	}
	expecting_error = 0;
	check(strstr(error_text, "red zone") != NULL, "free reports the damaged red zone");
	b[ZA_SIZE(ZA_BLOCK(b)) - ZA_HDR - 1] ^= 0x01;
	Z_Free(b);
	/* a bad owner pointer is found as well */
	owner = a;
	expect_check_error("proper user");
	owner = c;
	/* a smashed header (underflow into the previous block's header) breaks the valid bit */
	ZA_BLOCK(a)->sf &= ~ZAF_VALID;
	expect_check_error("valid bit");
	ZA_BLOCK(a)->sf |= ZAF_VALID;
	consistent();
	arena_reset(1u << 20, ship_sides, ship_prefer, 0); /* guard-free mode does not reserve guard bytes */
	a = Z_Malloc(100, PU_STATIC, NULL);
	check(blk_bytes(a) == ((ZA_HDR + 100 + 15) & ~(size_t)15), "no forced guard when red zones are off");
	arena_reset(1u << 20, ship_sides, ship_prefer, 1);
	a = Z_Malloc(100, PU_STATIC, NULL);
	check(blk_bytes(a) >= ((ZA_HDR + 100 + ZA_GUARD_MIN + 15) & ~(size_t)15), "forced guard when red zones are on");
	empty_zone();
	printf("PASS red zones: overflow, last guard byte, free-time check, bad owner, damaged header\n");
}

/* ---------------------------------------------------------------------------------------------- */
/* Reporting and arena from the C heap                                                              */
/* ---------------------------------------------------------------------------------------------- */

static void reporting(void)
{
	void *o = NULL;
	size_t got;
	arena_reset(2u << 20, ship_sides, ship_prefer, 0);
	Z_Malloc(1000, PU_STATIC, NULL);
	Z_Malloc(5000, PU_LEVEL, NULL);
	Z_Malloc(7000, PU_CACHE, &o);
	cons_clear();
	PS2Mem_Report(0);
	check(strstr(cons_log, "tag   1 PU_STATIC") && strstr(cons_log, "tag  50 PU_LEVEL") && strstr(cons_log, "tag  49 PU_CACHE"),
		"report lists the tags in use");
	check(strstr(cons_log, "arena 2097152 B") != NULL && strstr(cons_log, "fragmentation") != NULL, "report has arena health");
	cons_clear();
	PS2Mem_Line("test");
	check(strstr(cons_log, "[zmem] test frame=") != NULL, "one-line summary");
	empty_zone();

	/* arena taken from the C heap: all that is free minus the reserve, capped */
	ZA_Shutdown();
	got = ZA_InitHeap(3u << 20, 0);
	check(got == (heap_capacity - (3u << 20) - 65536) && ZA_Ready(), "arena = available capacity - reserve - heap metadata allowance");
	check(((uintptr_t)ZA_First() % 64) == 0, "arena base is 64-byte aligned");
	aligned_release(ZA_First());
	ZA_Shutdown();
	got = ZA_InitHeap(1u << 20, 4u << 20);
	check(got == (4u << 20), "arena capped");
	aligned_release(ZA_First());
	ZA_Shutdown();
	check(ZA_InitHeap(heap_capacity, 0) == 0, "no arena when the reserve eats the heap");
	injected_heap_limit = 3u << 20;
	got = ZA_InitHeap(1u << 20, 0);
	check(got && got <= injected_heap_limit, "arena retries recover across a libc limit mismatch larger than four MiB");
	aligned_release(ZA_First());
	ZA_Shutdown();
	injected_heap_limit = 0;
	printf("PASS report by tag, one-line summary, ZA_InitHeap sizing\n");
}

static void memory_peaks_and_budgets(void)
{
	unsigned char *p, *original;
	void *owner = NULL, *replacement = NULL, *oldcache = NULL;
	zastats_t before, after;
	size_t i;
	int guard;
	static const char *bad_kib[] = {"-1", "9999999999999999999999", "123bad"};
	test_parm = "-zram";
	test_value = "128";
	check(PS2Mem_RamClass() == 32, "override cannot promote retail physical RAM to 128 MiB");
	test_parm = "-zreserve";
	test_value = "2048";
	check(Z_KiBParm(test_parm, 0) == (2u << 20), "KiB budget parses exactly");
	for (i = 0; i < sizeof bad_kib / sizeof bad_kib[0]; i++)
	{
		test_value = bad_kib[i];
		expecting_error = 1;
		if (!setjmp(error_jump))
		{
			(void)Z_KiBParm(test_parm, 0);
			check(0, "malformed KiB budget must be rejected");
		}
		expecting_error = 0;
		check(strstr(error_text, "nonnegative KiB") != NULL, "malformed budget reports units and reason");
	}
	test_parm = test_value = NULL;
	check(PS2Mem_HeapAvailable(32u << 20, (32u << 20) - (512u << 10), 12u << 20, 1u << 20)
		== (21u << 20) - (512u << 10), "retail budget subtracts existing heap and actual stack");
	check(PS2Mem_HeapAvailable(128u << 20, (128u << 20) - (512u << 10), 12u << 20, 1u << 20)
		== (117u << 20) - (512u << 10), "128 MiB reported RAM supports extended heap when stack permits");
	check(PS2Mem_HeapAvailable(128u << 20, 31u << 20, 12u << 20, 0) == (19u << 20),
		"128 MiB loader with retail-address stack cannot budget through stack");
	check(!PS2Mem_HeapAvailable(32u << 20, 31u << 20, SIZE_MAX, 4096), "invalid break fails closed");
	check(!PS2Mem_HeapAvailable(32u << 20, 128u << 20, 0, 0), "stack beyond detected RAM fails closed");
	check(PS2Mem_HeapAvailable(32u << 20, 31u << 20, 12u << 20, SIZE_MAX) == (19u << 20),
		"invalid free chunk accounting cannot wrap budget");
	for (guard = 0; guard <= 1; guard++)
	{
		arena_reset(1u << 20, ship_sides, ship_prefer, guard);
		p = Z_MallocAlign(400000, PU_LEVEL, &owner, 6);
		memset(p, 0x6B, 400000);
		original = p;
		ZA_Stats(&before);
		p = Z_ReallocAlign(p, 900000, PU_LEVEL, &replacement, 6);
		check(p == original && owner == NULL && replacement == p, "in-place grow retargets owner without duplicate block");
		for (i = 0; i < 900000; i++)
			check(p[i] == (i < 400000 ? 0x6B : 0), "in-place growth preserves prefix and zeroes extension");
		ZA_Stats(&after);
		check(after.usedblocks == before.usedblocks && after.globalpeak == after.used, "growth peak contains only enlarged block");
		p = Z_ReallocAlign(p, 100000, PU_LEVEL, &replacement, 6);
		check(p == original && p[99999] == 0x6B, "in-place shrink keeps address and payload");
		check(ZA_LargestFree() > 900000, "shrink releases contiguous tail immediately");
		consistent();
		owner = p;
		check(!Z_TryMallocAlign(2u << 20, PU_LEVEL, &owner, 6) && owner == p,
			"recoverable exhaustion leaves supplied owner intact");
		empty_zone();
	}
	arena_reset(1u << 20, ship_sides, ship_prefer, 0);
	Z_MallocAlign(600000, PU_CACHE, &oldcache, 6);
	Z_NextFrame();
	Z_PurgeLock(true);
	p = Z_TryMallocAlign(700000, PU_LEVEL, NULL, 6);
	check(p && !oldcache, "under render lock old untouched cache is retried and evicted");
	Z_PurgeLock(false);
	empty_zone();
	printf("PASS retail/dev/low-stack budgets, overflow, in-place realloc peaks/guards/owners, recoverable OOM and locked old-cache retry\n");
}

/* ---------------------------------------------------------------------------------------------- */
/* Synthetic level trace: fragmentation, misses, evictions                                         */
/* ---------------------------------------------------------------------------------------------- */

typedef struct
{
	size_t peak, evictedbytes, misses, failures, finalfree, finallargest, exitlargest;
	unsigned maxfrag, avgfrag, samples;
	double scans;
	size_t accesses;
} trace_t;

#define NTEX 420
static void *texslot[NTEX];
static size_t texsize[NTEX];

static void trace_alloc_level(unsigned level, size_t scale, void **keep, size_t *nkeep, int *failed)
{
	/* a level: many small structs of mixed sizes plus a few large arrays, as P_LoadLevel does */
	static const struct { size_t size; unsigned count; int bits; } part[] = {
		{36, 1800, 0}, {52, 2600, 0}, {144, 1400, 0}, {284, 700, 0}, {16, 900, 0}, {96, 600, 0},
		{2200, 8, 0}, {65536, 1, 6}, {9000, 3, 0}, {24000, 2, 0},
	};
	size_t i;
	unsigned k;
	(void)level;
	for (i = 0; i < sizeof part / sizeof part[0]; i++)
		for (k = 0; k < part[i].count * scale / 100; k++)
		{
			void *volatile p = NULL;
			size_t sz = part[i].size + (part[i].size > 100 ? rndn((uint32_t)part[i].size / 4) : 0);
			expecting_error = 1;
			if (!setjmp(error_jump))
				p = Z_MallocAlign(sz, PU_LEVEL, NULL, part[i].bits);
			else
				*failed += 1;
			expecting_error = 0;
			if (p)
				keep[(*nkeep)++] = p;
		}
}

/* lazy: long-lived blocks (sprites, patches, HUD graphics) keep being allocated during play, as the engine loads them
 * on first use; without it all long-lived data exists before the first level, which the engine does not do */
static int trace_zones = 1, trace_smallfit = 1024; /* policy knobs of the trace: the frontier (PS2-75) and the small-request best fit (PS2-74) */
static void *sprslot[48];
static size_t sprsize[48];

static trace_t run_trace(size_t arena, int twosided, int prefer, uint64_t seed, int lazy)
{
	enum { LEVELS = 14, FRAMES = 240, MAXKEEP = 50000 };
	static void *keep[MAXKEEP];
	static void *tmp[512];
	trace_t t;
	zastats_t st;
	size_t nkeep, i, ntmp;
	unsigned level, frame;
	unsigned long fragsum = 0;
	void *statics[200];
	int failed;
	char msg[200];

	memset(&t, 0, sizeof t);
	memset(texslot, 0, sizeof texslot);
	rng_state = seed;
	arena_reset(arena, twosided, prefer, 0);
	za_smallfit = trace_smallfit;
	sprite_evictable = 1;
	memset(sprslot, 0, sizeof sprslot);
	failed = 0;
	for (i = 0; i < NTEX; i++)
		texsize[i] = 2048 + rndn(30000) + (rndn(10) == 0 ? 60000 : 0);
	for (i = 0; i < 48; i++)
		sprsize[i] = 3000 + rndn(20000) + (rndn(8) == 0 ? 50000 : 0);
	/* startup data: patches, tables (64 KiB aligned), sounds */
	for (i = 0; i < 200; i++)
		statics[i] = Z_MallocAlign(i < 10 ? 65536 : 300 + rndn(5000), i < 100 ? PU_STATIC : PU_PATCH, NULL, i < 10 ? 16 : 0);
	for (level = 0; level < LEVELS; level++)
	{
		size_t scale = (40 + (level * 53) % 80) * 4; /* 160%..476% of the reference level (~1 MiB at 100%) */
		unsigned base = rndn(NTEX - 150);
		nkeep = 0;
		Z_LevelPhase(false); /* the loader: the level is built from the bottom */
		Z_FlushCache();
		trace_alloc_level(level, scale, keep, &nkeep, &failed);
		Z_LevelPhase(true); /* play: caches from the bottom, long-lived blocks from the top, with the frontier between them if the trace has zones */
		if (!trace_zones)
			ZA_SetFrontier(NULL);
		ntmp = 0;
		for (frame = 0; frame < FRAMES; frame++)
		{
			unsigned a;
			for (a = 0; a < 14; a++)
			{
				unsigned id = base + (rndn(150) * rndn(150)) / 150; /* hot set is skewed */
				t.accesses++;
				if (texslot[id])
					Z_Touch(texslot[id]);
				else
				{
					void *volatile p = NULL;
					t.misses++;
					expecting_error = 1;
					if (!setjmp(error_jump))
						p = Z_Malloc(texsize[id], PU_CACHE, &texslot[id]);
					else
						failed++;
					expecting_error = 0;
					(void)p;
				}
			}
			if (lazy && rndn(FRAMES / 20) == 0)
			{
				static const int lazytag[] = {PU_PATCH, PU_PATCH_DATA, PU_HUDGFX, PU_STATIC};
				void *volatile p = NULL;
				expecting_error = 1;
				if (!setjmp(error_jump))
					p = Z_Malloc(500 + rndn(12000), lazytag[rndn(4)], NULL);
				else
					failed++;
				expecting_error = 0;
				(void)p;
			}
			if (lazy)
				for (a = 0; a < 3; a++) /* sprite patches: evictable with an owner, fetched again when gone (the engine's W_CachePatchNum) */
				{
					unsigned id = (rndn(24) * rndn(24)) / 12;
					if (sprslot[id])
						Z_Touch(sprslot[id]);
					else
					{
						void *volatile p = NULL;
						t.misses++;
						expecting_error = 1;
						if (!setjmp(error_jump))
							p = Z_Malloc(sprsize[id], PU_SPRITE, &sprslot[id]);
						else
							failed++;
						expecting_error = 0;
						(void)p;
					}
					t.accesses++;
				}
			/* mobj churn: short-lived level allocations */
			for (a = 0; a < 6; a++)
			{
				if (ntmp == 512 || (ntmp && rndn(2)))
				{
					size_t victim = rndn((uint32_t)ntmp);
					Z_Free(tmp[victim]);
					tmp[victim] = tmp[--ntmp];
				}
				else
				{
					void *volatile p = NULL;
					expecting_error = 1;
					if (!setjmp(error_jump))
						p = Z_Malloc(200 + rndn(400), PU_LEVEL, NULL);
					else
						failed++;
					expecting_error = 0;
					if (p)
						tmp[ntmp++] = p;
				}
			}
			Z_NextFrame();
			if (frame == FRAMES / 2)
			{
				size_t hole = 0;
				ZA_Stats(&st);
				fragsum += st.freebytes ? (unsigned long)(100 - st.largestfree * 100 / st.freebytes) : 0;
				t.samples++;
				if (st.freebytes && 100 - st.largestfree * 100 / st.freebytes > t.maxfrag)
					t.maxfrag = (unsigned)(100 - st.largestfree * 100 / st.freebytes);
				(void)hole;
			}
		}
		ZA_Stats(&st);
		if (st.peakused > t.peak)
			t.peak = st.peakused;
		for (i = 0; i < ntmp; i++)
			Z_Free(tmp[i]);
		Z_FreeTags(PU_LEVEL, PU_PURGELEVEL - 1);
		Z_LevelPhase(false);
		check(ZA_Check(msg, sizeof msg) == 0, "trace keeps the arena consistent");
		ZA_Stats(&st);
		t.exitlargest += st.largestfree / LEVELS; /* contiguous space left once a level is gone */
	}
	ZA_Stats(&st);
	t.evictedbytes = st.evictedbytes;
	t.failures = (size_t)failed;
	t.finalfree = st.freebytes;
	t.finallargest = st.largestfree;
	t.avgfrag = t.samples ? (unsigned)(fragsum / t.samples) : 0;
	t.scans = st.allocs ? (double)st.binsearch / (double)st.allocs : 0;
	(void)statics;
	Z_FreeTags(0, INT32_MAX);
	Z_LevelPhase(false);
	sprite_evictable = 0;
	za_smallfit = 1024;
	return t;
}

#ifndef TRACE_SEEDS
#define TRACE_SEEDS 8
#endif
static void fragmentation_trace(void)
{
	static const struct { const char *name; int sides, prefer, zones, smallfit; } cfg[] = {
		{"one-sided first-fit", 0, 1, 0, 0}, {"one-sided pick-by-address", 0, 4, 0, 0}, {"two-sided first-fit", 1, 1, 0, 0},
		{"two-sided address order (OPT3)", 1, 0, 0, 0}, {"  + small best fit (PS2-74)", 1, 0, 0, 1024},
		{"  + zones (PS2-75)", 1, 0, 1, 0}, {"  + zones + small best fit (shipped)", 1, 0, 1, 1024},
	};
	enum { NCFG = 7 };
	static const size_t arenas[] = {6u << 20, 7u << 20, 8u << 20};
	size_t i, a;
	int s, lazy;
	trace_t r[2][NCFG];
	int def = ship_sides ? NCFG - 1 : (ship_prefer > 1 ? 1 : 0);

	printf("fragmentation trace: 14 levels x 240 frames, arenas 10/11/12 MiB x %d seeds (arena base skew 0/16/32/48 KiB) summed;\n"
		"  frag = unusable share of free space mid-level, exit = mean largest free block after a level is freed,\n"
		"  misses = cache rebuilds, OOM = refused allocations (the arenas are small on purpose: levels of 160%%..476%% of a reference level)\n",
		TRACE_SEEDS);
	for (lazy = 0; lazy < 2; lazy++)
	{
		printf(" %s\n", lazy ? "long-lived blocks keep arriving during play (engine behaviour: sprites, patches, HUD graphics on first use)"
			: "all long-lived blocks exist before the first level (does not happen in the engine)");
		for (i = 0; i < NCFG; i++)
		{
			unsigned runs = 0;
			trace_zones = cfg[i].zones;
			trace_smallfit = cfg[i].smallfit;
			memset(&r[lazy][i], 0, sizeof r[lazy][i]);
			for (a = 0; a < sizeof arenas / sizeof arenas[0]; a++)
				for (s = 1; s <= TRACE_SEEDS; s++)
				{
					trace_t t;
					arena_skew = (size_t)((s - 1) % 4) * 16384; /* the same (arena, seed, skew) for every policy */
					t = run_trace(arenas[a], cfg[i].sides, cfg[i].prefer ? cfg[i].prefer : ship_prefer, 1000u * (unsigned)s + (unsigned)a, lazy);
					r[lazy][i].misses += t.misses;
					r[lazy][i].evictedbytes += t.evictedbytes;
					r[lazy][i].failures += t.failures;
					r[lazy][i].avgfrag += t.avgfrag;
					r[lazy][i].exitlargest += t.exitlargest;
					r[lazy][i].scans += t.scans;
					r[lazy][i].accesses += t.accesses;
					runs++;
				}
			printf("  %-28s frag %2u%%  exit %6zu KiB  misses %6zu (%.1f%%)  evicted %7.1f MiB  OOM %3zu  scans/alloc %.2f\n",
				cfg[i].name, r[lazy][i].avgfrag / runs, (r[lazy][i].exitlargest / runs) >> 10, r[lazy][i].misses,
				100.0 * (double)r[lazy][i].misses / (double)r[lazy][i].accesses, (double)r[lazy][i].evictedbytes / (1 << 20),
				r[lazy][i].failures, r[lazy][i].scans / runs);
			r[lazy][i].scans /= runs; /* per run */
		}
	}
	arena_skew = 0;
	trace_zones = 1;
	trace_smallfit = 1024;
	/* The shipped default (za_twosided/za_prefer) is checked on the trace that has the engine's allocation pattern: it
	 * must not refuse more allocations nor rebuild more cache blocks than the one-sided first-fit baseline. */
	check(r[1][def].failures <= r[1][0].failures && r[1][def].misses <= r[1][0].misses + r[1][0].misses / 8,
		"default policy refuses no more and rebuilds at most 12% more than one-sided first-fit (engine allocation pattern)");
	/* On the artificial trace the policies are within noise of each other (the 64 KiB-aligned blocks dominate the
	 * outcome): only a gross regression is an error. */
	check(r[0][def].misses <= r[0][0].misses + r[0][0].misses / 10, "default policy rebuilds no more than 10% more cache blocks (static-first trace)");
	check(r[0][def].failures <= r[0][0].failures + 40, "default policy refuses at most 40 allocations more (static-first trace)");
	check(r[1][def].scans < 24.0, "allocation cost stays small (free blocks placed-tested per allocation; address-ordered fit)");
	printf("PASS fragmentation trace\n");
}

#endif /* PS2 */

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
#ifdef PS2
	int trace_only = 0, arg;
	for (arg = 1; arg < argc; arg++)
		if (!strcmp(argv[arg], "--trace-only"))
			trace_only = 1;
		else if (!strcmp(argv[arg], "--prefer") && arg + 1 < argc)
			za_prefer = atoi(argv[++arg]);
		else
			check(0, "unknown test argument");
	ship_sides = za_twosided;
	ship_prefer = za_prefer;
	arena_reset(8u << 20, ship_sides, ship_prefer, 0);
	Z_Init();
	printf("PS2 arena host: pointer=%zu header=%zu min block=%u hdr-align=16 default sides=%d prefer=%d\n",
		sizeof(void *), (size_t)ZA_HDR, (unsigned)ZA_MINBLK, za_twosided, za_prefer);
#ifdef ZONE_EXPECT_HDR16
	check(ZA_HDR == 16, "release header is 16 bytes");
#endif
	if (!trace_only)
	{
		alignment_and_cycles();
		coalescing();
		lru_semantics();
		reclaim_hook_tests();
		cache_lifetimes();
		redzones();
		reporting();
		memory_peaks_and_budgets();
		torture_all();
	}
	fragmentation_trace();
#else
	(void)argc; (void)argv;
	void *owner = NULL;
	void *p;
	Z_Init(); /* the original allocator needs its list head set up */
	/* An unmatched unlock and requests above the fake PS2 budget remain harmless on the host. */
	Z_PurgeLock(false);
	Z_PurgeLock(true);
	Z_NextFrame();
	Z_Touch(NULL);
	p = Z_Malloc(9u << 20, PU_STATIC, &owner);
	check(p == owner, "host profile still uses the original unbudgeted allocator");
	Z_PurgeLock(false);
	Z_CheckHeap(1);
	Z_Free(p);
	check(owner == NULL, "host owner clearing unchanged");
	printf("PASS host PS2_PROFILE lock/frame no-ops and unchanged malloc semantics\n");
#endif
	printf("PASS all %u checks\n", checks);
	return 0;
}
