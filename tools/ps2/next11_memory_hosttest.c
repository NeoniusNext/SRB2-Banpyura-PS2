/* Actual slope representation and lifecycle. Geometry arithmetic is unchanged.
 * Save transport, Lua VM and hash sink are explicit controlled boundaries. */
#include <stdint.h>
#ifdef __mips__
#include <malloc.h>
void init_scr(void);
#define aligned_alloc memalign
#endif
typedef uint8_t UINT8;
static int netgame;
static _Bool PS2Lua_InCall(void) { return 0; }
void PS2Spill_Reset(void) {}
#define max(a,b) ((a) > (b) ? (a) : (b))
#include MEMORY_ZONE
typedef uint16_t UINT16;
typedef int32_t fixed_t;
typedef uint32_t angle_t;
typedef int64_t INT64;
typedef uint64_t UINT64;
#define FRACBITS 16
#define FRACUNIT 65536
#define FUNCMATH
#define FUNCINLINE
#define ATTRINLINE inline
#include MEMORY_TYPES
#include MEMORY_FIXED
_Static_assert(sizeof(boolean) == 1 && sizeof(((pslope_t *)0)->moved) == 1, "real production bool width");
_Static_assert(sizeof(((pslope_t *)0)->flags) == 1 && sizeof(((pslope_t *)0)->id) == 2, "full flag/id widths");
static pslope_t *slopelist;
static UINT16 slopecount;
#include MEMORY_SLOPES
/* Independent expected slot counts for the two verified native/EE ABIs.
 * The actual production formula executes inside Slope_Add, not here. */
static unsigned expected_pool_slots(void)
{
    const size_t stride = (sizeof(pslope_t)+15) & ~(size_t)15;
    check(stride == 112 || stride == 128, "verified full-width slope ABI stride");
#if MEMORY_SOURCE_BYTE_POOL && !defined(PS2_NOOPT_SLOPE_POOL_BYTES)
    return stride == 112 ? 73 : 64;
#else
    return 64;
#endif
}
typedef struct { pslope_t *floorspriteslope; } mobj_t;
#include MEMORY_FLOOR

/* The original save format is a sequence of little-endian full 32-bit words. */
typedef struct { UINT8 bytes[64]; size_t cursor, length; } save_t;
#define MD2_FLOORSPRITESLOPE 1u
static void save_word(save_t *s, UINT32 v)
{
    check(s->cursor + 4 <= sizeof s->bytes, "save output bound");
    for (unsigned i = 0; i < 4; i++) s->bytes[s->cursor++] = (UINT8)(v >> (8*i));
    if (s->cursor > s->length) s->length = s->cursor;
}
static UINT32 read_word(save_t *s)
{
    check(s->cursor + 4 <= s->length, "save input bound");
    UINT32 v = 0;
    for (unsigned i = 0; i < 4; i++) v |= (UINT32)s->bytes[s->cursor++] << (8*i);
    return v;
}
#define P_WriteFixed(s,v) save_word((s),(UINT32)(v))
#define P_WriteAngle(s,v) save_word((s),(UINT32)(v))
#define P_ReadFixed(s) ((fixed_t)read_word(s))
#define P_ReadAngle(s) ((angle_t)read_word(s))
#include MEMORY_SAVE

/* Run the real Lua getter with a controlled option/stack service. */
typedef struct { pslope_t *object; int option, kind; UINT32 value; const void *pointer; } lua_State;
#define META_SLOPE "slope"
#define META_VECTOR3 "vector3"
#define META_VECTOR2 "vector2"
static int slope_fields_ref;
static void *luaL_checkudata(lua_State *L, int arg, const char *meta)
{ check(arg == 1 && !strcmp(meta, META_SLOPE), "Lua slope userdata lookup"); return &L->object; }
static int Lua_optoption(lua_State *L, int arg, int fallback, int ref)
{ (void)fallback; (void)ref; check(arg == 2, "Lua field option"); return L->option; }
static int luaL_error(lua_State *L, const char *message, ...)
{ (void)L; (void)message; return -1; }
static void lua_pushboolean(lua_State *L, int value) { L->kind = 1; L->value = value != 0; }
static void lua_pushfixed(lua_State *L, fixed_t value) { L->kind = 2; L->value = (UINT32)value; }
static void lua_pushangle(lua_State *L, angle_t value) { L->kind = 3; L->value = value; }
static void lua_pushinteger(lua_State *L, int value) { L->kind = 4; L->value = (UINT32)value; }
static void LUA_PushUserdata(lua_State *L, const void *p, const char *meta)
{ check(!strcmp(meta, META_VECTOR3) || !strcmp(meta, META_VECTOR2), "Lua vector userdata"); L->kind = 5; L->pointer = p; }
#include MEMORY_LUA
typedef struct { UINT32 words[16]; unsigned n; } gcfp_t;
static void gcw(gcfp_t *h, UINT32 value)
{ check(h->n < 16, "hardware fingerprint sink bound"); h->words[h->n++] = value; }
#include MEMORY_RENDER

static FILE *snapshot_file;
static unsigned cases;
static pslope_t anchors[4];
/* Borrowed identity witnesses for the common64x73 construction boundary. */
static pslope_t *failure_pointers[4672];
static void word(UINT32 value)
{
    UINT8 bytes[4] = {(UINT8)value, (UINT8)(value>>8), (UINT8)(value>>16), (UINT8)(value>>24)};
    check(fwrite(bytes, 1, sizeof bytes, snapshot_file) == sizeof bytes, "literal snapshot write");
}
static void double_word(const double *v)
{ UINT64 bits; memcpy(&bits, v, sizeof bits); word((UINT32)bits); word((UINT32)(bits >> 32)); }
static UINT64 double_bits(const double *v) { UINT64 bits; memcpy(&bits, v, sizeof bits); return bits; }
static void set_double(double *v, UINT64 bits) { memcpy(v, &bits, sizeof bits); }
static UINT32 mixed(UINT32 x)
{ x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16); }
static void fill(pslope_t *s, UINT32 i)
{
    static const UINT64 special[] = {0, 0x8000000000000000ull, 1, 0xfffffffffffffull,
        0x3ff0000000000000ull, 0xbff0000000000000ull, 0x7ff0000000000000ull,
        0xfff0000000000000ull, 0x7ff80000deadbeefull, 0x7ff0000000000001ull};
    s->id = (UINT16)mixed(i + 1); s->next = &anchors[i % 4];
    s->o.x = (fixed_t)mixed(i+2); s->o.y = (fixed_t)mixed(i+3); s->o.z = (fixed_t)mixed(i+4);
    s->normal.x = (fixed_t)mixed(i+5); s->normal.y = (fixed_t)mixed(i+6); s->normal.z = (fixed_t)mixed(i+7);
    s->d.x = (fixed_t)mixed(i+8); s->d.y = (fixed_t)mixed(i+9);
    s->zdelta = (fixed_t)mixed(i+10); s->zangle = mixed(i+11); s->xydirection = mixed(i+12);
    double *d[7] = {&s->dorigin.x, &s->dorigin.y, &s->dorigin.z, &s->dnormdir.x, &s->dnormdir.y, &s->dnormdir.z, &s->dzdelta};
    for (unsigned j = 0; j < 7; j++)
        set_double(d[j], (i % 3) ? special[(i+j) % (sizeof special/sizeof special[0])] :
                   ((UINT64)mixed(i+j+13) << 32) | mixed(i+j+29));
    s->moved = (i & 1) != 0; s->flags = (UINT8)i;
}
static void same(const pslope_t *a, const pslope_t *b)
{
#define EQ(f) check(a->f == b->f, "full named slope field " #f)
    EQ(id); EQ(next); EQ(o.x); EQ(o.y); EQ(o.z); EQ(normal.x); EQ(normal.y); EQ(normal.z);
    EQ(d.x); EQ(d.y); EQ(zdelta); EQ(zangle); EQ(xydirection); EQ(moved); EQ(flags);
#undef EQ
#define DB(f) check(double_bits(&a->f) == double_bits(&b->f), "full double bits " #f)
    DB(dorigin.x); DB(dorigin.y); DB(dorigin.z); DB(dnormdir.x); DB(dnormdir.y); DB(dnormdir.z); DB(dzdelta);
#undef DB
}
static void snapshot(const pslope_t *s, UINT32 nextid)
{
    word(s->id); word(nextid); word((UINT32)s->o.x); word((UINT32)s->o.y); word((UINT32)s->o.z);
    word((UINT32)s->normal.x); word((UINT32)s->normal.y); word((UINT32)s->normal.z);
    word((UINT32)s->d.x); word((UINT32)s->d.y); word((UINT32)s->zdelta); word(s->zangle); word(s->xydirection);
    double_word(&s->dorigin.x); double_word(&s->dorigin.y); double_word(&s->dorigin.z);
    double_word(&s->dnormdir.x); double_word(&s->dnormdir.y); double_word(&s->dnormdir.z); double_word(&s->dzdelta);
    word(s->moved); word(s->flags);
}
static void lua_check(pslope_t *s)
{
    lua_State L = {0}; L.object = s;
    static const int opts[] = {slope_valid,slope_o,slope_d,slope_zdelta,slope_normal,slope_zangle,slope_xydirection,slope_flags};
    for (unsigned i = 0; i < sizeof opts/sizeof opts[0]; i++)
    {
        L.option = opts[i]; L.kind = 0; L.value = 0; L.pointer = NULL;
        check(slope_get(&L) == 1, "actual Lua getter returns one value");
        if (L.option == slope_o) check(L.kind == 5 && L.pointer == &s->o, "Lua origin pointer identity");
        else if (L.option == slope_d) check(L.kind == 5 && L.pointer == &s->d, "Lua direction pointer identity");
        else if (L.option == slope_normal) check(L.kind == 5 && L.pointer == &s->normal, "Lua normal pointer identity");
        else if (L.option == slope_valid) check(L.kind == 1 && L.value == 1, "Lua valid");
        else if (L.option == slope_zdelta) check(L.kind == 2 && L.value == (UINT32)s->zdelta, "Lua full zdelta");
        else if (L.option == slope_zangle) check(L.kind == 3 && L.value == s->zangle, "Lua full zangle");
        else if (L.option == slope_xydirection) check(L.kind == 3 && L.value == s->xydirection, "Lua full direction");
        else check(L.kind == 4 && L.value == s->flags, "Lua full flags");
    }
    L.object = NULL; L.option = slope_valid;
    check(slope_get(&L) == 1 && L.value == 0, "Lua invalid userdata validity");
    L.option = slope_flags; check(slope_get(&L) == -1, "Lua invalid userdata error");
}
static void begin(int sides, int guard)
{ arena_reset(16u << 20, sides, ship_prefer, guard); slopelist = NULL; slopecount = 0; }
static void finish(unsigned kind, size_t count, size_t request, size_t chunks)
{
    zastats_t st; consistent(); ZA_Stats(&st);
    printf("MEMORY11 case=%u kind=%u n=%zu request=%zu used=%zu peak=%zu chunks=%zu\n",
        cases++, kind, count, request, st.used, st.globalpeak, chunks);
    Z_FreeTags(PU_LEVEL, PU_LEVEL); slopelist = NULL; slopecount = 0;
    consistent(); check(Z_TotalUsage() == 0, "complete level cleanup");
}
static void table_case(size_t count, int sides, int guard)
{
    begin(sides, guard);
    pslope_t *values = Z_Malloc(count * sizeof *values, PU_LEVEL, NULL);
    UINT8 *neighbor = Z_Malloc(113, PU_LEVEL, NULL); memset(neighbor, 0x5d, 113);
    pslope_t *copy = Z_Malloc(count * sizeof *copy, PU_LEVEL, NULL);
    memset(values, 0xa7, count * sizeof *values); memset(copy, 0x69, count * sizeof *copy);
    for (size_t i = 0; i < count; i++) fill(&values[i], (UINT32)i);
    M_Memcpy(copy, values, count * sizeof *copy);
    for (size_t i = 0; i < count; i++)
    {
        pslope_t assigned = copy[i]; same(&values[i], &copy[i]); same(&values[i], &assigned);
        check(copy[i].next == &anchors[i%4], "full array pointer identity");
        check((uintptr_t)&copy[i].dorigin % 8 == 0 && (uintptr_t)&copy[i].dzdelta % 8 == 0,
              "every array double naturally aligned");
        snapshot(&copy[i], (UINT32)i%4 + 1);
        if (i < 256 || i + 1 == count) lua_check(&copy[i]);
    }
    for (unsigned i = 0; i < 113; i++) check(neighbor[i] == 0x5d, "array neighbor canary");
    finish(0, count, 2 * count * sizeof *copy, 0);
}
static void zero_slope(const pslope_t *s)
{
    pslope_t zero = {0}; same(s, &zero);
}
static void pool_case(size_t count, int sides, int guard)
{
    begin(sides, guard);
    const size_t stride = (sizeof(pslope_t) + 15) & ~(size_t)15;
    const unsigned slots = expected_pool_slots();
    pslope_t *previous = NULL, *chunk = NULL;
    for (size_t i = 0; i < count; i++)
    {
        pslope_t *s = Slope_Add((UINT8)i);
        if (!(i % slots))
        {
            chunk = s;
            check(ZA_TAG(ZA_BLOCK(s)) == PU_LEVEL && ZA_BLOCK(s)->realsize == slots * stride, "actual complete byte-budget pool chunk request");
            check(ZA_BLOCK(s)->realsize <= 8192, "chunk stays inside preceding byte budget");
            check(ZA_BLOCK(s)->user == NULL, "pool chunk retains no owner");
        }
        check(s == (pslope_t *)((UINT8 *)chunk + (i % slots) * stride), "actual pool stride and full capacity");
        check((uintptr_t)s % 16 == 0, "every pool slot sixteen aligned");
        check(s->id == (UINT16)(i + 1) && s->flags == (UINT8)i && s->next == previous, "actual constructor full id/flag/list");
        pslope_t expected = {0}; expected.id = (UINT16)(i+1); expected.flags = (UINT8)i; expected.next = previous;
        same(s, &expected); lua_check(s);
        pslope_t filled; memset(&filled, 0xa7, sizeof filled); fill(&filled, (UINT32)i);
        UINT16 id = s->id; pslope_t *next = s->next; UINT8 flag = s->flags;
        *s = filled; s->id = id; s->next = next; s->flags = flag;
        check(s->next == previous, "pool typed assignment preserves exact list pointer");
        snapshot(s, i ? (UINT32)i : 0);
        check(SaveSlope(s) == (UINT16)(i+1), "actual save slope full id");
        previous = s;
    }
    check(slopelist == previous && slopecount == (UINT16)count, "published slope list/count");
    static const UINT16 ids[] = {0,1,2,63,64,65,127,128,129,2089,32767,32768,65535};
    for (size_t i = 0; i < sizeof ids/sizeof ids[0]; i++)
    {
        pslope_t *expected = slopelist;
        while (expected && expected->id != ids[i]) expected = expected->next;
        check(P_SlopeById(ids[i]) == expected, "actual id lookup preserves full ids and list order");
        word(expected != NULL); if (expected) word(expected->id);
    }
    check(SaveSlope(NULL) == UINT32_MAX, "actual absent slope save sentinel");
    finish(1, count, ((count + slots - 1)/slots) * slots * stride, (count+slots-1)/slots);
}
static void floor_case(int sides, int guard)
{
    begin(sides, guard); mobj_t m = {0};
    pslope_t expected = {0}; expected.normal.z = FRACUNIT;
    check(P_CreateFloorSpriteSlope(&m) == m.floorspriteslope, "floor constructor returns actual owner");
    same(m.floorspriteslope, &expected);
    fill(m.floorspriteslope, 0x12345678u);
    P_CreateFloorSpriteSlope(&m); same(m.floorspriteslope, &expected);
    for (unsigned i = 0; i < 257; i++)
    {
        pslope_t value; memset(&value, 0xa7, sizeof value); fill(&value, i);
        *m.floorspriteslope = value;
        save_t save = {{0}, 0, 0}; actual_save_floor(&save, &m, MD2_FLOORSPRITESLOPE);
        check(save.length == 44, "unchanged saved floor slope word count");
        for (size_t j = 0; j < save.length; j++) word(save.bytes[j]);
        save.cursor = 0; actual_load_floor(&save, &m, MD2_FLOORSPRITESLOPE);
        expected = (pslope_t){0}; expected.zdelta = value.zdelta; expected.zangle = value.zangle; expected.xydirection = value.xydirection;
        expected.o = value.o; expected.d = value.d; expected.normal = value.normal; expected.moved = true;
        same(m.floorspriteslope, &expected); check(save.cursor == save.length, "complete saved floor slope read");
        snapshot(m.floorspriteslope, 0); lua_check(m.floorspriteslope);
    }
    zastats_t st; consistent(); ZA_Stats(&st);
    word(m.floorspriteslope->moved); P_RemoveFloorSpriteSlope(&m); check(!m.floorspriteslope, "floor remove clears owner");
    P_RemoveFloorSpriteSlope(&m); check(!m.floorspriteslope, "empty floor removal");
    check(Z_TotalUsage() == 0, "floor replacement/removal frees all roots");
    finish(2, 257, sizeof(pslope_t), 0);
}
static void query_case(int sides, int guard)
{
    begin(sides, guard);
    for (UINT32 i = 0; i < 4096; i++)
    {
        pslope_t s = {0};
        s.o.x = (fixed_t)(mixed(i+1) % 0x2000000u) - 0x1000000;
        s.o.y = (fixed_t)(mixed(i+2) % 0x2000000u) - 0x1000000;
        s.o.z = (fixed_t)(mixed(i+3) % 0x2000000u) - 0x1000000;
        s.d.x = (fixed_t)(mixed(i+4) % 131073u) - 65536;
        s.d.y = (fixed_t)(mixed(i+5) % 131073u) - 65536;
        s.zdelta = (fixed_t)(mixed(i+6) % 131073u) - 65536;
        s.normal.x = (fixed_t)mixed(i+7); s.normal.y = (fixed_t)mixed(i+8);
        const fixed_t x = (fixed_t)(mixed(i+9) % 0x2000000u) - 0x1000000;
        const fixed_t y = (fixed_t)(mixed(i+10) % 0x2000000u) - 0x1000000;
        s.dorigin.x = s.o.x / (double)FRACUNIT; s.dorigin.y = s.o.y / (double)FRACUNIT; s.dorigin.z = s.o.z / (double)FRACUNIT;
        s.dnormdir.x = s.d.x / (double)FRACUNIT; s.dnormdir.y = s.d.y / (double)FRACUNIT; s.dzdelta = s.zdelta / (double)FRACUNIT;
        fixed_t z = P_GetSlopeZAt(&s, x, y); word((UINT32)z);
        check(P_GetZAt(&s, x, y, 0x12345678) == z, "actual slope selection query");
        check(P_GetZAt(NULL, x, y, 0x12345678) == 0x12345678, "actual null fallback query");
        double dz = R_GetSlopeZAt(&s, x, y); double_word(&dz);
        gcfp_t hash = {{0}, 0}; gc_slope(&hash, &s);
        check(hash.n == 8 && hash.words[0] == (UINT32)s.o.x && hash.words[7] == (UINT32)s.normal.y, "hardware named slope fingerprint");
        for (unsigned j = 0; j < hash.n; j++) word(hash.words[j]);
    }
    gcfp_t hash = {{0}, 0}; gc_slope(&hash, NULL); check(hash.n == 1 && hash.words[0] == 0x51a9e001u, "hardware null slope fingerprint");
    word(hash.words[0]); finish(3, 4096, 0, 0);
}
static void pool_epoch_case(int sides, int guard)
{
    begin(sides, guard); zlevelpool_t pool = {0};
    const size_t stride = (sizeof(pslope_t)+15) & ~(size_t)15;
    const unsigned slots = expected_pool_slots();
    pslope_t *first = Z_LevelPoolAlloc(&pool, sizeof(pslope_t), slots); zero_slope(first);
    fill(first, 101); UINT32 epoch = pool.epoch; check(pool.used == 1, "pool starts first slot");
    Z_FreeTags(PU_LEVEL, PU_LEVEL); check(zlevelepoch != epoch, "real level epoch invalidation");
    pslope_t *next = Z_LevelPoolAlloc(&pool, sizeof(pslope_t), slots); zero_slope(next);
    check(pool.used == 1 && pool.epoch == zlevelepoch, "new level resets pool count/epoch");
    check(ZA_BLOCK(next)->realsize == slots * stride, "epoch replacement keeps complete chunk");
    snapshot(next, 0); finish(4, 2, 2*slots*stride, 2);
}
static void pool_failure_case(int sides, int guard)
{
    begin(sides, guard); zlevelpool_t pool = {0};
    const unsigned slots = expected_pool_slots();
    for (unsigned i = 0; i < slots; i++) zero_slope(Z_LevelPoolAlloc(&pool, sizeof(pslope_t), slots));
    zlevelpool_t saved = pool;
    const size_t stride = (sizeof(pslope_t)+15) & ~(size_t)15;
    void *blockers[32]; unsigned nblockers = 0;
    for (;;)
    {
        zastats_t free_state; ZA_Stats(&free_state);
        if (free_state.largestfree < slots * stride) break;
        check(nblockers < 32, "bounded geometric arena fill");
        blockers[nblockers++] = Z_Malloc(free_state.largestfree/2, PU_LEVEL, NULL);
    }
    expecting_error = 1;
    if (!setjmp(error_jump)) { Z_LevelPoolAlloc(&pool, sizeof(pslope_t), slots); check(0, "full pool failure expected"); }
    expecting_error = 0;
    check(pool.chunk == saved.chunk && pool.used == saved.used && pool.epoch == saved.epoch, "allocation failure preserves unpublished pool state");
    for (unsigned i = 0; i < nblockers; i++) Z_Free(blockers[i]);
    pslope_t *retry = Z_LevelPoolAlloc(&pool, sizeof(pslope_t), slots); zero_slope(retry);
    check(pool.used == 1 && pool.chunk == retry && pool.epoch == zlevelepoch, "failure retry publishes complete new chunk");
    snapshot(retry, 0); finish(5, 1, 0, 2);
}
static void constructor_failure_case(int sides, int guard)
{
    begin(sides, guard);
    const unsigned count = 4672, slots = expected_pool_slots();
    const size_t stride = (sizeof(pslope_t)+15) & ~(size_t)15;
    check(count % slots == 0, "common fixed count fills both64 and73 slot chunks");
    pslope_t *previous = NULL;
    for (unsigned i = 0; i < count; i++)
    {
        pslope_t *s = Slope_Add((UINT8)i);
        pslope_t expected = {0}; expected.id = (UINT16)(i+1); expected.flags = (UINT8)i; expected.next = previous;
        same(s, &expected); failure_pointers[i] = s; previous = s;
    }
    pslope_t *saved_list = slopelist; UINT16 saved_count = slopecount;
    void *blockers[32]; unsigned nblockers = 0;
    for (;;)
    {
        zastats_t free_state; ZA_Stats(&free_state);
        if (free_state.largestfree < slots * stride) break;
        check(nblockers < 32, "bounded geometric constructor arena fill");
        blockers[nblockers++] = Z_Malloc(free_state.largestfree/2, PU_LEVEL, NULL);
    }
    expecting_error = 1;
    if (!setjmp(error_jump)) { Slope_Add(0xe5); check(0, "actual constructor must fail before publication"); }
    expecting_error = 0;
    check(slopelist == saved_list && slopecount == saved_count && saved_count == count,
          "actual constructor failure preserves complete published list and count");
    word(slopecount);
    pslope_t *walk = slopelist;
    for (unsigned i = count; i; i--)
    {
        check(walk == failure_pointers[i-1], "every preexisting slope retains exact pointer identity after failure");
        pslope_t expected = {0}; expected.id = (UINT16)i; expected.flags = (UINT8)(i-1);
        expected.next = i > 1 ? failure_pointers[i-2] : NULL;
        same(walk, &expected); snapshot(walk, i-1); walk = walk->next;
    }
    check(walk == NULL, "complete fixed-count failure list terminates");
    for (unsigned i = 0; i < nblockers; i++) Z_Free(blockers[i]);
    pslope_t *retry = Slope_Add(0xe5), expected = {0};
    expected.id = count+1; expected.flags = 0xe5; expected.next = saved_list;
    same(retry, &expected);
    check(slopelist == retry && slopecount == count+1 && ZA_BLOCK(retry)->realsize == slots * stride,
          "actual constructor retry publishes exactly the next full record in a complete new chunk");
    snapshot(retry, count); word(slopecount); lua_check(retry);
    finish(6, count+1, ((count+1+slots-1)/slots)*slots*stride, (count+1+slots-1)/slots);
    memset(failure_pointers, 0, sizeof failure_pointers);
}
int main(int argc, char **argv)
{
    static const size_t arrays[] = {1,2,3,7,31,64,65,256,8192,32768};
    static const size_t pools[] = {1,2,3,7,31,63,64,65,72,73,74,127,128,129,2089,65537};
#ifdef __mips__
    init_scr(); check(ZA_HDR == 16, "EE arena header ABI");
#endif
    check(argc == 2, "usage snapshot"); snapshot_file = fopen(argv[1], "wb"); check(snapshot_file != NULL, "snapshot open");
    printf("MEMORY11 BEGIN\n"); fflush(stdout); ship_sides = za_twosided; ship_prefer = za_prefer;
    for (int sides = 0; sides < 2; sides++) for (int guard = 0; guard < 2; guard++)
    {
        for (size_t i = 0; i < sizeof arrays/sizeof arrays[0]; i++) table_case(arrays[i], sides, guard);
        for (size_t i = 0; i < sizeof pools/sizeof pools[0]; i++) pool_case(pools[i], sides, guard);
        floor_case(sides, guard); query_case(sides, guard); pool_epoch_case(sides, guard); pool_failure_case(sides, guard);
        constructor_failure_case(sides, guard);
    }
    memory_backing_check(); check(fclose(snapshot_file) == 0, "snapshot close");
    const unsigned slots = expected_pool_slots();
    printf("MEMORY11 DONE cases=%u checks=%u sizeof=%zu align=%zu stride=%zu slots=%u\n", cases, checks,
        sizeof(pslope_t), _Alignof(pslope_t), (sizeof(pslope_t)+15)&~(size_t)15, slots);
#ifndef __mips__
    if (arena_raw) { aligned_release(arena_raw); arena_raw = NULL; }
#endif
    return 0;
}
