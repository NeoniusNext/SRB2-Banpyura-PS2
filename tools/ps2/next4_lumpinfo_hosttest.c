/* Exact production metadata types with the real PS2 arena, via its existing boundary stubs. */
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
#define main zone_regression_main
#include "zone_hosttest.c"
#undef main
#include LUMPINFO_TYPES

static uint64_t metadata_hash;
static void hash_field(const void *p, size_t n)
{
	const UINT8 *b = p;
	while (n--) metadata_hash = (metadata_hash ^ *b++) * UINT64_C(1099511628211);
}
static void metadata_case(size_t n, int guard, int sides)
{
	lumpinfo_t *entries, *owner = NULL;
	UINT8 *neighbor;
	zastats_t before, during, after;
	size_t i, code;
	char longname[] = "LongName", fullname[] = "Folder/LongName.ext", diskpath[] = "mass:/Addon/LongName.ext";
	arena_reset(5u << 20, sides, ship_prefer, guard);
	ZA_Stats(&before);
	entries = Z_MallocAlign(n * sizeof *entries, PU_STATIC, &owner, 0);
	neighbor = Z_MallocAlign(129, PU_STATIC, NULL, 0);
	memset(neighbor, 0x69, 129);
	check(entries && owner == entries, "directory root and natural alignment");
	check((uintptr_t)&entries[0].hash % _Alignof(UINT32) == 0
		&& (uintptr_t)&entries[0].longname % _Alignof(char *) == 0
		&& (uintptr_t)&entries[0].size % _Alignof(size_t) == 0, "metadata field natural alignment");
	for (i = 0; i < n; ++i)
	{
		lumpinfo_t *e = &entries[i];
		memset(e, 0xA7, sizeof *e);
		e->position = ~0UL - i; e->disksize = (unsigned long)i * 65537UL;
		memcpy(e->name, "AbCd1234", 9); e->hash = UINT32_MAX - (UINT32)i;
		e->longname = longname; e->fullname = fullname; e->diskpath = diskpath;
		e->size = SIZE_MAX - i;
	}
	for (code = CM_NOCOMPRESSION; code <= CM_UNSUPPORTED; ++code)
	for (i = 0; i < n; ++i)
	{
		lumpinfo_t *e = &entries[i];
		e->compression = (compmethod)code;
		check(e->compression == code, "every codec retains its exact enum value");
		check(e->position == ~0UL - i && e->disksize == (unsigned long)i * 65537UL
			&& e->size == SIZE_MAX - i && e->hash == UINT32_MAX - (UINT32)i, "full-width metadata values");
		check(!memcmp(e->name, "AbCd1234", 9) && e->longname == longname
			&& e->fullname == fullname && e->diskpath == diskpath, "codec store preserves names and pointers");
		/* Hash logical values in fixed widths, never padding or process addresses. */
		{
			uint64_t values[] = { e->position, e->disksize, e->hash, e->size, e->compression };
			hash_field(values, sizeof values);
			hash_field(e->name, sizeof e->name);
		}
	}
	for (i = 0; i < 129; ++i) check(neighbor[i] == 0x69, "following allocation canary");
	ZA_Stats(&during);
	check(during.usedblocks == before.usedblocks + 2, "same allocation count");
	check(ZA_BLOCK(entries)->realsize == n * sizeof *entries, "exact directory request");
	consistent();
	Z_Free(neighbor); Z_Free(entries); ZA_Stats(&after);
	check(!owner && after.used == before.used, "directory owner and allocation cleanup");
	consistent();
	printf("case n=%zu guard=%d sides=%d used=%zu peak=%zu hash=%016llx\n", n, guard, sides,
		during.used - before.used, during.globalpeak, (unsigned long long)metadata_hash);
}

int main(void)
{
	static const size_t counts[] = { 1,2,3,7,15,16,17,31,32,63,64,65,127,255,256,1000,12614,169,2054,215,32767,65535 };
	size_t i;
	int guard, sides;
#ifdef __mips__
	init_scr();
	check(ZA_HDR == 16, "EE release arena ABI");
#endif
	ship_sides = za_twosided; ship_prefer = za_prefer;
	metadata_hash = UINT64_C(1469598103934665603);
	for (guard = 0; guard < 2; ++guard)
	for (sides = 0; sides < 2; ++sides)
	for (i = 0; i < sizeof counts / sizeof *counts; ++i) metadata_case(counts[i], guard, sides);
	printf("LUMPINFO4 DONE cases=%zu checks=%u sizeof=%zu align=%zu codecbytes=%zu\n",
		4 * sizeof counts / sizeof *counts, checks, sizeof(lumpinfo_t), _Alignof(lumpinfo_t), sizeof(((lumpinfo_t *)0)->compression));
	if (arena_raw) { aligned_release(arena_raw); arena_raw = NULL; }
	return 0;
}
