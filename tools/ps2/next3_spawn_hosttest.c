/* Actual spawn snapshot construction and line archive functions, real zone.
 * Engine services outside that path are replaced by small deterministic stubs.
 * Native measurements retain their native pointer/header widths. */
#include <stdint.h>
typedef uint8_t UINT8;
typedef int8_t SINT8;
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
typedef UINT32 angle_t;
typedef INT16 mtag_t;
typedef UINT32 sectorflags_t;
typedef UINT32 sectorspecialflags_t;
typedef struct { int unused; } extracolormap_t;
typedef struct { mtag_t *tags; UINT16 count; mtag_t inline_tag; } taglist_t;
#define NUMLINEARGS 10
#define NUMLINESTRINGARGS 2
#define NO_SIDEDEF UINT32_MAX
#include SPAWN_TYPES
typedef spawnsector_t sector_t;
typedef spawnside_t side_t;
typedef struct
{
	INT32 *args;
	char *stringargs[NUMLINESTRINGARGS];
	INT32 executordelay;
	UINT32 secportal;
	INT16 special, flags, callcount;
	UINT32 sidenum[2];
} line_t;
static size_t numlines, numsectors, numsides;
static line_t *lines;
static sector_t *sectors;
static side_t *sides;
static spawnline_t *spawnlines;
static spawnsector_t *spawnsectors;
static spawnside_t *spawnsides;
#include SPAWN_BUILD
#ifndef SPAWN_NEW
static const spawnline_t *P_GetSpawnLine(size_t i, spawnline_t *scratch)
{ (void)scratch; return &spawnlines[i]; }
#endif
static void load_spawn_gate(void)
{
#include SPAWN_GATE
}

typedef struct { UINT8 *buf; size_t pos, size; } save_t;
#include SPAWN_WRITES
#include SPAWN_FLAGS
#include SPAWN_EQUAL
static UINT32 GetSideDiff(const side_t *live, const spawnside_t *saved)
{
	return (live->toptexture != saved->toptexture)
		| (UINT32)(live->textureoffset != saved->textureoffset) << 1;
}
static void ArchiveSide(save_t *save, const side_t *side, UINT32 diff)
{
	P_WriteUINT32(save, diff);
	if (diff & 1) P_WriteINT32(save, side->toptexture);
	if (diff & 2) P_WriteINT32(save, side->textureoffset);
}
#include SPAWN_ARCHIVE

static uint64_t digest;
static FILE *wire_file;
static void hash_bytes(const void *p, size_t len)
{
	const UINT8 *bytes = p;
	while (len--) digest = (digest ^ *bytes++) * UINT64_C(1099511628211);
}
static void hash_word(UINT32 v) { hash_bytes(&v, sizeof(v)); }
static void check_snapshot(size_t wanted_nonzero)
{
	size_t i, j, count = 0;
	spawnline_t scratch;
	for (i = 0; i < numlines; i++)
	{
		const spawnline_t *s = P_GetSpawnLine(i, &scratch);
		for (j = 0; j < NUMLINEARGS; j++)
		{
			check(s->args[j] == lines[i].args[j], "full signed argument snapshot");
			hash_word((UINT32)s->args[j]);
		}
		for (j = 0; j < NUMLINEARGS && !s->args[j]; j++) {}
		count += j != NUMLINEARGS;
		for (j = 0; j < NUMLINESTRINGARGS; j++)
		{
			check(s->stringargs[j] == lines[i].stringargs[j], "original string ownership");
			if (s->stringargs[j]) hash_bytes(s->stringargs[j], strlen(s->stringargs[j]) + 1);
			else hash_word(0);
		}
		check(s->executordelay == lines[i].executordelay && s->secportal == lines[i].secportal
			&& s->special == lines[i].special, "all snapshot metadata");
		hash_word((UINT32)s->executordelay);
		hash_word(s->secportal);
		hash_word((UINT32)s->special);
	}
	check(count == wanted_nonzero, "exact populated vector count");
	for (i = 0; i < numsectors; i++)
	{
		spawnsector_t expected = sectors[i];
		expected.tags.tags = spawnsectors[i].tags.tags;
		check(!memcmp(&spawnsectors[i], &expected, sizeof(expected)), "complete sector snapshot");
		if (sectors[i].tags.count)
			check(!memcmp(spawnsectors[i].tags.tags, sectors[i].tags.tags,
				sectors[i].tags.count * sizeof(mtag_t)), "sector tags copied independently");
	}
	check(!memcmp(spawnsides, sides, numsides * sizeof(*sides)), "complete side snapshot");
}

static void emit_archive(save_t *save)
{
	uint64_t bytes;
	save->pos = 0;
	ArchiveLines(save);
	hash_word((UINT32)save->pos);
	hash_bytes(save->buf, save->pos);
	bytes = save->pos;
	check(fwrite(&bytes, sizeof(bytes), 1, wire_file) == 1
		&& fwrite(save->buf, 1, save->pos, wire_file) == save->pos, "complete framed archive bytes");
}

static void setup_inputs(size_t n, size_t count)
{
	size_t i, j;
	numlines = n;
	numsectors = 5;
	numsides = 7;
	lines = calloc(n ? n : 1, sizeof(*lines));
	sectors = calloc(numsectors, sizeof(*sectors));
	sides = calloc(numsides, sizeof(*sides));
	check(lines && sectors && sides, "fixture input memory");
	for (i = 0; i < n; i++)
	{
		lines[i].args = calloc(NUMLINEARGS, sizeof(INT32));
		check(lines[i].args != NULL, "fixture args memory");
		if (i < count)
			for (j = 0; j < NUMLINEARGS; j++)
				lines[i].args[j] = j & 1 ? INT32_MAX - (INT32)(i & 1023)
					: INT32_MIN + (INT32)(i & 1023);
		lines[i].special = i % 3 ? (INT16)(i & UINT16_MAX) : (INT16)(321 + (i & 1));
		lines[i].executordelay = INT32_MIN + (INT32)i;
		lines[i].secportal = UINT32_MAX - (UINT32)i;
		lines[i].stringargs[0] = i & 1 ? NULL : "original first";
		lines[i].stringargs[1] = i & 2 ? NULL : "original second";
		lines[i].sidenum[0] = i & 2 ? NO_SIDEDEF : (UINT32)(i % numsides);
		lines[i].sidenum[1] = i & 4 ? NO_SIDEDEF : (UINT32)((i + 3) % numsides);
	}
	for (i = 0; i < numsectors; i++)
	{
		sectors[i].tags.count = (UINT16)i;
		sectors[i].tags.tags = calloc(i ? i : 1, sizeof(mtag_t));
		for (j = 0; j < i; j++) sectors[i].tags.tags[j] = (mtag_t)(i * 1700 + j);
		sectors[i].special = (INT16)(i * 19);
		sectors[i].floorheight = INT32_MIN + (INT32)i;
		sectors[i].ceilingheight = INT32_MAX - (INT32)i;
		sectors[i].floorxscale = 65536;
		sectors[i].ceilingyscale = 123456789;
		sectors[i].flags = UINT32_MAX;
		sectors[i].triggertag = INT16_MIN;
		sectors[i].triggerer = 255;
	}
	for (i = 0; i < numsides; i++)
	{
		sides[i].textureoffset = INT32_MIN + (INT32)i;
		sides[i].rowoffset = INT32_MAX - (INT32)i;
		sides[i].toptexture = (INT32)i;
		sides[i].bottomtexture = INT32_MIN;
		sides[i].midtexture = INT32_MAX;
		sides[i].repeatcnt = INT16_MIN + (INT16)i;
	}
}
static void free_inputs(void)
{
	size_t i;
	for (i = 0; i < numlines; i++) free(lines[i].args);
	for (i = 0; i < numsectors; i++) free(sectors[i].tags.tags);
	free(lines); free(sectors); free(sides);
	lines = NULL; sectors = NULL; sides = NULL;
}

static void run_case(size_t n, size_t count, int arena_mode, int load_mode)
{
	size_t i, j, before_allocs, before_bytes, peak, retained, allocs, request;
	const spawnline_t *snapshot;
	spawnline_t scratch;
	void *staticblock;
	save_t save = { malloc(256), 0, 256 };
	setup_inputs(n, count);
	arena_reset(16u << 20, arena_mode & 1, 0, (arena_mode >> 1) & 1);
	/* Some preexisting level/static allocations exercise each side of the arena. */
	staticblock = Z_Malloc(37, PU_STATIC, NULL);
	(void)Z_Malloc(97, PU_LEVEL, NULL);
	before_allocs = za_nalloc;
	before_bytes = za_used;
	netgame = load_mode == 0;
	test_parm = load_mode ? "-keepspawn" : NULL;
	load_spawn_gate();
	check_snapshot(count);
	request = ZA_BLOCK(spawnlines)->realsize;
	check(request <= n * sizeof(spawnline_t), "snapshot request bounded by fixed layout");
	check(za_nalloc - before_allocs == 4, "unchanged four snapshot allocations");
#ifdef SPAWN_NEW
#ifndef PS2_NOOPT_SPAWNLINES
	check(compactspawnlines == (n * sizeof(compactspawnline_t) + count * NUMLINEARGS * sizeof(INT32)
		< n * sizeof(spawnline_t)), "adaptive representation threshold");
#endif
#endif
	digest = UINT64_C(1469598103934665603);
	check_snapshot(count);
	emit_archive(&save);
	for (i = 0; i < n; i++)
	{
		/* The snapshot must remain immutable when zero and nonzero live vectors change. */
		for (j = 0; j < NUMLINEARGS; j++) lines[i].args[j] ^= (INT32)(UINT32_C(0x81234567) + j);
		if (!(i % 7)) lines[i].special ^= INT16_MAX;
		if (!(i % 11)) lines[i].executordelay ^= INT32_MAX;
		if (!(i % 13)) lines[i].secportal ^= UINT32_C(0x80000000);
		if (lines[i].stringargs[0] && !(i % 17)) lines[i].stringargs[0] = "replacement first";
		if (lines[i].stringargs[1] && !(i % 19)) lines[i].stringargs[1] = "replacement second";
		lines[i].callcount = (INT16)i;
		snapshot = P_GetSpawnLine(i, &scratch);
		for (j = 0; j < NUMLINEARGS; j++)
		{
			INT32 old = i < count ? (j & 1 ? INT32_MAX - (INT32)(i & 1023)
				: INT32_MIN + (INT32)(i & 1023)) : 0;
			check(snapshot->args[j] == old, "live write leaves immutable snapshot intact");
		}
	}
	for (i = 0; i < numsides; i++) sides[i].toptexture ^= INT32_MAX, sides[i].textureoffset ^= 65535;
	emit_archive(&save);
	peak = za_peak;
	retained = za_used - before_bytes;
	allocs = za_nalloc - before_allocs;
	printf("case n=%zu k=%zu arena=%d mode=%d digest=%llu peak=%zu retained=%zu allocs=%zu request=%zu\n",
		n, count, arena_mode, load_mode, (unsigned long long)digest, peak, retained, allocs, request);
	consistent();
	Z_FreeTags(PU_LEVEL, PU_LEVEL);
	check(za_used == blk_bytes(staticblock), "level release leaves original static only");
	consistent();
	free(save.buf);
	free_inputs();
}

static void failure_cases(void)
{
	volatile size_t fail, mode;
	for (mode = 0; mode < 2; mode++)
	for (fail = 0; fail < 4; fail++)
	{
		/* Use the production allocator's nth-allocation fault hook and guard. */
		size_t before_allocs;
		zguard_t guard;
		setup_inputs(1200, mode ? 1200 : 0);
		arena_reset(1u << 20, 0, 0, 0);
		before_allocs = za_nalloc;
		zoomnth = (UINT32)fail + 1;
		zoomnth_count = zoomnth_after = 0;
		netgame = false;
		if (Z_GUARD_TRY(&guard))
		{
			P_CopySpawnState();
			Z_GuardPop(&guard);
			check(0, "injected construction allocation must fail");
		}
		else Z_GuardLanded(&guard);
		zoomnth = 0;
		check(za_nalloc - before_allocs == fail, "failed at requested construction allocation");
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		consistent();
		/* Reload after the abandoned partial allocation in the same arena. */
		P_CopySpawnState();
		digest = UINT64_C(1469598103934665603);
		check_snapshot(mode ? 1200 : 0);
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		check(za_used == 0, "failed construction and reload release every level block");
		free_inputs();
	}
#ifdef SPAWN_NEW
	for (mode = 0; mode < 2; mode++)
	{
		/* A failed replacement must not publish a representation for a different root. */
		zguard_t guard;
		spawnline_t *previous;
		boolean previous_mode;
		spawnline_t scratch;
		size_t i, j;
		setup_inputs(1200, mode ? 1200 : 0);
		arena_reset(1u << 20, 0, 0, 0);
		P_CopySpawnState();
		previous = spawnlines;
		previous_mode = compactspawnlines;
		for (i = 0; i < numlines; i++)
			for (j = 0; j < NUMLINEARGS; j++) lines[i].args[j] = mode ? 0 : INT32_MAX;
		zoomnth = 2;
		zoomnth_count = zoomnth_after = 0;
		if (Z_GUARD_TRY(&guard))
		{
			P_CopySpawnState();
			Z_GuardPop(&guard);
			check(0, "replacement root allocation must fail");
		}
		else Z_GuardLanded(&guard);
		zoomnth = 0;
		check(spawnlines == previous && compactspawnlines == previous_mode,
			"failed allocation preserves root and representation together");
		check(P_GetSpawnLine(0, &scratch)->args[0] == (mode ? INT32_MIN : 0),
			"previous root remains readable after refused replacement");
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		P_CopySpawnState();
		digest = UINT64_C(1469598103934665603);
		check_snapshot(mode ? 0 : 1200);
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		check(!za_used, "replacement failure and retry release every block");
		free_inputs();
	}
	numlines = SIZE_MAX / sizeof(spawnline_t) + 1;
	expecting_error = 1;
	if (!setjmp(error_jump)) { (void)P_AllocSpawnLines(); check(0, "snapshot multiplication overflow refused"); }
	expecting_error = 0;
	check(strstr(error_text, "overflow") != NULL, "snapshot overflow before input access/allocation");
	numlines = 0;
#endif
	puts("failure cases passed");
}

static void cache_eviction(void)
{
	size_t mode, i;
	for (mode = 0; mode < 2; mode++)
	{
		void *cache = NULL;
		setup_inputs(1200, mode ? 1200 : 200);
		if (!mode)
			for (i = 0; i < 200; i++)
			{
				memset(lines[i].args, 0, NUMLINEARGS * sizeof(INT32));
				lines[i].args[i % NUMLINEARGS] = (i / NUMLINEARGS) & 1 ? INT32_MAX : INT32_MIN;
			}
		arena_reset(1u << 20, 1, 0, 1);
		Z_Malloc(65536, PU_CACHE, &cache);
		P_CopySpawnState();
		Z_ReleaseCache(cache);
		(void)Z_EvictLRU((size_t)-1, false);
		check(!cache, "real cache eviction clears its owner");
		digest = UINT64_C(1469598103934665603);
		check_snapshot(mode ? 1200 : 200);
		consistent();
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		check(!za_used, "cache eviction retains only complete level snapshots until level release");
		free_inputs();
	}
}

static void ordinary_single_player(void)
{
	setup_inputs(31, 0);
	arena_reset(1u << 20, 0, 0, 0);
	netgame = false;
	test_parm = NULL;
	load_spawn_gate();
	check(!spawnsectors && !spawnlines && !spawnsides && !za_used && !za_nalloc,
		"ordinary single player keeps its zero snapshot allocation cost");
	free_inputs();
}

int main(int argc, char **argv)
{
	static const size_t ns[] = {0, 1, 2, 3, 7, 10, 31, 64, 257, 1200, 25100, 70001};
	size_t ni, k, threshold;
	int mode, load;
	check(argc == 2, "framed archive output path");
	wire_file = fopen(argv[1], "wb");
	check(wire_file != NULL, "framed archive output file");
	ordinary_single_player();
	printf("native pointer=%zu fixed=%zu\n", sizeof(void *), sizeof(spawnline_t));
	for (ni = 0; ni < sizeof(ns) / sizeof(ns[0]); ni++)
		for (mode = 0; mode < 4; mode++)
			for (load = 0; load < 2; load++)
			{
				size_t n = ns[ni];
				threshold = n * (sizeof(spawnline_t) - (sizeof(void*) == 4 ? 24 : 40)) / 40;
				const size_t counts[] = {0, n ? 1 : 0, n / 20, n / 2,
					threshold ? threshold - 1 : 0, threshold, threshold < n ? threshold + 1 : n, n};
				for (k = 0; k < sizeof(counts) / sizeof(counts[0]); k++)
					run_case(n, counts[k], mode, load);
			}
	cache_eviction();
	failure_cases();
	check(!fclose(wire_file), "archive output close");
	if (arena_raw) aligned_release(arena_raw);
	printf("PASS checks=%u\n", checks);
	return 0;
}
