/* Production TEXTMAP counting/parsing plus the real zone allocator. The x64
 * build checks portable semantics; EE layout and execution need target checks. */
#include <stdint.h>
typedef uint8_t UINT8;
static int netgame;
static int PS2Lua_InCall(void) { return 0; }
void PS2Spill_Reset(void) {}
#define max(a,b) ((a) > (b) ? (a) : (b))
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
typedef uint16_t UINT16;
typedef uint64_t UINT64;
#include MEMORY_TOKENIZER_SOURCE
#include MEMORY_TOKENIZER_GLOBALS
static size_t nummapthings, numlines, numsides, numvertexes, numsectors;
enum { CONS_ERROR, CONS_WARNING, CONS_NOTICE };
static void CONS_Alert(int level, const char *fmt, ...)
{ (void)level; (void)fmt; }
#define fastcmp(a,b) (!strcmp((a),(b)))
#include MEMORY_TEXTMAP_SOURCE
#include MEMORY_TEXTMAP_PARSE_SOURCE

static uint64_t stream_hash;
static unsigned stream_type;
static size_t pair_count;
static int fixture_twosided = 1, fixture_redzone;
static void hash_bytes(const void *p, size_t n)
{
	const unsigned char *b = p;
	while (n--) { stream_hash ^= *b++; stream_hash *= UINT64_C(1099511628211); }
}
static void parser(UINT32 num, const char *param, const char *val)
{
	hash_bytes(&stream_type, sizeof stream_type);
	hash_bytes(&num, sizeof num);
	hash_bytes(param, strlen(param) + 1);
	hash_bytes(val, strlen(val) + 1);
	pair_count++;
}

static void release_all_positions(void)
{
	textmap_block_t *all[] = { &mapthingBlocks, &linedefBlocks, &sidedefBlocks, &vertexBlocks, &sectorBlocks };
	size_t i;
	for (i = 0; i < 5; i++)
	{
		Z_Free(all[i]->pos);
		memset(all[i], 0, sizeof *all[i]);
	}
}

static void run_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *text;
	size_t len, counts[5], i, k, retained, loaded;
	static const unsigned order[5] = { TMS_VERTEX, TMS_SECTOR, TMS_LINEDEF, TMS_SIDEDEF, TMS_THING };
	textmap_block_t *all[] = { &mapthingBlocks, &linedefBlocks, &sidedefBlocks, &vertexBlocks, &sectorBlocks };
	zastats_t stats;
	check(f != NULL, "open fixture");
	check(fseek(f, 0, SEEK_END) == 0, "seek fixture");
	len = (size_t)ftell(f);
	check(len < 32u * 1024 * 1024, "bounded fixture");
	check(fseek(f, 0, SEEK_SET) == 0, "rewind fixture");
	text = malloc(len + 1);
	check(text != NULL && fread(text, 1, len, f) == len, "read fixture");
	text[len] = '\0';
	fclose(f);
	arena_reset(64u << 20, fixture_twosided, 16, fixture_redzone);
	M_TokenizerOpen(text, len);
	globalTokenizer->line = 0;
	check(TextmapCount(len), "production count accepts fixture");
	free(text);
	counts[0] = nummapthings; counts[1] = numlines; counts[2] = numsides;
	counts[3] = numvertexes; counts[4] = numsectors;
	loaded = Z_TotalUsage();
	stream_hash = UINT64_C(14695981039346656037);
	pair_count = 0;
	for (k = 0; k < 5; k++)
	{
		stream_type = order[k];
		for (i = 0; i < counts[stream_type]; i++)
		{
			if (tmscan_on)
				M_TokenizerScanParse(&tmscan, stream_type, (UINT32)i, parser);
			else
				TextmapParse(all[stream_type]->pos[i], i, parser);
		}
		TextmapReleasePos(all[stream_type]);
	}
	Tokenizer_SRB2ScanFree(&tmscan);
	tmscan_on = false;
	M_TokenizerClose();
	retained = Z_TotalUsage();
#ifndef PS2_NOOPT_TEXTMAPMEM
	check(retained == 0, "no position/scan storage retained after parse");
#endif
	ZA_Stats(&stats);
	printf("fixture=%s hash=%llu pairs=%zu counts=%zu,%zu,%zu,%zu,%zu loaded=%zu retained=%zu peak=%zu\n",
		path, (unsigned long long)stream_hash, pair_count, counts[0], counts[1], counts[2], counts[3], counts[4], loaded, retained, stats.peakused);
	release_all_positions();
	check(Z_TotalUsage() == 0, "fixture cleanup");
	consistent();
}

static void growth_edges(void)
{
	void *p = NULL, *old;
	UINT32 capacity = 0;
	size_t i;
	textmap_block_t block = { 0 };
	size_t count = 0;
	arena_reset(4u << 20, 0, 16, 1);
	check(ScanGrow(&p, &capacity, 1024, 12), "initial scan growth");
	memset(p, 0x3a, (size_t)capacity * 12);
	old = p;
	check(ScanGrow(&p, &capacity, 1025, 12), "contiguous scan growth");
	for (i = 0; i < 1024 * 12; i++) check(((UINT8 *)p)[i] == 0x3a, "growth retains every live byte");
#ifndef PS2_NOOPT_TEXTMAPMEM
	check(p == old, "resize uses the adjacent free block");
	check(!ScanGrow(&p, &capacity, UINT32_MAX, 12), "oversized scan falls back without overflow");
	check(p == old && capacity == 2048, "refused growth leaves scan intact");
#endif
	{
		void *barrier = Z_Malloc(1024, PU_LEVEL, NULL);

		old = p;
		check(ScanGrow(&p, &capacity, 2049, 12), "blocked scan growth");
		check(p != old, "occupied neighbour forces a move");
		for (i = 0; i < 1024 * 12; i++) check(((UINT8 *)p)[i] == 0x3a, "moving growth retains every live byte");
		Z_Free(barrier);
	}
	Z_Free(p);
	/* Both size_t/count overflow and allocator-size overflow are rejected before
	 * a count/owner/capacity is published. */
#ifndef PS2_NOOPT_TEXTMAPMEM
	count = (size_t)0x7FFFFFF0u / sizeof *block.pos;
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		TextmapStorePos(&block, &count);
		check(0, "position overflow must be diagnosed");
	}
	expecting_error = 0;
	check(block.pos == NULL && block.capacity == 0, "position overflow leaves root/capacity intact");
#else
	(void)block; (void)count; (void)old;
#endif
	consistent();
}

static void abort_restart(void)
{
	static const char *inputs[] = {
		"namespace = \"srb2\";\nvertex { x = 1; y = 2; }\n",
		"namespace = \"srb2\";\n/* fallback */ vertex { x = 3; y = 4; }\n",
		"namespace = \"srb2\";\nvertex { x = 5; y = 6; }\nvertex { x = 7; y = 8; }\n"
	};
	textmap_block_t *all[] = { &mapthingBlocks, &linedefBlocks, &sidedefBlocks, &vertexBlocks, &sectorBlocks };
	size_t pass, i;
	arena_reset(4u << 20, 1, 16, 0);
	for (pass = 0; pass < 3; pass++)
	{
		M_TokenizerOpen(inputs[pass], strlen(inputs[pass]));
		globalTokenizer->line = 0;
		check(TextmapCount(strlen(inputs[pass])), "restart count succeeds");
		check(numvertexes == (pass == 2 ? 2 : 1) && !numlines && !numsides && !numsectors && !nummapthings,
			"restart discards counts and capacities from abandoned load");
		/* The load guard already does this when a level cannot be completed. */
		Z_FreeTags(PU_LEVEL, PU_LEVEL);
#ifndef PS2_NOOPT_TEXTMAPMEM
		check(!tmscan.pairs && !tmscan.bfirst && !tmscan.btype, "abandoned scan owners cleared");
		for (i = 0; i < 5; i++)
			check(!tmscan.list[i] && !all[i]->pos, "abandoned per-type owners cleared");
#else
		(void)all; (void)i;
		Tokenizer_SRB2ScanFree(&tmscan); /* old PU_STATIC tables need explicit cleanup */
#endif
		M_TokenizerClose();
		check(Z_TotalUsage() == 0, "aborted-load temporary roots cleaned");
		consistent();
	}
	memset(&tmscan, 0, sizeof tmscan);
	release_all_positions();
}

int main(int argc, char **argv)
{
	int i;
	check(argc > 1, "fixture arguments");
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--one-sided")) fixture_twosided = 0;
		else if (!strcmp(argv[i], "--redzones")) fixture_redzone = 1;
		else run_file(argv[i]);
	growth_edges();
	abort_restart();
	printf("PASS textmap memory semantics (%zu-bit host, %u checks)\n", 8 * sizeof(void *), checks);
	aligned_release(arena_raw);
	return 0;
}
