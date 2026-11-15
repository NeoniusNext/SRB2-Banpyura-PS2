/* Run the actual metadata constructors/readers/free path with the actual arena.
 * Only unrelated engine/platform services and cooked-pack I/O are boundary stubs;
 * the separate pack corpus test executes the complete production pack reader. */
#define _GNU_SOURCE 1
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef int16_t INT16;
static int netgame;
static int PS2Lua_InCall(void) { return 0; }
void PS2Spill_Reset(void) {}
#define max(a,b) ((a) > (b) ? (a) : (b))
#define main zone_regression_main
#include "zone_hosttest.c"
#undef main
#undef PATHSEP
#define PATHSEP "/"
#define ATTRPACK __attribute__((packed))
#define LONG(x) (x)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define HAVE_ZLIB 1
#define PS2_FULLLOADER 1
#define ZWAD 1
#define CONS_ERROR 0
#define CONS_WARNING 1
#define CONS_Alert(level, ...) CONS_Printf(__VA_ARGS__)
#define strnicmp strncasecmp
#define stricmp strcasecmp
typedef void aatree_t;
typedef void wpack_t;
#include LL_TYPES
#ifndef W_LUMPLOCATION_ACCESSORS
#define W_LumpPosition(l) ((l)->position)
#define W_LumpDiskPath(l) ((l)->diskpath)
#define W_InitLumpPosition(l,p) ((l)->position = (p))
#define W_SetLumpPosition(l,p) ((l)->position = (p))
#define W_SetLumpDiskPath(l,p) ((l)->diskpath = (p))
#endif
#include LL_WADTYPE

static size_t fixture_strlcpy(char *dest, const char *src, size_t capacity)
{
	size_t n = strlen(src);
	if (capacity) { size_t copy = n < capacity - 1 ? n : capacity - 1; memcpy(dest, src, copy); dest[copy] = 0; }
	return n;
}
#define strlcpy fixture_strlcpy
static char *strupr(char *s) { char *p = s; for (; *p; ++p) *p = toupper((unsigned char)*p); return s; }
static const char *M_FileError(FILE *f) { (void)f; return "fixture I/O error"; }
static int direrror;
static INT32 pathisdirectory(const char *path)
{
	struct stat st;
	if (stat(path, &st)) { direrror = errno; return -1; }
	return !!S_ISDIR(st.st_mode);
}
static void M_AATreeFree(void *tree) { check(!tree, "unused folder tree remains null"); }
static void WPack_Close(void *pack) { check(!pack, "cooked pack context handled in separate corpus test"); }
static void WPack_Shutdown(void) {}
static size_t WPack_ReadLumpN(wpack_t *pack, FILE *handle, UINT32 id,
	const lumpinfo_t *l, void *dest, size_t size, size_t offset)
{
	(void)pack; (void)handle; (void)id; (void)l; (void)dest; (void)size; (void)offset;
	check(0, "generic fixture never substitutes cooked pack reads"); return 0;
}
static const char *WPack_LastError(void) { return "fixture pack stub"; }
static int w_softreads;
static UINT16 numwadfiles;
static wadfile_t **wadfiles;
static struct { UINT16 wad, lump; size_t size; UINT8 *data; } inflcache;

static unsigned alloc_call;
static void *fixture_malloc(size_t size, int tag, void *user)
{
	++alloc_call;
	return Z_MallocAlign(size, tag, user, 0);
}
static void *fixture_calloc(size_t size, int tag, void *user)
{
	return memset(fixture_malloc(size, tag, user), 0, size);
}
/* An I_Error normally exits the process. The fault fixture catches it, so it
 * releases libc temporaries that the original fatal path never unwinds. */
static void *libc_blocks[256];
static size_t libc_count;
static void *fixture_libc_malloc(size_t size)
{
	void *p = malloc(size);
	check(libc_count < sizeof libc_blocks / sizeof *libc_blocks, "libc temporary tracker bound");
	if (p) libc_blocks[libc_count++] = p;
	return p;
}
static void fixture_libc_free(void *p)
{
	size_t i;
	if (!p) return;
	for (i = 0; i < libc_count; ++i) if (libc_blocks[i] == p) break;
	check(i < libc_count, "actual constructor frees the same libc temporary");
	libc_blocks[i] = libc_blocks[--libc_count]; free(p);
}
#undef Z_Malloc
#undef Z_Calloc
#define Z_Malloc(s,t,u) fixture_malloc(s,t,u)
#define Z_Calloc(s,t,u) fixture_calloc(s,t,u)
#include "../../src/lzf.h"
#define malloc fixture_libc_malloc
#define free fixture_libc_free
#include LL_PRODUCTION
#undef malloc
#undef free
#include "../../src/lzf.c"

static uint64_t digest = UINT64_C(1469598103934665603);
static void hash_bytes(const void *data, size_t n)
{
	const UINT8 *p = data;
	while (n--) digest = (digest ^ *p++) * UINT64_C(1099511628211);
}
static void hash_entry(const lumpinfo_t *l, int path_mode)
{
	uint64_t fields[] = { W_LumpPosition(l), l->disksize, l->size, l->hash, l->compression,
		W_LumpDiskPath(l) != NULL };
	hash_bytes(fields, sizeof fields); hash_bytes(l->name, sizeof l->name);
	if (l->longname) hash_bytes(l->longname, strlen(l->longname) + 1);
	if (l->fullname) hash_bytes(l->fullname, strlen(l->fullname) + 1);
	if (path_mode && W_LumpDiskPath(l))
	{
		/* Root paths vary between output directories; the stored pointer and
		 * complete path are checked separately against the independent oracle. */
		hash_bytes(l->fullname, strlen(l->fullname) + 1);
	}
}

static void metadata_case(size_t n, int guard, int sides, int fragmented)
{
	lumpinfo_t *root = NULL, *entries;
	char longname[] = "LongName", fullname[] = "Folder/LongName.ext";
	char firstpath[] = "mass:/Addon/LongName.ext", secondpath[] = "mc0:/Addon/Other.bin";
	UINT8 *neighbor;
	void *holes[16];
	zastats_t before, during, after;
	size_t i, code;
	static const unsigned long positions[] = { 0, 1, 65535UL, 0x7FFFFFFFUL, 0x80000000UL, 0xFFFFFFFFUL, ULONG_MAX };
	arena_reset(6u << 20, sides, ship_prefer, guard);
	if (fragmented)
	{
		for (i = 0; i < 16; ++i) holes[i] = Z_Malloc(32 + i * 31, PU_STATIC, NULL);
		for (i = 0; i < 16; i += 2) { Z_Free(holes[i]); holes[i] = NULL; }
	}
	ZA_Stats(&before);
	entries = Z_Malloc(n * sizeof *entries, PU_STATIC, &root);
	neighbor = Z_Malloc(129, PU_STATIC, NULL); memset(neighbor, 0x69, 129);
	check(root == entries, "directory root owner published by unchanged allocator");
	check((uintptr_t)&entries[0].hash % _Alignof(UINT32) == 0
		&& (uintptr_t)&entries[0].longname % _Alignof(char *) == 0
		&& (uintptr_t)&entries[0].size % _Alignof(size_t) == 0,
		"all full-width fields retain natural alignment");
	for (i = 0; i < n; ++i)
	{
		lumpinfo_t *l = entries + i;
		memset(l, 0xA7, sizeof *l);
		W_InitLumpPosition(l, i % 3 ? positions[i % 7] : 0);
		W_SetLumpDiskPath(l, NULL);
		if (!(i % 3)) W_SetLumpDiskPath(l, firstpath);
		l->disksize = ULONG_MAX - i; l->size = SIZE_MAX - i;
		memcpy(l->name, "AbCd1234", 9); l->hash = UINT32_MAX - (UINT32)i;
		l->longname = longname; l->fullname = fullname;
	}
	for (code = CM_NOCOMPRESSION; code <= CM_UNSUPPORTED; ++code)
	for (i = 0; i < n; ++i)
	{
		lumpinfo_t *l = entries + i, copy;
		unsigned long expected = i % 3 ? positions[i % 7] : 0;
		l->compression = code;
		if (!(i % 3))
		{
			W_SetLumpPosition(l, 0);
			check(W_LumpPosition(l) == 0 && W_LumpDiskPath(l) == firstpath,
				"redundant folder zero store preserves exact pointer");
			W_SetLumpDiskPath(l, secondpath);
			check(W_LumpPosition(l) == 0 && W_LumpDiskPath(l) == secondpath,
				"folder path replacement preserves position zero");
			W_SetLumpDiskPath(l, NULL);
			check(W_LumpPosition(l) == 0 && !W_LumpDiskPath(l), "clearing path keeps both conceptual zeros");
			W_SetLumpDiskPath(l, firstpath);
		}
		else
		{
			W_SetLumpDiskPath(l, NULL);
			check(W_LumpPosition(l) == expected && !W_LumpDiskPath(l), "archive null store preserves all offset bits");
			W_SetLumpPosition(l, expected ^ ULONG_MAX); W_SetLumpPosition(l, expected);
		}
		memcpy(&copy, l, sizeof copy);
		check(W_LumpPosition(&copy) == expected
			&& W_LumpDiskPath(&copy) == (!(i % 3) ? firstpath : NULL), "full record copies retain location tag and data");
		check(l->disksize == ULONG_MAX - i && l->size == SIZE_MAX - i
			&& l->hash == UINT32_MAX - (UINT32)i && l->compression == code,
			"location updates preserve full-width metadata and codecs");
		check(!memcmp(l->name, "AbCd1234", 9) && l->longname == longname && l->fullname == fullname,
			"location updates preserve all name bytes and pointer identities");
		hash_entry(l, 0);
	}
	for (i = 0; i < 129; ++i) check(neighbor[i] == 0x69, "following allocation canary");
	ZA_Stats(&during);
	check(during.usedblocks == before.usedblocks + 2, "no extra directory allocation");
	check(ZA_BLOCK(entries)->realsize == n * sizeof *entries, "exact directory allocation request");
	consistent(); Z_Free(neighbor); Z_Free(entries); ZA_Stats(&after);
	check(!root && after.used == before.used, "owner and directory cleanup unchanged");
	if (fragmented) for (i = 0; i < 16; ++i) Z_Free(holes[i]);
	consistent();
	printf("case n=%zu guard=%d sides=%d fragmented=%d request=%zu used=%zu peak=%zu hash=%016llx\n",
		n, guard, sides, fragmented, n * sizeof *entries, during.used - before.used,
		during.globalpeak, (unsigned long long)digest);
}

static void attach(lumpinfo_t *entries, UINT16 n, FILE *handle, int type, const char *filename)
{
	wadfile_t *wad = Z_Calloc(sizeof *wad, PU_STATIC, NULL);
	wad->lumpinfo = entries; wad->numlumps = n; wad->handle = handle;
	wad->filename = Z_StrDup(filename); wad->type = type;
	wadfiles = Z_Realloc(wadfiles, (numwadfiles + 1) * sizeof *wadfiles, PU_STATIC, NULL);
	wadfiles[numwadfiles++] = wad;
}
static void shutdown_case(void)
{
	if (inflcache.data) Z_Free(inflcache.data);
	W_Shutdown(); numwadfiles = 0; wadfiles = NULL;
	check(libc_count == 0, "actual success path frees every libc temporary");
	check(Z_TotalUsage() == 0, "actual shutdown frees archive and folder locations exactly once");
	consistent();
}
static void read_oracle(FILE *f, char *path, size_t *length, unsigned *id)
{
	check(fscanf(f, "%1023[^\t]\t%zu\t%u\n", path, length, id) == 3, "independent fixture oracle row");
}
static UINT8 source_byte(unsigned id, size_t offset) { return (UINT8)((offset * 37 + id * 17) ^ (offset >> 7)); }
static void check_reads(UINT16 wad, UINT16 lump, size_t length, unsigned id)
{
	size_t offsets[] = { 0, 1, length / 2, length ? length - 1 : 0, length, length + 1 };
	size_t sizes[] = { 0, 1, 7, 257, length + 1 };
	size_t i, j, k;
	UINT8 *buffer = malloc(length + 512);
	check(buffer != NULL, "read check buffer");
	check(W_LumpLengthPwad(wad, lump) == length, "actual length reader agrees with source");
	for (i = 0; i < sizeof offsets / sizeof *offsets; ++i)
	for (j = 0; j < sizeof sizes / sizeof *sizes; ++j)
	{
		size_t off = offsets[i], requested = sizes[j], expected, got;
		expected = off >= length ? 0 : (!requested || requested > length - off) ? length - off : requested;
		memset(buffer, 0xA5, length + 512);
		got = W_ReadLumpHeaderPwad_(wad, lump, buffer + 3, requested, off);
		check(got == expected, "actual generic partial-read count");
		for (k = 0; k < got; ++k) check(buffer[k + 3] == source_byte(id, off + k), "actual generic decoded byte versus raw oracle");
		for (k = 0; k < 3; ++k) check(buffer[k] == 0xA5, "partial destination leading guard");
		for (k = got + 3; k < length + 512; ++k) check(buffer[k] == 0xA5, "partial destination trailing guard");
		hash_bytes(buffer + 3, got);
	}
	free(buffer);
}
static void lookup_checks(UINT16 wad)
{
	UINT16 i, start, expected;
	wadfile_t *wf = wadfiles[wad];
	for (i = 0; i < wf->numlumps; ++i)
	{
		char mixed[64], upper[64];
		strlcpy(mixed, wf->lumpinfo[i].name, sizeof mixed);
		for (start = 0; mixed[start]; ++start) if (start & 1) mixed[start] = toupper((unsigned char)mixed[start]);
		strlcpy(upper, mixed, sizeof upper); strupr(upper);
		for (start = 0; start <= wf->numlumps; ++start)
		{
			for (expected = start; expected < wf->numlumps; ++expected)
				if (wf->lumpinfo[expected].hash == quickncasehash(upper, 8) && !strncmp(wf->lumpinfo[expected].name, upper, 8)) break;
			check(W_CheckNumForNamePwad(mixed, wad, start) == (expected == wf->numlumps ? INT16_MAX : expected),
				"indexed uppercased-query lookup and start ordering preserve independent linear result");
		}
		check(W_CheckNumForFullNamePK3(wf->lumpinfo[i].fullname, wad, 0) <= i, "complete-name lookup retains first override order");
		/* This API uppercases the query and compares stored long names exactly;
		 * the original case-sensitive behavior is intentionally preserved. */
		{
			char query[257]; strlcpy(query, wf->lumpinfo[i].longname, sizeof query); strupr(query);
			for (expected = 0; expected < wf->numlumps; ++expected) if (!strcmp(wf->lumpinfo[expected].longname, query)) break;
			check(W_CheckNumForLongNamePwad(query, wad, 0) == (expected == wf->numlumps ? INT16_MAX : expected),
				"existing-index long-name semantics unchanged");
		}
	}
	check(W_CheckNumForNamePwad("ABSENT!!", wad, 0) == INT16_MAX, "absent add-on lookup");
}

static void folder_mutation_checks(UINT16 lump, lumpinfo_t *l)
{
	const char *path = W_LumpDiskPath(l);
	char hidden[2048];
	FILE *file;
	size_t j;
	UINT8 data[129], dest[8];
	check(l->size == 0 && path != NULL, "folder mutation fixture starts empty");
	file = fopen(path, "wb"); check(file != NULL, "open external file for fixture append");
	for (j = 0; j < sizeof data; ++j) data[j] = source_byte(1, j);
	check(fwrite(data, 1, sizeof data, file) == sizeof data, "external file append"); fclose(file);
	check_reads(0, lump, sizeof data, 1);
	check(l->size == sizeof data && l->disksize == sizeof data && W_LumpPosition(l) == 0
		&& W_LumpDiskPath(l) == path, "folder stat updates size while retaining location identity");
	file = fopen(path, "wb"); check(file != NULL, "restore external empty file"); fclose(file);
	check(W_LumpLengthPwad(0, lump) == 0, "external file truncation remains visible");
	snprintf(hidden, sizeof hidden, "%s.temporarily-hidden", path);
	check(rename(path, hidden) == 0, "temporarily remove external file");
	expecting_error = 1;
	if (!setjmp(error_jump)) { (void)W_LumpLengthPwad(0, lump); check(0, "missing external file should retain existing error"); }
	expecting_error = 0;
	check(strstr(error_text, "doesn't exist") != NULL && W_LumpPosition(l) == 0
		&& W_LumpDiskPath(l) == path, "missing-file error preserves complete stored location");
	check(mkdir(path, 0700) == 0, "replace external file with directory");
	memset(dest, 0xA5, sizeof dest);
	check(W_LumpLengthPwad(0, lump) == 0 && W_ReadLumpHeaderPwad_(0, lump, dest, sizeof dest, 0) == 0,
		"external file becoming directory keeps existing zero-length behavior");
	for (j = 0; j < sizeof dest; ++j) check(dest[j] == 0xA5, "directory replacement read leaves destination untouched");
	check(rmdir(path) == 0 && rename(hidden, path) == 0, "restore original external file path");
}

static void constructor_case(const char *fixtures, const char *name, int kind, int guard, int sides)
{
	char input[2048], oracle[2048], relative[1024], path[2048];
	FILE *handle = NULL, *expected;
	lumpinfo_t *entries;
	UINT16 n = 0, folders = 0, i;
	zastats_t st;
	size_t length; unsigned id;
	arena_reset(8u << 20, sides, ship_prefer, guard); numwadfiles = 0; wadfiles = NULL;
	memset(&inflcache, 0, sizeof inflcache); alloc_call = 0;
	snprintf(input, sizeof input, "%s/%s", fixtures, name);
	snprintf(oracle, sizeof oracle, "%s/%s.oracle", fixtures, name);
	expected = fopen(oracle, "r"); check(expected != NULL, "open independent constructor oracle");
	if (kind != RET_FOLDER) { handle = fopen(input, "rb"); check(handle != NULL, "open constructor input"); }
	if (kind == RET_FOLDER) entries = getdirectoryfiles(input, &n, &folders);
	else if (kind == RET_WAD) entries = ResGetLumpsWad(handle, &n, input);
	else if (kind == RET_PK3) entries = ResGetLumpsZipFast(handle, &n);
	else entries = ResGetLumpsStandalone(handle, &n, "OBJCTCFG");
	check(entries && n > 0, "actual resource constructor returns complete directory");
	check(ZA_BLOCK(entries)->realsize == n * sizeof *entries, "constructor uses unchanged count and one exact directory request");
	attach(entries, n, handle, kind, input);
	for (i = 0; i < n; ++i)
	{
		lumpinfo_t *l = entries + i;
		read_oracle(expected, relative, &length, &id);
		check(!strcmp(l->fullname, relative), "actual full name matches independent archive/folder oracle");
		if (kind == RET_FOLDER)
		{
			snprintf(path, sizeof path, "%s/%s", input, relative);
			check(W_LumpPosition(l) == 0 && W_LumpDiskPath(l) && !strcmp(W_LumpDiskPath(l), path),
				"folder keeps complete path and effective position zero after sorting");
		}
		else check(!W_LumpDiskPath(l), "every archive and standalone record has effective null diskpath");
		if (kind == RET_PK3) check((W_LumpPosition(l) & ZIP_LAZYPOS) != 0, "ZIP preserves lazy header position before first read");
		if (kind == RET_PK3 && i == 1)
		{
			FILE *bad = tmpfile(); UINT8 guard = 0xA5; unsigned long before = W_LumpPosition(l);
			check(bad != NULL, "truncated local-header fixture"); wadfiles[0]->handle = bad;
			check(W_ReadLumpHeaderPwad_(0, i, &guard, 1, 0) == 0 && guard == 0xA5
				&& W_LumpPosition(l) == before && !W_LumpDiskPath(l), "failed ZIP resolution retains complete lazy offset and null path");
			wadfiles[0]->handle = handle; fclose(bad);
		}
		check_reads(0, i, length, id);
		if (kind == RET_PK3) check((W_LumpPosition(l) & ZIP_LAZYPOS) == 0 || !length,
			"ZIP lazy position resolves only when nonempty data is read");
		hash_entry(l, kind == RET_FOLDER);
	}
	check(fgetc(expected) == EOF, "constructor count matches independent oracle"); fclose(expected);
	lookup_checks(0);
	if (kind == RET_FOLDER)
		for (i = 0; i < n; ++i)
			if (!strcmp(entries[i].fullname, "Empty.bin")) folder_mutation_checks(i, entries + i);
	/* A subsequent add-on is an independent directory, with its own tag/index.
	 * The last file is searched first by the engine; both file directories keep
	 * the same per-file ordering and values while the root vector grows. */
	{
		FILE *again = kind == RET_FOLDER ? NULL : fopen(input, "rb");
		lumpinfo_t *other; UINT16 count = 0, nf = 0;
		if (kind == RET_FOLDER) other = getdirectoryfiles(input, &count, &nf);
		else if (kind == RET_WAD) other = ResGetLumpsWad(again, &count, input);
		else if (kind == RET_PK3) other = ResGetLumpsZipFast(again, &count);
		else other = ResGetLumpsStandalone(again, &count, "OBJCTCFG");
		check(other && count == n, "subsequent add-on preserves constructor count"); attach(other, count, again, kind, input);
		lookup_checks(1); lookup_checks(0);
		for (i = 0; i < n; ++i) check(!strcmp(entries[i].fullname, other[i].fullname), "root-vector growth preserves earlier directory");
	}
	ZA_Stats(&st);
	printf("constructor name=%s guard=%d sides=%d count=%u calls=%u used=%zu peak=%zu hash=%016llx\n",
		name, guard, sides, n, alloc_call, st.used, st.globalpeak, (unsigned long long)digest);
	shutdown_case();
}

static void failures(const char *fixtures)
{
	char input[2048]; unsigned stage;
	snprintf(input, sizeof input, "%s/plain.wad", fixtures);
	for (stage = 1; stage <= 13; ++stage)
	{
		FILE *handle = fopen(input, "rb"); UINT16 n = 0;
		zguard_t guard;
		check(handle != NULL, "fault fixture file"); arena_reset(1u << 20, 0, 0, 0);
		alloc_call = 0; zoomnth = stage; zoomnth_count = zoomnth_after = 0;
		if (Z_GUARD_TRY(&guard))
		{
			(void)ResGetLumpsWad(handle, &n, input); Z_GuardPop(&guard);
			check(0, "actual zone constructor injection must fail");
		}
		else Z_GuardLanded(&guard);
		zoomnth = 0;
		check(n == 0, "actual allocation failure leaves count unpublished as before");
		while (libc_count) fixture_libc_free(libc_blocks[libc_count - 1]);
		consistent(); Z_FreeTags(0, INT32_MAX); fclose(handle); consistent();
		handle = fopen(input, "rb"); alloc_call = 0;
		{
			lumpinfo_t *entries = ResGetLumpsWad(handle, &n, input);
			check(entries && n == 6, "constructor reinitializes after fatal-allocation cleanup");
			attach(entries, n, handle, RET_WAD, input); shutdown_case();
		}
	}
	puts("constructor failure stages=13 and clean retries passed");
}
static void eviction(void)
{
	void *cache = NULL, *request;
	lumpinfo_t *l;
	arena_reset(1u << 20, 0, 0, 0);
	l = Z_Calloc(sizeof *l, PU_STATIC, NULL); W_InitLumpPosition(l, ULONG_MAX);
	Z_Malloc(600u << 10, PU_CACHE, &cache); zframe = 10;
	request = Z_Malloc(500u << 10, PU_STATIC, NULL);
	check(!cache && W_LumpPosition(l) == ULONG_MAX && !W_LumpDiskPath(l), "real cache eviction preserves static location and full offset");
	Z_Free(request); Z_Free(l); consistent(); puts("real cache eviction passed");
}
int main(int argc, char **argv)
{
	static const size_t counts[] = { 1,2,3,7,15,16,17,31,32,63,64,65,127,255,256,1000,12614,169,2054,215,32767,65535 };
	size_t i; int sides, guard, fragmented;
	check(argc == 2, "fixture directory argument"); ship_sides = za_twosided; ship_prefer = za_prefer;
	for (sides = 0; sides < 2; ++sides)
	for (guard = 0; guard < 2; ++guard)
	for (fragmented = 0; fragmented < 2; ++fragmented)
	for (i = 0; i < sizeof counts / sizeof *counts; ++i) metadata_case(counts[i], guard, sides, fragmented);
	for (sides = 0; sides < 2; ++sides)
	for (guard = 0; guard < 2; ++guard)
	{
		constructor_case(argv[1], "folder", RET_FOLDER, guard, sides);
		constructor_case(argv[1], "plain.wad", RET_WAD, guard, sides);
		constructor_case(argv[1], "compressed.wad", RET_WAD, guard, sides);
		constructor_case(argv[1], "mixed.pk3", RET_PK3, guard, sides);
		constructor_case(argv[1], "standalone.soc", RET_SOC, guard, sides);
	}
	failures(argv[1]); eviction();
	printf("LUMPLOCATION7 DONE cases=176 constructors=20 checks=%u sizeof=%zu align=%zu hash=%016llx\n",
		checks, sizeof(lumpinfo_t), _Alignof(lumpinfo_t), (unsigned long long)digest);
	if (arena_raw) { aligned_release(arena_raw); arena_raw = NULL; }
	return 0;
}
