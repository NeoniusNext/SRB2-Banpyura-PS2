/* Actual converter in the real PS2 zone; generation/column lookup are fixtures. */
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
typedef uint8_t UINT8;
typedef struct { unsigned topdelta, length; size_t data_offset; } post_t;
typedef struct { unsigned num_posts; post_t *posts; UINT8 *pixels; } column_t;
typedef struct { INT32 width, height; } texture_t;
#define TRANSPARENTPIXEL 255
static texture_t texture_fixture;
static texture_t *textures[] = {&texture_fixture};
static INT32 numtextures = 1;
static UINT8 *texturecache, *output_live;
static post_t posts[3];
static column_t column;
static unsigned checks_before_alloc, generations, evictions;

static UINT8 pattern(size_t x, size_t y)
{
	return (x + y) % 9 == 0 ? TRANSPARENTPIXEL : (UINT8)(x * 73 + y * 19 + 5);
}

static void R_CheckTextureCache(size_t texnum)
{
	size_t x, y, size = (size_t)texture_fixture.width * texture_fixture.height;
	(void)texnum;
	checks_before_alloc++;
	if (output_live)
		check(ZA_TAG(ZA_BLOCK(output_live)) == PU_RENDERWORK, "reserved output pinned across generation");
	if (!texturecache)
	{
		void *scratch = Z_Malloc(4096, PU_RENDERWORK, NULL);
		texturecache = Z_Malloc(size, PU_RENDERWORK, &texturecache);
		for (x = 0; x < (size_t)texture_fixture.width; x++)
		for (y = 0; y < (size_t)texture_fixture.height; y++)
			texturecache[x * texture_fixture.height + y] = pattern(x, y);
		Z_Free(scratch);
		Z_ChangeTag(texturecache, PU_CACHE);
		generations++;
	}
	else
		Z_Touch(texturecache);
}

static column_t *R_GetColumn(size_t texnum, size_t col)
{
	unsigned height = (unsigned)texture_fixture.height;
	check(output_live != NULL, "output survives every column lookup");
	if (!texturecache) R_CheckTextureCache(texnum);
	column.num_posts = (col % 3 == 0) ? 0 : 3;
	column.pixels = texturecache + col * height;
	posts[0].topdelta = 0; posts[0].length = height / 2; posts[0].data_offset = 0;
	posts[1].topdelta = height / 3; posts[1].length = height / 2; posts[1].data_offset = 0;
	posts[2].topdelta = height - 1; posts[2].length = 5; posts[2].data_offset = 0;
	column.posts = posts;
	return &column;
}

static void *flat_alloc(size_t size, INT32 tag, void **owner)
{
	/* Force a legal old-root eviction before acquiring any source alias. */
	if (!checks_before_alloc && texturecache)
	{
		Z_Free(texturecache);
		evictions++;
	}
	output_live = Z_Malloc(size, tag, owner);
	return output_live;
}
#undef Z_Malloc
#define Z_Malloc(size, tag, owner) flat_alloc(size, tag, owner)
#include FLAT_SOURCE
#undef Z_Malloc
#define Z_Malloc(size, tag, owner) Z_MallocAlign(size, tag, owner, 0)

int main(int argc, char **argv)
{
	static const INT32 widths[] = {1, 7, 64, 320, 512};
	static const INT32 heights[] = {1, 3, 24, 100, 1024};
	unsigned w, h, warm, cases = 0;
	uint64_t hash = 1469598103934665603ull;
	(void)argv;
	for (w = 0; w < sizeof widths / sizeof widths[0]; w++)
	for (h = 0; h < sizeof heights / sizeof heights[0]; h++)
	for (warm = 0; warm < 2; warm++)
	{
		UINT8 *converted;
		size_t size, i;
		arena_reset(3u << 20, 1, 0, 1);
		texture_fixture.width = widths[w]; texture_fixture.height = heights[h];
		texturecache = NULL; output_live = NULL;
		checks_before_alloc = generations = 0;
		if (warm) { R_CheckTextureCache(0); Z_ReleaseCache(texturecache); }
		checks_before_alloc = 0;
		size = (size_t)widths[w] * heights[h];
		converted = Picture_TextureToFlat(0);
		check(converted == output_live, "independent output remains allocated");
		check(texturecache && converted != texturecache, "column root and output never alias");
		check(checks_before_alloc == 1, "one cache acquisition");
		for (i = 0; i < size; i++) { hash ^= converted[i]; hash *= 1099511628211ull; }
		if (argc > 1) { converted[0] ^= 1; check(converted[0] == TRANSPARENTPIXEL, "negative output corruption"); }
		consistent();
		Z_Free(converted); output_live = NULL; Z_Free(texturecache);
		check(texturecache == NULL, "source owner clears independently");
		empty_zone();
		cases++;
	}
	printf("FLAT PASS cases=%u hash=%llu evictions=%u checks=%u\n", cases, (unsigned long long)hash, evictions, checks);
	return 0;
}
