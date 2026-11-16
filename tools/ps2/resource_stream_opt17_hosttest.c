/* Production SRP2 reader + extracted, unchanged production TEXC fetch.
 * The allocator always moves realloc and guards every engine allocation. */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static unsigned checks;
#define REQUIRE(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); exit(1); } } while (0)
static struct block { void *ptr, *raw; size_t size; void **owner; } blocks[128];
static size_t live, peak, maxrequest, limit = SIZE_MAX;
static unsigned attempts, reallocs, failat, deny, touches;
static void guardcheck(void)
{
	unsigned i, j;
	for (i = 0; i < 128; ++i) if (blocks[i].ptr)
		for (j = 0; j < 64; ++j)
			REQUIRE(((unsigned char *)blocks[i].raw)[j] == 0xA7
				&& ((unsigned char *)blocks[i].ptr)[blocks[i].size + j] == 0xA7);
}
static struct block *findblock(void *p)
{
	unsigned i;
	for (i = 0; i < 128; ++i) if (blocks[i].ptr == p) return blocks + i;
	return NULL;
}
static void *tracked_malloc(size_t n)
{
	unsigned i;
	void *raw;
	++attempts;
	if (n > maxrequest) maxrequest = n;
	if (deny || n > limit || attempts == failat) return NULL;
	REQUIRE(!posix_memalign(&raw, 64, n + 128));
	memset(raw, 0xA7, n + 128);
	for (i = 0; i < 128 && blocks[i].ptr; ++i) {}
	REQUIRE(i < 128);
	blocks[i] = (struct block){ (unsigned char *)raw + 64, raw, n, NULL };
	live += n;
	if (live > peak) peak = live;
	return blocks[i].ptr;
}
static void tracked_free(void *p)
{
	struct block *b;
	if (!p) return;
	guardcheck();
	b = findblock(p); REQUIRE(b != NULL);
	if (b->owner) *b->owner = NULL;
	live -= b->size;
	free(b->raw); memset(b, 0, sizeof *b);
}
static void *tracked_realloc(void *p, size_t n)
{
	struct block *old = p ? findblock(p) : NULL;
	void *q;
	++reallocs;
	REQUIRE(!p || old != NULL);
	q = tracked_malloc(n);
	if (q && p) { memcpy(q, p, old->size < n ? old->size : n); tracked_free(p); }
	return q;
}
static int no_sleep(const struct timespec *req, struct timespec *rem)
{
	(void)req; (void)rem; return 0;
}
#define malloc tracked_malloc
#define free tracked_free
#define realloc tracked_realloc
#define nanosleep no_sleep
#include RESOURCE_PACK_SOURCE
#undef malloc
#undef free
#undef realloc
#undef nanosleep

void *Z_MallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits)
{
	void *p = tracked_malloc(size);
	(void)tag; (void)alignbits;
	if (p && user) { findblock(p)->owner = user; *(void **)user = p; }
	return p;
}
void *Z_CallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits)
{
	void *p = Z_MallocAlign(size, tag, user, alignbits);
	if (p) memset(p, 0, size);
	return p;
}
void Z_Free(void *p) { tracked_free(p); }
void CONS_Alert(alerttype_t level, const char *fmt, ...) { (void)level; (void)fmt; }
void I_OutputMsg(const char *fmt, ...) { (void)fmt; }
void I_Error(const char *fmt, ...) { (void)fmt; REQUIRE(0); }

static struct
{
	FILE *handle;
	lumpinfo_t *lumps;
	UINT8 *arena;
	UINT32 *toff;
	UINT32 s_hit, s_miss, s_fail, s_resident, s_read, s_readcyc, s_cyc;
} TC;
static int selected;
static int TC_Lump(INT32 texnum) { return texnum < 0 ? -1 : selected; }
static UINT32 TC_Cyc(void) { return 0; }
#define Z_Touch(p) ((void)(p), (void)++touches)
#include RESOURCE_FETCH_SOURCE
#undef Z_Touch

struct fixture { lumpinfo_t lump; unsigned pattern; UINT32 crc; } fixtures[16];
static UINT32 fixture_count;
static FILE *fp;
static pkstream_t *stream;
static UINT32 tex_offsets[16];
static unsigned reports;
static char report_text[128];
static unsigned char source_byte(unsigned p, size_t i)
{
	if (p == 0) return (unsigned char)(i / 97 % 13);
	if (p == 1) return (unsigned char)(((i * 1103515245u) ^ (i >> 3) ^ (i * i * 12345u)) >> 17);
	return (i / WPACK_BLOCK) & 1 ? source_byte(1, i) : source_byte(0, i);
}
static void padfile(FILE *f)
{
	while (ftell(f) % WPACK_SECTOR) fputc(0, f);
}
static void addfixture(UINT32 size, int codec, unsigned pattern)
{
	struct fixture *f = fixtures + fixture_count++;
	UINT8 *plain = malloc(size ? size : 1), encoded[LZ4_COMPRESSBOUND(WPACK_BLOCK)];
	UINT32 b, nb = 1 + (size ? size - 1 : 0) / WPACK_BLOCK, index[128];
	long start, end;
	REQUIRE(fixture_count <= 16 && nb <= 128 && plain != NULL);
	for (b = 0; b < size; ++b) plain[b] = source_byte(pattern, b);
	padfile(fp); start = ftell(fp);
	memset(f, 0, sizeof *f);
	W_InitLumpPosition(&f->lump, start); f->lump.size = size; f->lump.compression = codec;
	f->pattern = pattern; f->crc = crc32(0, plain, size);
	if (codec == CM_NOCOMPRESSION) fwrite(plain, 1, size, fp);
	else
	{
		if (size > WPACK_BLOCK) { memset(index, 0, nb * 4); fwrite(index, 4, nb, fp); }
		for (b = 0; b < nb; ++b)
		{
			const UINT32 bs = min(WPACK_BLOCK, size - b * WPACK_BLOCK);
			const int compressed = LZ4_compress_default((char *)plain + b * WPACK_BLOCK, (char *)encoded, bs, sizeof encoded);
			REQUIRE(compressed > 0);
			if (size > WPACK_BLOCK && (UINT32)compressed >= bs)
			{ index[b] = bs | SRP2_RAWBLOCK; fwrite(plain + b * WPACK_BLOCK, 1, bs, fp); }
			else { index[b] = compressed; fwrite(encoded, 1, compressed, fp); }
		}
		if (size > WPACK_BLOCK)
		{ end = ftell(fp); fseek(fp, start, SEEK_SET); fwrite(index, 4, nb, fp); fseek(fp, end, SEEK_SET); }
	}
	f->lump.disksize = ftell(fp) - start;
	free(plain);
}
static void prepare(int medium)
{
	medium_forced = medium;
	stream = StreamFor(fp);
	if (!stream->buf) stream->buf = tracked_malloc(stream->pol->cap);
	REQUIRE(stream->buf && AllocBuffers());
	TC.handle = fp;
}
static void checkbytes(const UINT8 *storage, UINT32 size, unsigned pattern)
{
	UINT32 i;
	for (i = 0; i < size + 128; ++i)
		REQUIRE(storage[i] == (i >= 63 && i < size + 63 ? source_byte(pattern, i - 63) : 0xA5));
}
static void fetch_tests(void)
{
	UINT32 i;
	UINT32 max_disk = 0;
	unsigned mode, medium, noheap_success = 0;
	for (medium = 0; medium < WPK_MEDIA; ++medium)
	{
		prepare(medium);
		for (i = 0; i < fixture_count; ++i)
		{
			const lumpinfo_t *l = &fixtures[i].lump;
			UINT8 *storage, *stored;
			if (!l->size || l->size > 1048576) continue;
			storage = malloc(l->size + 128); stored = malloc(l->disksize);
			REQUIRE(storage && stored);
			fseek(fp, W_LumpPosition(l), SEEK_SET); REQUIRE(fread(stored, 1, l->disksize, fp) == l->disksize);
			TC.lumps = (lumpinfo_t *)l; selected = 0; TC.toff = tex_offsets;
			for (mode = 0; mode < 3; ++mode)
			{
				const unsigned oldattempts = attempts;
				const size_t oldlive = live;
				const unsigned oldhit = TC.s_hit, oldresident = TC.s_resident, oldread = TC.s_read;
				TC.arena = mode == 1 ? stored : NULL; tex_offsets[0] = 1;
				memset(storage, 0xA5, l->size + 128);
				deny = mode == 2;
				if (RESOURCE_BASELINE && mode == 2) REQUIRE(!PS2TexC_Fetch(0, storage + 63, l->size));
				else
				{
					REQUIRE(PS2TexC_Fetch(0, storage + 63, l->size)); checkbytes(storage, l->size, fixtures[i].pattern);
					REQUIRE(TC.s_hit == oldhit + 1 && TC.s_resident == oldresident + (mode == 1)
						&& TC.s_read == oldread + (mode != 1));
					if (mode == 2) ++noheap_success;
				}
				deny = 0; REQUIRE(live == oldlive);
				if (!RESOURCE_BASELINE || mode == 1) REQUIRE(attempts == oldattempts);
			}
			TC.arena = NULL;
			{
				const unsigned oldfail = TC.s_fail;
				memset(storage, 0xA5, l->size + 128);
				REQUIRE(!PS2TexC_Fetch(0, storage + 63, l->size - 1));
				REQUIRE(TC.s_fail == oldfail + 1);
				for (mode = 0; mode < l->size + 128; ++mode) REQUIRE(storage[mode] == 0xA5);
			}
			if (l->disksize > max_disk) max_disk = l->disksize;
			free(storage); free(stored);
		}
		WPack_Shutdown(); REQUIRE(live == 0);
	}
	REQUIRE(!PS2TexC_Fetch(-1, NULL, 0));
	printf("fetch max_stored=%u noheap_success=%u\n", max_disk, noheap_success);
}
static void verify_report(UINT32 lump, const char *what)
{
	(void)lump; ++reports; snprintf(report_text, sizeof report_text, "%s", what);
}
static wpack_t verify_context(lumpinfo_t *lumps)
{
	wpack_t pk = {0};
	wpack_sum_t sum = {0};
	UINT32 i, table[16];
	for (i = 0; i < fixture_count; ++i) { lumps[i] = fixtures[i].lump; table[i] = fixtures[i].crc; }
	fseek(fp, 0, SEEK_SET); REQUIRE(fwrite(table, 4, fixture_count, fp) == fixture_count); fflush(fp);
	SumAdd(&sum, table, fixture_count * 4);
	pk.numlumps = fixture_count; pk.crcoffset = 2048;
	fseek(fp, pk.crcoffset, SEEK_SET); REQUIRE(fwrite(table, 4, fixture_count, fp) == fixture_count); fflush(fp);
	pk.chkcrc = SumFold(&sum); stream->len = 0;
	return pk;
}
static void verify_tests(void)
{
	lumpinfo_t lumps[16];
	wpack_t pk;
	wpack_iostat_t before, after;
	size_t base, verify_peak, request;
	unsigned oldattempts;
	prepare(WPK_DVD); pk = verify_context(lumps);
	base = live; peak = live; maxrequest = 0; reports = 0;
	WPack_GetStats(&before);
	REQUIRE(WPack_Verify(&pk, fp, lumps, fixture_count, verify_report) == 0 && reports == 0);
	WPack_GetStats(&after);
	verify_peak = peak - base; request = maxrequest;
	REQUIRE(live == base);
	if (!RESOURCE_BASELINE) REQUIRE(request <= 262144 && verify_peak <= 262144 + fixture_count * 4 + 65 * 4);
	printf("verify max_request=%zu peak_extra=%zu cmds=%u hits=%u misses=%u bulk=%u bytes=%llu\n",
		request, verify_peak, after.cmds-before.cmds, after.hits-before.hits, after.misses-before.misses,
		after.bulk-before.bulk, (unsigned long long)(after.bytes-before.bytes));
	limit = 262144; reports = 0;
	REQUIRE((WPack_Verify(&pk, fp, lumps, fixture_count, verify_report) == 0) == !RESOURCE_BASELINE);
	REQUIRE(live == base); limit = SIZE_MAX;
	oldattempts = attempts; failat = oldattempts + 1; reports = 0;
	REQUIRE(WPack_Verify(&pk, fp, lumps, fixture_count, verify_report) == 1 && reports == 1 && live == base);
	failat = attempts + 2; reports = 0;
	REQUIRE(WPack_Verify(&pk, fp, lumps, fixture_count, verify_report) == 1 && reports == 1 && live == base);
	failat = 0;
	/* Table corruption must fail before decoding any lump. */
	fseek(fp, pk.crcoffset, SEEK_SET); fputc(1, fp); fflush(fp); stream->len = 0; reports = 0;
	REQUIRE(WPack_Verify(&pk, fp, lumps, fixture_count, verify_report) == 1 && reports == 1
		&& !strcmp(report_text, "the CRC table is damaged"));
	pk = verify_context(lumps);
	/* Change the final byte of a >256 KiB raw lump: the chained tail CRC matters. */
	fseek(fp, W_LumpPosition(lumps+4) + lumps[4].size - 1, SEEK_SET); fputc(source_byte(fixtures[4].pattern, lumps[4].size-1)^1, fp); fflush(fp);
	stream->len = 0; reports = 0;
	REQUIRE(WPack_Verify(&pk, fp, lumps, fixture_count, verify_report) == 1 && reports == 1 && !strcmp(report_text, "CRC32 mismatch"));
	fseek(fp, W_LumpPosition(lumps+4) + lumps[4].size - 1, SEEK_SET); fputc(source_byte(fixtures[4].pattern, lumps[4].size-1), fp); fflush(fp);
	stream->len = 0; reports = 0;
	REQUIRE(WPack_Verify(&pk, fp, lumps, fixture_count, verify_report) == 0 && reports == 0);
	{
		UINT32 saved, invalid = 0;
		const long position = W_LumpPosition(lumps+7);
		fseek(fp, position, SEEK_SET); REQUIRE(fread(&saved, 4, 1, fp) == 1);
		fseek(fp, position, SEEK_SET); REQUIRE(fwrite(&invalid, 4, 1, fp) == 1); fflush(fp); stream->len = 0; reports = 0;
		REQUIRE(WPack_Verify(&pk, fp, lumps, fixture_count, verify_report) == 1 && reports == 1
			&& !strcmp(report_text, "cannot be read or decoded"));
		fseek(fp, position, SEEK_SET); REQUIRE(fwrite(&saved, 4, 1, fp) == 1); fflush(fp); stream->len = 0;
	}
	WPack_Shutdown(); REQUIRE(live == 0);
}
static void damage_tests(void)
{
	const lumpinfo_t *l = &fixtures[3].lump;
	UINT8 *out = malloc(l->size+128);
	UINT32 bad_index = 0, old_index;
	long file_size;
	unsigned oldfail;
	REQUIRE(out);
	prepare(WPK_USB); TC.lumps = (lumpinfo_t *)l; selected = 0; TC.arena = NULL;
	stream->len = 0; WPack_InjectErrors(1, 4); oldfail = TC.s_fail;
	REQUIRE(!PS2TexC_Fetch(0, out+63, l->size) && TC.s_fail == oldfail+1 && WPack_LastError()[0]);
	WPack_InjectErrors(0, 0);
	file_size = stream->size;
	REQUIRE(!ftruncate(fileno(fp), W_LumpPosition(l)+l->size-1)); stream->len = 0;
	REQUIRE(!PS2TexC_Fetch(0, out+63, l->size));
	REQUIRE(!ftruncate(fileno(fp), file_size));
	free(out); WPack_Shutdown(); REQUIRE(live == 0);
	/* The fixture file was truncated, so use a separate blocked lump copy. */
	fclose(fp); fp = tmpfile(); REQUIRE(fp); fseek(fp, 4096, SEEK_SET); fixture_count = 0;
	addfixture(1048576, CM_LZ4, 2); addfixture(65536, CM_LZ4, 0); padfile(fp); fflush(fp); prepare(WPK_DVD);
	l = &fixtures[0].lump; out = malloc(l->size+128); REQUIRE(out);
	fseek(fp, W_LumpPosition(l), SEEK_SET); REQUIRE(fread(&old_index, 4, 1, fp) == 1);
	fseek(fp, W_LumpPosition(l), SEEK_SET); REQUIRE(fwrite(&bad_index, 4, 1, fp) == 1); fflush(fp); stream->len = 0;
	TC.lumps = (lumpinfo_t *)l; TC.arena = NULL; selected = 0;
	memset(out, 0xA5, l->size+128); REQUIRE(!PS2TexC_Fetch(0, out+63, l->size));
	for (oldfail = 0; oldfail < l->size+128; ++oldfail) REQUIRE(out[oldfail] == 0xA5);
	TC.arena = malloc(l->disksize); TC.toff[0] = 1; REQUIRE(TC.arena);
	fseek(fp, W_LumpPosition(l), SEEK_SET); REQUIRE(fread(TC.arena, 1, l->disksize, fp) == l->disksize);
	REQUIRE(!PS2TexC_Fetch(0, out+63, l->size)); free(TC.arena); TC.arena = NULL;
	/* Invalid LZ4 body, both as a disk read and as a resident stored composite. */
	l = &fixtures[1].lump; TC.lumps = (lumpinfo_t *)l;
	TC.arena = malloc(l->disksize); REQUIRE(TC.arena); memset(TC.arena, 0xFF, l->disksize);
	fseek(fp, W_LumpPosition(l), SEEK_SET); REQUIRE(fwrite(TC.arena, 1, l->disksize, fp) == l->disksize); fflush(fp); stream->len = 0;
	REQUIRE(!PS2TexC_Fetch(0, out+63, l->size));
	free(TC.arena); TC.arena = NULL; REQUIRE(!PS2TexC_Fetch(0, out+63, l->size));
	free(out); WPack_Shutdown(); REQUIRE(live == 0);
}
static void prefetch_tests(void)
{
	lumpinfo_t l = {0};
	unsigned i, oldreallocs;
	pf_item_t *old;
	if (RESOURCE_BASELINE) return;
	W_InitLumpPosition(&l, 4096); l.disksize = l.size = 128; l.compression = CM_NOCOMPRESSION;
	failat = attempts+1; WPack_PrefetchAdd(fp, &l); REQUIRE(!pf.items && !pf.n && !pf.cap); failat = 0;
	for (i = 0; i < 65536; ++i)
	{
		if (pf.n && pf.n == pf.cap)
		{
			old = pf.items; failat = attempts+1;
			WPack_PrefetchAdd(fp, &l); REQUIRE(pf.items == old && pf.n == pf.cap); failat = 0;
		}
		WPack_PrefetchAdd(fp, &l); REQUIRE(pf.n == i+1);
	}
	old = pf.items; oldreallocs = reallocs;
	WPack_PrefetchAdd(fp, &l); REQUIRE(reallocs == oldreallocs && pf.items == old && pf.n == 65536 && pf.cap == 65536);
	REQUIRE(findblock(pf.items) != NULL);
	WPack_PrefetchDrop(); REQUIRE(pf.items == old && pf.cap == 65536 && !pf.n);
	WPack_Shutdown(); REQUIRE(!pf.items && !pf.cap && !pf.n && !pf.blob && live == 0);
	prepare(WPK_DVD); WPack_PrefetchBegin(); WPack_PrefetchAdd(fp, &l);
	REQUIRE(WPack_PrefetchRun(4096, NULL) == 128 && pf.blob);
	old = pf.items; WPack_PrefetchDrop(); REQUIRE(pf.items == old && pf.cap == 1024 && !pf.blob && !pf.n);
	WPack_PrefetchAdd(fp, &l); REQUIRE(WPack_PrefetchRun(4096, NULL) == 128 && pf.blob);
	WPack_Shutdown(); REQUIRE(!pf.items && !pf.cap && !pf.n && !pf.blob && live == 0);
	printf("prefetch max_items=65536 shutdown_live=%zu\n", live);
}
int main(void)
{
	fp = tmpfile(); REQUIRE(fp); fseek(fp, 4096, SEEK_SET);
	addfixture(0, CM_NOCOMPRESSION, 0); addfixture(1, CM_NOCOMPRESSION, 0);
	addfixture(65536, CM_NOCOMPRESSION, 1); addfixture(1048576, CM_NOCOMPRESSION, 1);
	addfixture(4122587, CM_NOCOMPRESSION, 1); addfixture(17, CM_LZ4, 0);
	addfixture(65536, CM_LZ4, 0); addfixture(65537, CM_LZ4, 2);
	addfixture(262144, CM_LZ4, 2); addfixture(262145, CM_LZ4, 2);
	addfixture(1048576, CM_LZ4, 2); addfixture(4194305, CM_LZ4, 2);
	padfile(fp); fflush(fp);
	fetch_tests(); verify_tests(); damage_tests(); prefetch_tests();
	fclose(fp); guardcheck(); REQUIRE(live == 0);
	printf("PASS checks=%u live=%zu\n", checks, live);
	return 0;
}
