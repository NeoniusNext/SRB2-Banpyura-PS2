/* Typed differential fixture. The generated includes are complete production
 * functions; only allocator, packet ownership, logging and flush boundaries
 * are controlled. Both variants run at the same addresses, in saved worlds. */
#include "common.h"
#include "screen.h"
#include <stdarg.h>

#define CM_COUNT 8
#define TABLE_BYTES (256 * 32)
#define LOG_BYTES (512 * 1024)
#define PACKETS 128
#define DRAWS 128
#define GUARD 0xa7

lighttable_state_t H;
u8 *lt_idx[LT_MAX], lt_gen[LT_MAX];
u32 spal[256], pal_key[1024];
u8 pal_val[1024], clut_hint[256];
int spal_set, pal_last_mode, ps2hwd_dbg_flags;
RGBA_t mapPalette[256], *pLocalPalette, *pMasterPalette;
UINT8 *colormaps;
consvar_t cv_glpaletterendering;
struct hwdriver_s hwdriver;
viddef_t vid;

typedef struct { u8 before[16], data[TABLE_BYTES], after[16]; } __attribute__((aligned(16))) driver_block_t;
typedef struct { u8 before[256]; RGBA_t data[TABLE_BYTES]; u8 after[256]; } __attribute__((aligned(256))) zone_block_t;
typedef struct { u32 param, keyed; } pending_t;
typedef union
{
    RGBA_t p0[256];
    struct { u8 pad[1]; RGBA_t colors[256]; } __attribute__((packed)) p1;
    struct { u8 pad[2]; RGBA_t colors[256]; } __attribute__((packed)) p2;
    struct { u8 pad[3]; RGBA_t colors[256]; } __attribute__((packed)) p3;
} __attribute__((aligned(4))) rgba_alignment_t;
typedef struct
{
    driver_block_t driver[LT_MAX];
    zone_block_t zone[CM_COUNT];
    u8 driver_used[LT_MAX], zone_used[CM_COUNT];
    void *zone_owner[CM_COUNT];
    size_t zone_size[CM_COUNT];
    int zone_tag[CM_COUNT];
    u32 driver_mallocs, driver_frees, zone_mallocs, zone_frees;
    int malloc_fail, shader;
    extracolormap_t cmap[CM_COUNT];
    u8 input[CM_COUNT][TABLE_BYTES], default_input[TABLE_BYTES];
    RGBA_t local[256], master[256], reverse[256], screen[256];
    union { RGBA_t rgba[TABLE_BYTES / 4]; u8 bytes[TABLE_BYTES]; } alias;
    rgba_alignment_t unaligned;
    u8 unaligned_input[TABLE_BYTES + 4];
    int alias_on;
    pending_t pending[DRAWS];
    u32 pending_n, flush_calls, cache_flush_calls, tex_drop_calls;
    u32 draw_n, draw[DRAWS][256];
    qw_t packets[PACKETS][72];
    u32 packet_n;
    u32 vram[CLUT_SLOTS][256];
    char text[LOG_BYTES];
    size_t text_n;
    UINT32 result;
    gcfp_t fingerprint[CM_COUNT + 1];
} runtime_t;
static runtime_t R;
typedef struct
{
    runtime_t r;
    lighttable_state_t h;
    u8 *indices[LT_MAX], generation[LT_MAX];
    u32 screen_palette[256], hash_key[1024];
    u8 hash_value[1024], hint[256];
    int screen_set, last_mode, debug_flags, shader_value;
    consvar_t cv;
    RGBA_t map[256];
    RGBA_t *local_pointer, *master_pointer;
    UINT8 *colormaps_pointer;
    viddef_t video;
} snapshot_t;
static snapshot_t saved[2];
static u64 cases, compared_bytes;
static u64 rgb_checks;
static const char *case_name;
static int active_variant;

static void fail(const char *what)
{
    printf("LIGHTTABLE FAIL case=%llu name=%s variant=%d: %s\n", (unsigned long long)cases, case_name, active_variant, what);
    exit(1);
}
#define CHECK(x) do { if (!(x)) fail(#x); } while (0)

static void append_text(const char *fmt, va_list ap)
{
    int n = vsnprintf(R.text + R.text_n, sizeof R.text - R.text_n, fmt, ap);
    CHECK(n >= 0 && (size_t)n < sizeof R.text - R.text_n);
    R.text_n += (size_t)n;
}
void I_OutputMsg(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); append_text(fmt, ap); va_end(ap);
}
void CONS_Alert(alerttype_t level, const char *fmt, ...)
{
    va_list ap; (void)level; va_start(ap, fmt); append_text(fmt, ap); va_end(ap);
}
boolean HWR_UseShader(void) { return R.shader != 0; }
extracolormap_t *R_GetDefaultColormap(void) { return &R.cmap[0]; }
void HWR_GCacheFlush(void) { R.cache_flush_calls++; }

void *Z_MallocAlign(size_t size, INT32 tag, void *owner, INT32 alignbits)
{
    unsigned i;
    CHECK(size == sizeof R.zone[0].data && tag == PU_HWRLIGHTTABLEDATA);
    CHECK(alignbits == (INT32)sizeof(void *));
    for (i = 0; i < CM_COUNT && R.zone_used[i]; i++);
    CHECK(i < CM_COUNT);
    CHECK(((uintptr_t)R.zone[i].data & (((uintptr_t)1 << alignbits) - 1)) == 0);
    R.zone_used[i] = 1; R.zone_owner[i] = owner; R.zone_size[i] = size; R.zone_tag[i] = tag; R.zone_mallocs++;
    if (owner) { void *p = R.zone[i].data; memcpy(owner, &p, sizeof p); }
    return R.zone[i].data;
}
void Z_FreeTags(INT32 low, INT32 high)
{
    unsigned i;
    for (i = 0; i < CM_COUNT; i++)
        if (R.zone_used[i] && R.zone_tag[i] >= low && R.zone_tag[i] <= high)
        {
            if (R.zone_owner[i]) { void *p = NULL; memcpy(R.zone_owner[i], &p, sizeof p); }
            R.zone_used[i] = 0; R.zone_owner[i] = NULL; R.zone_size[i] = 0; R.zone_tag[i] = 0; R.zone_frees++;
        }
}
static void *fixture_driver_malloc(size_t size)
{
    unsigned i;
    CHECK(size == TABLE_BYTES);
    if (R.malloc_fail) return NULL;
    for (i = 1; i < LT_MAX && R.driver_used[i]; i++);
    CHECK(i < LT_MAX);
    R.driver_used[i] = 1; R.driver_mallocs++;
    return R.driver[i].data;
}
static void fixture_driver_free(void *p)
{
    unsigned i;
    if (!p) return;
    for (i = 1; i < LT_MAX && R.driver[i].data != p; i++);
    CHECK(i < LT_MAX && R.driver_used[i]);
    R.driver_used[i] = 0; R.driver_frees++;
}
static qw_t *pk_alloc(u32 n)
{
    CHECK(n == 72 && R.packet_n < PACKETS);
    return R.packets[R.packet_n++];
}
static qw_t *ad_alloc(u32 n)
{
    CHECK(n == 1 && R.packet_n < PACKETS);
    return R.packets[R.packet_n++];
}
static inline void ad_set(qw_t *p, u64 value, u64 reg) { p->d[0] = value; p->d[1] = reg; }
static void ov_flush_all(void);
static void tex_drop(int rec, int why) { (void)rec; (void)why; R.tex_drop_calls++; }

#include "source-before.inc"
#include "stores.inc"
#include "source-after.inc"
#ifdef NEXT12_LIGHTTABLE_RGB_MASK
#include "rgb-expression.inc"
#endif

static RGBA_t *alignment_palette(rgba_alignment_t *p, unsigned offset)
{
    switch (offset) { case 0: return p->p0; case 1: return p->p1.colors; case 2: return p->p2.colors; default: return p->p3.colors; }
}
static void rgba_bits(void)
{
#ifdef NEXT12_LIGHTTABLE_RGB_MASK
    unsigned offset, rgb, channel, value, alpha;
    rgba_alignment_t storage;
    case_name = "exhaustive packed RGB/alpha extraction";
    CHECK(sizeof(RGBA_t) == 4 && _Alignof(RGBA_t) == 1);
    CHECK(offsetof(RGBA_t, s.red) == 0 && offsetof(RGBA_t, s.green) == 1 && offsetof(RGBA_t, s.blue) == 2 && offsetof(RGBA_t, s.alpha) == 3);
    memset(&storage, 0x73, sizeof storage);
    for (offset = 0; offset < 4; offset++)
    {
        RGBA_t *c = alignment_palette(&storage, offset);
        CHECK(((uintptr_t)c & 3) == offset);
        for (rgb = 0; rgb < 0x1000000u; rgb++)
        {
            c->s.red = (u8)rgb; c->s.green = (u8)(rgb >> 8); c->s.blue = (u8)(rgb >> 16); c->s.alpha = (u8)(rgb * 37);
            CHECK(fixture_actual_rgb(c) == ((u32)c->s.red | ((u32)c->s.green << 8) | ((u32)c->s.blue << 16)));
            rgb_checks++;
        }
        for (channel = 0; channel < 3; channel++) for (value = 0; value < 256; value++) for (alpha = 0; alpha < 256; alpha++)
        {
            c->s.red = 17; c->s.green = 91; c->s.blue = 223; c->s.alpha = (u8)alpha;
            if (channel == 0) c->s.red = (u8)value; else if (channel == 1) c->s.green = (u8)value; else c->s.blue = (u8)value;
            CHECK(fixture_actual_rgb(c) == ((u32)c->s.red | ((u32)c->s.green << 8) | ((u32)c->s.blue << 16)));
            rgb_checks++;
        }
    }
#endif
}

static void ingest_packets(u32 first)
{
    u32 i;
    for (i = first; i < R.packet_n; i++)
    {
        qw_t *p = R.packets[i];
        if (p[0].d[1] == GIF_REGS_AD && p[1].d[1] == GSR_BITBLTBUF)
        {
            u32 block = (u32)(p[1].d[0] >> 32) & 0x3fffu;
            CHECK(block >= H.clut_base && ((block - H.clut_base) % CLUT_BLOCKS) == 0);
            CHECK((block - H.clut_base) / CLUT_BLOCKS < CLUT_SLOTS);
            CHECK(p[2].d[1] == GSR_TRXPOS && p[3].d[1] == GSR_TRXREG && p[4].d[1] == GSR_TRXDIR);
            CHECK(p[70].d[1] == GIF_REGS_AD && p[71].d[1] == GSR_TEXFLUSH);
            memcpy(R.vram[(block - H.clut_base) / CLUT_BLOCKS], p[6].w, 1024);
        }
    }
}
static void consume(u32 param, u32 keyed)
{
    u32 start = R.packet_n, block, expected[256], *stored;
    int i;
    CHECK(R.draw_n < DRAWS);
#include "source-mode.inc"
    block = clut_get(CK_LIGHT, param, keyed);
    ingest_packets(start);
    CHECK(block >= H.clut_base && (block - H.clut_base) / CLUT_BLOCKS < CLUT_SLOTS);
    stored = R.vram[(block - H.clut_base) / CLUT_BLOCKS];
    pal_light_entries(param, keyed, expected);
    for (i = 0; i < 256; i++)
    {
        R.draw[R.draw_n][i] = stored[clut_slot((u32)i)];
        /* Cache entries intentionally wrap generation exactly as production;
         * compare the full existing cache rather than invent a wrap repair. */
        if (R.packet_n > start && lt_gen[param & 1023] == ((param >> 10) & 255))
            CHECK(R.draw[R.draw_n][i] == expected[i]);
    }
    R.draw_n++;
}
static void ov_flush_all(void)
{
    u32 i, count = R.pending_n;
    pending_t pending[DRAWS];
    R.flush_calls++;
    memcpy(pending, R.pending, count * sizeof *pending); R.pending_n = 0;
    for (i = 0; i < count; i++) consume(pending[i].param, pending[i].keyed);
}
static void fingerprint(void)
{
    int i;
    for (i = 0; i <= CM_COUNT; i++)
    {
        gc_init_fp(&R.fingerprint[i]); gc_cmap(&R.fingerprint[i], i < CM_COUNT ? &R.cmap[i] : NULL);
    }
}
static void guards(void)
{
    unsigned i, j;
    for (i = 0; i < LT_MAX; i++)
        for (j = 0; j < sizeof R.driver[i].before; j++)
            CHECK(R.driver[i].before[j] == GUARD && R.driver[i].after[j] == GUARD);
    for (i = 0; i < CM_COUNT; i++)
        for (j = 0; j < sizeof R.zone[i].before; j++)
            CHECK(R.zone[i].before[j] == GUARD && R.zone[i].after[j] == GUARD);
#if defined(PS2) && !defined(PS2_NOOPT_LIGHTTABLE_INDEXED)
    if (active_variant)
    {
        CHECK(R.zone_mallocs == 0 && R.zone_frees == 0);
        for (i = 0; i < CM_COUNT; i++) CHECK(!R.cmap[i].gl_lighttable.data && !R.zone_used[i]);
    }
#endif
}
static void capture(snapshot_t *s)
{
    memset(s, 0, sizeof *s);
    memcpy(&s->r, &R, sizeof R); memcpy(&s->h, &H, sizeof H);
    memcpy(s->indices, lt_idx, sizeof lt_idx); memcpy(s->generation, lt_gen, sizeof lt_gen);
    memcpy(s->screen_palette, spal, sizeof spal); memcpy(s->hash_key, pal_key, sizeof pal_key);
    memcpy(s->hash_value, pal_val, sizeof pal_val); memcpy(s->hint, clut_hint, sizeof clut_hint);
    s->screen_set = spal_set; s->last_mode = pal_last_mode; s->debug_flags = ps2hwd_dbg_flags;
    s->cv = cv_glpaletterendering; memcpy(s->map, mapPalette, sizeof mapPalette);
    s->local_pointer = pLocalPalette; s->master_pointer = pMasterPalette; s->colormaps_pointer = colormaps; s->video = vid;
}
static void restore(const snapshot_t *s)
{
    memcpy(&R, &s->r, sizeof R); memcpy(&H, &s->h, sizeof H);
    memcpy(lt_idx, s->indices, sizeof lt_idx); memcpy(lt_gen, s->generation, sizeof lt_gen);
    memcpy(spal, s->screen_palette, sizeof spal); memcpy(pal_key, s->hash_key, sizeof pal_key);
    memcpy(pal_val, s->hash_value, sizeof pal_val); memcpy(clut_hint, s->hint, sizeof clut_hint);
    spal_set = s->screen_set; pal_last_mode = s->last_mode; ps2hwd_dbg_flags = s->debug_flags;
    cv_glpaletterendering = s->cv; memcpy(mapPalette, s->map, sizeof mapPalette);
    pLocalPalette = s->local_pointer; pMasterPalette = s->master_pointer; colormaps = s->colormaps_pointer; vid = s->video;
}
static void compare(void)
{
    unsigned i;
#if defined(PS2) && !defined(PS2_NOOPT_LIGHTTABLE_INDEXED)
    /* These exact fields constitute the intended deleted redundant allocation.
     * All driver allocations, table bytes, metadata, caches and outputs compare
     * literally, including pointers, at shared addresses. */
    runtime_t *a = &saved[0].r, *b = &saved[1].r;
    for (i = 0; i < CM_COUNT; i++)
    {
        CHECK(!b->cmap[i].gl_lighttable.data && !b->zone_used[i]);
        a->cmap[i].gl_lighttable.data = NULL;
    }
    memset(a->zone, 0, sizeof a->zone); memset(b->zone, 0, sizeof b->zone);
    memset(a->zone_used, 0, sizeof a->zone_used); memset(b->zone_used, 0, sizeof b->zone_used);
    memset(a->zone_owner, 0, sizeof a->zone_owner); memset(b->zone_owner, 0, sizeof b->zone_owner);
    memset(a->zone_size, 0, sizeof a->zone_size); memset(b->zone_size, 0, sizeof b->zone_size);
    memset(a->zone_tag, 0, sizeof a->zone_tag); memset(b->zone_tag, 0, sizeof b->zone_tag);
    a->zone_mallocs = b->zone_mallocs = a->zone_frees = b->zone_frees = 0;
#else
    (void)i;
#endif
    if (memcmp(&saved[0], &saved[1], sizeof saved[0]))
    {
        const u8 *a = (const u8 *)&saved[0], *b = (const u8 *)&saved[1]; size_t at;
        for (at = 0; at < sizeof saved[0] && a[at] == b[at]; at++);
        printf("first differing snapshot byte=%zu reference=%02x candidate=%02x\n", at, a[at], b[at]);
        fail("complete semantic snapshots differ");
    }
    compared_bytes += sizeof saved[0];
}
static void reset_output(void)
{
    R.text_n = 0; memset(R.text, 0, sizeof R.text); R.packet_n = R.draw_n = 0;
    memset(R.packets, 0x5c, sizeof R.packets); memset(R.draw, 0, sizeof R.draw);
    R.flush_calls = R.cache_flush_calls = R.tex_drop_calls = 0; R.result = 0;
}
static u32 mix(u32 x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16); }
static void palettes(u32 seed, int duplicate, int misses)
{
    unsigned i;
    for (i = 0; i < 256; i++)
    {
        u32 rgb = i | (((i * 37 + seed) & 255) << 8) | (((i * 83 + (seed >> 8)) & 255) << 16);
        R.reverse[i].rgba = rgb | ((u32)(i ^ 0x69) << 24);
        R.local[i] = R.reverse[(u8)(i * 13u)]; /* overwritten below once complete */
        R.screen[i].rgba = mix(seed + i) | 0xff000000u;
    }
    if (duplicate) { R.reverse[251] = R.reverse[3]; R.reverse[255] = R.reverse[1]; R.reverse[128] = R.reverse[7]; }
    for (i = 0; i < 256; i++)
    {
        R.local[i] = R.reverse[(u8)(i * 13u + 5u)];
        mapPalette[i] = R.reverse[(u8)(i * 7u + 19u)];
        if (misses && (i % 11 == 0)) { R.local[i].s.red ^= 0x80; mapPalette[i].s.green ^= 0x40; }
    }
}
static void inputs(u32 seed)
{
    unsigned i, k;
    for (k = 0; k < CM_COUNT; k++)
        for (i = 0; i < TABLE_BYTES; i++) R.input[k][i] = (u8)mix(seed + i + k * 331);
    for (i = 0; i < TABLE_BYTES; i++) R.default_input[i] = (u8)(i * 113 + (i / 256) * 37 + seed);
}
static UINT32 getter(extracolormap_t *cm)
{
    return active_variant ? candidate_HWR_GetLightTableID(cm) : reference_HWR_GetLightTableID(cm);
}
static UINT32 direct_create(UINT8 *src)
{
    RGBA_t buffer[TABLE_BYTES];
#if defined(PS2) && !defined(PS2_NOOPT_LIGHTTABLE_INDEXED)
    if (active_variant) return PS2HWD_CreateIndexedLightTable(src, HWR_GetTexturePalette());
#endif
    return HWR_CreateLightTable(src, buffer);
}
static void direct_update(UINT32 id, UINT8 *src)
{
    RGBA_t buffer[TABLE_BYTES];
#if defined(PS2) && !defined(PS2_NOOPT_LIGHTTABLE_INDEXED)
    if (active_variant) { PS2HWD_UpdateIndexedLightTable(id, src, HWR_GetTexturePalette()); return; }
#endif
    HWR_UpdateLightTable(id, src, buffer);
}
enum { GET, NEED, PALETTES, TEXTURE_PALETTE, SCREEN_PALETTE, INPUTS, MODE, QUEUE, CONSUME,
       ENGINE_CLEAR, DRIVER_CLEAR, RESET_ID, CONTEXT, UP, PAL_SET, MALLOC_FAIL, DIRECT_CREATE,
       DIRECT_UPDATE, EXHAUST, LEVEL_RESET, DEBUG, ALIAS, PALGEN_WRAP };
typedef struct { int type, a, b; u32 c; } operation_t;
static void apply(operation_t o)
{
    unsigned i, row, keyed; extracolormap_t *cm = &R.cmap[(unsigned)o.a % CM_COUNT];
    switch (o.type)
    {
    case GET: R.result = getter(o.a < 0 ? NULL : cm); break;
    case NEED: cm->gl_lighttable.needs_update = o.b != 0; break;
    case PALETTES: palettes(o.c, o.a, o.b); break;
    case TEXTURE_PALETTE: hw_SetTexturePalette(o.a ? NULL : R.reverse); break;
    case SCREEN_PALETTE: hw_SetScreenPalette(o.a ? NULL : R.screen); break;
    case INPUTS: inputs(o.c); break;
    case MODE: cv_glpaletterendering.value = o.a; R.shader = o.b; pMasterPalette = o.c ? R.master : NULL; break;
    case QUEUE:
        CHECK(R.pending_n < DRAWS);
        R.pending[R.pending_n].param = cm->gl_lighttable.id | ((u32)lt_gen[cm->gl_lighttable.id] << 10) | ((u32)o.b << 18);
        R.pending[R.pending_n++].keyed = o.c; break;
    case CONSUME:
        for (row = 0; row < 32; row++) for (keyed = 0; keyed < 2; keyed++)
            consume(cm->gl_lighttable.id | ((u32)lt_gen[cm->gl_lighttable.id] << 10) | (row << 18), keyed);
        ov_flush_all(); break;
    case ENGINE_CLEAR: vid.glstate = o.a; HWR_ClearLightTables(); break;
    case DRIVER_CLEAR: hw_ClearLightTables(); break;
    case RESET_ID: cm->gl_lighttable.id = 0; break;
    case CONTEXT:
    {
        u32 oldpal[256]; int oldset = H.pal_set;
        memcpy(oldpal, H.pal, sizeof oldpal); ov_flush_all();
        /* Exact light-table-relevant Shutdown/Init/SetScreenSize transitions:
         * lt_idx/lt_gen survive; shutdown clears screen-palette validity. */
        spal_set = 0; pal_last_mode = -1; memset(&H, 0, sizeof H); H.up = 1; H.clut_base = 4096;
        if (oldset) { memcpy(H.pal, oldpal, sizeof oldpal); H.pal_set = 1; pal_build(); H.pal_gen++; }
        break;
    }
    case UP: H.up = o.a; break;
    case PAL_SET: H.pal_set = o.a; break;
    case MALLOC_FAIL: R.malloc_fail = o.a; break;
    case DIRECT_CREATE: R.result = direct_create(R.default_input); break;
    case DIRECT_UPDATE: direct_update(o.c, R.default_input); break;
    case EXHAUST:
        for (i = 1; i < LT_MAX; i++) CHECK(direct_create(R.default_input) == i);
        CHECK(direct_create(R.default_input) == 0); break;
    case LEVEL_RESET:
        HWR_ClearLightTables();
        for (i = 0; i < CM_COUNT; i++) { R.cmap[i].gl_lighttable.id = 0; R.cmap[i].gl_lighttable.needs_update = false; }
        inputs(o.c); break;
    case DEBUG:
        ps2hwd_dbg_flags = o.a;
        for (i = 0; i < 70; i++) CHECK(pal_row_of((int)i - 20) == ((o.a & 16384) ? 0 : (i <= 21 ? 0 : (i >= 52 ? 31 : (int)i - 21))));
        break;
    case ALIAS:
        R.alias_on = o.a;
        if (o.a >= 2)
        {
            unsigned offset = (unsigned)o.a - 2;
            CHECK(offset < 4);
            pLocalPalette = alignment_palette(&R.unaligned, offset);
            CHECK(((uintptr_t)pLocalPalette & 3) == offset);
            memcpy(pLocalPalette, R.local, sizeof R.local);
            for (i = 0; i < sizeof R.unaligned_input; i++) R.unaligned_input[i] = (u8)mix(i + o.c);
            R.cmap[7].colormap = R.unaligned_input + offset;
        }
        else if (o.a)
        {
            memcpy(R.alias.rgba, R.local, sizeof R.local);
            for (i = sizeof R.local; i < sizeof R.alias; i++) R.alias.bytes[i] = (u8)mix(i + o.c);
            pLocalPalette = R.alias.rgba; R.cmap[7].colormap = R.alias.bytes;
        }
        else { pLocalPalette = R.local; R.cmap[7].colormap = R.input[7]; }
        break;
    case PALGEN_WRAP: H.pal_gen = o.c; break;
    default: fail("unknown operation");
    }
    fingerprint(); guards();
}
static void step(const char *name, operation_t op)
{
    snapshot_t *a = malloc(sizeof *a), *b = malloc(sizeof *b);
    CHECK(a && b);
    case_name = name; cases++;
    active_variant = 0; restore(&saved[0]); reset_output(); apply(op); capture(a);
    active_variant = 1; restore(&saved[1]); reset_output(); apply(op); capture(b);
    memcpy(&saved[0], a, sizeof *a); memcpy(&saved[1], b, sizeof *b);
    compare();
    /* Comparison temporarily removes only the intentionally deleted zone
     * allocation. Restore the complete original worlds for the next step. */
    memcpy(&saved[0], a, sizeof *a); memcpy(&saved[1], b, sizeof *b);
    free(a); free(b);
}
#define STEP(n,t,a,b,c) step(n, (operation_t){t,a,b,c})

int main(void)
{
    unsigned i, j;
    case_name = "initialization";
    rgba_bits();
    memset(&R, 0, sizeof R); memset(&H, 0, sizeof H);
    for (i = 0; i < LT_MAX; i++) { memset(R.driver[i].before, GUARD, 16); memset(R.driver[i].after, GUARD, 16); memset(R.driver[i].data, 0x39, TABLE_BYTES); }
    for (i = 0; i < CM_COUNT; i++) { memset(R.zone[i].before, GUARD, 256); memset(R.zone[i].after, GUARD, 256); memset(R.zone[i].data, 0x6d, sizeof R.zone[i].data); }
    pLocalPalette = R.local; pMasterPalette = R.master; colormaps = R.default_input;
    H.clut_base = 4096; pal_last_mode = -1; R.shader = 1; cv_glpaletterendering.value = 1; vid.glstate = VID_GL_LIBRARY_LOADED;
    for (i = 0; i < CM_COUNT; i++)
    {
        R.cmap[i].colormap = R.input[i]; R.cmap[i].rgba = mix(i); R.cmap[i].fadergba = mix(i + 77);
        R.cmap[i].fadestart = i; R.cmap[i].fadeend = i + 20; R.cmap[i].flags = i & 3;
    }
    hwdriver.pfnCreateLightTable = hw_CreateLightTable; hwdriver.pfnUpdateLightTable = hw_UpdateLightTable;
    hwdriver.pfnClearLightTables = hw_ClearLightTables;
    palettes(9, 0, 0); inputs(19); capture(&saved[0]); capture(&saved[1]);

    STEP("driver-down create", GET, -1, 0, 0);
    STEP("driver-up", UP, 1, 0, 0);
    STEP("reverse-palette unset retry", GET, -1, 0, 0);
    STEP("set reverse palette", TEXTURE_PALETTE, 0, 0, 0);
    STEP("default global indices create", GET, -1, 0, 0);
    STEP("default metadata indices create", GET, 0, 0, 0);
    STEP("plain/keyed actual CLUT", CONSUME, 0, 0, 0);
    STEP("distinct duplicate RGB forward palettes", PALETTES, 1, 1, 0x8529);
    STEP("stable ID does not resubmit", GET, -1, 0, 0);
    STEP("set duplicate reverse palette", TEXTURE_PALETTE, 0, 0, 0);
    STEP("set screen flash palette", SCREEN_PALETTE, 0, 0, 0);
    STEP("same screen palette no-op", SCREEN_PALETTE, 0, 0, 0);
    STEP("null screen palette no-op", SCREEN_PALETTE, 1, 0, 0);
    STEP("default RGB cache must recapture when ID zero", RESET_ID, 0, 0, 0);
    STEP("recaptured forward palette duplicate/miss", GET, -1, 0, 0);
    STEP("flash consumes canonical roundtrip", CONSUME, 0, 0, 0);
    for (i = 1; i < CM_COUNT; i++) STEP("create all engine colormaps", GET, i, 0, 0);
    for (i = 0; i < 4; i++)
    {
        STEP("forward palette policy", MODE, i & 1, (i >> 1) & 1, i != 2);
        STEP("queue old rows", QUEUE, 1, 17, 1);
        STEP("script source edits", INPUTS, 0, 0, 891 + i);
        STEP("script marks dirty", NEED, 1, 1, 0);
        STEP("update must flush previous rows", GET, 1, 0, 0);
        STEP("new plain/keyed CLUT rows", CONSUME, 1, 0, 0);
    }
    STEP("use local forward palette", MODE, 0, 1, 1);
    STEP("valid input/forward palette byte alias", ALIAS, 1, 0, 67);
    STEP("alias marks dirty", NEED, 7, 1, 0);
    STEP("aliased source/forward palette update", GET, 7, 0, 0);
    STEP("aliased source consumer", CONSUME, 7, 0, 0);
    STEP("restore separate inputs", ALIAS, 0, 0, 0);
#ifdef NEXT12_LIGHTTABLE_RGB_MASK
    for (i = 0; i < 4; i++)
    {
        STEP("packed palette and indices offset", ALIAS, 2 + i, 0, 31 + i);
        STEP("packed palette offset dirty", NEED, 7, 1, 0);
        STEP("packed palette offset update", GET, 7, 0, 0);
        STEP("packed palette offset consumer", CONSUME, 7, 0, 0);
    }
    STEP("restore after packed offsets", ALIAS, 0, 0, 0);
#endif
    STEP("context reset retains table lifetime", CONTEXT, 0, 0, 0);
    STEP("context retains ID with historical forward RGB", GET, 2, 0, 0);
    STEP("after context reset lighting", CONSUME, 2, 0, 0);
    STEP("context new screen flash", SCREEN_PALETTE, 0, 0, 0);
    STEP("new level ID re-use", LEVEL_RESET, 0, 0, 8931);
    STEP("new level table", GET, 1, 0, 0);
    for (i = 0; i < 260; i++)
    {
        STEP("8-bit generation wrap input", INPUTS, 0, 0, 0x7131 + i);
        STEP("8-bit generation wrap dirty", NEED, 1, 1, 0);
        STEP("8-bit generation wrap update", GET, 1, 0, 0);
        if ((i % 63) == 0) STEP("8-bit generation cache row", CONSUME, 1, 0, 0);
    }
    STEP("palette generation wrap start", PALGEN_WRAP, 0, 0, 0xffffffffu);
    STEP("palette generation wrap flush", PALETTES, 1, 1, 412);
    STEP("palette generation wraps normally", TEXTURE_PALETTE, 0, 0, 0);
    for (i = 0; i < 4; i++)
    {
        STEP("debug row/order policy", DEBUG, (i & 1 ? 16 : 0) | (i & 2 ? 16384 : 0), 0, 0);
        STEP("reset CLUT debug cache", CONTEXT, 0, 0, 0);
        STEP("debug consumer", CONSUME, 1, 0, 0);
    }
    STEP("debug reset", DEBUG, 0, 0, 0);
    STEP("driver-only clear", DRIVER_CLEAR, 0, 0, 0);
    STEP("stale engine ID unchanged", GET, 1, 0, 0);
    STEP("stale engine ID dirty", NEED, 1, 1, 0);
    STEP("stale engine ID update rejected", GET, 1, 0, 0);
    STEP("invalid direct update zero", DIRECT_UPDATE, 0, 0, 0);
    STEP("invalid direct update bound", DIRECT_UPDATE, 0, 0, LT_MAX);
    STEP("invalid direct update UINT32_MAX", DIRECT_UPDATE, 0, 0, 0xffffffffu);
    STEP("engine clear library not loaded", ENGINE_CLEAR, VID_GL_LIBRARY_NOTLOADED, 0, 0);
    STEP("new level cleanup", LEVEL_RESET, 0, 0, 412);
    STEP("engine clear normal loaded", ENGINE_CLEAR, VID_GL_LIBRARY_LOADED, 0, 0);
    STEP("ID reset for allocation retry", RESET_ID, 1, 0, 0);
    STEP("driver allocation failure enabled", MALLOC_FAIL, 1, 0, 0);
    STEP("driver allocation failure getter", GET, 1, 0, 0);
    STEP("driver allocation failure direct", DIRECT_CREATE, 0, 0, 0);
    STEP("driver allocation succeeds", MALLOC_FAIL, 0, 0, 0);
    STEP("driver allocation retry getter", GET, 1, 0, 0);
    STEP("driver-down update setup", UP, 0, 0, 0);
    STEP("driver-down dirty", NEED, 1, 1, 0);
    STEP("driver-down update", GET, 1, 0, 0);
    STEP("driver-up again", UP, 1, 0, 0);
    STEP("capacity clean start", ENGINE_CLEAR, VID_GL_LIBRARY_LOADED, 0, 0);
    STEP("all 1023 IDs and exact limit behavior", EXHAUST, 0, 0, 0);
    STEP("clear every capacity allocation", ENGINE_CLEAR, VID_GL_LIBRARY_LOADED, 0, 0);
    STEP("ID reuse after exhaustive clear", DIRECT_CREATE, 0, 0, 0);
    STEP("same indexed direct update", DIRECT_UPDATE, 0, 0, 1);
    for (j = 0; j < 12; j++)
    {
        STEP("random temporal level", LEVEL_RESET, 0, 0, mix(j));
        STEP("random temporal palette", PALETTES, j & 1, j % 3 == 0, mix(j + 69));
        STEP("random reverse palette", TEXTURE_PALETTE, 0, 0, 0);
        STEP("random forward selection", MODE, j & 1, j % 3 != 0, j % 5 != 0);
        for (i = 0; i < CM_COUNT; i++) { STEP("random table", GET, i ? (int)i : -1, 0, 0); STEP("random complete rows", CONSUME, i, 0, 0); }
    }
    printf("LIGHTTABLE PASS cases=%llu compared_bytes=%llu rgb_checks=%llu RGBA_size=%zu RGBA_align=%zu typed_H=%zu typed_colormap=%zu full_driver_slots=%u rows=32 keyed=2 policies=palette/context/reset/alias/capacity\n", (unsigned long long)cases, (unsigned long long)compared_bytes, (unsigned long long)rgb_checks, sizeof(RGBA_t), _Alignof(RGBA_t), sizeof H, sizeof R.cmap[0], LT_MAX - 1);
    return 0;
}
