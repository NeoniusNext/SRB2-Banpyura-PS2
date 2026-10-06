/* Execute real map linking: same subsectors, sector line order/counts and sound origins. */
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
typedef INT32 fixed_t;
typedef struct { fixed_t x, y; } vertex_t;
typedef struct line_s line_t;
typedef struct { fixed_t x, y, z; } soundorg_t;
typedef struct { size_t linecount; line_t **lines; soundorg_t soundorg; fixed_t floorheight; } sector_t;
struct line_s { vertex_t *v1, *v2; sector_t *frontsector, *backsector; };
typedef struct { sector_t *sector; } side_t;
typedef struct { side_t *sidedef; } seg_t;
typedef struct { size_t firstline; sector_t *sector; } subsector_t;
enum { BOXTOP, BOXBOTTOM, BOXLEFT, BOXRIGHT };
#define FRACBITS 16
#define DBG_SETUP 0
static vertex_t vertices[8192];
static line_t lines[4096];
static sector_t sectors[1024];
static side_t sides[1024];
static seg_t segs[1024];
static subsector_t subsectors[1024];
static size_t numsectors, numlines, numsides, numsegs, numsubsectors;
static void CorruptMapError(const char *s) { I_Error("corrupt fixture: %s", s); }
static void M_ClearBox(fixed_t *b) { b[BOXTOP] = b[BOXRIGHT] = INT32_MIN; b[BOXBOTTOM] = b[BOXLEFT] = INT32_MAX; }
static void M_AddToBox(fixed_t *b, fixed_t x, fixed_t y)
{
    if (x < b[BOXLEFT]) b[BOXLEFT] = x;
    if (x > b[BOXRIGHT]) b[BOXRIGHT] = x;
    if (y < b[BOXBOTTOM]) b[BOXBOTTOM] = y;
    if (y > b[BOXTOP]) b[BOXTOP] = y;
}
#include MEMORY_LINK_SOURCE

static void mix(uint64_t *h, UINT32 value) { *h ^= value; *h *= 1099511628211ull; }
static void scenario(const char *name, size_t ns, size_t nl, int mode)
{
    uint64_t hash = 1469598103934665603ull;
    size_t i, entries = 0;
    zastats_t st;
    arena_reset(1u << 20, ship_sides, ship_prefer, 1);
    memset(sectors, 0, sizeof sectors);
    numsectors = numsides = numsegs = numsubsectors = ns; numlines = nl;
    for (i = 0; i < ns; i++)
    {
        sides[i].sector = &sectors[i]; segs[i].sidedef = &sides[i]; subsectors[i].firstline = i;
        sectors[i].floorheight = (fixed_t)((UINT32)(i * 7) << FRACBITS);
    }
    for (i = 0; i < nl; i++)
    {
        size_t front = mode == 1 ? 0 : rndn((UINT32)ns);
        size_t back = rndn((UINT32)ns);
        vertices[2*i].x = (fixed_t)((UINT32)(rndn(4096) - 2048) << FRACBITS);
        vertices[2*i].y = (fixed_t)((UINT32)(rndn(4096) - 2048) << FRACBITS);
        vertices[2*i+1].x = (fixed_t)((UINT32)(rndn(4096) - 2048) << FRACBITS);
        vertices[2*i+1].y = (fixed_t)((UINT32)(rndn(4096) - 2048) << FRACBITS);
        lines[i].v1 = &vertices[2*i]; lines[i].v2 = &vertices[2*i+1];
        lines[i].frontsector = &sectors[front];
        lines[i].backsector = mode == 2 ? &sectors[front] : ((i % 3) ? &sectors[back] : NULL);
    }
    P_LinkMapData(); consistent(); ZA_Stats(&st);
    for (i = 0; i < ns; i++)
    {
        check(subsectors[i].sector == &sectors[i], "subsector lookup unchanged");
        mix(&hash, (UINT32)sectors[i].linecount);
        if (!sectors[i].linecount) check(sectors[i].lines == NULL, "empty sector pointer NULL");
        for (size_t j = 0; j < sectors[i].linecount; j++) mix(&hash, (UINT32)(sectors[i].lines[j] - lines));
        mix(&hash, (UINT32)sectors[i].soundorg.x); mix(&hash, (UINT32)sectors[i].soundorg.y); mix(&hash, (UINT32)sectors[i].soundorg.z);
        entries += sectors[i].linecount;
    }
    printf("%s hash=%llu entries=%zu used=%zu blocks=%zu\n", name, (unsigned long long)hash, entries, st.used, st.usedblocks);
    Z_FreeTags(PU_LEVEL, PU_PURGELEVEL - 1); empty_zone();
}

int main(void)
{
    ship_sides = za_twosided; ship_prefer = za_prefer;
    scenario("scattered", 1024, 4096, 0);
    scenario("one-front", 1024, 4096, 1);
    scenario("same-back", 1024, 4096, 2);
    scenario("no-lines", 1024, 0, 0);
    scenario("small", 3, 7, 0);
    return 0;
}
