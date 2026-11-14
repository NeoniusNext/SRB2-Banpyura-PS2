/* Actual sprite install/loader/rotation-cache accessors with the real zone.
 * Platform resource metadata and the image-rotation kernel are boundary stubs.
 * Literal snapshots contain semantic fields, never object padding/addresses. */
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
#include MEMORY_ZONE
#include <ctype.h>
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef INT32 fixed_t;
typedef UINT32 lumpnum_t;
#define LUMPERROR UINT32_MAX
#define WADFILENUM(n) ((UINT16)((n) >> 16))
#define LUMPNUM(n) ((UINT16)(n))
#define ROTSPRITE
#define PS2_LIMITS
#define PS2_DYNLIMITS
#define FRACUNIT 65536
#define FRACBITS 16
#define FEETADJUST (4 * FRACUNIT)
#define ROTANGLES 72
#define FUNCMATH
#define FUNCINLINE
#define ATTRINLINE
#define ATTRPACK __attribute__((packed))
#define DBG_SETUP 1
enum { CONS_ERROR, CONS_WARNING };
static unsigned diagnostic_count, cv_debug;
static void CONS_Alert(int kind, const char *fmt, ...)
{ (void)kind; (void)fmt; diagnostic_count++; }
#include MEMORY_TYPES
#include MEMORY_HELPERS
typedef struct { INT32 x, y; } spriteframepivot_t;
typedef struct { boolean available; spriteframepivot_t *pivot; } spriteinfo_t;
typedef struct { INT16 width_pixels, offset_pixels, topoffset_pixels, height_pixels; } sprcache_t;
typedef struct { char name[9]; char *longname; size_t size; INT16 header[4]; int good; } lumpinfo_t;
typedef struct { UINT16 numlumps; lumpinfo_t *lumpinfo; } wadfile_t;
static wadfile_t **wadfiles;
static size_t numspritelumps, max_spritelumps;
static sprcache_t *spritecachedinfo;
static spriteframe_t sprtemp_small[PS2_SMALL_MAXFRAMENUM];
static spriteframe_t *sprtemp = sprtemp_small;
static size_t maxframe;
static const char *spritename;
static boolean ps2_fulllimits;
static FILE *snapshot_file;
static unsigned cases, stockframes, stockroots;

#define strlcpy memory_strlcpy
static size_t memory_strlcpy(char *out, const char *in, size_t bytes)
{
	size_t n = strlen(in);
	if (bytes) { size_t take = n < bytes - 1 ? n : bytes - 1; memcpy(out, in, take); out[take] = 0; }
	return n;
}
static void PS2Limits_Grow(void)
{
	check(!ps2_fulllimits, "scratch grows once");
#include MEMORY_GROWTH
	ps2_fulllimits = true;
}
static boolean W_IsLumpFolder(UINT16 wad, UINT16 lump)
{ (void)wad; (void)lump; return false; }
static const char *W_CheckNameForNumPwad(UINT16 wad, UINT16 lump)
{ return wadfiles[wad]->lumpinfo[lump].name; }
static size_t W_LumpLengthPwad(UINT16 wad, UINT16 lump)
{ return wadfiles[wad]->lumpinfo[lump].size; }
static boolean W_ReadPatchHeaderPwad(UINT16 wad, UINT16 lump, INT16 *width,
	INT16 *height, INT16 *top, INT16 *left)
{
	const lumpinfo_t *entry = &wadfiles[wad]->lumpinfo[lump];
	*width = entry->header[0]; *height = entry->header[1];
	*left = entry->header[2]; *top = entry->header[3];
	return entry->good;
}
#include MEMORY_LOADER

static patch_t sourcepatch;
static unsigned rotate_calls;
static INT32 last_xpivot, last_ypivot;
static lumpnum_t last_lump;
static rotsprite_t *RotatedPatch_Create(INT32);
static patch_t *W_CachePatchNum(lumpnum_t lump, INT32 tag)
{ (void)tag; last_lump = lump; return &sourcepatch; }
static void RotatedPatch_DoRotation(rotsprite_t *sprite, patch_t *patch, INT32 angle,
	INT32 xpivot, INT32 ypivot, boolean flip)
{
	INT32 index = angle + (flip ? sprite->angles : 0);
	patch_t *out;
	check(patch == &sourcepatch && !sprite->patches[index], "new rotation receives original patch and empty cache");
	out = Z_Calloc(sizeof *out, PU_STATIC, NULL);
	out->leftoffset = (INT16)xpivot; out->topoffset = (INT16)ypivot;
	out->width = (INT16)angle; out->height = (INT16)flip;
	sprite->patches[index] = out;
	last_xpivot = xpivot; last_ypivot = ypivot; rotate_calls++;
}
#include MEMORY_ROTATION

static void word(UINT32 value)
{
	UINT8 bytes[4] = {(UINT8)value, (UINT8)(value >> 8), (UINT8)(value >> 16), (UINT8)(value >> 24)};
	check(fwrite(bytes, 1, sizeof bytes, snapshot_file) == sizeof bytes, "literal semantic snapshot write");
}
static void snapshot(const spriteframe_t *frame)
{
	word(frame->rotate); word(frame->flip);
	for (unsigned r = 0; r < 16; r++) { word(frame->lumppat[r]); word(frame->lumpid[r]); }
	word(frame->rotated != NULL);
}
static void begin(int sides, int guard)
{
	arena_reset(16u << 20, sides, ship_prefer, guard);
	sprtemp = sprtemp_small; ps2_fulllimits = false;
	memset(sprtemp_small, 0xFF, sizeof sprtemp_small);
	memset(&sprindex, 0, sizeof sprindex);
	numspritelumps = 0; max_spritelumps = 8192;
	spritecachedinfo = Z_Calloc(max_spritelumps * sizeof *spritecachedinfo, PU_STATIC, &spritecachedinfo);
	maxframe = (size_t)-1; spritename = "TEST";
}
static void finish(void)
{
	zastats_t stats;
	R_FreeSpriteIndex();
	Z_Free(spritecachedinfo); spritecachedinfo = NULL;
	if (sprtemp != sprtemp_small) Z_Free(sprtemp);
	sprtemp = sprtemp_small;
	consistent(); ZA_Stats(&stats);
	check(stats.used == 0, "all fixture roots released");
}

static rotsprite_t *pointer_tables[17][16];
static void table_case(size_t count, int sides, int guard)
{
	spriteframe_t *frames, *copy;
	UINT8 *neighbor;
	zastats_t stats;
	arena_reset(16u << 20, sides, ship_prefer, guard);
	frames = Z_Malloc(count * sizeof *frames, PU_STATIC, NULL);
	neighbor = Z_Malloc(113, PU_STATIC, NULL); memset(neighbor, 0x5D, 113);
	copy = Z_Malloc(count * sizeof *copy, PU_STATIC, NULL);
	memset(frames, 0xA7, count * sizeof *frames);
	memset(copy, 0x69, count * sizeof *copy);
	for (size_t i = 0; i < count; i++)
	{
		frames[i].rotate = (UINT8)i; frames[i].flip = (UINT16)(i * 40503u);
		frames[i].rotated = pointer_tables[i % 17];
		for (unsigned r = 0; r < 16; r++)
		{
			frames[i].lumppat[r] = UINT32_MAX - (UINT32)(i * 65537u + r);
			frames[i].lumpid[r] = (UINT16)(UINT16_MAX - i * 17u - r);
		}
	}
	M_Memcpy(copy, frames, count * sizeof *copy);
	for (size_t i = 0; i < count; i++)
	{
		check(copy[i].rotate == (UINT8)i && copy[i].flip == (UINT16)(i * 40503u), "full flag bytes survive write and array copy");
		check(copy[i].rotated == pointer_tables[i % 17], "complete rotated-table pointer identity");
		check((uintptr_t)&copy[i].lumppat % _Alignof(lumpnum_t) == 0
			&& (uintptr_t)&copy[i].lumpid % _Alignof(UINT16) == 0
			&& (uintptr_t)&copy[i].rotated % _Alignof(rotsprite_t **) == 0, "each record naturally aligned");
		for (unsigned r = 0; r < 16; r++)
			check(copy[i].lumppat[r] == UINT32_MAX - (UINT32)(i * 65537u + r)
				&& copy[i].lumpid[r] == (UINT16)(UINT16_MAX - i * 17u - r), "all sixteen full-width identifiers survive");
	}
	for (unsigned i = 0; i < 113; i++) check(neighbor[i] == 0x5D, "neighbor canary");
	ZA_Stats(&stats); consistent();
	printf("MEMORY8 case=%u n=%zu sides=%d guard=%d request=%zu used=%zu peak=%zu\n",
		cases++, count, sides, guard, 2 * count * sizeof *frames, stats.used, stats.globalpeak);
	Z_Free(copy); Z_Free(neighbor); Z_Free(frames); ZA_Stats(&stats);
	check(!stats.used, "copied tables released");
}

static void synthetic_case(unsigned mode, int sides, int guard)
{
	static const char *names[][8] = {
		{"TESTA0", "TESTB0C0", "TESTD0", NULL},
		{"TESTA1", "TESTA2A8", "TESTA3A7", "TESTA4A6", "TESTA5", NULL},
		{"TESTAL", "TESTAR", "TESTBLBR", "TESTA0", "TESTAL", NULL},
		{"0", "63", "64", "255", NULL},
		{"0_1", "0_2+0_8", "0_3+0_7", "0_4+0_6", "0_5", "255", NULL},
		{"TESTA0", "TESTBAD", "TESTE0", "TESTF0", NULL}
	};
	lumpinfo_t lumps[20]; wadfile_t wad, *files[] = {&wad};
	spritedef_t definition = {0, NULL};
	zastats_t stats;
	unsigned n = 0;
	int longname = mode == 3 || mode == 4;
	begin(sides, guard); wadfiles = files;
	memset(lumps, 0, sizeof lumps);
	while (names[mode][n])
	{
		lumpinfo_t *entry = &lumps[n];
		strlcpy(entry->name, names[mode][n], sizeof entry->name);
		entry->longname = (char *)names[mode][n]; entry->size = 40; entry->good = 1;
		entry->header[0] = (INT16)(32767 - n); entry->header[1] = (INT16)(-32768 + n);
		entry->header[2] = (INT16)(-32768 + n * 7); entry->header[3] = (INT16)(32767 - n);
		n++;
	}
	if (mode == 5) { lumps[2].size = 8; lumps[3].good = 0; }
	wad.numlumps = (UINT16)n; wad.lumpinfo = lumps;
	check(R_AddSingleSpriteDef("TEST", &definition, 0, 0, n, longname), "actual loader installs synthetic sprite");
	check(definition.numframes == (mode == 0 ? 4 : mode == 2 ? 2 : mode == 3 || mode == 4 ? 256 : 1), "exact synthetic frame count");
	for (size_t f = 0; f < definition.numframes; f++) snapshot(&definition.spriteframes[f]);
	if (!longname)
	{
		lumpnum_t old = definition.spriteframes[0].lumppat[0];
		unsigned before = numspritelumps;
		check(!R_AddSingleSpriteDef("NONE", &definition, 0, 0, n, false), "unrelated add-on preserves definition");
		check(definition.spriteframes[0].lumppat[0] == old && numspritelumps == before, "unrelated add-on has no data/allocation effect");
		strlcpy(lumps[0].name, "TEST@0", sizeof lumps[0].name);
		check(R_AddSingleSpriteDef("TEST", &definition, 0, 0, n, false), "actual add-on replacement grows root");
		check(definition.numframes == 64, "full short-name capacity preserved");
		for (size_t f = 0; f < definition.numframes; f++) snapshot(&definition.spriteframes[f]);
	}
	for (size_t i = 0; i < numspritelumps; i++)
	{
		word((UINT16)spritecachedinfo[i].width_pixels); word((UINT16)spritecachedinfo[i].offset_pixels);
		word((UINT16)spritecachedinfo[i].topoffset_pixels); word((UINT16)spritecachedinfo[i].height_pixels);
	}
	ZA_Stats(&stats); consistent();
	printf("MEMORY8 case=%u mode=%u sides=%d guard=%d frames=%zu lumps=%zu used=%zu peak=%zu\n",
		cases++, mode, sides, guard, definition.numframes, numspritelumps, stats.used, stats.globalpeak);
	Z_Free(definition.spriteframes); finish();
}

static void rotation_case(int sides, int guard)
{
	spriteframepivot_t pivots[256]; spriteinfo_t info = {false, pivots};
	spriteframe_t *frame;
	begin(sides, guard);
	memset(pivots, 0, sizeof pivots);
	frame = Z_Calloc(2 * sizeof *frame, PU_STATIC, NULL);
	memset(&sourcepatch, 0, sizeof sourcepatch); sourcepatch.leftoffset = -32768; sourcepatch.height = 32767;
	for (unsigned a = 0; a < 16; a++) frame->lumppat[a] = UINT32_MAX - a - 1;
	for (unsigned a = 0; a < 16; a++)
	for (unsigned flip = 0; flip < 2; flip++)
	for (INT32 angle = 1; angle < ROTANGLES; angle++)
	{
		unsigned before = rotate_calls;
		patch_t *patch;
		info.available = ((a + flip + angle) & 1);
		pivots[255].x = INT32_MIN + angle; pivots[255].y = INT32_MAX - angle;
		patch = Patch_GetRotatedSprite(frame, 255, a, flip, &info, angle);
		check(patch && frame->rotated && frame->rotated[a] && rotate_calls == before + 1, "each rotation publishes exactly one cached patch");
		check(last_lump == frame->lumppat[a] && last_xpivot == (info.available ? pivots[255].x : -32768)
			&& last_ypivot == (info.available ? pivots[255].y : 32767 / 2), "rotation reads exact frame, lump and pivot");
		check(Patch_GetRotatedSprite(frame, 255, a, flip, &info, angle) == patch && rotate_calls == before + 1,
			"cache hit returns same patch without a new kernel call");
		word(a); word(flip); word((UINT16)patch->width); word((UINT16)patch->height);
		word((UINT32)last_xpivot); word((UINT32)last_ypivot); word(last_lump);
	}
	check(!Patch_GetRotatedSprite(frame, 255, 0, false, &info, 0)
		&& !Patch_GetRotatedSprite(frame, 255, 0, false, &info, ROTANGLES), "invalid roll angles retain null result");
	M_Memcpy(frame + 1, frame, sizeof *frame);
	check(frame[1].rotated == frame->rotated && frame[1].flip == frame->flip, "copied rotated frame retains shared table identity");
	for (unsigned a = 0; a < 16; a++)
	{
		rotsprite_t *rotated = frame->rotated[a];
		for (unsigned i = 0; i < 2 * ROTANGLES; i++) Z_Free(rotated->patches[i]);
		Z_Free(rotated->patches); Z_Free(rotated);
	}
	Z_Free(frame->rotated); Z_Free(frame); finish();
	printf("MEMORY8 case=%u rotate=1 sides=%d guard=%d\n", cases++, sides, guard);
}

static UINT32 read_word(FILE *in)
{
	UINT8 bytes[4]; check(fread(bytes, 1, 4, in) == 4, "manifest word");
	return bytes[0] | ((UINT32)bytes[1] << 8) | ((UINT32)bytes[2] << 16) | ((UINT32)bytes[3] << 24);
}
static char *read_string(FILE *in)
{
	UINT8 bytes[2]; unsigned length; char *text;
	check(fread(bytes, 1, 2, in) == 2, "manifest string length"); length = bytes[0] | ((unsigned)bytes[1] << 8);
	text = malloc(length + 1); check(text != NULL, "manifest string allocation");
	check(fread(text, 1, length, in) == length, "manifest string bytes"); text[length] = 0; return text;
}
static void stock_case(const char *path)
{
	FILE *in = fopen(path, "rb");
	UINT32 packs, groups, count;
	spritedef_t *definitions;
	zastats_t stats;
	check(in != NULL, "stock manifest open"); packs = read_word(in); groups = read_word(in); count = read_word(in);
	wadfiles = calloc(packs, sizeof *wadfiles); definitions = calloc(count, sizeof *definitions);
	check(wadfiles && definitions, "stock boundary metadata");
	for (unsigned w = 0; w < packs; w++)
	{
		wadfile_t *wad = wadfiles[w] = calloc(1, sizeof *wad);
		wad->numlumps = (UINT16)read_word(in); wad->lumpinfo = calloc(wad->numlumps, sizeof *wad->lumpinfo);
		check(wad && wad->lumpinfo, "stock metadata array");
		for (unsigned l = 0; l < wad->numlumps; l++)
		{
			lumpinfo_t *entry = &wad->lumpinfo[l]; char *name = read_string(in);
			strlcpy(entry->name, name, sizeof entry->name); free(name); entry->longname = read_string(in);
			entry->size = read_word(in); entry->good = read_word(in);
			for (unsigned j = 0; j < 2; j++)
			{
				UINT32 pair = read_word(in); entry->header[j * 2] = (INT16)pair; entry->header[j * 2 + 1] = (INT16)(pair >> 16);
			}
		}
	}
	begin(1, 0);
	for (unsigned g = 0; g < groups; g++)
	{
		UINT32 w = read_word(in), start = read_word(in), end = read_word(in), id = read_word(in);
		char *name = read_string(in);
		check(w < packs && id < count && start <= end && end <= wadfiles[w]->numlumps, "stock group bounds");
		R_AddSingleSpriteDef(name, &definitions[id], w, start, end, false);
		free(name);
	}
	check(fgetc(in) == EOF, "complete stock manifest consumed"); fclose(in);
	word(count);
	for (unsigned d = 0; d < count; d++)
	{
		printf("MEMORY8 stockdef=%u frames=%zu\n", d, definitions[d].numframes);
		word(d); word(definitions[d].numframes);
		if (definitions[d].numframes) stockroots++;
		stockframes += definitions[d].numframes;
		for (size_t f = 0; f < definitions[d].numframes; f++) snapshot(&definitions[d].spriteframes[f]);
	}
	for (size_t i = 0; i < numspritelumps; i++)
	{
		word((UINT16)spritecachedinfo[i].width_pixels); word((UINT16)spritecachedinfo[i].offset_pixels);
		word((UINT16)spritecachedinfo[i].topoffset_pixels); word((UINT16)spritecachedinfo[i].height_pixels);
	}
	ZA_Stats(&stats); consistent();
	printf("MEMORY8 case=%u stock=1 frames=%u roots=%u lumps=%zu cachecap=%zu used=%zu peak=%zu\n",
		cases++, stockframes, stockroots, numspritelumps, max_spritelumps, stats.used, stats.globalpeak);
	for (unsigned d = 0; d < count; d++) Z_Free(definitions[d].spriteframes);
	finish(); free(definitions);
	for (unsigned w = 0; w < packs; w++)
	{
		for (unsigned l = 0; l < wadfiles[w]->numlumps; l++) free(wadfiles[w]->lumpinfo[l].longname);
		free(wadfiles[w]->lumpinfo); free(wadfiles[w]);
	}
	free(wadfiles); wadfiles = NULL;
}

int main(int argc, char **argv)
{
	static const size_t sizes[] = {1,2,3,7,31,64,65,256,8192,65535};
#ifdef __mips__
	init_scr();
	check(ZA_HDR == 16, "EE arena header ABI");
#endif
	check(argc == 3, "usage stock-manifest snapshot");
	snapshot_file = fopen(argv[2], "wb"); check(snapshot_file != NULL, "snapshot open");
	printf("MEMORY8 BEGIN\n"); fflush(stdout);
	ship_sides = za_twosided; ship_prefer = za_prefer;
	for (int sides = 0; sides < 2; sides++)
	for (int guard = 0; guard < 2; guard++)
	{
		for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) table_case(sizes[i], sides, guard);
		for (unsigned mode = 0; mode < 6; mode++) synthetic_case(mode, sides, guard);
		rotation_case(sides, guard);
	}
	stock_case(argv[1]);
	memory_backing_check();
	check(fclose(snapshot_file) == 0, "snapshot close");
	printf("MEMORY8 DONE cases=%u checks=%u stockframes=%u stockroots=%u sizeof=%zu align=%zu\n",
		cases, checks, stockframes, stockroots, sizeof(spriteframe_t), _Alignof(spriteframe_t));
#ifndef __mips__
	if (arena_raw) { aligned_release(arena_raw); arena_raw = NULL; }
#endif
	return 0;
}
