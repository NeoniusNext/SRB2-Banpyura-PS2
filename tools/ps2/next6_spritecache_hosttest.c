/* Frozen/current production sprite-header stores, getters and reserve growth.
 * Real z_zone.c/ps2_mem.c; native arena sizes retain native header widths. */
#include <stdint.h>
typedef uint8_t UINT8;
static int netgame;
static int PS2Lua_InCall(void) { return 0; }
void PS2Spill_Reset(void) {}
#define max(a,b) ((a) > (b) ? (a) : (b))
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef INT32 fixed_t;
#define FRACBITS 16
#define FRACUNIT 65536
#define FEETADJUST (4 * FRACUNIT)
#include SC_TYPES
#ifndef SPRCACHE_WIDTH
#define SPRCACHE_WIDTH(c) ((c).width)
#define SPRCACHE_OFFSET(c) ((c).offset)
#define SPRCACHE_TOPOFFSET(c) ((c).topoffset)
#define SPRCACHE_HEIGHT(c) ((c).height)
#endif
static sprcache_t *spritecachedinfo;
static size_t numspritelumps, max_spritelumps;
#include SC_INIT
static void write_sprite_header(INT16 width, INT16 height, INT16 topoffset, INT16 leftoffset)
{
#include SC_WRITE
}
static void finish_sprite_header(void)
{
#include SC_GROW
}

static uint64_t digest = UINT64_C(1469598103934665603);
static void check_entry(size_t id, UINT16 width, UINT16 height, UINT16 top, UINT16 left)
{
	const UINT32 got[] = { (UINT32)SPRCACHE_WIDTH(spritecachedinfo[id]),
		(UINT32)SPRCACHE_OFFSET(spritecachedinfo[id]),
		(UINT32)SPRCACHE_TOPOFFSET(spritecachedinfo[id]),
		(UINT32)SPRCACHE_HEIGHT(spritecachedinfo[id]) };
	const UINT32 expected[] = { (UINT32)width << 16, (UINT32)left << 16,
		((UINT32)top << 16) + UINT32_C(262144), (UINT32)height << 16 };
	size_t j;
	for (j = 0; j < 4; ++j)
	{
		UINT32 v = got[j];
		check(v == expected[j], "all signed header bits and adjusted top bits are exact");
		check((v & UINT32_C(65535)) == 0, "cached value has no fractional bits");
		digest = (digest ^ v) * UINT64_C(1099511628211);
	}
}
static void input_for(size_t n, UINT16 values[4])
{
	values[0] = (UINT16)n;
	values[1] = (UINT16)(n * 32769u + 16383u);
	values[2] = (UINT16)(n * 65521u + 32764u);
	values[3] = (UINT16)(n * 8191u + 65535u);
}
static void exhaustive(void)
{
	UINT32 n;
	UINT16 values[4];
	arena_reset(1u << 20, 0, 0, 1);
	R_InitSpriteLumps();
	check(max_spritelumps == 8192 && numspritelumps == 0, "unchanged initial capacity and count");
	for (n = 0; n < 65536u; ++n)
	{
		input_for(n, values);
		write_sprite_header((INT16)values[0], (INT16)values[1], (INT16)values[2], (INT16)values[3]);
		check_entry(0, values[0], values[1], values[2], values[3]);
	}
	consistent();
	Z_Free(spritecachedinfo);
	check(!spritecachedinfo && !za_used, "exhaustive owner and cleanup");
	printf("exhaustive hash=%016llx inputs=65536 fields=4\n", (unsigned long long)digest);
}
static void reserve_case(size_t count, int sides, int guard, int fragmented)
{
	void *neighbors[5] = { NULL };
	size_t i, nlive = 0, before_allocs, oldcap, moves = 0;
	sprcache_t *before;
	UINT16 values[4];
	zastats_t stats;
	arena_reset(8u << 20, sides, ship_prefer, guard);
	before_allocs = za_nalloc;
	R_InitSpriteLumps();
	if (fragmented) neighbors[nlive++] = Z_Malloc(257, PU_STATIC, NULL);
	for (i = 0; i < count; ++i)
	{
		input_for(i, values);
		write_sprite_header((INT16)values[0], (INT16)values[1], (INT16)values[2], (INT16)values[3]);
		oldcap = max_spritelumps; before = spritecachedinfo;
		finish_sprite_header();
		if (oldcap != max_spritelumps)
		{
			check(max_spritelumps == oldcap * 2, "unchanged doubling capacity");
			moves += before != spritecachedinfo;
			if (fragmented) neighbors[nlive++] = Z_Malloc(257, PU_STATIC, NULL);
		}
	}
	check(numspritelumps == count && max_spritelumps >= 8192, "preserved table count and reserve");
	for (i = 0; i < count; ++i)
	{
		input_for(i, values);
		check_entry(i, values[0], values[1], values[2], values[3]);
	}
	check(ZA_BLOCK(spritecachedinfo)->realsize == max_spritelumps * sizeof *spritecachedinfo,
		"reserve uses entry size, without shrinking capacity");
	consistent(); ZA_Stats(&stats);
	printf("case n=%zu sides=%d guard=%d fragmented=%d cap=%zu request=%zu used=%zu peak=%zu allocs=%zu moves=%zu hash=%016llx\n",
		count, sides, guard, fragmented, max_spritelumps, (size_t)ZA_BLOCK(spritecachedinfo)->realsize,
		stats.used, stats.globalpeak, za_nalloc - before_allocs, moves, (unsigned long long)digest);
	for (i = 0; i < nlive; ++i) Z_Free(neighbors[i]);
	Z_Free(spritecachedinfo);
	check(!spritecachedinfo && !za_used, "add-on reserve owner and cleanup");
	consistent();
}
static void failures(void)
{
	volatile size_t stage;
	for (stage = 0; stage < 4; ++stage)
	{
		zguard_t guard;
		void *neighbor = NULL;
		sprcache_t *old = NULL;
		size_t i, oldcap = 0;
		UINT16 values[4];
		arena_reset(8u << 20, 0, 0, 1);
		if (stage)
		{
			R_InitSpriteLumps();
			for (i = 0; i < ((size_t)8192 << (stage - 1)) - 1; ++i)
			{
				input_for(i, values);
				write_sprite_header((INT16)values[0], (INT16)values[1], (INT16)values[2], (INT16)values[3]);
				finish_sprite_header();
			}
			neighbor = Z_Malloc(257, PU_STATIC, NULL);
			old = spritecachedinfo; oldcap = max_spritelumps;
		}
		zoomnth = 1; zoomnth_count = zoomnth_after = 0;
		if (Z_GUARD_TRY(&guard))
		{
			if (stage)
			{
				input_for(numspritelumps, values);
				write_sprite_header((INT16)values[0], (INT16)values[1], (INT16)values[2], (INT16)values[3]);
				finish_sprite_header();
			}
			else R_InitSpriteLumps();
			Z_GuardPop(&guard);
			check(0, "injected reserve allocation must fail");
		}
		else Z_GuardLanded(&guard);
		zoomnth = 0;
		check(spritecachedinfo == old, "failure preserves prior cache owner");
		if (stage)
		{
			check(max_spritelumps == 2 * oldcap, "preceding failed-growth capacity publication is unchanged");
			for (i = 0; i < numspritelumps; ++i)
			{
				input_for(i, values);
				check_entry(i, values[0], values[1], values[2], values[3]);
			}
			Z_Free(neighbor); Z_Free(spritecachedinfo);
		}
		check(!spritecachedinfo && !za_used, "failed add-on load releases all retained blocks");
		R_InitSpriteLumps();
		write_sprite_header(INT16_MIN, INT16_MAX, INT16_MAX, -1);
		check_entry(0, 32768, 32767, 32767, 65535);
		Z_Free(spritecachedinfo); consistent();
	}
	puts("failure stages=4 passed");
}
static void eviction(void)
{
	void *cache = NULL, *request;
	UINT16 values[4];
	sprcache_t *root;
	arena_reset(1u << 20, 0, 0, 0);
	R_InitSpriteLumps(); root = spritecachedinfo;
	input_for(32767, values);
	write_sprite_header((INT16)values[0], (INT16)values[1], (INT16)values[2], (INT16)values[3]);
	Z_Malloc(600u << 10, PU_CACHE, &cache);
	zframe = 10;
	request = Z_Malloc(500u << 10, PU_STATIC, NULL);
	check(!cache && spritecachedinfo == root, "cache eviction preserves static sprite-header root");
	check_entry(0, values[0], values[1], values[2], values[3]);
	Z_Free(request); Z_Free(spritecachedinfo); consistent();
	puts("cache eviction passed");
}
int main(void)
{
	static const size_t counts[] = { 0, 1, 8191, 8192, 8193, 16383, 16384, 32767, 32768, 65534 };
	size_t i;
	int sides, guard, fragmented;
	ship_sides = za_twosided; ship_prefer = za_prefer;
	exhaustive();
	for (sides = 0; sides < 2; ++sides)
	for (guard = 0; guard < 2; ++guard)
	for (fragmented = 0; fragmented < 2; ++fragmented)
	for (i = 0; i < sizeof counts / sizeof *counts; ++i) reserve_case(counts[i], sides, guard, fragmented);
	failures(); eviction();
	printf("SPRCACHE6 DONE cases=80 checks=%u sizeof=%zu align=%zu hash=%016llx\n",
		checks, sizeof(sprcache_t), _Alignof(sprcache_t), (unsigned long long)digest);
	if (arena_raw) { aligned_release(arena_raw); arena_raw = NULL; }
	return 0;
}
