/* Actual production spawn construction, side comparison and world archives.
 * Uses the real zone/arena; unrelated engine services have deterministic stubs.
 * Native measurements retain native pointer/header widths. */
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
static spawnside_t *spawnsides, *side_shadow;
static extracolormap_t colormaps_fixture[3] = {{1}, {2}, {3}};
static ffloor_t ffloors_fixture[3] = {{NULL, 1, 1, 255, 255}, {NULL, UINT32_MAX, 0, 127, 255}, {NULL, 0, UINT32_MAX, INT16_MIN, INT16_MAX}};
static struct { char name[8]; } levelflats[4] = {{"FLAT0000"}, {"FLAT0001"}, {"FLAT0002"}, {"FLAT0003"}};
#include SPAWN_BUILD
#ifndef SPAWN_NEW
static const spawnside_t *P_GetSpawnSide(size_t i, spawnside_t *scratch)
{ (void)scratch; return &spawnsides[i]; }
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

/* Independent field inventory and defaults, never extracted from the new
 * compressed list. kind is the original width, with 1 denoting boolean. */
#ifdef SIDE_UDMF
#define SIDE_EXTRA(X) \
	X(offsetx_top, 0, 4) X(offsetx_mid, 0, 4) X(offsetx_bottom, 0, 4) \
	X(offsety_top, 0, 4) X(offsety_mid, 0, 4) X(offsety_bottom, 0, 4) \
	X(scalex_top, FRACUNIT, 4) X(scalex_mid, FRACUNIT, 4) X(scalex_bottom, FRACUNIT, 4) \
	X(scaley_top, FRACUNIT, 4) X(scaley_mid, FRACUNIT, 4) X(scaley_bottom, FRACUNIT, 4) \
	X(light, 0, 2) X(light_top, 0, 2) X(light_mid, 0, 2) X(light_bottom, 0, 2) \
	X(lightabsolute, false, 1) X(lightabsolute_top, false, 1) \
	X(lightabsolute_mid, false, 1) X(lightabsolute_bottom, false, 1)
#define SIDE_FIELD_COUNT 26
#else
#define SIDE_EXTRA(X)
#define SIDE_FIELD_COUNT 6
#endif
#define SIDE_FIELDS(X) \
	X(textureoffset, 0, 4) X(rowoffset, 0, 4) X(toptexture, 0, 4) \
	X(bottomtexture, 0, 4) X(midtexture, 0, 4) X(repeatcnt, 0, 2) SIDE_EXTRA(X)

static void side_fields(side_t *s, size_t index, size_t populated)
{
	size_t field_index = 0;
	static const UINT32 extremes[] = {UINT32_C(0x80000000), INT32_MAX, UINT32_MAX, UINT32_C(0x81234567)};
	UINT32 value = extremes[index % 4];
#define SET_FIELD(field, initial, kind) \
	s->field = ((field_index++ + index) % SIDE_FIELD_COUNT < populated) \
		? (kind == 1 ? 1u : kind == 2 ? (UINT32)(index & 1 ? INT16_MAX : INT16_MIN) \
			: value == (UINT32)(initial) ? value ^ 1 : value) : (UINT32)(initial);
	SIDE_FIELDS(SET_FIELD)
#undef SET_FIELD
}

static uint64_t digest;
static FILE *wire_file;
static void hash_bytes(const void *p, size_t len)
{
	const UINT8 *bytes = p;
	while (len--) digest = (digest ^ *bytes++) * UINT64_C(1099511628211);
}
static void hash_word(UINT32 v) { hash_bytes(&v, sizeof(v)); }
static void check_side_snapshot(void)
{
	size_t i;
	for (i = 0; i < numsides; i++)
	{
		spawnside_t scratch;
		const spawnside_t *actual = P_GetSpawnSide(i, &scratch), *expected = &side_shadow[i];
#define CHECK_FIELD(field, initial, kind) check(actual->field == expected->field, "complete side field " #field); hash_word((UINT32)actual->field);
		SIDE_FIELDS(CHECK_FIELD)
#undef CHECK_FIELD
	}
}

static void check_snapshot(void)
{
	size_t i, j;
	spawnline_t scratch;
	for (i = 0; i < numlines; i++)
	{
		const spawnline_t *s = P_GetSpawnLine(i, &scratch);
		for (j = 0; j < NUMLINEARGS; j++) check(s->args[j] == lines[i].args[j], "unchanged signed line arguments");
		for (j = 0; j < NUMLINESTRINGARGS; j++) check(s->stringargs[j] == lines[i].stringargs[j], "unchanged string ownership");
		check(s->executordelay == lines[i].executordelay && s->secportal == lines[i].secportal
			&& s->special == lines[i].special, "unchanged line metadata");
	}
	for (i = 0; i < numsectors; i++)
	{
		spawnsector_t scratch, expected;
		const spawnsector_t *actual = P_GetSpawnSector(i, &scratch);
		memset(&expected, 0, sizeof(expected));
		memcpy(&expected, &sectors[i], sizeof(expected));
		expected.tags.tags = actual->tags.tags;
		expected.tags.inline_tag = 0;
		check(!memcmp(actual, &expected, sizeof(expected)), "unchanged complete sector snapshot");
		check(!actual->tags.count || (actual->tags.tags != sectors[i].tags.tags
			&& !memcmp(actual->tags.tags, sectors[i].tags.tags, actual->tags.count * sizeof(mtag_t))), "independently copied tags");
	}
	check_side_snapshot();
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
		&& fwrite(save->buf, 1, save->pos, wire_file) == save->pos, "complete framed production archive bytes");
}

static void update_sides(size_t count)
{
	size_t i;
	for (i = 0; i < numsides; i++)
	{
		side_fields(&sides[i], i, count / numsides + (i < count % numsides));
		side_shadow[i] = sides[i];
	}
}

static void setup_inputs(size_t n, size_t count)
{
	size_t i, j;
	numlines = (n + 1) / 2 + 2;
	numsectors = 7;
	numsides = n;
	lines = calloc(numlines, sizeof(*lines));
	sectors = calloc(numsectors, sizeof(*sectors));
	sides = calloc(n ? n : 1, sizeof(*sides));
	side_shadow = calloc(n ? n : 1, sizeof(*side_shadow));
	check(lines && sectors && sides && side_shadow, "fixture input memory");
	for (i = 0; i < numlines; i++)
	{
		lines[i].args = calloc(NUMLINEARGS, sizeof(INT32));
		check(lines[i].args != NULL, "fixture args memory");
		if (i < 3)
			for (j = 0; j < NUMLINEARGS; j++)
				lines[i].args[j] = j & 1 ? INT32_MAX - (INT32)i : INT32_MIN + (INT32)i;
		lines[i].special = i % 3 ? (INT16)(i & UINT16_MAX) : (INT16)(321 + (i & 1));
		lines[i].executordelay = INT32_MIN + (INT32)i;
		lines[i].secportal = UINT32_MAX - (UINT32)i;
		lines[i].stringargs[0] = i & 1 ? NULL : "original first";
		lines[i].stringargs[1] = i & 2 ? NULL : "original second";
		lines[i].sidenum[0] = 2 * i < n ? (UINT32)(2 * i) : NO_SIDEDEF;
		lines[i].sidenum[1] = 2 * i + 1 < n ? (UINT32)(2 * i + 1) : NO_SIDEDEF;
	}
	/* Exercise a shared side in both slots, and an entirely absent side pair. */
	if (n) lines[numlines - 1].sidenum[0] = lines[numlines - 1].sidenum[1] = 0;
	for (i = 0; i < numsectors; i++)
	{
		sectors[i].tags.count = (UINT16)(i % 5);
		sectors[i].tags.tags = calloc(5, sizeof(mtag_t));
		check(sectors[i].tags.tags != NULL, "fixture tag memory");
		for (j = 0; j < sectors[i].tags.count; j++) sectors[i].tags.tags[j] = (mtag_t)(i * 1700 + j);
		sectors[i].special = (INT16)(i * 19);
		sectors[i].floorheight = INT32_MIN + (INT32)i;
		sectors[i].ceilingheight = INT32_MAX - (INT32)i;
		sectors[i].floorpic = (INT32)(i % 4);
		sectors[i].ceilingpic = (INT32)((i + 1) % 4);
		sectors[i].lightlevel = (INT16)(i & UINT16_MAX);
		sectors[i].extra_colormap = i % 3 ? &colormaps_fixture[i % 3] : NULL;
		sectors[i].ffloors = i % 2 ? &ffloors_fixture[i % 3] : NULL;
		sectors[i].floorxscale = sectors[i].flooryscale = sectors[i].ceilingxscale = sectors[i].ceilingyscale = FRACUNIT;
		sectors[i].gravity = FRACUNIT;
		sectors[i].flags = MSF_FLIPSPECIAL_FLOOR;
		sectors[i].portal_floor = sectors[i].portal_ceiling = UINT32_MAX;
	}
	sectors[1].flags = UINT32_MAX;
	sectors[2].specialflags = UINT32_C(0x80000000);
	sectors[3].floorlightabsolute = true;
	update_sides(count);
}

static void free_inputs(void)
{
	size_t i;
	for (i = 0; i < numlines; i++) free(lines[i].args);
	for (i = 0; i < numsectors; i++) free(sectors[i].tags.tags);
	free(lines); free(sectors); free(sides); free(side_shadow);
	lines = NULL; sectors = NULL; sides = NULL;
}

static void run_case(size_t n, size_t count, int arena_mode, int load_mode)
{
	size_t i, j, before_allocs, before_bytes, peak, retained, allocs, request;
	void *staticblock;
	save_t save = { malloc(256), 0, 256 };
	setup_inputs(n, count);
	arena_reset(16u << 20, arena_mode & 1, 0, (arena_mode >> 1) & 1);
	staticblock = Z_Malloc(37, PU_STATIC, NULL);
	(void)Z_Malloc(97, PU_LEVEL, NULL);
	before_allocs = za_nalloc;
	before_bytes = za_used;
	netgame = load_mode == 0;
	test_parm = load_mode ? "-keepspawn" : NULL;
	load_spawn_gate();
	request = ZA_BLOCK(spawnsides)->realsize;
	check(request <= n * sizeof(spawnside_t), "side request bounded by fixed layout");
	check(za_nalloc - before_allocs == 4, "unchanged four snapshot allocations");
#ifdef SPAWN_NEW
	{
		boolean wanted = false;
#ifndef PS2_NOOPT_SPAWNSIDES
		wanted = count <= (UINT32_MAX >> SIDE_FIELD_COUNT)
			&& 4 * n + 4 * count < n * sizeof(spawnside_t);
#endif
		check(compactspawnsides == wanted, "exact adaptive/index representation threshold");
		check(request == (wanted ? 4 * n + 4 * count : n * sizeof(spawnside_t)), "exact selected payload");
	}
#endif
	digest = UINT64_C(1469598103934665603);
	check_snapshot();
	emit_archive(&save);
	for (i = 0; i < numlines; i++)
	{
		for (j = 0; j < NUMLINEARGS; j++) lines[i].args[j] ^= (INT32)(UINT32_C(0x81234567) + j);
		if (!(i % 7)) lines[i].special ^= INT16_MAX;
		if (!(i % 11)) lines[i].executordelay ^= INT32_MAX;
		if (!(i % 13)) lines[i].secportal ^= UINT32_C(0x80000000);
		if (lines[i].stringargs[0] && !(i % 17)) lines[i].stringargs[0] = "replacement first";
		if (lines[i].stringargs[1] && !(i % 19)) lines[i].stringargs[1] = "replacement second";
		lines[i].callcount = (INT16)i;
	}
	for (i = 0; i < numsides; i++)
	{
#define MUTATE_FIELD(field, initial, kind) sides[i].field ^= kind == 1 ? 1 : kind == 2 ? INT16_MAX : UINT32_C(0x81234567);
		SIDE_FIELDS(MUTATE_FIELD)
#undef MUTATE_FIELD
	}
	check_side_snapshot();
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
		size_t before_allocs;
		zguard_t guard;
		setup_inputs(1200, mode ? 1200 * SIDE_FIELD_COUNT : 0);
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
		check(za_nalloc - before_allocs == fail, "failed at requested allocation site");
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		consistent();
		P_CopySpawnState();
		digest = UINT64_C(1469598103934665603);
		check_snapshot();
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		check(!za_used, "failed construction/reload release every level block");
		free_inputs();
	}
#ifdef SPAWN_NEW
	for (mode = 0; mode < 2; mode++)
	{
		zguard_t guard;
		spawnside_t *previous, scratch, original;
		boolean previous_mode;
		setup_inputs(1200, mode ? 1200 * SIDE_FIELD_COUNT : 0);
		arena_reset(1u << 20, 0, 0, 0);
		P_CopySpawnState();
		previous = spawnsides;
		previous_mode = compactspawnsides;
		original = *P_GetSpawnSide(0, &scratch);
		update_sides(mode ? 0 : 1200 * SIDE_FIELD_COUNT);
		zoomnth = 3;
		zoomnth_count = zoomnth_after = 0;
		if (Z_GUARD_TRY(&guard))
		{
			P_CopySpawnState();
			Z_GuardPop(&guard);
			check(0, "replacement side root allocation must fail");
		}
		else Z_GuardLanded(&guard);
		zoomnth = 0;
		check(spawnsides == previous && compactspawnsides == previous_mode,
			"failed allocation preserves side root and representation together");
		check(!memcmp(P_GetSpawnSide(0, &scratch), &original, sizeof(original)), "previous side root stays readable");
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		P_CopySpawnState();
		digest = UINT64_C(1469598103934665603);
		check_snapshot();
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		check(!za_used, "replacement failure/retry release every block");
		free_inputs();
	}
	numsides = SIZE_MAX / sizeof(spawnside_t) + 1;
	expecting_error = 1;
	if (!setjmp(error_jump)) { (void)P_AllocSpawnSides(); check(0, "side multiplication overflow refused"); }
	expecting_error = 0;
	check(strstr(error_text, "overflow") != NULL, "side overflow before input access/allocation");
	numsides = 0;
#endif
	puts("failure cases passed");
}

static void cache_eviction(void)
{
	size_t mode;
	for (mode = 0; mode < 2; mode++)
	{
		void *cache = NULL;
		setup_inputs(1200, mode ? 1200 * SIDE_FIELD_COUNT : 0);
		arena_reset(1u << 20, 1, 0, 1);
		Z_Malloc(65536, PU_CACHE, &cache);
		P_CopySpawnState();
		Z_ReleaseCache(cache);
		(void)Z_EvictLRU((size_t)-1, false);
		check(!cache, "real cache eviction clears owner");
		digest = UINT64_C(1469598103934665603);
		check_snapshot();
		consistent();
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
		check(!za_used, "level snapshots survive cache eviction and release completely");
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
		"ordinary single player keeps zero snapshot allocations");
	free_inputs();
}

int main(int argc, char **argv)
{
	static const size_t ns[] = {0, 1, 2, 3, 7, 10, 31, 64, 257, 1200, 25100, 70001};
	size_t ni, k;
	int mode, load;
	check(argc == 2, "framed archive output path");
	wire_file = fopen(argv[1], "wb");
	check(wire_file != NULL, "framed archive output file");
	ordinary_single_player();
	printf("native pointer=%zu fixed=%zu boolean=%zu fields=%d\n", sizeof(void *), sizeof(spawnside_t), sizeof(boolean), SIDE_FIELD_COUNT);
	for (ni = 0; ni < sizeof(ns) / sizeof(ns[0]); ni++)
		for (mode = 0; mode < (ns[ni] > 1200 ? 1 : 4); mode++)
			for (load = 0; load < (ns[ni] > 1200 ? 1 : 2); load++)
			{
				size_t n = ns[ni], threshold = (sizeof(spawnside_t) - 4) * n / 4;
				const size_t counts[] = {0, n ? 1 : 0, n, 2 * n,
					threshold ? threshold - 1 : 0, threshold, threshold + (n != 0), n * SIDE_FIELD_COUNT
#ifdef SIDE_UDMF
					, n * SIDE_FIELD_COUNT < 62 ? n * SIDE_FIELD_COUNT : 62,
					n * SIDE_FIELD_COUNT < 63 ? n * SIDE_FIELD_COUNT : 63,
					n * SIDE_FIELD_COUNT < 64 ? n * SIDE_FIELD_COUNT : 64
#endif
				};
				for (k = 0; k < sizeof(counts) / sizeof(counts[0]); k++) run_case(n, counts[k], mode, load);
			}
	cache_eviction();
	failure_cases();
	check(!fclose(wire_file), "archive output close");
	if (arena_raw) aligned_release(arena_raw);
	printf("PASS checks=%u\n", checks);
	return 0;
}
