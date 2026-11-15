/* Actual vertex loaders/default loop/consumers with the unchanged real zone.
 * Resource/tokenizer, Lua VM and slope geometry are explicit boundaries.
 * Literal snapshots preserve logical fields and indices, excluding padding. */
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
#include MEMORY_ZONE
#include <stddef.h>
#include <stdbool.h>
/* The allocator fixture's stand-in is int. Production boolean is C bool:
 * use its actual one-byte representation in every extracted engine type. */
#define boolean bool
typedef int16_t INT16;
typedef uint16_t UINT16;
typedef int64_t INT64;
typedef uint64_t UINT64;
typedef INT32 fixed_t;
typedef uint32_t angle_t;
#define FUNCMATH
#define FUNCINLINE
#define ATTRINLINE
#define FRACBITS 16
#define FRACUNIT 65536
#define FLOAT_TO_FIXED FloatToFixed
#define FIXED_TO_FLOAT FixedToFloat
#include MEMORY_TYPES
#include MEMORY_MATH
#include "m_swap.h"
#include "byteptr.h"
typedef struct { INT16 x, y; } __attribute__((packed)) mapvertex_t;
typedef struct { vertex_t *v1, *v2; } line_t;
typedef struct { vertex_t *v1, *v2; } seg_t;
typedef struct { fixed_t x, y; } vector2_t;
#define MAXFFLOORS 40
#define MAXVIDWIDTH 640
#include MEMORY_DRAWSEG
typedef struct { fixed_t x, y, z; } vector3_t;
typedef struct { vector3_t points[3]; } pslope_t;
typedef struct {
    size_t linecount;
    line_t **lines;
    fixed_t floorheight, ceilingheight;
    pslope_t *f_slope, *c_slope;
    boolean hasslope;
} sector_t;
typedef struct {
    size_t numVertices, numVerticesAlloc;
    vertex_t *origVerts, *tmpVerts, **vertices;
} polyobj_t;
static size_t numvertexes, numlines, numsectors;
static vertex_t *vertexes;
static line_t *lines;
static sector_t *sectors;
static unsigned diagnostic_count;
enum { CONS_WARNING };
static void CONS_Alert(int kind, const char *format, ...)
{ (void)kind; (void)format; diagnostic_count++; }
#define TMNAME(p, lit) ((p)[0] == (lit)[0] && (p)[1] == (lit)[1] && !strcmp((p), (lit)))
#include MEMORY_LOAD
static unsigned parse_mode;
static FILE *stock_input;
static UINT32 read_word(FILE *stream)
{
    unsigned char bytes[4];
    check(fread(bytes, 1, 4, stream) == 4, "manifest word");
    return (UINT32)bytes[0] | ((UINT32)bytes[1] << 8) | ((UINT32)bytes[2] << 16) | ((UINT32)bytes[3] << 24);
}
static char *read_string(FILE *stream)
{
    UINT32 n = read_word(stream);
    char *out = malloc((size_t)n + 1);
    check(n < 4096 && out, "manifest string bounds");
    check(fread(out, 1, n, stream) == n, "manifest string bytes");
    out[n] = 0;
    return out;
}
static void fixture_parse(UINT32 i, void (*parser)(UINT32, const char *, const char *))
{
    static const char *const values[] = {"0", "-32768", "32767.5", "0.0000152587890625",
        "-0.0000152587890625", "32768", "-32768.5", "2147483647", "nan", "inf", "-inf", " 123.25"};
    if (parse_mode == 100)
    {
        UINT32 count = read_word(stock_input);
        for (UINT32 p = 0; p < count; p++)
        {
            char *name = read_string(stock_input), *value = read_string(stock_input);
            parser(i, name, value);
            free(name); free(value);
        }
        return;
    }
    /* The production default loop must initialize hostile preceding bytes. */
    check(vertexes[i].x == INT32_MAX && vertexes[i].y == INT32_MAX
        && vertexes[i].floorzset == false && vertexes[i].ceilingzset == false
        && vertexes[i].floorz == 0 && vertexes[i].ceilingz == 0, "actual UDMF defaults");
    if (parse_mode == 1) { parser(i, "y", "1"); return; }
    if (parse_mode == 2) { parser(i, "x", "1"); return; }
    parser(i, "x", values[i % 12]);
    parser(i, "y", values[(i+5) % 12]);
    if (i & 1) parser(i, "zfloor", values[(i+7) % 12]);
    if (i & 2) parser(i, "zceiling", values[(i+11) % 12]);
    parser(i, "unrecognized", "999999999");
    if (i & 4) { parser(i, "x", "7.5"); parser(i, "x", "-8.25"); }
    if (i & 8) { parser(i, "zfloor", "-2"); parser(i, "zfloor", "32767.5"); }
}
#define TEXTMAP_PARSE(kind, blocks, i, parser) fixture_parse((i), (parser))
#include MEMORY_TEXTMAP
#include MEMORY_HASH

/* Slope geometry service captures every exact input delivered by real selection. */
static pslope_t slope_records[8];
static size_t slope_count;
static pslope_t *Slope_Add(unsigned flags)
{ check(!flags && slope_count < 8, "slope record boundary"); return &slope_records[slope_count++]; }
static void ReconfigureViaVertexes(pslope_t *slope, vector3_t a, vector3_t b, vector3_t c)
{ slope->points[0] = a; slope->points[1] = b; slope->points[2] = c; }
#include MEMORY_SLOPE

/* Read-only Lua userdata getter service; the actual getter and index run intact. */
typedef struct {
    vertex_t *vertex;
    unsigned field, kind;
    int64_t value;
    char error[128];
} lua_State;
#define META_VERTEX "vertex_t"
static int vertex_fields_ref;
static void *luaL_checkudata(lua_State *L, int i, const char *meta)
{ check(i == 1 && !strcmp(meta, META_VERTEX), "Lua userdata metatable"); return &L->vertex; }
static unsigned Lua_optoption(lua_State *L, int index, int fallback, int reference)
{ (void)fallback; (void)reference; check(index == 2, "Lua field argument"); return L->field; }
static int luaL_error(lua_State *L, const char *message)
{ snprintf(L->error, sizeof L->error, "%s", message); return -1; }
static void lua_pushboolean(lua_State *L, int b) { L->kind = 1; L->value = !!b; }
static void lua_pushfixed(lua_State *L, fixed_t f) { L->kind = 2; L->value = f; }
static void lua_pushinteger(lua_State *L, ptrdiff_t n) { L->kind = 3; L->value = n; }
#include MEMORY_LUA

/* Only the unchanged rotation table/multiply service is controlled. */
static fixed_t fixture_cos(angle_t a) { static const fixed_t q[] = {FRACUNIT, 0, -FRACUNIT, 0}; return q[a & 3]; }
static fixed_t fixture_sin(angle_t a) { return fixture_cos((a + 3) & 3); }
#define FINECOSINE(a) fixture_cos(a)
#define FINESINE(a) fixture_sin(a)
static fixed_t FixedMul(fixed_t a, fixed_t b) { return (fixed_t)(((int64_t)a * b) >> FRACBITS); }
#include MEMORY_POLY
static drawseg_t *drawsegs, *ds_p, *curdrawsegs, *firstseg;
static size_t maxdrawsegs;
static INT32 viewwidth;
#include MEMORY_DRAWGROWTH
#include MEMORY_DRAWLIFE
#define PS2_OPT_SEGS
#define RQ_INLINE static inline
#define RQ_MAX INT64_MAX
static UINT32 PS2_Clz(UINT32 v) { return v ? (UINT32)__builtin_clz(v) : 32; }
#include MEMORY_RAY
static angle_t xtoviewangle[MAXVIDWIDTH], viewangle;
static fixed_t viewx, viewy;
static seg_t *curline;
static sector_t *frontsector, *backsector;
#define ANGLETOFINESHIFT 19
#include MEMORY_DRAWSTORE
static FILE *snapshot_file;
static unsigned cases, stockmaps, stockvertices;
static void word(UINT32 value)
{
    unsigned char bytes[4] = {(UINT8)value, (UINT8)(value >> 8), (UINT8)(value >> 16), (UINT8)(value >> 24)};
    check(fwrite(bytes, 1, sizeof bytes, snapshot_file) == sizeof bytes, "literal snapshot write");
}
static void snapshot(const vertex_t *v)
{ word(v->x); word(v->y); word(v->floorzset); word(v->ceilingzset); word(v->floorz); word(v->ceilingz); }
static void same(const vertex_t *a, const vertex_t *b)
{
    check(a->x == b->x && a->y == b->y && a->floorzset == b->floorzset && a->ceilingzset == b->ceilingzset
        && a->floorz == b->floorz && a->ceilingz == b->ceilingz, "all logical vertex fields equal");
}
static void fill(vertex_t *v, size_t i)
{
    static const UINT32 edge[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0xffff, 0x10000, 0x80000001};
    v->x = (fixed_t)(0x9e3779b9u * (UINT32)i + 0x80000000u);
    v->y = (fixed_t)(0xf1357ae1u * (UINT32)i + 0x7fffffffu);
    v->floorzset = (i & 1) != 0; v->ceilingzset = (i & 2) != 0;
    v->floorz = (fixed_t)(i < 8 ? edge[i] : 0xa511e9b3u * (UINT32)i);
    v->ceilingz = (fixed_t)(i < 8 ? edge[7-i] : 0x63d83595u * (UINT32)i);
}
static void lua_check(vertex_t *v, size_t index)
{
    static const unsigned kinds[] = {1, 2, 2, 2, 1, 2, 1};
    int64_t expected[] = {1, v->x, v->y, v->floorz, v->floorzset, v->ceilingz, v->ceilingzset};
    lua_State L = {0}; L.vertex = v;
    for (unsigned f = 0; f <= vertex_ceilingzset; f++)
    {
        L.field = f;
        check(vertex_get(&L) == 1 && L.kind == kinds[f] && L.value == expected[f], "actual Lua getter preserves value/type");
    }
    check(vertex_num(&L) == 1 && L.kind == 3 && L.value == (int64_t)index, "actual Lua index preserves typed stride");
}
static void begin(int sides, int guard)
{ arena_reset(16u << 20, sides, ship_prefer, guard); numvertexes = numlines = numsectors = 0; vertexes = NULL; lines = NULL; sectors = NULL; }
static void finish(unsigned kind, size_t count, size_t request, unsigned extra)
{
    zastats_t st;
    consistent(); ZA_Stats(&st);
    printf("MEMORY10 case=%u kind=%u n=%zu request=%zu used=%zu peak=%zu extra=%u\n",
        cases++, kind, count, request, st.used, st.globalpeak, extra);
    empty_zone();
    vertexes = NULL; lines = NULL; sectors = NULL;
}
static void table_case(size_t count, int sides, int guard)
{
    vertex_t *values, *copy;
    UINT8 *neighbor;
    begin(sides, guard);
    values = Z_Malloc(count * sizeof *values, PU_LEVEL, NULL);
    neighbor = Z_Malloc(113, PU_LEVEL, NULL); memset(neighbor, 0x5d, 113);
    copy = Z_Malloc(count * sizeof *copy, PU_LEVEL, NULL);
    memset(values, 0xa7, count * sizeof *values); memset(copy, 0x69, count * sizeof *copy);
    for (size_t i = 0; i < count; i++) fill(&values[i], i);
    memcpy(copy, values, count * sizeof *copy);
    vertexes = copy; numvertexes = count;
    for (size_t i = 0; i < count; i++)
    {
        same(&values[i], &copy[i]); lua_check(&copy[i], i); snapshot(&copy[i]);
        check((uintptr_t)&copy[i].x % _Alignof(fixed_t) == 0 && (uintptr_t)&copy[i].floorz % _Alignof(fixed_t) == 0
            && (uintptr_t)&copy[i].ceilingz % _Alignof(fixed_t) == 0, "full fixed fields naturally aligned in every array element");
    }
    ps2lp_hash_t hs = actual_vertex_hash(); word(hs.h[0]); word(hs.h[1]);
    for (unsigned i = 0; i < 113; i++) check(neighbor[i] == 0x5d, "neighbor canary");
    finish(0, count, 2 * count * sizeof *copy, 0);
}
static void doom_case(int sides, int guard)
{
    mapvertex_t source[257];
    begin(sides, guard); numvertexes = 257;
    vertexes = Z_Malloc(numvertexes * sizeof *vertexes, PU_LEVEL, NULL);
    memset(vertexes, 0xa7, numvertexes * sizeof *vertexes);
    for (size_t i = 0; i < numvertexes; i++) { source[i].x = (INT16)(i * 257u); source[i].y = (INT16)(0x8000u + i*251u); }
    P_LoadVertices((UINT8 *)source);
    for (size_t i = 0; i < numvertexes; i++)
    {
        check((UINT32)vertexes[i].x == (UINT32)(INT32)source[i].x * FRACUNIT
            && (UINT32)vertexes[i].y == (UINT32)(INT32)source[i].y * FRACUNIT, "traditional full signed coordinates");
        check(!vertexes[i].floorzset && !vertexes[i].ceilingzset && !vertexes[i].floorz && !vertexes[i].ceilingz, "traditional height defaults");
        snapshot(&vertexes[i]); lua_check(&vertexes[i], i);
    }
    finish(1, numvertexes, numvertexes * sizeof *vertexes, 0);
}
static void text_case(int sides, int guard)
{
    begin(sides, guard); numvertexes = 96;
    vertexes = Z_Malloc(numvertexes * sizeof *vertexes, PU_LEVEL, NULL);
    memset(vertexes, 0xa7, numvertexes * sizeof *vertexes);
    parse_mode = 0; actual_textmap_vertices();
    for (size_t i = 0; i < numvertexes; i++)
    {
        check(vertexes[i].floorzset == !!((i & 1) || (i & 8)) && vertexes[i].ceilingzset == !!(i & 2), "UDMF actual presence flags");
        snapshot(&vertexes[i]); lua_check(&vertexes[i], i);
    }
    for (unsigned missing = 1; missing <= 2; missing++)
    {
        parse_mode = missing; expecting_error = 1;
        if (!setjmp(error_jump)) { actual_textmap_vertices(); check(0, "missing UDMF coordinate should fail"); }
        expecting_error = 0;
        check(strstr(error_text, missing == 1 ? "no x value" : "no y value") != NULL, "unchanged missing-coordinate diagnostic");
    }
    lua_State L = {0}; L.field = vertex_valid;
    check(vertex_get(&L) == 1 && L.kind == 1 && !L.value, "invalid Lua vertex valid=false");
    L.field = vertex_floorz;
    check(vertex_get(&L) == -1 && !strcmp(L.error, "accessed vertex_t doesn't exist anymore."), "invalid Lua vertex diagnostic");
    finish(2, numvertexes, numvertexes * sizeof *vertexes, 0);
}
static void extra_case(unsigned mode, int sides, int guard)
{
    UINT8 data[8 + 8*17], *out = data, *in;
    vertex_t expected[5];
    UINT8 *neighbor = NULL;
    unsigned diag = diagnostic_count;
    begin(sides, guard); numvertexes = 5; numlines = 5;
    vertexes = Z_Calloc(numvertexes * sizeof *vertexes, PU_LEVEL, NULL);
    for (size_t i = 0; i < numvertexes; i++) { fill(&vertexes[i], i); expected[i] = vertexes[i]; }
    lines = Z_Calloc(numlines * sizeof *lines, PU_LEVEL, NULL);
    for (size_t i = 0; i < numlines; i++) { lines[i].v1 = &vertexes[i]; lines[i].v2 = &vertexes[(i+1) % 5]; }
    if (mode == 1) { neighbor = Z_Malloc(257, PU_LEVEL, NULL); memset(neighbor, 0x6b, 257); }
    WRITEUINT32(out, mode == 2 ? 4 : 5); WRITEUINT32(out, mode == 3 ? 0 : 17);
    for (unsigned i = 0; i < 17; i++) { WRITEUINT32(out, 0x80000000u + 0x01234567u*i); WRITEUINT32(out, 0x7fffffffu - 0x01234567u*i); }
    in = data;
    check(P_LoadExtraVertices(&in) == (mode != 2), "extra-node result");
    check(diagnostic_count == diag + (mode == 2), "extra-node mismatch diagnostic");
    check(in == data + (mode < 2 ? sizeof data : 8), "extra-node exact byte consumption");
    check(numvertexes == (mode < 2 ? 22u : 5u), "extra-node vertex count");
    for (size_t i = 0; i < 5; i++)
    {
        same(&expected[i], &vertexes[i]);
        check(lines[i].v1 == &vertexes[i] && lines[i].v2 == &vertexes[(i+1)%5], "extra-node relocates line pointers with typed stride");
    }
    for (size_t i = 5; i < numvertexes; i++)
        check((UINT32)vertexes[i].x == 0x80000000u + 0x01234567u*(i-5)
            && (UINT32)vertexes[i].y == 0x7fffffffu - 0x01234567u*(i-5)
            && !vertexes[i].floorzset && !vertexes[i].ceilingzset && !vertexes[i].floorz && !vertexes[i].ceilingz,
            "extra-node full coordinates and zero-filled height fields");
    for (size_t i = 0; i < numvertexes; i++) { snapshot(&vertexes[i]); lua_check(&vertexes[i], i); }
    if (neighbor) for (unsigned i = 0; i < 257; i++) check(neighbor[i] == 0x6b, "extra-node live neighbor");
    ps2lp_hash_t hs = actual_vertex_hash(); word(hs.h[0]); word(hs.h[1]);
    finish(3, numvertexes, numvertexes * sizeof *vertexes, mode);
}
static void slope_case(int sides, int guard)
{
    vertex_t v[3]; line_t ls[3][2], *refs[3][2]; sector_t sc[4];
    begin(sides, guard);
    for (size_t i = 0; i < 3; i++) fill(&v[i], i+1);
    for (unsigned p = 0; p < 3; p++)
    {
        ls[p][0].v1 = &v[p]; ls[p][0].v2 = &v[(p+1)%3];
        ls[p][1].v1 = &v[(p+2)%3]; ls[p][1].v2 = &v[p];
        refs[p][0] = &ls[p][0]; refs[p][1] = &ls[p][1];
    }
    for (unsigned flags = 0; flags < 64; flags++)
    {
        memset(sc, 0, sizeof sc); slope_count = 0;
        for (unsigned k = 0; k < 3; k++) { v[k].floorzset = !!(flags & (1u << k)); v[k].ceilingzset = !!(flags & (8u << k)); }
        for (unsigned p = 0; p < 3; p++)
        {
            sc[p].linecount = 3; sc[p].lines = refs[p]; sc[p].floorheight = INT32_MIN + p; sc[p].ceilingheight = INT32_MAX - p;
        }
        sc[3].linecount = flags & 1 ? 2 : 4; /* Must skip before dereferencing lines. */
        sectors = sc; numsectors = 4; SpawnVertexSlopes();
        check(slope_count == 3u * (!!(flags & 7) + !!(flags & 56)), "all six independent vertex presence flags select planes");
        for (unsigned p = 0; p < 3; p++)
        {
            check((sc[p].f_slope != NULL) == !!(flags & 7) && (sc[p].c_slope != NULL) == !!(flags & 56), "floor/ceiling slope presence");
            check(sc[p].hasslope == !!flags && !sc[3].hasslope, "triangular-sector restriction retained");
            for (unsigned ceiling = 0; ceiling < 2; ceiling++)
            {
                pslope_t *slope = ceiling ? sc[p].c_slope : sc[p].f_slope;
                word(slope != NULL);
                if (!slope) continue;
                for (unsigned k = 0; k < 3; k++)
                {
                    vertex_t *source = &v[(p+k)%3];
                    fixed_t z = ceiling ? (source->ceilingzset ? source->ceilingz : sc[p].ceilingheight)
                        : (source->floorzset ? source->floorz : sc[p].floorheight);
                    check(slope->points[k].x == source->x && slope->points[k].y == source->y && slope->points[k].z == z,
                        "actual slope selection preserves all full-width coordinate/height values");
                    word(slope->points[k].x); word(slope->points[k].y); word(slope->points[k].z);
                }
            }
        }
    }
    finish(4, 3, 0, 0);
}
static void poly_case(int sides, int guard)
{
    polyobj_t po = {0}; vector2_t center = {7*FRACUNIT, -3*FRACUNIT};
    begin(sides, guard); numvertexes = 257;
    vertexes = Z_Calloc(numvertexes * sizeof *vertexes, PU_LEVEL, NULL);
    for (size_t i = 0; i < numvertexes; i++)
    {
        fill(&vertexes[i], i); Polyobj_addVertex(&po, &vertexes[i]);
        size_t count = po.numVertices, capacity = po.numVerticesAlloc;
        Polyobj_addVertex(&po, &vertexes[i]);
        check(po.numVertices == count && po.numVerticesAlloc == capacity, "polyobject duplicate preserves capacity/count");
    }
    check(po.numVertices == 257 && po.numVerticesAlloc == 512, "polyobject unbounded doubling retained");
    for (size_t i = 0; i < numvertexes; i++)
    {
        check(po.vertices[i] == &vertexes[i], "polyobject vertex pointer identity"); same(&po.origVerts[i], &vertexes[i]);
        po.tmpVerts[i] = *po.vertices[i];
        for (angle_t a = 0; a < 4; a++)
        {
            *po.vertices[i] = po.origVerts[i];
            Polyobj_rotatePoint(po.vertices[i], &center, a);
            check(po.vertices[i]->floorzset == po.origVerts[i].floorzset && po.vertices[i]->ceilingzset == po.origVerts[i].ceilingzset
                && po.vertices[i]->floorz == po.origVerts[i].floorz && po.vertices[i]->ceilingz == po.origVerts[i].ceilingz,
                "polyobject rotation preserves complete height fields");
            snapshot(po.vertices[i]);
        }
        *po.vertices[i] = po.tmpVerts[i]; same(po.vertices[i], &po.origVerts[i]); lua_check(po.vertices[i], i);
    }
    finish(5, numvertexes, (numvertexes + 2*po.numVerticesAlloc)*sizeof *vertexes + po.numVerticesAlloc*sizeof *po.vertices, 0);
}
static void stock_case(const char *path)
{
    stock_input = fopen(path, "rb"); check(stock_input != NULL, "stock manifest open");
    UINT32 count = read_word(stock_input);
    for (UINT32 map = 0; map < count; map++)
    {
        char *name = read_string(stock_input);
        UINT32 kind = read_word(stock_input), n = read_word(stock_input);
        begin(1, 0); numvertexes = n;
        vertexes = Z_Calloc(numvertexes*sizeof *vertexes, PU_LEVEL, NULL);
        if (!kind)
        {
            UINT8 *data = malloc(4u*n); check(data != NULL, "stock traditional source");
            check(fread(data, 4, n, stock_input) == n, "stock traditional bytes");
            P_LoadVertices(data); free(data);
        }
        else { check(kind == 1, "stock map kind"); parse_mode = 100; actual_textmap_vertices(); }
        word(kind); word(n);
        for (size_t i = 0; i < numvertexes; i++) { snapshot(&vertexes[i]); lua_check(&vertexes[i], i); }
        ps2lp_hash_t hs = actual_vertex_hash(); word(hs.h[0]); word(hs.h[1]);
        printf("MEMORY10 stockmap=%u format=%u n=%zu name=%s\n", map, kind, numvertexes, name);
        finish(6, numvertexes, numvertexes * sizeof *vertexes, map);
        stockmaps++; stockvertices += n; free(name);
    }
    check(fgetc(stock_input) == EOF, "complete stock manifest consumed"); fclose(stock_input); stock_input = NULL;
}
static seg_t endpoint_segments[17];
static INT16 endpoint_clips[19];
static fixed_t endpoint_columns[19];
static void draw_fill(drawseg_t *d, size_t i)
{
    vertex_t v;
    memset(d, 0xa7, sizeof *d); fill(&v, i);
    d->curline = &endpoint_segments[i % 17];
    d->x1 = v.x; d->x2 = v.y; d->scale1 = v.floorz; d->scale2 = v.ceilingz;
    d->scalestep = (fixed_t)(i * 0xf21359u); d->silhouette = (INT32)(i * 0x738912u);
    d->bsilheight = (fixed_t)(0x80000000u + i); d->tsilheight = (fixed_t)(0x7fffffffu-i);
    d->offsetx = (fixed_t)(i * 0x61c88647u);
    d->sprtopclip = &endpoint_clips[i%19]; d->sprbottomclip = &endpoint_clips[(i+1)%19];
    d->maskedtexturecol = &endpoint_columns[i%19]; d->maskedtextureheight = &endpoint_columns[(i+2)%19];
    d->invscale = &endpoint_columns[(i+3)%19]; d->thicksidecol = &endpoint_columns[(i+4)%19];
    d->numffloorplanes = (INT32)(i * 0xf1abcdu); d->numthicksides = (INT32)(i * 0x87ff12u);
    d->frontscale = NULL; d->frontscalewidth = 0; d->portalpass = (UINT8)i;
    d->leftpos.x = v.x; d->leftpos.y = v.y; d->rightpos.x = v.floorz; d->rightpos.y = v.ceilingz;
    for (size_t k = 0; k < MAXFFLOORS; k++)
    {
        d->ffloorplanes[k] = (struct visplane_s *)&endpoint_segments[(i+k)%17];
        d->thicksides[k] = (struct ffloor_s *)&endpoint_segments[(i+k+3)%17];
    }
}
static void draw_same(const drawseg_t *a, const drawseg_t *b)
{
#define EQ(f) check(a->f == b->f, "drawseg complete " #f)
    EQ(curline); EQ(x1); EQ(x2); EQ(scale1); EQ(scale2); EQ(scalestep); EQ(silhouette);
    EQ(bsilheight); EQ(tsilheight); EQ(offsetx); EQ(sprtopclip); EQ(sprbottomclip);
    EQ(maskedtexturecol); EQ(maskedtextureheight); EQ(invscale); EQ(numffloorplanes); EQ(numthicksides);
    EQ(thicksidecol); EQ(frontscale); EQ(frontscalewidth); EQ(portalpass);
    EQ(leftpos.x); EQ(leftpos.y); EQ(rightpos.x); EQ(rightpos.y);
    for (size_t k = 0; k < MAXFFLOORS; k++) { EQ(ffloorplanes[k]); EQ(thicksides[k]); }
#undef EQ
}
static void draw_snapshot(const drawseg_t *d)
{
    word((UINT32)(d->curline-endpoint_segments)); word(d->x1); word(d->x2);
    word(d->scale1); word(d->scale2); word(d->scalestep); word(d->silhouette); word(d->bsilheight); word(d->tsilheight);
    word(d->offsetx); word((UINT32)(d->sprtopclip-endpoint_clips)); word((UINT32)(d->sprbottomclip-endpoint_clips));
    word((UINT32)(d->maskedtexturecol-endpoint_columns)); word((UINT32)(d->maskedtextureheight-endpoint_columns));
    word((UINT32)(d->invscale-endpoint_columns)); word(d->numffloorplanes); word(d->numthicksides);
    word((UINT32)(d->thicksidecol-endpoint_columns)); word(d->frontscale != NULL); word(d->frontscalewidth); word(d->portalpass);
    word(d->leftpos.x); word(d->leftpos.y); word(d->rightpos.x); word(d->rightpos.y);
    for (size_t k = 0; k < MAXFFLOORS; k++)
    {
        word((UINT32)((seg_t *)d->ffloorplanes[k]-endpoint_segments));
        word((UINT32)((seg_t *)d->thicksides[k]-endpoint_segments));
    }
}
static void draw_table_case(size_t count, int sides, int guard)
{
    drawseg_t *values, *copy;
    UINT8 *neighbor;
    begin(sides, guard);
    values = Z_Malloc(count * sizeof *values, PU_STATIC, NULL);
    neighbor = Z_Malloc(113, PU_STATIC, NULL); memset(neighbor, 0x5d, 113);
    copy = Z_Malloc(count * sizeof *copy, PU_STATIC, NULL);
    memset(copy, 0x69, count * sizeof *copy);
    for (size_t i = 0; i < count; i++) draw_fill(&values[i], i);
    M_Memcpy(copy, values, count * sizeof *copy);
    for (size_t i = 0; i < count; i++)
    {
        draw_same(&values[i], &copy[i]); draw_snapshot(&copy[i]);
        check((uintptr_t)&copy[i].leftpos.x % 4 == 0 && (uintptr_t)&copy[i].rightpos.y % 4 == 0
            && (uintptr_t)&copy[i].ffloorplanes[0] % _Alignof(void *) == 0, "all endpoints and following pointers naturally aligned");
    }
    for (unsigned i = 0; i < 113; i++) check(neighbor[i] == 0x5d, "drawseg neighbor canary");
    finish(7, count, 2 * count * sizeof *copy, 0);
}
static void draw_growth_case(int sides, int guard)
{
    static const INT32 widths[] = {1, 320, 640, 320, 1};
    begin(sides, guard);
    drawsegs = ds_p = curdrawsegs = firstseg = NULL; maxdrawsegs = 0;
    /* Real initial reserve and four 1.5x growth boundaries, with live late
     * pointer arrays, external owners and the actual high-water scale roots. */
    for (unsigned grow = 0; grow < 5; grow++)
    {
        size_t old = maxdrawsegs, cur = old ? old / 3 : 0, first = old ? old / 2 : 0;
        ds_p = drawsegs + old; curdrawsegs = drawsegs + cur; firstseg = old ? drawsegs + first : NULL;
        actual_drawseg_growth();
        check(maxdrawsegs == (old ? old + old/2 : 128), "unchanged drawseg 128/1.5x capacity");
        check(ds_p == drawsegs + old && curdrawsegs == drawsegs + cur && firstseg == (old ? drawsegs + first : NULL), "all drawseg pointer indices repaired");
        for (size_t i = 0; i < old; i++)
        {
            drawseg_t expected; draw_fill(&expected, i);
            if (!(i % 11)) { expected.frontscale = drawsegs[i].frontscale; expected.frontscalewidth = 640; }
            draw_same(&expected, &drawsegs[i]);
            if (!(i % 11)) for (unsigned k = 0; k < 640; k++) check((UINT32)drawsegs[i].frontscale[k] == (UINT32)i*65537u+k, "scale root and values survive moving drawseg growth");
        }
        for (size_t i = old; i < maxdrawsegs; i++)
        {
            draw_fill(&drawsegs[i], i);
            if (!(i % 11))
            {
                for (size_t w = 0; w < sizeof widths/sizeof widths[0]; w++)
                {
                    viewwidth = widths[w]; R_AllocDrawSegFrontScale(&drawsegs[i]);
                    check(drawsegs[i].frontscalewidth == (viewwidth > 320 || w >= 2 ? 640 : viewwidth), "actual frontscale high-water policy");
                }
                for (unsigned k = 0; k < 640; k++) drawsegs[i].frontscale[k] = (fixed_t)((UINT32)i*65537u+k);
            }
        }
    }
    for (size_t i = 0; i < maxdrawsegs; i++) draw_snapshot(&drawsegs[i]);
    viewwidth = 0; expecting_error = 1;
    if (!setjmp(error_jump)) { R_AllocDrawSegFrontScale(&drawsegs[0]); check(0, "invalid view width should fail"); }
    expecting_error = 0;
    check(!strcmp(error_text, "R_AllocDrawSegFrontScale: invalid view width"), "unchanged invalid-width error");
    consistent();
    R_ReleaseDrawSegScales();
    check(!drawsegs && !ds_p && !curdrawsegs && !firstseg && !maxdrawsegs, "actual drawseg/scale release clears all roots/capacity");
    zastats_t st; ZA_Stats(&st); check(!st.used, "all external scale roots freed with drawseg storage");
    R_ReleaseDrawSegScales();
    finish(8, 648, 0, 0);
}
static void draw_intersection_case(int sides, int guard)
{
    drawseg_t storage;
    vertex_t v[2]; seg_t seg = {&v[0], &v[1]}; sector_t front = {0}, back = {0};
    begin(sides, guard); ds_p = &storage; curline = &seg; frontsector = &front;
    for (unsigned row = 0; row < 4096; row++)
    {
        draw_fill(&storage, row); fill(&v[0], row); fill(&v[1], row+7);
        viewx = (fixed_t)(row*0xdeadbeefu); viewy = (fixed_t)(row*0x734921u);
        viewangle = (angle_t)(row << ANGLETOFINESHIFT);
        xtoviewangle[0] = (angle_t)((row*3u) << ANGLETOFINESHIFT);
        xtoviewangle[639] = (angle_t)((row*5u+1u) << ANGLETOFINESHIFT);
        front.hasslope = !!(row & 1); back.hasslope = !!(row & 2); backsector = row & 4 ? &back : NULL;
        fixed_t leftx = storage.leftpos.x, lefty = storage.leftpos.y, rightx = storage.rightpos.x, righty = storage.rightpos.y;
        if (front.hasslope || (backsector && back.hasslope))
        {
            angle_t a = (xtoviewangle[0]+viewangle) >> ANGLETOFINESHIFT;
            RQ_RayHitsSeg(v[0].x, v[0].y, v[1].x, v[1].y, viewx, viewy, FINESINE(a), FINECOSINE(a), &leftx, &lefty);
            a = (xtoviewangle[639]+viewangle) >> ANGLETOFINESHIFT;
            RQ_RayHitsSeg(v[0].x, v[0].y, v[1].x, v[1].y, viewx, viewy, FINESINE(a), FINECOSINE(a), &rightx, &righty);
        }
        actual_drawseg_intersections(0, 639);
        check(storage.leftpos.x == leftx && storage.leftpos.y == lefty && storage.rightpos.x == rightx && storage.rightpos.y == righty,
            "actual slope/no-slope endpoint stores retain exact full coordinate bits");
        draw_snapshot(&storage);
    }
    ds_p = NULL; finish(9, 4096, 0, 0);
}
int main(int argc, char **argv)
{
    static const size_t sizes[] = {1,2,3,7,31,64,65,256,8192,65535};
#ifdef __mips__
    init_scr(); check(ZA_HDR == 16, "EE arena header ABI");
#endif
    check(argc == 3, "usage stock-manifest snapshot");
    snapshot_file = fopen(argv[2], "wb"); check(snapshot_file != NULL, "snapshot open");
    printf("MEMORY10 BEGIN\n"); fflush(stdout);
    ship_sides = za_twosided; ship_prefer = za_prefer;
    for (int sides = 0; sides < 2; sides++)
    for (int guard = 0; guard < 2; guard++)
    {
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) table_case(sizes[i], sides, guard);
        doom_case(sides, guard); text_case(sides, guard);
        for (unsigned mode = 0; mode < 4; mode++) extra_case(mode, sides, guard);
        slope_case(sides, guard); poly_case(sides, guard);
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0] - 1; i++) draw_table_case(sizes[i], sides, guard);
        draw_growth_case(sides, guard); draw_intersection_case(sides, guard);
    }
    stock_case(argv[1]); memory_backing_check();
    check(fclose(snapshot_file) == 0, "snapshot close");
    printf("MEMORY10 DONE cases=%u checks=%u stockmaps=%u stockvertices=%u sizeof=%zu align=%zu drawseg=%zu\n",
        cases, checks, stockmaps, stockvertices, sizeof(vertex_t), _Alignof(vertex_t), sizeof(drawseg_t));
#ifndef __mips__
    if (arena_raw) { aligned_release(arena_raw); arena_raw = NULL; }
#endif
    return 0;
}
