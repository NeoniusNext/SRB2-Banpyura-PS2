/* Actual z_zone.c + ps2_mem.c, with the existing zone test's engine boundary. */
#include <stdint.h>
#ifdef __mips__
#include <malloc.h>
void init_scr(void);
#define aligned_alloc memalign
#endif
typedef uint8_t UINT8;
static int netgame;
static int PS2Lua_InCall(void) { return 0; }
void PS2Spill_Reset(void) {}
#define max(a,b) ((a) > (b) ? (a) : (b))
#define main ZoneRegressionMain
#include "zone_hosttest.c"
#undef main
#include "ps2_audio.h"
typedef struct {
	ps2_sample sample;
	void *owner, *next;
	size_t bytes;
	uint64_t used;
	uint32_t start_seq;
	uint8_t data[];
} cache_layout;
static size_t requested_saved, arena_saved, cases;
#ifdef SFX_ZONE_CORPUS
#include SFX_ZONE_CORPUS
#endif
static void Shrink(size_t oldsize, size_t newsize, int guard)
{
	void *owner = NULL, *neighbor_owner = NULL, *replacement = NULL;
	unsigned char *p, *original, *neighbor;
	size_t i, oldblock;
	zastats_t before, after;
	arena_reset((oldsize + 8191) & ~(size_t)4095, ship_sides, ship_prefer, guard);
	p = Z_MallocAlign(oldsize, PU_SOUND, &owner, 6);
	neighbor = Z_MallocAlign(257, PU_STATIC, &neighbor_owner, 6);
	memset(p, 0xa5, oldsize); memset(neighbor, 0x6c, 257);
	original = p; oldblock = blk_bytes(p); ZA_Stats(&before);
	p = Z_ReallocAlign(p, newsize, PU_SOUND, &replacement, 6);
	ZA_Stats(&after);
	check(p == original && replacement == p && !owner, "SFX shrink preserves address and retargets owner");
	check(neighbor_owner == neighbor, "SFX shrink preserves live neighbor owner");
	check(ZA_BLOCK(p)->realsize == newsize, "SFX shrink records exact retained request");
	check(after.usedblocks == before.usedblocks && after.globalpeak == before.globalpeak,
		"SFX shrink has no new allocation or transient arena peak");
	check(before.used - after.used == oldblock - blk_bytes(p)
		&& after.freebytes - before.freebytes == before.used - after.used, "SFX shrink returns exact arena bytes");
	for (i = 0; i < newsize; i++) check(p[i] == 0xa5, "SFX retained PCM canary");
	for (i = 0; i < 257; i++) check(neighbor[i] == 0x6c, "SFX live neighbor canary");
	consistent();
	requested_saved += oldsize - newsize; arena_saved += before.used - after.used; cases++;
	Z_Free(p); Z_Free(neighbor); check(!replacement && !neighbor_owner, "SFX owners clear on release");
	consistent();
}
int main(int argc, char **argv)
{
	int guard;
	size_t size, drop, corpus_requested = 0, corpus_arena = 0, corpus_files = 0;
#ifdef __mips__
	init_scr();
	check(ZA_HDR == 16 && sizeof(cache_layout) == 56, "EE zone and cached SFX ABI sizes");
#endif
	ship_sides = za_twosided; ship_prefer = za_prefer;
	for (guard = 0; guard <= 1; guard++)
	for (size = 64; size < 512; size += 17)
	for (drop = 1; drop <= 64 && drop < size; drop++) Shrink(size, size - drop, guard);
	for (guard = 0; guard <= 1; guard++)
	{
		Shrink(2100000, 2099999, guard); Shrink(2100000, 1048583, guard);
		Shrink(8192, 65, guard); Shrink(2048, 2047, guard);
	}
	if (argc == 2)
	{
		FILE *fp = fopen(argv[1], "r"); size_t original, retained;
		check(fp != NULL, "SFX corpus request file");
		requested_saved = arena_saved = 0;
		while (fscanf(fp, "%zu %zu", &original, &retained) == 2)
		{
			Shrink(sizeof(cache_layout) + original, sizeof(cache_layout) + retained, 0);
			corpus_files++;
		}
		fclose(fp); corpus_requested = requested_saved; corpus_arena = arena_saved;
	}
#ifdef SFX_ZONE_CORPUS
	else
	{
		requested_saved = arena_saved = 0;
		for (size = 0; size < sizeof(sfx_zone_corpus) / sizeof(*sfx_zone_corpus); size++)
		{
			Shrink(sizeof(cache_layout) + sfx_zone_corpus[size][0], sizeof(cache_layout) + sfx_zone_corpus[size][1], 0);
			corpus_files++;
		}
		corpus_requested = requested_saved; corpus_arena = arena_saved;
	}
#endif
	printf("SFX3 ZONE DONE checks=%u cases=%zu header=%zu corpus_files=%zu corpus_requested_saved=%zu corpus_arena_saved=%zu\n",
		checks, cases, sizeof(cache_layout), corpus_files, corpus_requested, corpus_arena);
	if (arena_raw) { aligned_release(arena_raw); arena_raw = NULL; }
	return 0;
}
