/* Calls the unchanged complete-engine LTO cache helper, exposed by objcopy. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct
{
	uint32_t hash[64], number[64];
	char name[64][32];
} cache_t;

extern cache_t Engine_Cache;
extern uint16_t Engine_Cursor;
extern uint32_t Engine_CheckLumpInCache(const char *, int);

static unsigned rows;
static uint64_t digest = 0xcbf29ce484222325ULL;

static void Fail(const char *why)
{
	printf("CACHE10 FAIL row=%u %s\n", rows, why);
	exit(1);
}

static uint32_t Hash(const char *p, unsigned limit)
{
	uint32_t h = 5381;
	unsigned i;
	for (i = 0; i < limit && p[i]; i++) h = h * 33 ^ tolower(p[i]);
	return h;
}

static uint32_t Count(void)
{
	uint32_t value;
	__asm__ volatile("mfc0 %0,$9" : "=r"(value));
	return value;
}

static void Digest(const void *data, unsigned bytes)
{
	const unsigned char *p = data;
	while (bytes--) digest = (digest ^ *p++) * 0x100000001b3ULL;
}

static void Row(unsigned mode, unsigned cursor, unsigned depth, unsigned collide)
{
	static const char *const names[] = {"Q", "ABCD1234", "Q", "ABCD1234", "ABCDEFGHIJKLMNOPQRSTUVWXY123456"};
	cache_t expected;
	const char *query = names[mode];
	unsigned i, repeat, call, islong = mode >= 2;
	unsigned hit = (cursor - depth) & 63;
	uint32_t hash = Hash(query, islong ? 32 : 8), best = UINT32_MAX;
	uint32_t result = depth == 64 ? UINT32_MAX : 0x10000000u + hit;
	uint32_t actual;
	memset(&expected, 0, sizeof expected);
	for (i = 0; i < 64; i++)
	{
		expected.hash[i] = collide ? hash : hash ^ 1;
		expected.number[i] = 0x10000000u + i;
		strcpy(expected.name[i], "NO_MATCH");
	}
	if (depth < 64)
	{
		expected.hash[hit] = hash;
		strcpy(expected.name[hit], query);
	}
	memcpy(&Engine_Cache, &expected, sizeof expected);
	Engine_Cursor = cursor;
	actual = Engine_CheckLumpInCache(query, islong);
	if (actual != result || Engine_Cursor != (depth == 64 ? cursor : hit)
		|| memcmp(&Engine_Cache, &expected, sizeof expected)) Fail("result/cache/cursor");
	Digest(&actual, sizeof actual);
	Digest(&Engine_Cursor, sizeof Engine_Cursor);
	Digest(&Engine_Cache, sizeof Engine_Cache);
	for (repeat = 0; repeat < 5; repeat++)
	{
		uint32_t start, elapsed;
		Engine_Cursor = cursor;
		start = Count();
		for (call = 0; call < 64; call++)
		{
			Engine_Cursor = cursor;
			actual = Engine_CheckLumpInCache(query, islong);
		}
		elapsed = Count() - start;
		if (elapsed < best) best = elapsed;
	}
	if (actual != result || Engine_Cursor != (depth == 64 ? cursor : hit)
		|| memcmp(&Engine_Cache, &expected, sizeof expected)) Fail("timed result/cache/cursor");
	printf("CACHE10 ROW id=%u mode=%u cursor=%u depth=%u collide=%u calls=64 count=%u result=%08x digest=%08x%08x\n",
		rows++, mode, cursor, depth, collide, (unsigned)best, (unsigned)actual, (unsigned)(digest >> 32), (unsigned)digest);
}

int __wrap_main(int argc, char **argv)
{
	static const unsigned cursors[] = {0, 1, 31, 62, 63};
	static const unsigned depths[] = {0, 1, 7, 31, 62, 63, 64};
	unsigned mode, cursor, depth, collide;
	(void)argc; (void)argv;
	if (sizeof Engine_Cache != 2560) Fail("cache layout");
	for (mode = 0; mode < 5; mode++)
	for (cursor = 0; cursor < sizeof cursors / sizeof cursors[0]; cursor++)
	for (depth = 0; depth < sizeof depths / sizeof depths[0]; depth++)
	for (collide = 0; collide < 2; collide++)
		Row(mode, cursors[cursor], depths[depth], collide);
	printf("CACHE10 DONE rows=%u digest=%08x%08x\n", rows, (unsigned)(digest >> 32), (unsigned)digest);
	return 0;
}
