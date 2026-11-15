#include "next2_lookup_common.h"

UINT16 numwadfiles = 2;
static wadfile_t *files[2];
wadfile_t **wadfiles = files;
static size_t allocations;
static UINT16 folder_start, folder_end;
static unsigned checks;
static UINT32 rng = 0x26355;

void *Z_MallocAlign(size_t n, INT32 tag, void *user, INT32 align)
{
	void *p = malloc(n ? n : 1);
	(void)tag; (void)user; (void)align;
	if (!p) abort();
	allocations++;
	return p;
}
void *Z_CallocAlign(size_t n, INT32 tag, void *user, INT32 align)
{
	void *p = Z_MallocAlign(n, tag, user, align);
	memset(p, 0, n);
	return p;
}
#ifndef NEXT2_LOOKUP_EE
int strupr(char *s)
{
	for (; *s; s++) *s = (char)toupper((unsigned char)*s);
	return 0;
}
#if !defined(__GLIBC_PREREQ)
#define NEXT2_NEED_STRLCPY
#elif !__GLIBC_PREREQ(2, 38)
#define NEXT2_NEED_STRLCPY
#endif
#ifdef NEXT2_NEED_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t n)
{
	size_t len = strlen(src), copy = n && len >= n ? n - 1 : len;
	if (n) { memcpy(dst, src, copy); dst[copy] = 0; }
	return len;
}
#endif
#endif
UINT16 W_CheckNumForFolderStartPK3(const char *n, UINT16 w, UINT16 s)
{ (void)n; (void)w; (void)s; return folder_start; }
UINT16 W_CheckNumForFolderEndPK3(const char *n, UINT16 w, UINT16 s)
{ (void)n; (void)w; (void)s; return folder_end; }
UINT16 W_CheckNumForMarkerStartPwad(const char *n, UINT16 w, UINT16 s)
{ (void)n; (void)w; (void)s; return folder_start; }
UINT16 W_CheckNumForNamePwad(const char *n, UINT16 w, UINT16 s)
{ (void)n; (void)w; (void)s; return folder_end; }

static UINT32 random32(void)
{ rng ^= rng << 13; rng ^= rng >> 17; return rng ^= rng << 5; }
static void require(int yes, const char *what)
{
	if (!yes) { fprintf(stderr, "LOOKUP FAIL %s after %u checks\n", what, checks); exit(1); }
}

static void make_names(wadfile_t *wf, unsigned n, int collision)
{
	unsigned i;
	memset(wf, 0, sizeof *wf);
	wf->type = RET_PK3;
	wf->numlumps = (UINT16)n;
	wf->lumpinfo = calloc(n ? n : 1, sizeof *wf->lumpinfo);
	require(wf->lumpinfo != NULL, "names allocation");
	for (i = 0; i < n; i++)
	{
		char name[310];
		unsigned id = i % 11 == 10 ? i - 1 : i, length;
		if (collision) snprintf(name, sizeof name, "COLLIDE_%u_END", id);
		else snprintf(name, sizeof name, "%07u_EXTENDED_%u", id, id);
		if (i % 17 == 0) name[8] = 'e';
		if (i % 29 == 0) name[0] = 0;
		if (i % 19 == 0)
		{
			length = i % 3 == 0 ? 255 : i % 3 == 1 ? 256 : 300;
			memset(name + strlen(name), 'X', length - strlen(name));
			name[length] = 0;
		}
		wf->lumpinfo[i].longname = malloc(strlen(name) + 1);
		require(wf->lumpinfo[i].longname != NULL, "long name allocation");
		strcpy(wf->lumpinfo[i].longname, name);
		strlcpy(wf->lumpinfo[i].name, name, sizeof wf->lumpinfo[i].name);
		wf->lumpinfo[i].hash = quickncasehash(wf->lumpinfo[i].name, 8);
	}
}

static void free_names(wadfile_t *wf)
{
	unsigned i;
	for (i = 0; i < wf->numlumps; i++) free(wf->lumpinfo[i].longname);
	free(wf->lumpinfo);
}

static void lookup_cases(wadfile_t *wf, unsigned count, int indexed)
{
	unsigned i;
	wadfile_t a = *wf, b = *wf;
	if (indexed) { reference_index(&a); candidate_index(&b); }
	{
		const unsigned ids[] = {0, wf->numlumps / 2, wf->numlumps ? wf->numlumps - 1u : 0,
			32766, 32767, 32768, 65534};
		unsigned k, s;
		for (k = 0; k < sizeof ids / sizeof ids[0]; k++) if (ids[k] < wf->numlumps)
		for (s = 0; s < 3; s++)
		{
			const char *name = wf->lumpinfo[ids[k]].longname;
			UINT16 start = (UINT16)(s == 0 ? 0 : ids[k] + s - 1), ra, rb;
			size_t before = allocations;
			files[0] = &a; ra = reference_long(name, 0, start);
			files[0] = &b; rb = candidate_long(name, 0, start);
			require(ra == rb && allocations == before, "boundary/sentinel/start result");
			checks++;
		}
	}
	for (i = 0; i < count; i++)
	{
		char query[320];
		UINT16 start = (UINT16)(random32() & 65535), ra, rb;
		size_t before;
		unsigned id = wf->numlumps ? random32() % wf->numlumps : 0;
		if (wf->numlumps && i % 4) strcpy(query, wf->lumpinfo[id].longname);
		else snprintf(query, sizeof query, "MISSING_%u", i);
		if (i % 3 == 0) { unsigned k; for (k = 0; query[k]; k++) query[k] = (char)tolower((unsigned char)query[k]); }
		if (i % 5 == 0) start = 0;
		if (i % 5 == 1) start = (UINT16)id;
		files[0] = &a; before = allocations;
		ra = reference_long(query, 0, start);
		require(before == allocations, "reference long lookup allocated");
		files[0] = &b; before = allocations;
		rb = candidate_long(query, 0, start);
		require(before == allocations, "candidate long lookup allocated");
		require(ra == rb, "long result");
		// Check the original two-interval namespace semantics, including
		// empty, reversed, missing and sentinel ranges and cached metadata.
		a.type = b.type = i & 1 ? RET_PK3 : RET_WAD;
		a.flatsknown = b.flatsknown = i & 2 ? true : false;
		folder_start = i % 7 == 0 ? INT16_MAX : (UINT16)(random32() % (wf->numlumps + 1u));
		folder_end = i % 9 == 0 ? INT16_MAX : (UINT16)(random32() % (wf->numlumps + 1u));
		a.flatsstart = b.flatsstart = folder_start;
		a.flatsend = b.flatsend = folder_end;
		files[0] = &a; before = allocations;
		ra = reference_patch(query, 0, true);
		require(before == allocations, "reference long patch allocated");
		files[0] = &b; before = allocations;
		rb = candidate_patch(query, 0, true);
		require(before == allocations, "candidate long patch allocated");
		require(ra == rb && a.flatsknown == b.flatsknown && a.flatsstart == b.flatsstart && a.flatsend == b.flatsend, "patch result/range");
		checks += 2;
	}
	free(a.namebucket); free(a.namenext);
	free(b.namebucket); free(b.namenext);
}

static void cache_cases(void)
{
	unsigned i;
	next2_cache_state a, b;
	require(reference_cache_bytes() == 2560 && candidate_cache_bytes() == 2560, "cache storage changed");
	for (i = 0; i < 100000; i++)
	{
		char name[80], query[80];
		unsigned n = random32(), longname = n & 1, k;
		lumpnum_t number = random32(), ra, rb;
		snprintf(name, sizeof name, "N%06u_%u", n % 90, n % 90);
		if (i % 23 == 0) memset(name, 'L', 33), name[33] = 0;
		if (i % 31 == 0) memset(name, 'M', 31), name[31] = 0;
		if (i % 41 == 0) memset(name, 'T', 32), name[32] = 0;
		if (i % 61 == 0) name[0] = 0;
		if (i % 97 == 0) reference_cache_reset(), candidate_cache_reset();
		if (i % 3) reference_cache_add(number, name, longname), candidate_cache_add(number, name, longname);
		strcpy(query, name);
		if (i % 2) for (k = 0; query[k]; k++) query[k] = (char)tolower((unsigned char)query[k]);
		ra = reference_cache_check(query, longname);
		rb = candidate_cache_check(query, longname);
		require(ra == rb, "cache hit/miss");
		reference_cache_state(&a); candidate_cache_state(&b);
		require(!memcmp(&a, &b, sizeof a), "cache slots/cursor");
		checks++;
	}
}

static void real_names(const char *path)
{
	wadfile_t wf;
	UINT32 n, size, i;
	FILE *f = fopen(path, "rb");
	require(f && fread(&n, sizeof n, 1, f) == 1 && n <= UINT16_MAX, "asset names header");
	memset(&wf, 0, sizeof wf);
	wf.type = RET_PK3; wf.numlumps = (UINT16)n;
	wf.lumpinfo = calloc(n, sizeof *wf.lumpinfo);
	require(wf.lumpinfo != NULL, "asset index allocation");
	for (i = 0; i < n; i++)
	{
		require(fread(&size, sizeof size, 1, f) == 1 && size <= 65535, "asset name length");
		wf.lumpinfo[i].longname = calloc(size + 1, 1);
		require(wf.lumpinfo[i].longname && fread(wf.lumpinfo[i].longname, 1, size, f) == size, "asset name read");
		strlcpy(wf.lumpinfo[i].name, wf.lumpinfo[i].longname, sizeof wf.lumpinfo[i].name);
		wf.lumpinfo[i].hash = quickncasehash(wf.lumpinfo[i].name, 8);
	}
	fclose(f);
	lookup_cases(&wf, 4000, 0);
	lookup_cases(&wf, 4000, 1);
	free_names(&wf);
}

static UINT64 timer(void)
{
#ifdef NEXT2_LOOKUP_EE
	UINT32 value; __asm__ volatile("mfc0 %0,$9" : "=r"(value)); return value;
#else
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (UINT64)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

static void benchmark(void)
{
	wadfile_t wf;
	char cache_names[64][16];
	unsigned variant, rep, mode, i;
	volatile UINT32 sum = 0;
	UINT64 best[2];
#ifdef NEXT2_LOOKUP_EE
	const unsigned count = 128;
#else
	const unsigned count = 1024;
#endif
	make_names(&wf, 4096, 0);
	for (i = 0; i < 64; i++) snprintf(cache_names[i], sizeof cache_names[i], "CACHE%03u", i);
	files[0] = &wf;
	candidate_index(&wf);
	wf.flatsknown = true; wf.flatsstart = 1024; wf.flatsend = 2048;
	for (mode = 0; mode < 5; mode++)
	{
		best[0] = best[1] = ~(UINT64)0;
		for (rep = 0; rep < 5; rep++) for (variant = 0; variant < 2; variant++)
		{
			UINT64 start, elapsed;
			static UINT32 row_sum[2];
			UINT32 before_sum = sum;
			reference_cache_reset(); candidate_cache_reset();
			for (i = 0; i < 64; i++)
			{
				reference_cache_add(i, cache_names[i], false); candidate_cache_add(i, cache_names[i], false);
			}
			start = timer();
			for (i = 0; i < count; i++)
			{
				const char *name = wf.lumpinfo[3000 + i % 1000].longname;
				if (mode == 0) sum += variant ? candidate_long(name, 0, 0) : reference_long(name, 0, 0);
				else if (mode == 1) sum += variant ? candidate_long("NOTFOUND", 0, 0) : reference_long("NOTFOUND", 0, 0);
				else if (mode == 2) sum += variant ? candidate_patch(name, 0, true) : reference_patch(name, 0, true);
				else sum += variant ? candidate_cache_check(mode == 3 ? cache_names[i & 63] : "NOMATCH", false) : reference_cache_check(mode == 3 ? cache_names[i & 63] : "NOMATCH", false);
			}
			elapsed = (UINT32)(timer() - start);
			row_sum[variant] = sum - before_sum;
			if (variant) require(row_sum[0] == row_sum[1], "benchmark target results differ");
			if (elapsed < best[variant]) best[variant] = elapsed;
		}
		printf("LOOKUPBENCH mode=%u count=%u base=%llu candidate=%llu\n", mode, count, (unsigned long long)best[0], (unsigned long long)best[1]);
	}
	printf("LOOKUP DONE checksum=%u\n", (unsigned)sum);
	free(wf.namebucket); free(wf.namenext); free_names(&wf);
}

int main(int argc, char **argv)
{
#ifdef NEXT2_LOOKUP_EE
	(void)argc; (void)argv;
	benchmark();
#else
	static const unsigned counts[] = {0,1,2,31,64,1023,4096,32767,32768,65535};
	unsigned k, collision, indexed;
	if (argc > 1 && !strcmp(argv[1], "bench")) { benchmark(); return 0; }
	for (k = 0; k < sizeof counts / sizeof counts[0]; k++) for (collision = 0; collision < 2; collision++)
	{
		wadfile_t wf;
		make_names(&wf, counts[k], collision);
		for (indexed = 0; indexed < 2; indexed++) lookup_cases(&wf, counts[k] > 4096 ? 200 : 1000, indexed);
		free_names(&wf);
	}
	cache_cases();
	if (argc > 1) real_names(argv[1]);
	printf("LOOKUP PASS checks=%u cachebytes=%zu\n", checks, candidate_cache_bytes());
#endif
	return 0;
}
