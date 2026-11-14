/* Actual frozen/current functions are extracted verbatim by gs_next2_hosttest.py.
 * The sole mocked packet operations close VU chunks and rotate buffers; no GS is needed for exact packet-byte comparison. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

typedef uint64_t u64;
typedef unsigned int u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define CONS_ERROR 0
#define CONS_Alert(level, ...) fprintf(stderr, __VA_ARGS__)
static u32 ps2hwd_dbg_flags;
#include "common.inc"

static struct
{
    u32 pool_base, pool_blocks, used_blocks;
#include "base-ranges.inc"
    int free_n, cur;
    u32 wr, serial, clut_epoch, clut_loaded, clut_base, clut_tick, pal_gen;
    u32 pal[256];
    clutent_t clut_ent[CLUT_SLOTS];
    struct { u32 clut_uploads; } st;
    qw_t *ring[NBUF];
    u32 flushes, vu_flushes, ov_flushes;
} base_H;
static struct
{
    u32 pool_base, pool_blocks, used_blocks;
#include "cur-ranges.inc"
    int free_n, cur;
    u32 wr, serial, clut_epoch, clut_loaded, clut_base, clut_tick, pal_gen;
    u32 pal[256];
    clutent_t clut_ent[CLUT_SLOTS];
    struct { u32 clut_uploads; } st;
    qw_t *ring[NBUF];
    u32 flushes, vu_flushes, ov_flushes;
} cur_H;
static struct { int chunk_open; } base_V, cur_V;
static struct { int n; } base_OV, cur_OV;
static qw_t rings[2][NBUF][BUF_QW];
static u32 spal[256];
static int spal_set, mode;
static u8 table[4][8192];
static u8 *lt_idx[1024];
static int palette_mode(void) { return mode; }

static void init_tables(void)
{
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 8192; j++) table[i][j] = (u8)(i * 17 + j * 13);
        lt_idx[i + 1] = table[i];
    }
}

#define MOCKS(tag) \
static void tag##_vu_chunk_flush(void) \
{ tag##_V.chunk_open = 0; tag##_H.clut_loaded = 0; tag##_H.vu_flushes++; } \
static void tag##_pk_flush(void) \
{ tag##_H.flushes++; tag##_H.cur = (tag##_H.cur + 1) % NBUF; tag##_H.wr = 1; } \
static void tag##_ov_flush_all(void) \
{ tag##_H.ov_flushes++; tag##_OV.n = 0; } \
static void tag##_ov_flush_pending(void) \
{ tag##_H.ov_flushes++; tag##_OV.n = 0; }
MOCKS(base)
MOCKS(cur)
#include "base-production.inc"
#include "cur-production.inc"

static u32 rng = 0x6a09e667u;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static u32 packets, alloc_ops, range_checks;
static int fail(const char *why)
{
    printf("GS NEXT2 FAIL %s packets=%u alloc_ops=%u ranges=%u\n", why, packets, alloc_ops, range_checks);
    return 1;
}

static void init(u32 start, u32 count)
{
    int i;
    memset(&base_H, 0, sizeof base_H);
    memset(&cur_H, 0, sizeof cur_H);
    memset(rings, 0xa5, sizeof rings);
    for (i = 0; i < NBUF; i++)
    { base_H.ring[i] = rings[0][i]; cur_H.ring[i] = rings[1][i]; }
    base_H.pool_base = cur_H.pool_base = start;
    base_H.pool_blocks = cur_H.pool_blocks = count;
    base_H.free_n = cur_H.free_n = 1;
    base_H.free_start[0] = cur_H.free_start[0] = start;
    base_H.free_len[0] = cur_H.free_len[0] = count;
    base_H.wr = cur_H.wr = 1;
    base_H.clut_base = cur_H.clut_base = 2048;
    base_H.pal_gen = cur_H.pal_gen = 1;
    base_H.clut_loaded = cur_H.clut_loaded = 2048;
    base_OV.n = cur_OV.n = base_V.chunk_open = cur_V.chunk_open = 0;
    memset(base_clut_hint, 0, sizeof base_clut_hint);
    memset(cur_clut_hint, 0, sizeof cur_clut_hint);
    for (i = 0; i < 256; i++)
    {
        base_H.pal[i] = cur_H.pal[i] = rnd() & 0xffffffu;
        spal[i] = rnd() & 0xffffffu;
    }
}

static int ranges_equal(void)
{
    int i;
    u32 total = 0, end = base_H.pool_base;
    if (base_H.free_n != cur_H.free_n || base_H.used_blocks != cur_H.used_blocks) return 0;
    for (i = 0; i < base_H.free_n; i++)
    {
        u32 s = base_H.free_start[i], n = base_H.free_len[i];
        range_checks++;
        if (s != cur_H.free_start[i] || n != cur_H.free_len[i]) return 0;
        if (!n || s < end || (i && s == end) || s + n > VRAM_BLOCKS) return 0;
        total += n; end = s + n;
    }
    return total + base_H.used_blocks == base_H.pool_blocks;
}

static int allocator(void)
{
    struct { u32 s, n; } live[2048];
    int pass;
    /* Largest representable valid length, then maximum legal one-block fragmentation and both merge orders. */
    init(0, VRAM_BLOCKS);
    if (!ranges_equal()) return fail("whole GS range");
    init(1, VRAM_BLOCKS - 1);
    base_H.free_n = cur_H.free_n = 0;
    base_H.used_blocks = cur_H.used_blocks = VRAM_BLOCKS - 1;
    for (u32 b = 1; b < VRAM_BLOCKS; b += 2)
    {
        base_vram_free(b, 1); cur_vram_free(b, 1); alloc_ops++;
        if (!ranges_equal()) return fail("alternating blocks");
    }
    if (base_H.free_n != VRAM_BLOCKS / 2) return fail("fragmentation capacity");
    for (u32 b = VRAM_BLOCKS - 2; b >= 2; b -= 2)
    {
        base_vram_free(b, 1); cur_vram_free(b, 1); alloc_ops++;
        if (!ranges_equal()) return fail("reverse merge");
    }
    for (pass = 0; pass < 4; pass++)
    {
        const u32 starts[4] = {1, 512, 4096, 12000};
        int nlive = 0;
        init(starts[pass], VRAM_BLOCKS - starts[pass]);
#ifdef GS_NEXT2_EE
        const int iterations = 2000;
#else
        const int iterations = 100000;
#endif
        for (int it = 0; it < iterations; it++)
        {
            if (!nlive || (nlive < 2048 && rnd() % 3))
            {
                u32 n = (rnd() % 8) ? 1 + rnd() % 63 : 1 + rnd() % 2048;
                u32 align = 1u << (rnd() % 8), b, c;
                if (rnd() & 1) { b = base_vram_alloc(n, align); c = cur_vram_alloc(n, align); }
                else { b = base_vram_alloc_hi(n, align); c = cur_vram_alloc_hi(n, align); }
                if (b != c) return fail("allocation address");
                if (b)
                {
                    if ((b & (align - 1)) || b < starts[pass] || b + n > VRAM_BLOCKS) return fail("allocation bounds");
                    for (int j = 0; j < nlive; j++)
                        if (b < live[j].s + live[j].n && b + n > live[j].s) return fail("allocation overlap");
                    live[nlive].s = b; live[nlive++].n = n;
                }
            }
            else
            {
                int j = (int)(rnd() % (u32)nlive);
                base_vram_free(live[j].s, live[j].n); cur_vram_free(live[j].s, live[j].n);
                live[j] = live[--nlive];
            }
            alloc_ops++;
            if (!ranges_equal()) return fail("randomized free list");
        }
        while (nlive)
        {
            nlive--;
            base_vram_free(live[nlive].s, live[nlive].n); cur_vram_free(live[nlive].s, live[nlive].n);
            alloc_ops++;
            if (!ranges_equal()) return fail("final merge");
        }
    }
    return 0;
}

static int packet_equal(void)
{
#define SAME(field) if (base_H.field != cur_H.field) return 0
    SAME(cur); SAME(wr); SAME(serial); SAME(clut_epoch); SAME(clut_loaded); SAME(clut_tick);
    SAME(st.clut_uploads); SAME(flushes); SAME(vu_flushes); SAME(ov_flushes);
#undef SAME
    if (base_OV.n != cur_OV.n || base_V.chunk_open != cur_V.chunk_open) return 0;
    if (memcmp(base_H.clut_ent, cur_H.clut_ent, sizeof base_H.clut_ent)) return 0;
    if (memcmp(base_clut_hint, cur_clut_hint, sizeof base_clut_hint)) return 0;
    return memcmp(rings[0], rings[1], sizeof rings[0]) == 0;
}

static int palettes(void)
{
    int i;
    init_tables();
    init(1, VRAM_BLOCKS - 1);
    for (i = 0; i < 4096; i++)
    {
        u32 kinds[] = {CK_OPAQUE, CK_KEYED, CK_ALPHA, CK_WHITEKEY, CK_TINT, CK_MULT, CK_WIPE, CK_LIGHT};
        u32 kind = kinds[i & 7], param = rnd(), keyed = rnd() & 1;
        if (kind == CK_MULT) param = (rnd() % 3) | ((rnd() & 255) << 8);
        if (kind == CK_LIGHT) param = (rnd() % 7) | ((rnd() & 255) << 10) | ((rnd() & 31) << 18);
        if (kind == CK_WIPE) param = (rnd() % 3) | ((rnd() & 1) << 2) | ((rnd() & 255) << 8);
        mode = (i >> 3) & 1; spal_set = (i >> 4) & 1;
        ps2hwd_dbg_flags = (i & 32) ? 16 : 0;
        /* All 72 positions around a packet boundary, including one free qword, and a pending VU chunk. */
        if (i < 144) base_H.wr = cur_H.wr = BUF_QW - (u32)(i % 73);
        base_V.chunk_open = cur_V.chunk_open = (i & 3) == 0;
        base_H.clut_loaded = cur_H.clut_loaded = 2048 + 4 * (u32)(i % CLUT_SLOTS);
        base_clut_build_into(2048 + 4 * (u32)(i % CLUT_SLOTS), kind, param, keyed);
        cur_clut_build_into(2048 + 4 * (u32)(i % CLUT_SLOTS), kind, param, keyed);
        packets++;
        if (!packet_equal()) return fail("CLUT packet bytes/state");
    }
    ps2hwd_dbg_flags = 0;
    init(1, VRAM_BLOCKS - 1);
    for (i = 0; i < 2048; i++)
    {
        u32 param = 0x80808080u + (u32)(i % 151) * 0x01010101u;
        if (!(i % 97)) base_H.pal_gen = ++cur_H.pal_gen;
        base_OV.n = cur_OV.n = (i & 3) == 0;
        u32 b = base_clut_get(CK_TINT, param, (u32)(i & 1));
        u32 c = cur_clut_get(CK_TINT, param, (u32)(i & 1));
        packets++;
        if (b != c || !packet_equal()) return fail("CLUT cache miss/hit/replace");
        b = base_clut_get(CK_TINT, param, (u32)(i & 1));
        c = cur_clut_get(CK_TINT, param, (u32)(i & 1));
        if (b != c || !packet_equal()) return fail("CLUT cache repeat");
        if (!(i % 23))
        {
            b = base_clut_get(CK_OPAQUE, 0, 0); c = cur_clut_get(CK_OPAQUE, 0, 0);
            if (b != c || !packet_equal()) return fail("fixed palette refresh");
        }
    }
    init(1, VRAM_BLOCKS - 1);
    for (i = 0; i < 2048; i++)
    {
        rampspec_t s;
        memset(&s, 0, sizeof s);
        light_setup(&s.lp, (int)(rnd() & 255), (int)(rnd() & 1), 0, 31);
        s.fade = rnd() & 0xffffffu; s.tint = rnd(); s.alpha = (u8)rnd();
        s.variant = (u8)(RV_FADE + i % 5); s.scale = (u8)(i % 3);
        mode = (i >> 3) & 1; spal_set = (i >> 4) & 1;
        ps2hwd_dbg_flags = (i & 32) ? 16 : 0;
        base_OV.n = cur_OV.n = (i & 3) == 0;
        base_V.chunk_open = cur_V.chunk_open = (i & 3) == 0;
        u32 b = base_ramp_clut(&s), c = cur_ramp_clut(&s);
        packets++;
        if (b != c || !packet_equal()) return fail("ramp packet/cache replace");
        b = base_ramp_clut(&s); c = cur_ramp_clut(&s);
        if (b != c || !packet_equal()) return fail("ramp cache hit");
    }
    return 0;
}

#ifdef GS_NEXT2_EE
static u32 ticks(void) { u32 x; __asm__ volatile("mfc0 %0,$9" : "=r"(x)); return x; }
static volatile u32 sink;
static void benchmark(void)
{
    for (u32 kind = CK_OPAQUE; kind <= CK_LIGHT; kind++)
    {
        init(1, VRAM_BLOCKS - 1); mode = spal_set = 1; ps2hwd_dbg_flags = 0;
        u32 param = kind == CK_LIGHT ? 1u : kind == CK_MULT ? 0x8000u : 0x80a04020u;
        rampspec_t spec;
        memset(&spec, 0, sizeof spec);
        light_setup(&spec.lp, 128, 1, 0, 31); spec.variant = RV_FADE; spec.alpha = 255;
        for (int rep = 0; rep < 9; rep++)
        {
            u32 times[2];
            for (int j = 0; j < 2; j++)
            {
                int tag = j ^ (rep & 1);
                u32 t = ticks();
                for (int i = 0; i < 2048; i++)
                {
                    spec.fade = (u32)i;
                    if (!tag)
                    {
                        if (kind == CK_RAMP) sink ^= base_ramp_clut(&spec);
                        else base_clut_build_into(2048, kind, param, 0);
                        sink ^= base_H.ring[base_H.cur][base_H.wr - 3].w[0];
                    }
                    else
                    {
                        if (kind == CK_RAMP) sink ^= cur_ramp_clut(&spec);
                        else cur_clut_build_into(2048, kind, param, 0);
                        sink ^= cur_H.ring[cur_H.cur][cur_H.wr - 3].w[0];
                    }
                }
                times[tag] = ticks() - t;
            }
            printf("GSNEXT2BENCH kind=%u rep=%d builds=2048 base=%u current=%u\n", kind, rep, times[0], times[1]);
        }
    }
}
#endif

int main(void)
{
#ifndef GS_NEXT2_EE
    if (allocator() || palettes()) return 1;
    printf("GS NEXT2 PASS allocator_operations=%u exact_range_checks=%u packet_operations=%u range_array_bytes=%u->%u\n",
           alloc_ops, range_checks, packets,
           (unsigned)(sizeof base_H.free_start + sizeof base_H.free_len),
           (unsigned)(sizeof cur_H.free_start + sizeof cur_H.free_len));
#else
    init_tables();
    init(0, VRAM_BLOCKS);
    if (!ranges_equal()) return fail("EE whole GS range");
    init(1, VRAM_BLOCKS - 1); mode = spal_set = 1;
    for (u32 kind = CK_OPAQUE; kind <= CK_LIGHT; kind++)
    {
        if (kind == CK_RAMP) continue;
        base_clut_build_into(2048, kind, 1, 0);
        cur_clut_build_into(2048, kind, 1, 0);
        if (!packet_equal()) return fail("EE CLUT packet");
    }
    printf("GS NEXT2 EE PASS range_array_bytes=%u->%u; all 8 palette kinds have identical packets\n",
           (unsigned)(sizeof base_H.free_start + sizeof base_H.free_len),
           (unsigned)(sizeof cur_H.free_start + sizeof cur_H.free_len));
    benchmark();
#endif
    printf("GS NEXT2 DONE\n");
    return 0;
}
