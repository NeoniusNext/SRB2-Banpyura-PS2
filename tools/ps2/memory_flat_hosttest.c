/* Run actual raw-lump ownership transfer and flat consumer in the real PS2 zone. */
#ifdef _MSC_VER
#pragma warning(disable:4702) /* Unexecuted image-conversion stubs intentionally fail. */
#pragma warning(disable:4189) /* No-image-conversion fixture leaves the production length unused. */
#endif
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef UINT16 lumpnum_t;
typedef void *lumpcache_t;
typedef struct { UINT16 wad, lump; } texpatch_t;
typedef struct { INT32 type, width, height; UINT8 *flat; texpatch_t patches[1]; } texture_t;
enum { TEXTURETYPE_FLAT = 1, PICFMT_FLAT = 2 };
static UINT8 resource[262144];
static size_t resource_size, reads;
static void *last_read_ptr;
static lumpcache_t rawcache[1];
typedef struct { lumpcache_t *lumpcache; } wadfile_t;
static wadfile_t wadfile = {rawcache};
static wadfile_t *wadfiles[1] = {&wadfile};
static texture_t flat_fixture;
static texture_t *textures[1] = {&flat_fixture};
static INT32 numtextures = 1;
static size_t flatmemory;
static boolean TestValidLump(UINT16 wad, UINT16 lump) { return wad == 0 && lump == 0; }
static size_t W_LumpLengthPwad(UINT16 wad, UINT16 lump) { (void)wad; (void)lump; return resource_size; }
static size_t W_ReadLumpHeaderPwad(UINT16 wad, UINT16 lump, void *dest, size_t size, size_t offset)
{
    (void)wad; (void)lump; (void)size;
    check(offset == 0, "full flat read");
    memcpy(dest, resource, resource_size); reads++; last_read_ptr = dest;
    return resource_size;
}
static void *Picture_TextureToFlat(size_t texnum) { (void)texnum; check(0, "composite conversion outside raw-flat fixture"); return NULL; }
static void R_ReleaseTextureCache(INT32 texnum) { (void)texnum; check(0, "composite cache release outside raw-flat fixture"); }
#ifndef NO_PNG_LUMPS
static boolean Picture_IsLumpPNG(UINT8 *data, size_t size) { (void)data; (void)size; return false; }
static void *Picture_PNGConvert(UINT8 *data, int fmt, void *a, void *b, void *c, void *d, size_t size, void *e, int flags)
{ (void)data; (void)fmt; (void)a; (void)b; (void)c; (void)d; (void)size; (void)e; (void)flags; check(0, "not PNG"); return NULL; }
#endif
#include MEMORY_LUMPCACHE_SOURCE
#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_TEXPLACE)
#define R_TEXTURE_WORK_TAG PU_RENDERWORK
#else
#define R_TEXTURE_WORK_TAG PU_STATIC
#endif
#include MEMORY_FLAT_SOURCE

static void scenario(size_t length, size_t budget)
{
    zastats_t st;
    uint64_t hash = 1469598103934665603ull;
    UINT8 *root;
    size_t i;
    arena_reset(budget, ship_sides, ship_prefer, 1);
    memset(&flat_fixture, 0, sizeof flat_fixture); rawcache[0] = NULL;
    flat_fixture.type = TEXTURETYPE_FLAT; flat_fixture.width = 256; flat_fixture.height = (INT32)(length / 256);
    resource_size = length; reads = 0;
    for (i = 0; i < length; i++) resource[i] = (UINT8)(i * 33 + (i >> 9));
    root = R_GetFlatForTexture(0);
    check(root && !memcmp(root, resource, length), "all flat bytes preserved");
    check(!rawcache[0], "raw WAD owner detached/cleared");
    check(ZA_TAG(ZA_BLOCK(root)) == PU_CACHE && ZA_BLOCK(root)->user == (void **)&flat_fixture.flat, "derived owner can be evicted/rebuilt");
#ifndef PS2_NOOPT_flattransfer
    check(root == last_read_ptr, "transfer retains raw read destination without copying");
#else
    check(root != last_read_ptr, "reference duplicates payload");
#endif
    check(R_GetFlatForTexture(0) == root && reads == 1, "cache hit performs no I/O or allocation");
    consistent(); ZA_Stats(&st);
    check(st.usedblocks == 1, "one final flat allocation remains");
    for (i = 0; i < length; i++) { hash ^= root[i]; hash *= 1099511628211ull; }
    printf("flat-%zu hash=%llu peak=%zu allocs=%zu\n", length, (unsigned long long)hash, st.globalpeak, st.allocs);
    /* Re-reading the independent WAD cache must not change the derived owner or pixels. */
    check(W_CacheLumpNumPwad(0, 0, PU_STATIC) != root, "WAD cache rebuild has separate owner");
    check(flat_fixture.flat == root && !memcmp(root, resource, length), "WAD reload leaves texture root intact");
    Z_Free(rawcache[0]); Z_Free(root);
    check(flat_fixture.flat == NULL && rawcache[0] == NULL, "both owner slots cleared independently");
    root = R_GetFlatForTexture(0);
    check(!memcmp(root, resource, length), "evicted flat rebuilt exactly");
    Z_Free(root); empty_zone();
}

int main(int argc, char **argv)
{
    ship_sides = za_twosided; ship_prefer = za_prefer;
    if (argc > 1)
    {
        /* Test only the cold build, since a simultaneous independent WAD reload needs extra space. */
        arena_reset((size_t)strtoul(argv[1], NULL, 10), ship_sides, ship_prefer, 1);
        flat_fixture.type = TEXTURETYPE_FLAT; resource_size = 65536;
        check(R_GetFlatForTexture(0) != NULL, "flat fits small arena");
        consistent();
        return 0;
    }
    scenario(4096, 1u << 20);
    scenario(65536, 1u << 20);
    scenario(262144, 1u << 20);
    return 0;
}
