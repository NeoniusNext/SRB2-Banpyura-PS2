/* Actual spawn snapshot construction and world archive functions, real zone.
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
#define boolean zone_boolean
#include "zone_hosttest.c"
#undef boolean
#undef main
typedef _Bool boolean;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef INT32 fixed_t;
typedef UINT32 angle_t;
typedef INT16 mtag_t;
typedef UINT32 sectorflags_t;
typedef UINT32 sectorspecialflags_t;
typedef struct { int unused; } extracolormap_t;
typedef struct { mtag_t *tags; UINT16 count; mtag_t inline_tag; } taglist_t;
typedef struct ffloor_s { struct ffloor_s *next; UINT32 fofflags, spawnflags; INT16 alpha, spawnalpha; } ffloor_t;
#define FRACUNIT 65536
#define MSF_FLIPSPECIAL_FLOOR 1
#define SD_NONE 0
#define TO_PLAYER 0
#define NUMLINEARGS 10
#define NUMLINESTRINGARGS 2
#define NO_SIDEDEF UINT32_MAX
#include SPAWN_TYPES
#include SPAWN_LIVE
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
static spawnsector_t *sector_shadow;
static extracolormap_t colormaps_fixture[5] = {{1}, {2}, {3}, {4}, {5}};
static ffloor_t ffloors_fixture[3] = {{NULL, 1, 1, 255, 255}, {NULL, UINT32_MAX, 0, 127, 255}, {NULL, 0, UINT32_MAX, INT16_MIN, INT16_MAX}};
static struct { char name[8]; } levelflats[4] = {{"FLAT0000"}, {"FLAT0001"}, {"FLAT0002"}, {"FLAT0003"}};
#include SPAWN_BUILD
#ifndef SPAWN_NEW
static const spawnsector_t *P_GetSpawnSector(size_t i, spawnsector_t *scratch)
{ (void)scratch; return &spawnsectors[i]; }
#endif
static void load_spawn_gate(void)
{
#include SPAWN_GATE
}

typedef struct { UINT8 *buf; size_t pos, size; } save_t;
#include SPAWN_WRITES
#include SPAWN_FLAGS
#include SPAWN_EQUAL
#include SPAWN_TAG_COMPARE
#include SPAWN_FFLOORS
static UINT32 CheckAddNetColormapToList(const extracolormap_t *c) { return c ? (UINT32)c->unused : 0; }
#include SPAWN_SECTOR_ARCHIVE
#include SPAWN_SIDE_ARCHIVE
#include SPAWN_ARCHIVE

/* Independent full field inventory: do not derive this from the production
 * compressed field list. Every source field and its original width is checked. */
#define SECTOR_OPTIONAL(X) \
	X(floorxoffset, 0, 4) X(flooryoffset, 0, 4) X(ceilingxoffset, 0, 4) X(ceilingyoffset, 0, 4) \
	X(floorxscale, FRACUNIT, 4) X(flooryscale, FRACUNIT, 4) X(ceilingxscale, FRACUNIT, 4) X(ceilingyscale, FRACUNIT, 4) \
	X(floorangle, 0, 4) X(ceilingangle, 0, 4) X(floorlightlevel, 0, 2) X(ceilinglightlevel, 0, 2) \
	X(floorlightabsolute, false, 1) X(ceilinglightabsolute, false, 1) X(gravity, FRACUNIT, 4) \
	X(flags, MSF_FLIPSPECIAL_FLOOR, 4) X(specialflags, 0, 4) X(portal_floor, UINT32_MAX, 4) \
	X(portal_ceiling, UINT32_MAX, 4) X(triggertag, 0, 2) X(damagetype, SD_NONE, 0) X(triggerer, TO_PLAYER, 0)

static void optional_fields(sector_t *s, size_t index, size_t populated)
{
	size_t field_index = 0;
	static const UINT32 extremes[] = {UINT32_MAX, UINT32_C(0x80000000), INT32_MAX, UINT32_C(0x81234567)};
	UINT32 value = extremes[index % 4];
#define SET_FIELD(field, initial, kind) \
	s->field = ((field_index++ + index) % 22 < populated) \
		? (kind == 1 ? 1u : kind == 0 ? 255u - (UINT32)(index & 1) : kind == 2 ? (UINT32)(index & 1 ? INT16_MIN : INT16_MAX) \
			: value == (UINT32)(initial) ? value ^ 1 : value) : (UINT32)(initial);
	SECTOR_OPTIONAL(SET_FIELD)
#undef SET_FIELD
}

static void sync_shadow_fields(void)
{
	size_t i;
	for (i = 0; i < numsectors; i++)
	{
#define COPY_SHADOW(field, initial, kind) sector_shadow[i].field = sectors[i].field;
		SECTOR_OPTIONAL(COPY_SHADOW)
#undef COPY_SHADOW
	}
}

static uint64_t digest;
static FILE *wire_file;
static void hash_bytes(const void *p, size_t len)
{
	const UINT8 *bytes = p;
	while (len--) digest = (digest ^ *bytes++) * UINT64_C(1099511628211);
}
static void hash_word(UINT32 v) { hash_bytes(&v, sizeof(v)); }
static void check_sector_snapshot(void)
{
	size_t i;
	for (i = 0; i < numsectors; i++)
	{
		spawnsector_t sector_scratch;
		const spawnsector_t *actual = P_GetSpawnSector(i, &sector_scratch), *expected = &sector_shadow[i];
#define CHECK_FIELD(field, initial, kind) check(actual->field == expected->field, "sector field " #field); hash_word((UINT32)actual->field);
		SECTOR_OPTIONAL(CHECK_FIELD)
		CHECK_FIELD(floorheight, 0, 4) CHECK_FIELD(ceilingheight, 0, 4)
		CHECK_FIELD(floorpic, 0, 4) CHECK_FIELD(ceilingpic, 0, 4)
		CHECK_FIELD(lightlevel, 0, 2) CHECK_FIELD(special, 0, 2)
#undef CHECK_FIELD
		check(actual->extra_colormap == expected->extra_colormap, "original colormap pointer identity");
		hash_word(CheckAddNetColormapToList(actual->extra_colormap));
		check(actual->tags.count == expected->tags.count && !actual->tags.inline_tag, "exact copied tag metadata");
		check(!actual->tags.count || (actual->tags.tags != sectors[i].tags.tags
			&& !memcmp(actual->tags.tags, expected->tags.tags, actual->tags.count * sizeof(mtag_t))), "sector tags copied independently");
		hash_word(actual->tags.count);
		hash_bytes(actual->tags.tags, actual->tags.count * sizeof(mtag_t));
	}
}

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
	check_sector_snapshot();
	check(!memcmp(spawnsides, sides, numsides * sizeof(*sides)), "complete side snapshot");
}

static void emit_archive(save_t *save)
{
	uint64_t bytes;
	save->pos = 0;
	ArchiveSectors(save);
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
	numlines = 7;
	numsectors = n;
	numsides = 7;
	lines = calloc(numlines, sizeof(*lines));
	sectors = calloc(n ? n : 1, sizeof(*sectors));
	sector_shadow = calloc(n ? n : 1, sizeof(*sector_shadow));
	sides = calloc(numsides, sizeof(*sides));
	check(lines && sectors && sides && sector_shadow, "fixture input memory");
	for (i = 0; i < numlines; i++)
	{
		lines[i].args = calloc(NUMLINEARGS, sizeof(INT32));
		check(lines[i].args != NULL, "fixture args memory");
		if (i < 3)
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
		sectors[i].tags.count = (UINT16)(i % 5);
		sectors[i].tags.tags = calloc(5, sizeof(mtag_t));
		sector_shadow[i].tags.count = sectors[i].tags.count;
		sector_shadow[i].tags.tags = calloc(5, sizeof(mtag_t));
		check(sectors[i].tags.tags && sector_shadow[i].tags.tags, "fixture tag memory");
		for (j = 0; j < sectors[i].tags.count; j++)
			sector_shadow[i].tags.tags[j] = sectors[i].tags.tags[j] = (mtag_t)(i * 1700 + j);
		sectors[i].special = (INT16)(i * 19);
		sectors[i].floorheight = INT32_MIN + (INT32)i;
		sectors[i].ceilingheight = INT32_MAX - (INT32)i;
		sectors[i].floorpic = (INT32)(i % 4);
		sectors[i].ceilingpic = (INT32)((i + 1) % 4);
		sectors[i].lightlevel = (INT16)(i & UINT16_MAX);
		sectors[i].extra_colormap = i % 3 ? &colormaps_fixture[i % 5] : NULL;
		sectors[i].ffloors = i % 2 ? &ffloors_fixture[i % 3] : NULL;
		sector_shadow[i].floorheight = sectors[i].floorheight;
		sector_shadow[i].ceilingheight = sectors[i].ceilingheight;
		sector_shadow[i].floorpic = sectors[i].floorpic;
		sector_shadow[i].ceilingpic = sectors[i].ceilingpic;
		sector_shadow[i].lightlevel = sectors[i].lightlevel;
		sector_shadow[i].special = sectors[i].special;
		sector_shadow[i].extra_colormap = sectors[i].extra_colormap;
		optional_fields(&sectors[i], i, count);
	}
	sync_shadow_fields();
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
	for (i = 0; i < numsectors; i++) free(sectors[i].tags.tags), free(sector_shadow[i].tags.tags);
	free(lines); free(sectors); free(sides); free(sector_shadow);
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
	check_snapshot(3);
	request = ZA_BLOCK(spawnsectors)->realsize;
	check(request <= n * sizeof(spawnsector_t), "snapshot request bounded by fixed layout");
	check(za_nalloc - before_allocs == (n > 1 ? 4u : 3u), "unchanged snapshot allocation count");
#ifdef SPAWN_NEW
#ifndef PS2_NOOPT_SPAWNSECTORS
	check(compactspawnsectors == (n * sizeof(compactspawnsector_t) + n * count * sizeof(UINT32)
		< n * sizeof(spawnsector_t)), "adaptive representation threshold");
#endif
#endif
	digest = UINT64_C(1469598103934665603);
	check_snapshot(3);
	emit_archive(&save);
	for (i = 0; i < numlines; i++)
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
			INT32 old = i < 3 ? (j & 1 ? INT32_MAX - (INT32)(i & 1023)
				: INT32_MIN + (INT32)(i & 1023)) : 0;
			check(snapshot->args[j] == old, "live write leaves immutable snapshot intact");
		}
	}
	for (i = 0; i < numsectors; i++)
	{
		optional_fields(&sectors[i], i + 1, 22 - count);
		sectors[i].floorheight ^= INT32_MAX;
		sectors[i].ceilingheight ^= INT32_MAX;
		sectors[i].floorpic = (sectors[i].floorpic + 1) % 4;
		sectors[i].ceilingpic = (sectors[i].ceilingpic + 1) % 4;
		sectors[i].lightlevel ^= INT16_MAX;
		sectors[i].special ^= INT16_MAX;
		sectors[i].extra_colormap = &colormaps_fixture[(i + 1) % 5];
		sectors[i].crumblestate = (INT32)i;
		for (j = 0; j < sectors[i].tags.count; j++) sectors[i].tags.tags[j] ^= INT16_MAX;
	}
	/* Check the complete original sector snapshot after every live field changes. */
	check_sector_snapshot();
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
		setup_inputs(1200, mode ? 22 : 0);
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
		check_snapshot(3);
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		check(za_used == 0, "failed construction and reload release every level block");
		free_inputs();
	}
#ifdef SPAWN_NEW
	for (mode = 0; mode < 2; mode++)
	{
		/* A failed replacement must not publish a representation for a different root. */
		zguard_t guard;
		spawnsector_t *previous;
		boolean previous_mode;
		spawnsector_t scratch;
		INT32 original_offset;
		size_t i;
		setup_inputs(1200, mode ? 22 : 0);
		arena_reset(1u << 20, 0, 0, 0);
		P_CopySpawnState();
		previous = spawnsectors;
		previous_mode = compactspawnsectors;
		original_offset = P_GetSpawnSector(0, &scratch)->floorxoffset;
		for (i = 0; i < numsectors; i++) optional_fields(&sectors[i], i, mode ? 0 : 22);
		sync_shadow_fields();
		zoomnth = 1;
		zoomnth_count = zoomnth_after = 0;
		if (Z_GUARD_TRY(&guard))
		{
			P_CopySpawnState();
			Z_GuardPop(&guard);
			check(0, "replacement root allocation must fail");
		}
		else Z_GuardLanded(&guard);
		zoomnth = 0;
		check(spawnsectors == previous && compactspawnsectors == previous_mode,
			"failed allocation preserves root and representation together");
		check(P_GetSpawnSector(0, &scratch)->floorxoffset == original_offset,
			"previous root remains readable after refused replacement");
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		P_CopySpawnState();
		digest = UINT64_C(1469598103934665603);
		check_snapshot(3);
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		check(!za_used, "replacement failure and retry release every block");
		free_inputs();
	}
	numsectors = SIZE_MAX / sizeof(spawnsector_t) + 1;
	expecting_error = 1;
	if (!setjmp(error_jump)) { (void)P_AllocSpawnSectors(); check(0, "snapshot multiplication overflow refused"); }
	expecting_error = 0;
	check(strstr(error_text, "overflow") != NULL, "snapshot overflow before input access/allocation");
	numsectors = 0;
#endif
	puts("failure cases passed");
}

static void cache_eviction(void)
{
	size_t mode;
	for (mode = 0; mode < 2; mode++)
	{
		void *cache = NULL;
		setup_inputs(1200, mode ? 22 : 0);
		arena_reset(1u << 20, 1, 0, 1);
		Z_Malloc(65536, PU_CACHE, &cache);
		P_CopySpawnState();
		Z_ReleaseCache(cache);
		(void)Z_EvictLRU((size_t)-1, false);
		check(!cache, "real cache eviction clears its owner");
		digest = UINT64_C(1469598103934665603);
		check_snapshot(3);
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
	static const size_t ns[] = {0, 1, 2, 3, 7, 31, 64, 257, 1200, 25100, 70001};
	size_t ni, k;
	int mode, load;
	check(argc == 2, "framed archive output path");
	wire_file = fopen(argv[1], "wb");
	check(wire_file != NULL, "framed archive output file");
	ordinary_single_player();
	printf("native pointer=%zu fixed=%zu boolean=%zu\n", sizeof(void *), sizeof(spawnsector_t), sizeof(boolean));
	for (ni = 0; ni < sizeof(ns) / sizeof(ns[0]); ni++)
		for (mode = 0; mode < (ns[ni] > 1200 ? 1 : 4); mode++)
			for (load = 0; load < (ns[ni] > 1200 ? 1 : 2); load++)
				for (k = 0; k <= 22; k++) run_case(ns[ni], k, mode, load);
	cache_eviction();
	failure_cases();
	check(!fclose(wire_file), "archive output close");
	if (arena_raw) aligned_release(arena_raw);
	printf("PASS checks=%u\n", checks);
	return 0;
}
