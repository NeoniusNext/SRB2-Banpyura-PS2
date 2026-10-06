/* Execute actual builders and geometry predicates against the actual 32-bit PS2 zone. */
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
typedef INT32 fixed_t;
typedef uint16_t UINT16;
typedef struct { fixed_t x, y; } vertex_t;
typedef struct { vertex_t *v1, *v2; fixed_t dx, dy; int slopetype; INT32 validcount; } line_t;
enum { BOXTOP, BOXBOTTOM, BOXLEFT, BOXRIGHT };
enum { ST_HORIZONTAL, ST_VERTICAL, ST_POSITIVE, ST_NEGATIVE };
#define FRACBITS 16
#define MAPBTOFRAC 7
#define MAPBLOCKUNITS 128
#define ZCK(name) ((void)0)
static fixed_t FixedMul(fixed_t a, fixed_t b) { return (fixed_t)(((int64_t)a * b) >> FRACBITS); }
static vertex_t vertex_storage[8194];
static line_t line_storage[65536];
static vertex_t *vertexes = vertex_storage;
static line_t *lines = line_storage;
static size_t numvertexes, numlines;
static INT32 *blockmaplump, *blockmap;
static size_t ps2_blockmapwords;
#ifdef MEMORY_BLOCKMAP16_SOURCE
static UINT16 *ps2_blockmaplists;
#endif
static fixed_t bmaporgx, bmaporgy;
static INT32 bmapwidth, bmapheight;
static void **blocklinks, **polyblocklinks;
#ifdef _MSC_VER
#pragma warning(disable:4702) /* Engine geometry has a defensive return after noreturn I_Error. */
#endif
#include MEMORY_GEOMETRY_SOURCE
#include MEMORY_BLOCKMAP_SOURCE
#ifdef MEMORY_BLOCKMAP16_SOURCE
typedef struct { INT32 validcount; size_t numLines; line_t **lines; } polyobj_t;
typedef struct polymaplink_s { struct { struct polymaplink_s *next; } link; polyobj_t *po; } polymaplink_t;
static INT32 validcount;
static INT32 expected[4096];
static size_t expected_count, consumed, stop_after;
static boolean check_line(line_t *line)
{
    check(consumed < expected_count, "iterator callback count");
    check(line - lines == expected[consumed++], "exact iterator linedef order");
    return consumed < stop_after;
}
#include MEMORY_BLOCKMAP16_SOURCE
#include MEMORY_BLOCKMAP_ITERATOR_SOURCE

static void check_compact_iterator(const INT32 *reference, size_t cells)
{
    for (size_t cell = 0; cell < cells; cell++)
    {
        unsigned char seen[65536] = {0};
        const INT32 *list = reference + reference[4 + cell] + 1;
        expected_count = 0;
        for (; *list != -1; list++)
            if (!seen[*list]) { seen[*list] = 1; expected[expected_count++] = *list; }
        for (unsigned early = 0; early < 2; early++)
        {
            boolean result;
            validcount++; consumed = 0;
            stop_after = early ? 1 : SIZE_MAX;
            result = P_BlockLinesIterator((INT32)(cell % (size_t)bmapwidth), (INT32)(cell / (size_t)bmapwidth), check_line);
            check(consumed == (early && expected_count ? 1 : expected_count), "iterator stops at same callback");
            check(result == !(early && expected_count), "iterator return unchanged");
        }
    }
    check(P_BlockLinesIterator(-1, 0, check_line), "out of bounds block unchanged");
}

static void compact_check(size_t words)
{
    size_t cells = (size_t)bmapwidth * bmapheight, prefix = cells + 4;
    zastats_t before, after;
    INT32 *reference = malloc(words * sizeof(INT32));
    check(reference != NULL, "blockmap reference allocation");
    memcpy(reference, blockmaplump, words * sizeof(INT32));
    ZA_Stats(&before);
    P_CompactBlockmap(); consistent(); ZA_Stats(&after);
    check_compact_iterator(reference, cells);
    if (numlines)
    {
        check(ps2_blockmaplists != NULL, "eligible map compacted");
        check(before.used - after.used + 15 >= 2 * (words - prefix)
            && before.used - after.used <= 2 * (words - prefix) + 15,
            "compact size saves list storage up to alignment");
        for (size_t cell = 0; cell < cells; cell++)
        {
            const INT32 *a = reference + reference[4 + cell];
            const UINT16 *b = ps2_blockmaplists + blockmap[cell];
            do { check((UINT16)*a == *b, "all raw list entries and terminators exact"); } while (*a++ != -1 && *b++ != UINT16_MAX);
        }
    }
    printf("compact words=%zu before=%zu after=%zu saved=%zu\n", words, before.used, after.used, before.used - after.used);
    free(reference);
}

static void compact_boundaries(void)
{
    for (unsigned overflow = 0; overflow < 2; overflow++)
    {
        INT32 snapshot[10] = {0, 0, 1, 1, 5, 0, 0, 65534, 65534, -1};
        zastats_t before, after;
        void *empty_poly[1] = {0};
        arena_reset(4096, ship_sides, ship_prefer, 1);
        numlines = overflow ? 65536 : 65535;
        bmapwidth = bmapheight = 1; ps2_blockmapwords = 10;
        if (overflow) snapshot[7] = snapshot[8] = 65535;
        blockmaplump = Z_Malloc(sizeof snapshot, PU_LEVEL, NULL);
        memcpy(blockmaplump, snapshot, sizeof snapshot); blockmap = blockmaplump + 4;
        polyblocklinks = empty_poly;
        ZA_Stats(&before); P_CompactBlockmap(); ZA_Stats(&after);
        check((ps2_blockmaplists != NULL) == !overflow, "65534 compact / 65535 full width boundary");
        if (overflow) check(before.used == after.used && !memcmp(snapshot, blockmaplump, sizeof snapshot), "oversized fallback leaves full bytes unchanged");
        check_compact_iterator(snapshot, 1);
        Z_Free(blockmaplump); empty_zone();
    }
    for (unsigned invalid = 0; invalid < 2; invalid++)
    {
        INT32 snapshot[8] = {0, 0, 1, 1, 5, 0, 1, -1};
        arena_reset(4096, ship_sides, ship_prefer, 1);
        numlines = 2; bmapwidth = bmapheight = 1; ps2_blockmapwords = 8;
        if (invalid) snapshot[6] = 2; else snapshot[4] = 0;
        blockmaplump = Z_Malloc(sizeof snapshot, PU_LEVEL, NULL);
        memcpy(blockmaplump, snapshot, sizeof snapshot); blockmap = blockmaplump + 4;
        P_CompactBlockmap();
        check(!ps2_blockmaplists && !memcmp(snapshot, blockmaplump, sizeof snapshot), "invalid index / legacy prefix offset keeps old bytes");
        Z_Free(blockmaplump); empty_zone();
    }
    puts("PASS compact boundaries: line0, line65534, duplicates, early termination, line65535 full-width fallback, invalid lists/offsets unchanged");
}
#endif

static void setvertex(size_t n, int x, int y)
{
    vertexes[n].x = (fixed_t)((UINT32)x << FRACBITS);
    vertexes[n].y = (fixed_t)((UINT32)y << FRACBITS);
}

static void scenario(const char *name, size_t nlines, int extent, int kind, size_t budget)
{
    size_t i, words;
    uint64_t hash = 1469598103934665603ull;
    zastats_t st;
    arena_reset(budget, ship_sides, ship_prefer, 1);
    numlines = nlines; numvertexes = 2 * nlines + 2;
    /* Pin bounds in both axes, including the original builder's first-vertex else-if rule. */
    setvertex(0, -extent, -extent); setvertex(1, extent, extent);
    for (i = 0; i < nlines; i++)
    {
        int x = (int)rndn((uint32_t)(2 * extent)) - extent;
        int y = (int)rndn((uint32_t)(2 * extent)) - extent;
        int x2 = kind == 1 ? -x : (int)rndn((uint32_t)(2 * extent)) - extent;
        int y2 = kind == 1 ? y : (int)rndn((uint32_t)(2 * extent)) - extent;
        if (kind == 2) { x = (x / 128) * 128; x2 = x; }
        if (kind == 3) { x2 = x; y2 = y; }
        setvertex(2 + 2 * i, x, y); setvertex(3 + 2 * i, x2, y2);
        lines[i].v1 = &vertexes[2 + 2 * i]; lines[i].v2 = &vertexes[3 + 2 * i];
    }
    P_CreateBlockMap(); consistent(); ZA_Stats(&st);
    check(st.usedblocks == 3, "only final blockmap and blocklink arrays remain");
    check(blockmap == blockmaplump + 4, "blockmap root alias remains valid");
    words = (size_t)bmapwidth * (size_t)bmapheight + 6;
    for (i = 0; i < (size_t)bmapwidth * (size_t)bmapheight; i++)
    {
        size_t off = (size_t)blockmap[i];
        check(off >= (size_t)bmapwidth * (size_t)bmapheight + 4, "list follows offset table");
        check(blockmaplump[off++] == 0, "legacy zero header");
        while (blockmaplump[off] != -1)
        {
            check((size_t)blockmaplump[off] < numlines, "valid linedef index");
            off++;
        }
        if (off + 1 > words) words = off + 1;
    }
    for (i = 0; i < words; i++) { hash ^= (UINT32)blockmaplump[i]; hash *= 1099511628211ull; }
    printf("%s words=%zu hash=%llu peak=%zu allocs=%zu\n", name, words,
        (unsigned long long)hash, st.globalpeak, st.allocs);
#ifdef MEMORY_BLOCKMAP16_SOURCE
    compact_check(words);
#endif
    Z_Free(blocklinks); Z_Free(polyblocklinks); Z_Free(blockmaplump); empty_zone();
}

int main(int argc, char **argv)
{
    ship_sides = za_twosided; ship_prefer = za_prefer;
    if (argc > 1)
    {
        scenario("dense-tight", 2048, 1024, 0, (size_t)strtoul(argv[1], NULL, 10));
        return 0;
    }
    scenario("dense", 2048, 1024, 0, 8u << 20);
    scenario("sparse", 256, 8192, 3, 8u << 20);
    scenario("horizontal", 1024, 2048, 1, 8u << 20);
    scenario("vertical-boundary", 1024, 2048, 2, 8u << 20);
    scenario("oblique", 512, 2048, 0, 8u << 20);
    scenario("empty", 0, 1024, 0, 8u << 20);
    for (unsigned seed = 1; seed <= 48; seed++)
    {
        char name[32];
        rng_state = seed;
        snprintf(name, sizeof name, "random-%u", seed);
        scenario(name, seed * 7, 512 + seed * 27, (int)(seed % 4), 8u << 20);
    }
#ifdef MEMORY_BLOCKMAP16_SOURCE
    compact_boundaries();
#endif
    return 0;
}
