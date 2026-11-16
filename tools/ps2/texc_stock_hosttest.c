/* Exercise production Fetch + complete SRP2 reader against the actual
 * compositor's TXCD stream. Allocation and key-selection are test boundaries. */
#define main ResourceRegressionMain
#include "resource_stream_opt17_hosttest.c"
#undef main

static INT32 numtextures, skytexture, tex_lumps[65536];
static size_t numsides;
static struct { INT32 toptexture, midtexture, bottomtexture; } *sides;
static struct
{
	FILE *handle;
	lumpinfo_t *lumps;
	UINT16 *tlump;
	UINT32 *toff;
	UINT8 *arena;
	size_t budget, pre_bytes;
	UINT32 pre_n, pre_cyc;
} PTC;
static int PrefetchLump(INT32 tex) { return tex >= 0 && tex < numtextures ? tex_lumps[tex] : -1; }
static boolean PS2TexC_Present(void) { return true; }
static boolean P_PS2_AnimRange(INT32 tex, INT32 *first, INT32 *last)
{
	(void)tex; (void)first; (void)last; return false; /* Every stock texture is marked already. */
}
void CONS_Printf(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void Z_ChangeTag(void *ptr, INT32 tag) { (void)ptr; (void)tag; }
#define TC PTC
#define TC_Lump PrefetchLump
#include RESOURCE_PREFETCH_SOURCE
#undef TC_Lump
#undef TC

static void budget_tests(FILE *handle, lumpinfo_t *lumps)
{
	static const size_t budgets[] = {0, 262144, 524288, 1048576, 2097152};
	unsigned b;
	INT32 tex;
	PTC.handle = handle; PTC.lumps = lumps;
	PTC.tlump = Z_Calloc((size_t)numtextures * sizeof *PTC.tlump, PU_STATIC, &PTC.tlump);
	PTC.toff = Z_Calloc((size_t)numtextures * sizeof *PTC.toff, PU_STATIC, &PTC.toff);
	numsides = numtextures; sides = calloc(numsides, sizeof *sides); REQUIRE(sides);
	for (tex = 0; tex < numtextures; ++tex)
	{ sides[tex].toptexture = tex; sides[tex].midtexture = sides[tex].bottomtexture = -1; }
	for (b = 0; b < sizeof budgets / sizeof *budgets; ++b)
	{
		size_t base;
		if (PTC.arena) Z_Free(PTC.arena);
		PTC.budget = budgets[b]; base = live; peak = live;
		PS2TexC_PrefetchLevel();
		REQUIRE(PTC.pre_bytes <= PTC.budget && live == base + PTC.pre_bytes);
		printf("TEXC_BUDGET budget=%zu resident=%zu peak_extra=%zu temporary=%zu composites=%u\n",
			PTC.budget, PTC.pre_bytes, peak-base, peak-base-PTC.pre_bytes, PTC.pre_n);
		for (tex = 0; tex < numtextures; ++tex) if (PTC.arena && PTC.toff[tex])
		{
			const lumpinfo_t *l = lumps + tex_lumps[tex];
			UINT8 *raw = malloc(l->disksize); REQUIRE(raw);
			REQUIRE(WPack_ReadRaw(handle, l, raw) == l->disksize);
			REQUIRE(!memcmp(raw, PTC.arena+PTC.toff[tex]-1, l->disksize)); free(raw);
		}
		/* The existing optional prefetch must leave a usable direct-read path
		 * when neither work allocation can be obtained. */
		deny = 1; PS2TexC_PrefetchLevel(); deny = 0;
		REQUIRE(!PTC.arena && !PTC.pre_bytes && live == base);
	}
	Z_Free(PTC.toff); Z_Free(PTC.tlump); free(sides); sides = NULL;
}

int main(int argc, char **argv)
{
	FILE *oracle;
	void *pool, *setup;
	lumpinfo_t *lumps;
	UINT16 count;
	boolean nonmusic;
	UINT32 magic, version, records = 0, max_stored = 0;
	UINT64 bytes = 0, key;
	size_t opened, max_extra = 0;
	REQUIRE(argc == 3);
	for (selected = 0; selected < 65536; ++selected) tex_lumps[selected] = -1;
	fp = fopen(argv[1], "rb"); oracle = fopen(argv[2], "rb"); REQUIRE(fp && oracle);
	REQUIRE(fread(&magic, 4, 1, oracle) == 1 && magic == 0x44435854u);
	REQUIRE(fread(&version, 4, 1, oracle) == 1 && version == 1);
	setup = WPack_SetupHandleEx(fp, argv[1]); REQUIRE(setup && WPack_Detect(fp));
	lumps = WPack_GetLumps(fp, argv[1], &count, &pool, &nonmusic, NULL);
	REQUIRE(lumps && count > 1 && nonmusic);
	TC.handle = fp; TC.lumps = lumps; TC.toff = tex_offsets;
	opened = live;
	while (fread(&key, 8, 1, oracle) == 1)
	{
		UINT32 texnum, size, i;
		UINT16 width, height;
		UINT8 *expected, *storage, *stored;
		char name[17];
		const lumpinfo_t *l;
		unsigned before;
		REQUIRE(fread(&texnum, 4, 1, oracle) == 1 && fread(&width, 2, 1, oracle) == 1
			&& fread(&height, 2, 1, oracle) == 1 && fread(&size, 4, 1, oracle) == 1);
		REQUIRE(size > 0 && size <= 1048576 && size == (UINT32)width * height);
		snprintf(name, sizeof name, "%016llx", (unsigned long long)key);
		for (selected = 1; selected < count && strcmp(lumps[selected].fullname, name); ++selected) {}
		REQUIRE(selected < count); l = lumps+selected; REQUIRE(l->size == size);
		REQUIRE(texnum < 65536); tex_lumps[texnum] = selected;
		if ((INT32)texnum >= numtextures) numtextures = texnum+1;
		expected = malloc(size); storage = malloc(size+128); stored = malloc(l->disksize);
		REQUIRE(expected && storage && stored && fread(expected, 1, size, oracle) == size);
		REQUIRE(WPack_ReadRaw(fp, l, stored) == l->disksize);
		/* Caller destination and resident cache exist already. The production
		 * reader/fetch may not allocate while delivering either variant. */
		peak = live; before = attempts; deny = 1; TC.arena = NULL;
		memset(storage, 0xA5, size+128);
		REQUIRE(PS2TexC_Fetch(0, storage+63, size));
		REQUIRE(!memcmp(storage+63, expected, size) && attempts == before && live == opened);
		if (peak-live > max_extra) max_extra = peak-live;
		for (i = 0; i < 63; ++i) REQUIRE(storage[i] == 0xA5);
		for (i = size+63; i < size+128; ++i) REQUIRE(storage[i] == 0xA5);
		TC.arena = stored; tex_offsets[0] = 1;
		memset(storage, 0xA5, size+128);
		REQUIRE(PS2TexC_Fetch(0, storage+63, size));
		REQUIRE(!memcmp(storage+63, expected, size) && attempts == before && live == opened);
		for (i = 0; i < 63; ++i) REQUIRE(storage[i] == 0xA5);
		for (i = size+63; i < size+128; ++i) REQUIRE(storage[i] == 0xA5);
		deny = 0; TC.arena = NULL;
		if (l->disksize > max_stored) { max_stored = l->disksize; skytexture = texnum; }
		++records; bytes += size;
		free(expected); free(storage); free(stored);
	}
	REQUIRE(feof(oracle) && !ferror(oracle));
	REQUIRE(TC.s_read == records && TC.s_resident == records && TC.s_hit == 2*records && !TC.s_fail && !TC.s_miss);
	budget_tests(fp, lumps);
	Z_Free(lumps); Z_Free(pool); tracked_free(setup); WPack_Shutdown(); REQUIRE(live == 0);
	fclose(fp); fclose(oracle);
	printf("STOCK TEXC PASS records=%u literal_bytes_each_path=%llu max_old_temporary=%u new_fetch_extra=%zu checks=%u live=%zu\n",
		records, (unsigned long long)bytes, max_stored, max_extra, checks, live);
	return 0;
}
