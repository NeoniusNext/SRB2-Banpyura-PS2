/* Production patch/composite/name-cache functions in the real PS2 zone.
 * Native pointer widths are recorded, never presented as EE measurements. */
#include <stdint.h>
typedef uint8_t UINT8;
static int netgame;
static int PS2Lua_InCall(void) { return 0; }
void PS2Spill_Reset(void) {}
#define max(a,b) ((a) > (b) ? (a) : (b))
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
#include <strings.h>
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef INT32 fixed_t;
typedef INT32 tic_t;
typedef UINT16 lumpnum_t;
typedef UINT32 pictureflags_t;
typedef INT32 pictureformat_t;
#define ROTSPRITE
#define ATTRPACK __attribute__((packed))
#define SHORT(x) (x)
#define LONG(x) (x)
#define PNG_HEADER_SIZE 8
#define TRANSPARENTPIXEL 255
enum { PICFMT_PATCH, PICFMT_FLAT, PICFLAGS_USE_TRANSPARENTPIXEL = 1 };
enum { CONS_WARNING };
#define DBG_SETUP 0
static void CONS_Alert(int type, const char *fmt, ...) { (void)type; (void)fmt; }
static tic_t leveltime;
#include RESOURCE_TYPES
static void Patch_CalcDataSizes(softwarepatch_t *, size_t *, size_t *);
static void Patch_MakeColumns(softwarepatch_t *, size_t, INT16, UINT8 *, column_t *, post_t *, boolean);
static void Patch_Free(patch_t *);
#include RESOURCE_PATCH

static UINT8 *resource;
static size_t resource_size;
static patch_t *cachedpatch;
static size_t texturememory;
static INT32 ASTTextureBlendingThreshold[2] = {0,128};
static size_t W_LumpLengthPwad(UINT16 wad, UINT16 lump)
{ (void)wad; (void)lump; return resource_size; }
static size_t W_ReadLumpHeaderPwad(UINT16 wad, UINT16 lump, void *dest, size_t bytes, size_t offset)
{
	(void)wad; (void)lump;
	check(offset <= resource_size, "resource offset");
	if (!bytes || bytes > resource_size - offset) bytes = resource_size - offset;
	memcpy(dest, resource + offset, bytes);
	return bytes;
}
static void *W_CacheLumpNumPwad(UINT16 wad, UINT16 lump, INT32 tag)
{
	void *p = Z_Malloc(resource_size, tag, NULL);
	W_ReadLumpHeaderPwad(wad, lump, p, 0, 0);
	return p;
}
static boolean Picture_IsLumpPNG(const UINT8 *p, size_t n)
{ return n >= 16 && !memcmp(p, "\211SRPIC\r\n", 8); }
static patch_t *W_GetCachedPatchNumPwad(UINT16 wad, UINT16 lump)
{ (void)wad; (void)lump; return cachedpatch; }
static patch_t *W_CachePatchNumPwad(UINT16 wad, UINT16 lump, INT32 tag)
{
	(void)wad; (void)lump; (void)tag;
	if (!cachedpatch)
		cachedpatch = Patch_CreateFromDoomPatch((softwarepatch_t *)(resource + (Picture_IsLumpPNG(resource, resource_size) ? 8 : 0)));
	return cachedpatch;
}
static void *Picture_PNGConvert(const UINT8 *p, pictureformat_t fmt, INT32 *w, INT32 *h,
	INT16 *top, INT16 *left, size_t bytes, size_t *outbytes, pictureflags_t flags)
{
	(void)fmt; (void)w; (void)h; (void)top; (void)left; (void)bytes; (void)outbytes; (void)flags;
	return Patch_CreateFromDoomPatch((softwarepatch_t *)(p + 8));
}
static void *Picture_Convert(pictureformat_t in, void *p, pictureformat_t out, size_t bytes,
	size_t *outbytes, INT32 w, INT32 h, INT32 left, INT32 top, pictureflags_t flags)
{
	(void)in; (void)p; (void)out; (void)bytes; (void)outbytes;
	(void)w; (void)h; (void)left; (void)top; (void)flags;
	check(0, "unexercised flat conversion"); return NULL;
}
static UINT8 ASTBlendPaletteIndexes(UINT8 background, UINT8 foreground, int style, UINT8 alpha)
{ return (UINT8)(background * 7u + foreground * 13u + (UINT8)style * 3u + alpha); }
static UINT32 quickncasehash(const char *s, size_t n)
{
	UINT32 hash = 5381;
	for (; n-- && *s; s++)
	{
		unsigned c = (unsigned char)*s;
		if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
		hash = hash * 33u ^ c;
	}
	return hash;
}
#include RESOURCE_RENDERER
#include RESOURCE_NAMECACHE

static uint64_t digest;
static void hash_word(uint64_t value)
{
	for (unsigned i = 0; i < 8; i++)
	{ digest ^= (UINT8)(value >> (8 * i)); digest *= UINT64_C(1099511628211); }
}
static void hash_data(const UINT8 *p, size_t bytes)
{ while (bytes--) { digest ^= *p++; digest *= UINT64_C(1099511628211); } }

static void verify_patch(patch_t *patch, softwarepatch_t *source)
{
	UINT8 *begin, *end;
	size_t allposts = 0, allpixels = 0, previous;
	check(patch != NULL, "patch allocation");
	begin = (UINT8 *)patch; end = begin + ZA_BLOCK(patch)->realsize;
	check(patch->width == source->width && patch->height == source->height
		&& patch->leftoffset == source->leftoffset && patch->topoffset == source->topoffset, "every patch header field exact");
	check((uintptr_t)patch->columns % _Alignof(column_t) == 0 && (uintptr_t)patch->posts % _Alignof(post_t) == 0,
		"natural directory alignment");
	check((uintptr_t)patch->pixels % 16 == 0, "pixel base keeps preceding bulk-copy alignment");
	check(patch->embedded == 1 && !patch->hardware && !patch->rotated, "embedded flags and optional owners initialized");
	for (unsigned i = 0; i < 4; i++) check(!patch->flats[i], "flat owners initialized");
	hash_word((UINT16)patch->width); hash_word((UINT16)patch->height);
	hash_word((UINT16)patch->leftoffset); hash_word((UINT16)patch->topoffset);
	for (INT32 x = 0; x < patch->width; x++)
	{
		column_t *column = &patch->columns[x];
		UINT8 *post = (UINT8 *)source + source->columnofs[x];
		size_t prev = 0, offset = 0;
		unsigned n = 0;
		check((UINT8 *)(column + 1) <= end, "column inside root");
		for (; *post != 255; post += post[1] + 4, n++)
		{
			size_t top = post[0];
			post_t *actual = &column->posts[n];
			if (top <= prev) top += prev;
			prev = top;
			check(n < column->num_posts && (UINT8 *)(actual + 1) <= end, "post inside root and declared count");
			check(actual->topdelta == top && actual->length == post[1] && actual->data_offset == offset, "every decoded post field exact");
			check(column->pixels + offset >= begin && column->pixels + offset + post[1] <= end, "pixel run inside root");
			check(!memcmp(column->pixels + offset, post + 3, post[1]), "every decoded patch pixel exact");
			hash_word(top); hash_word(post[1]); hash_word(offset); hash_data(column->pixels + offset, post[1]);
			offset += post[1];
			allposts++; allpixels += post[1];
		}
		check(n == column->num_posts, "post count exact including empty columns");
		hash_word(n);
	}
	previous = (sizeof(patch_t) + 15u) & ~(size_t)15u;
	previous = (previous + sizeof(column_t) * patch->width + 15u) & ~(size_t)15u;
	previous = ((previous + sizeof(post_t) * allposts + 15u) & ~(size_t)15u) + allpixels;
	check(ZA_BLOCK(patch)->realsize <= previous, "each patch fits within preceding single-allocation request");
}

static void patch_corpus(const char *path)
{
	FILE *f = fopen(path, "rb");
	UINT32 bytes;
	size_t count = 0, retained = 0, requested = 0;
	check(f != NULL, "open patch corpus");
	arena_reset(64u << 20, ship_sides, ship_prefer, 1);
	digest = UINT64_C(14695981039346656037);
	while (fread(&bytes, sizeof bytes, 1, f) == 1)
	{
		softwarepatch_t *source = malloc(bytes);
		patch_t *patch;
		check(source != NULL && bytes >= 8 && fread(source, 1, bytes, f) == bytes, "read validated source patch");
		patch = Patch_CreateFromDoomPatch(source);
		verify_patch(patch, source);
		retained += blk_bytes(patch); requested += ZA_BLOCK(patch)->realsize;
		Patch_Free(patch);
		check(!Z_TotalUsage(), "embedded patch releases all memory");
		patch = Patch_TryCreateFromDoomPatch(source);
		verify_patch(patch, source);
		Patch_Free(patch);
		free(source); count++;
	}
	check(feof(f), "complete patch corpus"); fclose(f);
	consistent();
	printf("patches=%zu hash=%llu requested=%zu blocks=%zu\n", count, (unsigned long long)digest, requested, retained);
}

static void composite_case(unsigned width, unsigned height, unsigned mode, int cooked, int cached, int twosided)
{
	UINT8 raw[131072];
	size_t base = cooked ? 8 : 0, off = base + 8 + 4 * width;
	INT16 sw = width, sh = height;
	texture_t *texture = calloc(1, sizeof(*texture) + 3 * sizeof(texpatch_t));
	texture_t *table[2]; UINT8 *cache[2] = {0}; column_t *columns[2] = {0}; INT32 widths[2];
	zastats_t stats;
	check(texture != NULL, "composite descriptor");
	memset(raw, 0, sizeof raw);
	if (cooked) memcpy(raw, "\211SRPIC\r\n", 8);
	memcpy(raw + base, &sw, 2); memcpy(raw + base + 2, &sh, 2);
	for (unsigned x = 0; x < width; x++)
	{
		UINT32 column = off - base;
		memcpy(raw + base + 8 + 4 * x, &column, 4);
		for (unsigned y = (mode & 1) ? x % 3 : 0; y < height;)
		{
			unsigned length = (mode & 1) ? 1 + (x + y) % 7 : height;
			if ((mode & 2) && !(x % 5)) break;
			if (length > height - y) length = height - y;
			raw[off++] = y; raw[off++] = length; raw[off++] = 0;
			for (unsigned p = 0; p < length; p++) raw[off++] = (UINT8)(x * 17 + y + p);
			raw[off++] = 0; y += length + 1 + x % 3;
		}
		raw[off++] = 255;
	}
	resource = raw; resource_size = off; cachedpatch = NULL;
	arena_reset(4u << 20, twosided, ship_prefer, 1);
	if (cached) cachedpatch = Patch_CreateFromDoomPatch((softwarepatch_t *)(raw + base));
	texture->type = TEXTURETYPE_COMPOSITE; texture->width = width + (mode % 2); texture->height = height + 1;
	texture->patchcount = mode == 4 ? 0 : 3;
	for (INT32 i = 0; i < texture->patchcount; i++)
	{
		texpatch_t *patch = &texture->patches[i];
		patch->originx = i - 1; patch->originy = i - 1;
		patch->flip = (i + mode) % 4; patch->style = i ? AST_TRANSLUCENT : AST_COPY; patch->alpha = 128;
	}
	table[0] = table[1] = texture; textures = table; texturecache = cache; texturecolumns = columns;
	widths[1] = texture->width; texturewidth = widths; numtextures = 2;
	digest = UINT64_C(14695981039346656037);
	R_GenerateTexture(1); ZA_Stats(&stats);
	for (unsigned pass = 0; pass < 2; pass++)
	{
		hash_data(cache[1], (size_t)texture->width * texture->height);
		for (INT32 x = 0; x < texture->width; x++)
		{
			column_t *column = &columns[1][x];
			check(column->pixels == cache[1] + (size_t)texture->height * x, "composite pixel pointer exact");
			hash_word(column->num_posts);
			for (unsigned p = 0; p < column->num_posts; p++)
			{
				post_t *post = &column->posts[p];
				check(post->topdelta + post->length <= (unsigned)texture->height, "composite post stays inside texture");
				hash_word(post->topdelta); hash_word(post->length); hash_word(post->data_offset);
			}
		}
		if (!pass)
		{
			R_ReleaseTextureCache(1);
			check(Z_EvictLRU(SIZE_MAX, false) && !cache[1], "cache owner clears on eviction");
			R_CheckTextureCache(1); check(cache[1] != NULL, "cache reconstructs after eviction");
		}
	}
	printf("composite=%u,%u,%u,%d,%d,%d hash=%llu peak=%zu allocs=%zu retained=%zu\n",
		width, height, mode, cooked, cached, twosided, (unsigned long long)digest,
		stats.globalpeak, stats.allocs, Z_TotalUsage());
	Z_Free(cache[1]); check(!cache[1], "composite owner clears on free");
	if (cachedpatch) Patch_Free(cachedpatch);
	cachedpatch = NULL; free(texture); check(!Z_TotalUsage(), "no scratch allocation retained"); consistent();
}

static void name_cache(void)
{
	const unsigned count = 1200;
	texture_t **table = calloc(count, sizeof(*table));
	check(table != NULL, "name descriptors");
	arena_reset(4u << 20, ship_sides, ship_prefer, 1);
	R_ClearTextureNumCache(false);
	for (unsigned i = 0; i < count; i++)
	{
		char name[9];
		table[i] = calloc(1, sizeof(**table)); check(table[i] != NULL, "name descriptor");
		snprintf(name, sizeof name, "T%07u", i); memcpy(table[i]->name, name, 8);
		table[i]->hash = quickncasehash(table[i]->name, 8);
		table[i]->type = (i & 1) ? TEXTURETYPE_FLAT : TEXTURETYPE_COMPOSITE;
	}
	textures = table; numtextures = count;
	digest = UINT64_C(14695981039346656037);
	for (unsigned pass = 0; pass < 2; pass++)
		for (unsigned i = 0; i < count; i++)
		{
			char name[9]; INT32 id;
			memcpy(name, table[i]->name, 8); name[8] = 0;
			if (pass) name[0] = 't';
			id = R_CheckTextureNumForName(name); check(id == (INT32)i, "name cache exact ID/case"); hash_word(id);
			id = R_CheckFlatNumForName(name); check(id == ((i & 1) ? (INT32)i : -1), "flat namespace exact"); hash_word(id);
		}
	check(R_CheckTextureNumForName("-") == 0 && R_CheckFlatNumForName("-") == 0, "no-texture marker");
	check(R_CheckTextureNumForName("MISSING") == -1, "absent texture");
	check(tidcachelen == (INT32)count, "namespace hits do not append duplicate entries");
	printf("namecache=%u hash=%llu stride=%zu retained=%zu\n", count, (unsigned long long)digest, sizeof(*tidcache), Z_TotalUsage());
	R_ClearTextureNumCache(true); check(!tidcache && !tidcachelen && !Z_TotalUsage(), "cache clear releases owned storage");
	/* An add-on overrides an earlier name, after the engine's existing clear. */
	memcpy(table[count - 1]->name, table[0]->name, 8); table[count - 1]->hash = table[0]->hash;
	check(R_CheckTextureNumForName("T0000000") == (INT32)count - 1, "add-on takes last matching ID after invalidation");
	R_ClearTextureNumCache(false);
	for (unsigned i = 0; i < count; i++) free(table[i]);
	free(table); consistent();
}

static void patch_lifecycle(void)
{
	UINT8 data[64] = {1,0,32,0,0,0,0,0,12,0,0,0,0,32,0};
	patch_t *patch;
	for (unsigned i = 0; i < 32; i++) data[15 + i] = i;
	data[48] = 255;
	arena_reset(4096, ship_sides, ship_prefer, 1);
	check(Patch_TryCreateFromDoomPatch(NULL) == NULL, "null try source");
	patch = Patch_CreateFromDoomPatch(NULL);
	check(!patch->width && !patch->height && !patch->embedded, "null source retains empty ordinary patch behavior");
	Patch_Free(patch);
	patch = Patch_CreateFromDoomPatch((softwarepatch_t *)data);
	patch->flats[0] = Z_Malloc(16, PU_STATIC, NULL);
	patch->flats[3] = Z_Malloc(16, PU_STATIC, NULL);
	patch->rotated = Z_Calloc(sizeof(rotsprite_t), PU_STATIC, NULL);
	patch->rotated->angles = 2;
	patch->rotated->patches = Z_Calloc(2 * sizeof(void *), PU_STATIC, NULL);
	for (unsigned i = 0; i < 2; i++)
		patch->rotated->patches[i] = Patch_CreateFromDoomPatch((softwarepatch_t *)data);
	Patch_Free(patch);
	check(!Z_TotalUsage(), "embedded patch releases attached flats and rotated children"); consistent();
	arena_reset(256, ship_sides, ship_prefer, 0);
	{
		void *guard = Z_MallocAlign(64, PU_STATIC, NULL, 4);
		size_t used = Z_TotalUsage();
		check(Patch_TryCreateFromDoomPatch((softwarepatch_t *)data) == NULL && Z_TotalUsage() == used, "failed try allocation leaves no block");
		Z_Free(guard);
	}
	consistent();
}

int main(int argc, char **argv)
{
	static const unsigned sizes[][2] = {{1,1},{3,5},{7,9},{8,8},{17,33},{65,129},{127,127}};
	check(argc == 2, "patch corpus argument");
	ship_sides = za_twosided; ship_prefer = za_prefer;
	patch_corpus(argv[1]);
	patch_lifecycle();
	for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++)
		for (unsigned mode = 0; mode < 6; mode++)
			for (int cooked = 0; cooked < 2; cooked++)
				for (int cached = 0; cached < 2; cached++)
					for (int sides = 0; sides < 2; sides++)
						composite_case(sizes[i][0], sizes[i][1], mode, cooked, cached, sides);
	name_cache();
	printf("PASS resource semantics hostbits=%zu checks=%u\n", 8 * sizeof(void *), checks);
	aligned_release(arena_raw);
	return 0;
}
