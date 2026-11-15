/* Extended cache-order checks around the preceding production lookup suite. */
#define main next2_lookup_main
#include "next2_lookup_hosttest.c"
#undef main
#ifndef NEXT2_LOOKUP_EE
#include <sys/mman.h>
#include <unistd.h>

void reference_cache_set(const next2_cache_state *s);
void candidate_cache_set(const next2_cache_state *s);

static UINT64 cache_digest = 0xcbf29ce484222325ULL;

static void digest_bytes(const void *data, size_t size)
{
	const unsigned char *p = data;
	while (size--) cache_digest = (cache_digest ^ *p++) * 0x100000001b3ULL;
}

static void compare_cache(const next2_cache_state *initial, const char *name, boolean longname)
{
	next2_cache_state a, b;
	lumpnum_t ra, rb;
	reference_cache_set(initial);
	candidate_cache_set(initial);
	ra = reference_cache_check(name, longname);
	rb = candidate_cache_check(name, longname);
	require(ra == rb, "two-span result/order");
	reference_cache_state(&a);
	candidate_cache_state(&b);
	require(!memcmp(&a, &b, sizeof a), "two-span complete cache/cursor");
	digest_bytes(&ra, sizeof ra);
	digest_bytes(&a, sizeof a);
	checks++;
}

static void exhaustive_cache_spans(void)
{
	next2_cache_state initial;
	unsigned cursor, hit, mode, i;
	static const char *const queries[] = {"A", "ABCDE123", "ABCDE123LONG", "ABCDEFGHIJKLMNOPQRSTUVWXYZ12345"};
	for (mode = 0; mode < 8; mode++)
	for (cursor = 0; cursor < 64; cursor++)
	for (hit = 0; hit < 64; hit++)
	{
		const char *query = queries[mode / 2];
		boolean islong = mode & 1;
		memset(&initial, 0, sizeof initial);
		initial.cursor = cursor;
		for (i = 0; i < 64; i++)
		{
			/* All slots deliberately collide, so every skipped name is checked. */
			initial.hash[i] = quickncasehash(query, islong ? 32 : 8);
			initial.number[i] = 0x80000000u + i;
			strcpy(initial.name[i], "MISMATCH");
		}
		strncpy(initial.name[hit], query, islong ? 31 : 8);
		compare_cache(&initial, query, islong);
		/* Duplicate matching slots make circular precedence observable. */
		strncpy(initial.name[(hit + 17) & 63], query, islong ? 31 : 8);
		compare_cache(&initial, query, islong);
		/* The short-name predicate must reject a nine-byte stored name. */
		initial.name[hit][8] = 'X';
		compare_cache(&initial, query, false);
		/* Genuine misses still leave every cache byte and cursor unchanged. */
		compare_cache(&initial, "NO_MATCH", islong);
	}
	/* A nonterminated eight-byte short query ends directly at a guard page. */
	{
		long pagesize = sysconf(_SC_PAGESIZE);
		char *memory = mmap(NULL, (size_t)pagesize * 2, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		char *query;
		require(pagesize > 0 && memory != MAP_FAILED, "guard allocation");
		require(!mprotect(memory + pagesize, (size_t)pagesize, PROT_NONE), "guard protection");
		query = memory + pagesize - 8;
		memcpy(query, "ABCD1234", 8);
		memset(&initial, 0, sizeof initial);
		for (cursor = 0; cursor < 64; cursor++)
		for (hit = 0; hit < 64; hit++)
		{
			initial.cursor = cursor;
			initial.hash[hit] = quickncasehash(query, 8);
			initial.number[hit] = hit;
			memcpy(initial.name[hit], query, 8);
			compare_cache(&initial, query, false);
			memset(initial.name[hit], 0, 32);
			initial.hash[hit] = 0;
		}
		require(!munmap(memory, (size_t)pagesize * 2), "guard release");
	}
	printf("CACHE-SPANS PASS checks=%u digest=%016llx\n", checks, (unsigned long long)cache_digest);
}
#endif

int main(int argc, char **argv)
{
#ifndef NEXT2_LOOKUP_EE
	if (argc == 1 || strcmp(argv[1], "bench")) exhaustive_cache_spans();
#endif
	return next2_lookup_main(argc, argv);
}
