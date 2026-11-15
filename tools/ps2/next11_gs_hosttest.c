/* Actual complete packet builders with an independent CLUT index/packet/state oracle. */
#include <stdio.h>
#include <stdint.h>
#include "common.h"

clut_state_t H;
clut_v_t V;
clut_vu_t VU __attribute__((aligned(16)));
int ps2hwd_dbg_flags;
u32 fixture_spal[256];
int fixture_spal_set, fixture_palette_mode;
u8 *fixture_lt_idx[1024];
static u8 light_table[8192];
static qw_t rings[NBUF][BUF_QW + 4] __attribute__((aligned(64)));
static qw_t wanted_rings[NBUF][BUF_QW + 4] __attribute__((aligned(64)));
static clut_state_t wanted_h, saved_h;
static clut_v_t wanted_v;
static clut_vu_t wanted_vu;
static u32 fixture_cases, fixture_digest = 2166136261u, fixture_packets, flushes, vu_flushes;
static u32 wanted_flushes, wanted_vu_flushes;
static u32 fixture_rng = 0x1f8356c9u;
static u32 random_word(void) { fixture_rng ^= fixture_rng << 13; fixture_rng ^= fixture_rng >> 17; fixture_rng ^= fixture_rng << 5; return fixture_rng; }
#ifdef NEXT11_GS_EE
#include "low-globals.h"
#include "native-palette.h"
extern void *base___wrap_memcpy(void *, const void *, size_t);
extern void *cur___wrap_memcpy(void *, const void *, size_t);
extern void *base___wrap_memset(void *, int, size_t);
extern void *cur___wrap_memset(void *, int, size_t);
u8 cfg[56] __attribute__((aligned(8))), TX[224] __attribute__((aligned(8)));
u8 low_up_hist[16384] __attribute__((aligned(8)));
static unsigned char *native_initial, *native_wanted;
static unsigned native_bytes;
static void native_globals_init(void)
{
    unsigned i, at = 0;
    for (i = 0; i < sizeof low_globals / sizeof low_globals[0]; i++) native_bytes += low_globals[i].bytes;
    native_initial = malloc(native_bytes); native_wanted = malloc(native_bytes);
    if (!native_initial || !native_wanted) clut_abort();
    for (i = 0; i < sizeof low_globals / sizeof low_globals[0]; i++)
    { memcpy(native_initial + at, low_globals[i].address, low_globals[i].bytes); at += low_globals[i].bytes; }
}
static void native_globals_copy(unsigned char *bytes, int direction)
{
    unsigned i, at = 0;
    for (i = 0; i < sizeof low_globals / sizeof low_globals[0]; i++)
    {
        if (direction) memcpy(low_globals[i].address, bytes + at, low_globals[i].bytes);
        else memcpy(bytes + at, low_globals[i].address, low_globals[i].bytes);
        at += low_globals[i].bytes;
    }
}
static int native_globals_check(void)
{
    unsigned i, at = 0;
    for (i = 0; i < sizeof low_globals / sizeof low_globals[0]; i++)
    {
        if (memcmp(native_wanted + at, low_globals[i].address, low_globals[i].bytes)) return 0;
        at += low_globals[i].bytes;
    }
    return 1;
}
#endif

void clut_abort(void) { printf("GS NEXT11 FAIL unexpected boundary\n"); exit(1); }
void low_unexpected(void) { clut_abort(); }
void low_output(const char *format, ...) { (void)format; clut_abort(); }
void clut_pk_flush(void) { H.cur = (H.cur + 1) % NBUF; H.wr = 1; H.gsr.valid = 0; flushes++; }
void clut_vu_chunk_end(void) { V.chunk_open = VU.open = 0; H.gsr.valid = 0; H.clut_loaded = 0; H.serial++; vu_flushes++; }

static u32 permute(u32 index)
{
    return (index & 0xE7u) + ((index & 8u) * 2u) + ((index & 16u) / 2u);
}

static int direct_swizzles(void)
{
    static u32 buffer[280] __attribute__((aligned(64))), wanted[280];
    unsigned alignment, identity, pattern, tag, i;
    memset(&H, 0xC7, sizeof H); saved_h = H;
    for (alignment = 0; alignment < 4; alignment++)
        for (identity = 0; identity < 2; identity++)
            for (pattern = 0; pattern < 8196; pattern++)
            {
                u32 initial[280];
                for (i = 0; i < 280; i++)
                    initial[i] = pattern < 8192 ? 0 : pattern == 8192 ? UINT32_MAX :
                        pattern == 8193 ? i : pattern == 8194 ? (i * 0x01010101u) : random_word();
                if (pattern < 8192) initial[8 + alignment + pattern / 32u] = 1u << (pattern & 31u);
                memcpy(wanted, initial, sizeof initial);
                for (i = 0; i < 256; i++) wanted[8 + alignment + (identity ? i : permute(i))] = initial[8 + alignment + i];
                ps2hwd_dbg_flags = identity ? 16 : 0;
                for (tag = 0; tag < 2; tag++)
                {
                    memcpy(buffer, initial, sizeof buffer);
                    if (tag) cur_src_clut_swizzle(buffer + 8 + alignment);
                    else base_src_clut_swizzle(buffer + 8 + alignment);
                    if (memcmp(buffer, wanted, sizeof buffer))
                    {
                        printf("GS NEXT11 FAIL direct alignment=%u identity=%u pattern=%u tag=%u\n", alignment * 4u, identity, pattern, tag);
                        return 0;
                    }
                    fixture_cases++;
                }
            }
    /* The early identity return must not read the pointer. */
    ps2hwd_dbg_flags = 16;
    base_src_clut_swizzle(NULL); cur_src_clut_swizzle(NULL);
    if (memcmp(&H, &saved_h, sizeof H)) { printf("GS NEXT11 FAIL direct whole state\n"); return 0; }
    fixture_cases += 2;
    return 1;
}

static void palette_setup(unsigned mode, unsigned seed)
{
    unsigned i;
    fixture_palette_mode = mode & 1u; fixture_spal_set = (mode >> 1) & 1u;
    memset(fixture_lt_idx, 0, sizeof fixture_lt_idx); fixture_lt_idx[1] = light_table;
    for (i = 0; i < 8192; i++) light_table[i] = (u8)(i * 13u + seed);
    for (i = 0; i < 256; i++)
    {
        H.pal[i] = ((i * 7919u + seed * 37u) ^ (i * 0x731d1u)) & 0xFFFFFFu;
        fixture_spal[i] = ((i * 6311u + seed * 51u) ^ (i * 0x19173u)) & 0xFFFFFFu;
    }
}

static void initialize(unsigned seed, unsigned mode, unsigned boundary, unsigned identity)
{
    unsigned i;
#ifdef NEXT11_GS_EE
    native_globals_copy(native_initial, 1);
#endif
    memset(&H, 0xA5, sizeof H); memset(&V, 0, sizeof V); memset(&VU, 0, sizeof VU);
    memset(rings, 0x96, sizeof rings);
    for (i = 0; i < NBUF; i++) H.ring[i] = rings[i] + 2;
    H.cur = seed % NBUF; H.wr = boundary < 73 ? BUF_QW - boundary : 1;
    H.serial = seed & 1u ? UINT32_MAX : 43;
    H.clut_epoch = seed & 2u ? UINT32_MAX : 29;
    H.clut_tick = seed & 4u ? UINT32_MAX : 17;
    H.st.clut_uploads = seed & 8u ? UINT32_MAX : 31;
    H.clut_base = 2048; H.pal_gen = seed & 16u ? UINT32_MAX : 47;
    memset(H.clut_ent, 0, sizeof H.clut_ent);
    H.clut_ent[0].gen = seed & 32u ? H.pal_gen : H.pal_gen - 1u;
    H.clut_loaded = seed & 64u ? 2048 : 0;
    V.chunk_open = seed & 128u ? 1 : 0;
    VU.open = seed & 256u ? 1 : 0;
    palette_setup(mode, seed);
#ifdef NEXT11_GS_EE
    native_palette_setup(fixture_palette_mode, fixture_spal_set, fixture_spal, light_table);
    native_globals_copy(native_wanted, 0);
#endif
    flushes = vu_flushes = 0;
    ps2hwd_dbg_flags = identity ? 16 : 0;
    wanted_h = H; wanted_v = V; wanted_vu = VU;
    wanted_flushes = wanted_vu_flushes = 0;
    memcpy(wanted_rings, rings, sizeof rings);
}

static qw_t *oracle_reserve(u32 amount)
{
    qw_t *result;
    if (wanted_v.chunk_open && wanted_vu.open)
    {
        wanted_v.chunk_open = wanted_vu.open = 0;
        wanted_h.gsr.valid = 0; wanted_h.clut_loaded = 0; wanted_h.serial++;
        wanted_vu_flushes++;
    }
    if (wanted_h.wr + amount > BUF_QW)
    {
        wanted_h.cur = (wanted_h.cur + 1) % NBUF; wanted_h.wr = 1; wanted_h.gsr.valid = 0;
        wanted_flushes++;
    }
    result = wanted_rings[wanted_h.cur] + 2 + wanted_h.wr;
    wanted_h.wr += amount;
    return result;
}
static void oracle_word(qw_t *q, u64 value, u64 address) { q->d[0] = value; q->d[1] = address; }

static void oracle_image(u32 block, const u32 *entries, int epoch)
{
    unsigned i;
    qw_t *q;
    if (epoch) wanted_h.clut_epoch++;
    q = oracle_reserve(72);
    wanted_h.serial++;
    oracle_word(q + 0, 4ull | (1ull << 60), 14);
    oracle_word(q + 1, (u64)block << 32 | 1ull << 48, 80);
    oracle_word(q + 2, 0, 81);
    oracle_word(q + 3, 16ull | 16ull << 32, 82);
    oracle_word(q + 4, 0, 83);
    oracle_word(q + 5, 64ull | 1ull << 15 | 2ull << 58, 0);
    for (i = 0; i < 256; i++) memcpy((unsigned char *)(q + 6) + (ps2hwd_dbg_flags & 16 ? i : permute(i)) * 4u, entries + i, 4);
    oracle_word(q + 70, 1ull | 1ull << 15 | 1ull << 60, 14);
    oracle_word(q + 71, 0, 63);
    if (wanted_h.clut_loaded == block) wanted_h.clut_loaded = 0;
    wanted_h.st.clut_uploads++;
    fixture_packets++;
}

static u32 oracle_spec_hash(const rampspec_t *spec)
{
    const unsigned char *bytes = (const unsigned char *)spec;
    u32 value = 2166136261u;
    unsigned i;
    for (i = 0; i < sizeof *spec; i += 4)
    {
        u32 word = (u32)bytes[i] | (u32)bytes[i + 1] << 8 | (u32)bytes[i + 2] << 16 | (u32)bytes[i + 3] << 24;
        value = (value ^ word) * 16777619u;
    }
    return value;
}

static u32 oracle_call(unsigned family, u32 kind, u32 param, u32 keyed, const rampspec_t *spec)
{
    u32 entries[256];
    if (family == 0)
    {
        base_src_clut_entries(kind, param, keyed, entries);
        oracle_image(2048, entries, 1);
    }
    else if (family == 1)
    {
        if (kind || wanted_h.clut_ent[0].gen != wanted_h.pal_gen)
        {
            base_src_clut_entries(1, 0, 0, entries); oracle_image(2048, entries, 1);
            base_src_clut_entries(2, 0, 0, entries); oracle_image(2052, entries, 1);
            wanted_h.clut_ent[0].gen = wanted_h.pal_gen;
        }
        if (kind)
        {
            base_src_clut_entries(3, 0, 0, entries); oracle_image(2056, entries, 1);
            base_src_clut_entries(4, 0, 0, entries); oracle_image(2060, entries, 1);
        }
        {
            qw_t *q = oracle_reserve(2);
            wanted_h.serial++;
            oracle_word(q, 1ull | 1ull << 15 | 1ull << 60, 14); oracle_word(q + 1, 0, 63);
            wanted_h.clut_loaded = 0;
        }
    }
    else
    {
        const u64 key = 7ull | (u64)oracle_spec_hash(spec) << 16 | (u64)spec->variant << 8;
        wanted_h.clut_tick++;
        if (wanted_h.clut_ent[4].key != key)
        {
            wanted_h.clut_ent[4].key = key;
            wanted_h.clut_ent[4].stamp = wanted_h.clut_tick;
            base_src_ramp_entries(spec, entries); oracle_image(2064, entries, 0);
        }
        else wanted_h.clut_ent[4].stamp = wanted_h.clut_tick;
        return 2064;
    }
    return 0;
}

static u32 call_builder(int tag, unsigned family, u32 kind, u32 param, u32 keyed, const rampspec_t *spec)
{
    if (family == 0)
    {
        void (*volatile fn)(u32,u32,u32,u32) = tag ? cur_clut_build_into : base_clut_build_into;
        fn(2048,kind,param,keyed);
    }
    else if (family == 1)
    {
        void (*volatile fn)(int) = tag ? cur_clut_fixed_refresh : base_clut_fixed_refresh;
        fn((int)kind);
    }
    else
    {
        u32 (*volatile fn)(const rampspec_t *) = tag ? cur_ramp_clut : base_ramp_clut;
        return fn(spec);
    }
    return 0;
}

static int check_result(unsigned tag, unsigned family, unsigned kind, u32 returned, u32 expected)
{
#ifdef NEXT11_GS_EE
    if (!native_globals_check()) { printf("GS NEXT11 FAIL graph data changed\n"); return 0; }
#endif
    if (returned != expected || memcmp(&H, &wanted_h, sizeof H) || memcmp(&V, &wanted_v, sizeof V) ||
        memcmp(&VU, &wanted_vu, sizeof VU) || memcmp(rings, wanted_rings, sizeof rings) ||
        flushes != wanted_flushes || vu_flushes != wanted_vu_flushes)
    {
        printf("GS NEXT11 FAIL packet tag=%u family=%u kind=%u case=%u H=%d V=%d VU=%d ring=%d flush=%u/%u vu=%u/%u\n",
               tag, family, kind, fixture_cases, memcmp(&H,&wanted_h,sizeof H),memcmp(&V,&wanted_v,sizeof V),memcmp(&VU,&wanted_vu,sizeof VU),
               memcmp(rings,wanted_rings,sizeof rings),flushes,wanted_flushes,vu_flushes,wanted_vu_flushes);
        return 0;
    }
    fixture_digest = (fixture_digest ^ H.wr ^ H.serial ^ H.clut_epoch ^ H.clut_tick ^ H.st.clut_uploads ^ returned) * 16777619u;
    fixture_cases++;
    return 1;
}

#ifdef NEXT11_GS_EE
static u32 cycles(void) { u32 v; __asm__ volatile("mfc0 %0,$9" : "=r"(v) : : "memory"); return v; }
static u32 begin_time(void) { u32 v; __asm__ volatile("mfc0 %0,$12\n\tdi\n\tsync.p" : "=r"(v) : : "memory"); return v; }
static void end_time(u32 v) { if (v & 0x10000u) __asm__ volatile("ei\n\tsync.p" : : : "memory"); }
static int benchmark(void)
{
    static const unsigned matrix[][3] = {
        {0,1,0},{0,2,0},{0,3,0},{0,4,0},{0,5,0},{0,6,0},{0,8,0},{0,9,0},
        {1,0,1},{1,0,0},{1,1,0},
        {2,1,0},{2,2,0},{2,3,0},{2,4,0},{2,5,0},{2,1,1}
    };
    unsigned ci, identity, cache, length, rep, order, row = 0;
    for (cache = 0; cache < 2; cache++)
        for (identity = 0; identity < 2; identity++)
            for (ci = 0; ci < sizeof matrix / sizeof matrix[0]; ci++)
                for (length = 0; length < (matrix[ci][0] == 2 ? 1u : 3u); length++)
                {
                    const unsigned family = matrix[ci][0], kind = matrix[ci][1], flag = matrix[ci][2];
                    const unsigned count = length == 0 ? 1u : length == 1 ? (family == 1 ? 2u : 8u) : (family == 1 ? 16u : 64u);
                    const u32 param = kind == 9 ? 1u | 17u << 18 : kind == 6 ? 0x8001u : 0x80402010u;
                    rampspec_t spec;
                    memset(&spec,0,sizeof spec); light_setup(&spec.lp,128,1,0,31);
                    spec.fade = 0x341287; spec.tint = 0x80604020; spec.alpha = 97; spec.scale = 1;
                    spec.variant = (u8)(family == 2 ? kind : 1);
                    for (rep = 0; rep < 9; rep++)
                    {
                        u32 times[2] = {0,0};
                        for (order = 0; order < 2; order++)
                        {
                            unsigned j;
                            const unsigned tag = order ^ (rep & 1u);
                            u32 expected = 0, returned = 0, before, previous;
                            initialize(family == 1 && flag ? 32u : 0u,3,97,identity);
                            if (family == 2 && flag)
                            {
                                H.clut_ent[4].key = 7ull | (u64)oracle_spec_hash(&spec) << 16 | (u64)spec.variant << 8;
                                wanted_h = H;
                            }
                            for (j = 0; j < count; j++) expected = oracle_call(family,kind,param,1,&spec);
                            if (cache) { FlushCache(0); FlushCache(1); FlushCache(2); }
                            previous = begin_time(); before = cycles();
                            for (j = 0; j < count; j++) returned = call_builder((int)tag,family,kind,param,1,&spec);
                            times[tag] = cycles() - before; end_time(previous);
                            if (!check_result(tag,family,kind,returned,expected) || times[tag] >= 0x80000000u || flushes || vu_flushes) return 0;
                        }
                        printf("GS11 TIMING row=%u case=%u family=%u kind=%u flag=%u identity=%u cache=%u count=%u rep=%u old=%u new=%u\n",
                               row,ci,family,kind,flag,identity,cache,count,rep,times[0],times[1]);
                    }
                    row++;
                }
    return row == 156;
}
#endif

int main(void)
{
    static const u32 kinds[] = {1,2,3,4,5,6,8,9};
    unsigned i, tag;
#ifdef NEXT11_GS_EE
    void *(*volatile keep_memmove)(void *, const void *, size_t) = memmove;
    void *(*volatile keep_copy[2])(void *, const void *, size_t) = {base___wrap_memcpy,cur___wrap_memcpy};
    void *(*volatile keep_set[2])(void *, int, size_t) = {base___wrap_memset,cur___wrap_memset};
    if (((volatile u8 *)cfg)[0] || ((volatile u8 *)TX)[0] || ((volatile u8 *)low_up_hist)[0]) return 1;
    keep_memmove(rings, rings, 0);
    for (tag = 0; tag < 2; tag++) { keep_copy[tag](rings,rings,0); keep_set[tag](rings,0,0); }
    native_globals_init();
#endif
    if (!direct_swizzles()) return 1;
    for (i = 0; i < 2048; i++)
        for (tag = 0; tag < 2; tag++)
        {
            rampspec_t spec;
            const unsigned family = i % 3u;
            const u32 kind = family == 0 ? kinds[(i / 3u) % 8u] : family == 1 ? (i / 3u) % 2u : 0;
            const u32 param = kind == 9 ? 1u | (i & 31u) << 18 : kind == 6 ? (i % 3u) | (i & 255u) << 8 : 0x80402010u + i * 0x010101u;
            u32 expected, returned;
            memset(&spec, 0, sizeof spec);
            light_setup(&spec.lp, (int)(i & 255u), (int)(i & 1u), 0, 31);
            spec.fade = i * 7919u & 0xFFFFFFu; spec.tint = 0x80604020u + i * 0x010101u;
            spec.alpha = (u8)i; spec.variant = (u8)(1 + i % 5u); spec.scale = (u8)(i % 3u);
            initialize(i, i / 7u % 4u, i % 97u, i / 5u % 2u);
            if (family == 2 && (i & 4u))
            {
                H.clut_ent[4].key = 7ull | (u64)oracle_spec_hash(&spec) << 16 | (u64)spec.variant << 8;
                wanted_h = H;
            }
            expected = oracle_call(family,kind,param,i & 1u,&spec);
            returned = call_builder((int)tag,family,kind,param,i & 1u,&spec);
            if (!check_result(tag,family,kind,returned,expected)) return 1;
        }
    printf("GS NEXT11 EXACT cases=%u packets=%u digest=%08x\n",fixture_cases,fixture_packets,fixture_digest);
#ifdef NEXT11_GS_EE
    if (!benchmark()) return 1;
#endif
    printf("GS NEXT11 DONE\n");
    return 0;
}
