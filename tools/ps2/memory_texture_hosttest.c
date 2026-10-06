/* Real composite generator, real patch conversion, and real 32-bit PS2 zone under pressure. */
#ifdef _MSC_VER
#pragma warning(disable:4200) /* Production texture_t uses a GNU flexible tail. */
#pragma warning(disable:4702) /* Non-executed flat stubs intentionally fail. */
#endif
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
#undef __attribute__
#define ATTRPACK
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef INT32 fixed_t;
typedef UINT16 lumpnum_t;
typedef INT32 pictureformat_t;
typedef UINT32 pictureflags_t;
enum { PICFMT_PATCH, PICFMT_FLAT, PICFLAGS_USE_TRANSPARENTPIXEL = 1 };
#define SHORT(x) (x)
#define LONG(x) (x)
#define PNG_HEADER_SIZE 8
#define TRANSPARENTPIXEL 255
enum patchalphastyle { AST_COPY, AST_TRANSLUCENT, AST_ADD };
#include MEMORY_TEXTURE_TYPES
static UINT8 resource[70000];
static size_t resource_size;
static void *rawcache;
static patch_t *cachedpatch;
static UINT8 *cache_slot[2];
static column_t *column_slot[2];
static INT32 width_slot[2];
static size_t texturememory, flatmemory;
void R_CheckTextureCache(INT32 tex);
void R_ReleaseTextureCache(INT32 tex);
column_t *R_GetColumn(fixed_t tex, INT32 col);
void *Picture_TextureToFlat(size_t tex);
static INT32 ASTTextureBlendingThreshold[11] = {0,25,50,75,100,125,150,175,200,225,255};
static unsigned final_resizes, final_moves, post_resizes;
static int force_move;
static void Patch_CalcDataSizes(softwarepatch_t *, size_t *, size_t *);
static void Patch_MakeColumns(softwarepatch_t *, size_t, INT16, UINT8 *, column_t *, post_t *, boolean);
#include MEMORY_TEXTURE_PATCH
static size_t W_LumpLengthPwad(UINT16 w, UINT16 l) { (void)w; (void)l; return resource_size; }
static void *W_TakeLumpNumPwad(UINT16 w, UINT16 l, INT32 tag, void **owner)
{ (void)w; (void)l; (void)tag; (void)owner; check(0,"unexpected raw flat transfer"); return NULL; }
static void *W_CacheLumpNumPwad(UINT16 w, UINT16 l, INT32 tag)
{
    (void)w; (void)l;
    if (!rawcache) { rawcache = Z_Malloc(resource_size, tag, &rawcache); memcpy(rawcache, resource, resource_size); }
    else Z_ChangeTag(rawcache, tag);
    return rawcache;
}
static size_t W_ReadLumpHeaderPwad(UINT16 w, UINT16 l, void *dest, size_t bytes, size_t offset)
{ (void)w; (void)l; memcpy(dest, resource + offset, bytes); return bytes; }
static boolean Picture_IsLumpPNG(const UINT8 *src, size_t bytes)
{ return bytes >= 16 && !memcmp(src, "\211SRPIC\r\n", 8); }
static void *Picture_PNGConvert(const UINT8 *src, pictureformat_t fmt, INT32 *w, INT32 *h,
    INT16 *top, INT16 *left, size_t bytes, size_t *outsize, pictureflags_t flags)
{
    (void)fmt; (void)w; (void)h; (void)top; (void)left; (void)bytes; (void)outsize; (void)flags;
    return Patch_CreateFromDoomPatch((softwarepatch_t *)(src + 8));
}
static void *Picture_Convert(pictureformat_t in, void *src, pictureformat_t out, size_t bytes,
    size_t *outsize, INT32 w, INT32 h, INT32 left, INT32 top, pictureflags_t flags)
{ (void)in; (void)src; (void)out; (void)bytes; (void)outsize; (void)w; (void)h; (void)left; (void)top; (void)flags; check(0,"unexpected flat"); return NULL; }
static void *W_GetCachedPatchNumPwad(UINT16 w, UINT16 l) { (void)w; (void)l; return cachedpatch; }
static void *W_CachePatchNumPwad(UINT16 w, UINT16 l, INT32 tag)
{
    UINT8 *raw = W_CacheLumpNumPwad(w, l, PU_STATIC);
    if (!cachedpatch) { cachedpatch = Patch_CreateFromDoomPatch((softwarepatch_t *)raw); Z_SetUser(cachedpatch, &cachedpatch); }
    Z_ChangeTag(cachedpatch, tag); Z_Free(raw); return cachedpatch;
}
static UINT8 ASTBlendPaletteIndexes(UINT8 bg, UINT8 fg, int style, UINT8 alpha)
{ return (UINT8)(bg * 7u + fg * 13u + (UINT8)style * 3u + alpha); }
static void *tracked_resize(void *p, size_t bytes, INT32 tag, void *owner)
{
    void *barrier = NULL, *q;
    if (owner && force_move)
#ifdef PS2_NOOPT_TEXPLACE
        barrier = Z_Malloc(1024, PU_STATIC, NULL);
#else
        barrier = Z_Malloc(1024, PU_RENDERWORK, NULL);
#endif
    q = Z_ReallocAlign(p, bytes, tag, owner, sizeof(void *));
    if (owner) { final_resizes++; final_moves += q != p; }
    else post_resizes++;
    Z_Free(barrier); return q;
}
#undef Z_Realloc
#define Z_Realloc(p,s,t,u) tracked_resize(p,s,t,u)
#include MEMORY_TEXTURE_RENDERER
#include MEMORY_TEXTURE_FLAT

static void make_resource(boolean cooked, boolean fragmented)
{
    size_t base = cooked ? 8 : 0, off = base + 8 + 64 * 4;
    INT16 width = 64, height = 128;
    memset(resource, 0, sizeof resource);
    if (cooked) memcpy(resource, "\211SRPIC\r\n", 8);
    memcpy(resource + base, &width, 2); memcpy(resource + base + 2, &height, 2);
    for (UINT32 x = 0; x < 64; x++)
    {
        UINT32 rel = (UINT32)(off - base);
        memcpy(resource + base + 8 + x * 4, &rel, 4);
        if (fragmented)
        {
            for (unsigned y = 0; y < 128; y += 2)
            { resource[off++] = (UINT8)y; resource[off++] = 1; resource[off++] = 0;
              resource[off++] = (UINT8)(x * 17 + y); resource[off++] = 0; }
        }
        else
        {
            resource[off++] = 0; resource[off++] = 128; resource[off++] = 0;
            for (unsigned y = 0; y < 128; y++) resource[off++] = (UINT8)(x * 17 + y);
            resource[off++] = 0;
        }
        resource[off++] = 0xff;
    }
    resource_size = off;
}

static void scenario(boolean cooked, int move, size_t budget, boolean fragmented)
{
    texture_t *t;
    texture_t *table[2];
    UINT8 *root;
    zastats_t st;
    uint64_t hash = 1469598103934665603ull;
    arena_reset(budget, ship_sides, ship_prefer, 0); rawcache = NULL; cachedpatch = NULL;
    final_resizes = final_moves = post_resizes = 0; force_move = move;
    make_resource(cooked, fragmented);
    t = calloc(1, sizeof(*t) + 3 * sizeof(texpatch_t)); check(t != NULL, "texture descriptor");
    t->type = TEXTURETYPE_COMPOSITE; t->width = 65; t->height = 129; t->patchcount = 3;
    for (INT32 i = 0; i < 3; i++)
    { t->patches[i].originx = (INT16)(i - 1); t->patches[i].originy = (INT16)(i - 1);
      t->patches[i].flip = (UINT8)i; t->patches[i].style = i ? AST_TRANSLUCENT : AST_COPY; t->patches[i].alpha = 128; }
    if (fragmented)
    {
        t->patchcount = 2;
        for (INT32 i = 0; i < 2; i++)
        { t->patches[i].originx = t->patches[i].originy = 0; t->patches[i].flip = (UINT8)i; }
    }
    table[0] = table[1] = t;
    textures = table; texturecache = cache_slot; texturecolumns = column_slot; texturewidth = width_slot; numtextures = 2;
    width_slot[1] = 65;
    root = R_GenerateTexture(1); consistent(); ZA_Stats(&st);
    for (size_t i = 0; i < 65 * 129; i++) { hash ^= root[i]; hash *= 1099511628211ull; }
    for (INT32 x = 0; x < 65; x++)
    {
        column_t *c = &texturecolumns[1][x];
        hash ^= c->num_posts; hash *= 1099511628211ull;
        for (unsigned j = 0; j < c->num_posts; j++)
        { hash ^= c->posts[j].topdelta; hash *= 1099511628211ull;
          hash ^= c->posts[j].length; hash *= 1099511628211ull;
          hash ^= c->posts[j].data_offset; hash *= 1099511628211ull; }
    }
#ifndef PS2_NOOPT_texreuse
    check(final_resizes == 1, "production pixel root resized once");
#ifdef PS2_NOOPT_TEXPLACE
    check(fragmented || final_moves == (unsigned)move, "production pixel root grows in place / forced move");
#endif
#endif
    printf("texture cooked=%d move=%d hash=%llu peak=%zu resizes=%u moves=%u fragmented=%d allocs=%zu searches=%zu postresizes=%u\n", cooked, move,
        (unsigned long long)hash, st.globalpeak, final_resizes, final_moves, fragmented, st.allocs, st.binsearch, post_resizes);
    {
        UINT8 snapshot[65 * 129];
        unsigned metadata[65][129][3];
        unsigned counts[65];
        memcpy(snapshot, root, sizeof snapshot);
        for (INT32 x = 0; x < 65; x++)
        {
            column_t *c = &column_slot[1][x]; counts[x] = c->num_posts;
            check(counts[x] <= 129, "fixture post count");
            for (unsigned j = 0; j < counts[x]; j++)
            { metadata[x][j][0] = c->posts[j].topdelta; metadata[x][j][1] = c->posts[j].length;
              metadata[x][j][2] = c->posts[j].data_offset; }
        }
        Z_PurgeLock(true);
        R_ReleaseTextureCache(-1); R_ReleaseTextureCache(0); R_ReleaseTextureCache(numtextures);
        check(cache_slot[1] == root, "invalid/unused texture release ignored");
        R_ReleaseTextureCache(1);
        check(cache_slot[1] == root, "release does not free without pressure");
        R_CheckTextureCache(1);
        check(!Z_EvictLRU(SIZE_MAX, false), "refetch protects released texture root");
        R_ReleaseTextureCache(1);
        check(Z_EvictLRU(SIZE_MAX, false) && !cache_slot[1], "released real texture evicted under nested render lock");
        root = R_GetColumn(1, 0)->pixels;
        check(!memcmp(snapshot, root, sizeof snapshot), "getter rebuild preserves every pixel byte");
        for (INT32 x = 0; x < 65; x++)
        {
            column_t *c = R_GetColumn(1, x);
            check(c->num_posts == counts[x], "getter rebuild post counts exact");
            for (unsigned j = 0; j < counts[x]; j++)
                check(c->posts[j].topdelta == metadata[x][j][0] && c->posts[j].length == metadata[x][j][1]
                    && c->posts[j].data_offset == metadata[x][j][2], "getter rebuild every post field exact");
        }
        Z_PurgeLock(false); consistent();
    }
    if (budget >= 100000)
    {
        UINT8 expected_flat[65 * 129];
        UINT8 *flat;
        memset(expected_flat, TRANSPARENTPIXEL, sizeof expected_flat);
        for (INT32 x = 0; x < 65; x++)
        {
            column_t *c = R_GetColumn(1, x);
            for (unsigned p = 0; p < c->num_posts; p++)
                for (unsigned y = 0; y < c->posts[p].length; y++)
                {
                    UINT8 pixel = c->pixels[c->posts[p].data_offset + y];
                    check(c->posts[p].topdelta + y < 129, "post stays inside texture");
                    if (pixel != TRANSPARENTPIXEL)
                        expected_flat[(c->posts[p].topdelta + y) * 65 + x] = pixel;
                }
        }
        Z_PurgeLock(true);
        flat = R_GetFlatForTexture(1);
        check(!memcmp(flat, expected_flat, sizeof expected_flat), "composite flat exact independent reference pixels");
        check(Z_Age(ZA_BLOCK(cache_slot[1])) == 1 && Z_Age(ZA_BLOCK(flat)) == 0,
            "flat conversion releases copied column root and protects independent flat");
        check(Z_EvictLRU(SIZE_MAX, false) && !cache_slot[1] && t->flat == flat,
            "column eviction cannot invalidate active flat pixels");
        R_ReleaseFlatCache(1);
        check(Z_EvictLRU(SIZE_MAX, false) && !t->flat, "finished spans release independent flat root");
        flat = R_GetFlatForTexture(1);
        check(!memcmp(flat, expected_flat, sizeof expected_flat), "flat getter reloads exactly after both caches evicted");
        Z_PurgeLock(false);
        Z_Free(t->flat); consistent();
    }
    Z_Free(cache_slot[1]); check(cache_slot[1] == NULL, "final cache owner clears");
    if (cachedpatch) Patch_Free(cachedpatch);
    cachedpatch = NULL;
    Z_Free(rawcache); empty_zone(); free(t);
}

static void placement_pressure(void)
{
    texture_t *t = calloc(1, sizeof(*t) + 3 * sizeof(texpatch_t)), *table[2];
    void *owners[48] = {0};
    UINT8 snapshot[65 * 129];
    zastats_t st;
    void *workspace;
    check(t != NULL, "placement descriptor");
    arena_reset(1u << 20, ship_sides, ship_prefer, 0);
    rawcache = NULL; cachedpatch = NULL; force_move = 0;
    make_resource(false, false);
    t->type = TEXTURETYPE_COMPOSITE; t->width = 65; t->height = 129; t->patchcount = 3;
    for (INT32 i = 0; i < 3; i++)
    { t->patches[i].originx = t->patches[i].originy = (INT16)(i - 1);
      t->patches[i].flip = (UINT8)i; t->patches[i].style = i ? AST_TRANSLUCENT : AST_COPY; t->patches[i].alpha = 128; }
    table[0] = table[1] = t; textures = table; texturecache = cache_slot; texturecolumns = column_slot;
    texturewidth = width_slot; width_slot[1] = 65; numtextures = 2;
    Z_PurgeLock(true);
    for (unsigned i = 0; i < 48; i++)
    {
        UINT8 *root = R_GenerateTexture(1);
        if (!i) memcpy(snapshot, root, sizeof snapshot);
        else check(!memcmp(snapshot, root, sizeof snapshot), "placement leaves every generated pixel unchanged");
        Z_SetUser(root, &owners[i]); cache_slot[1] = NULL;
        Z_ReleaseCache(root);
        Z_Malloc(4096, PU_STATIC, NULL); /* Persistent renderer bank arriving between image caches. */
    }
    check(Z_EvictLRU(SIZE_MAX, false), "placement caches are evictable after synchronous use");
    for (unsigned i = 0; i < 48; i++) check(!owners[i], "all stable placement owners cleared");
    ZA_Stats(&st);
    workspace = Z_TryMallocAlign(530432, PU_RENDERWORK, NULL, 6);
#ifdef PS2_NOOPT_TEXPLACE
    check(!workspace, "old top placement fragments the remaining mandatory banks");
#else
    check(workspace != NULL, "cache-side placement preserves large contiguous workspace");
#endif
    printf("placement free=%zu largest=%zu workspace530432=%d\n", st.freebytes, st.largestfree, workspace != NULL);
    Z_PurgeLock(false); Z_Free(workspace);
    if (cachedpatch) Patch_Free(cachedpatch);
    Z_Free(rawcache); empty_zone(); free(t);
}

int main(int argc, char **argv)
{
    ship_sides = za_twosided; ship_prefer = za_prefer;
    if (argc > 1) { scenario(false, 0, (size_t)strtoul(argv[1], NULL, 10), argc > 2); return 0; }
    scenario(false, 0, 100000, false); scenario(true, 0, 100000, false); scenario(false, 1, 100000, false);
    scenario(false, 0, 300000, true); scenario(true, 0, 300000, true); scenario(false, 1, 300000, true);
    placement_pressure();
    return 0;
}
