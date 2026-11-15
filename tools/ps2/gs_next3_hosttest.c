/* Frozen/current production functions, extracted without implementation edits by gs_next3_hosttest.py. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef unsigned int u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define CONS_ERROR 0
#define CONS_Alert(level, ...) fprintf(stderr, __VA_ARGS__)
#define CONS_Printf(...) printf(__VA_ARGS__)
#define FX3_NOFILL 1u
#define HWDBG_COMPOSE 16777216u
static u32 ps2hwd_fx2, ps2hwd_dbg_flags;
#include "common.inc"
#include "palettes.inc"

static struct
{
    u32 pal[256], pool_base, serial, wr, flushes, vu_flushes;
    qw_t *ring[NBUF];
    int cur;
} base_H, cur_H;
static struct { int chunk_open; } base_V, cur_V;
static qw_t rings[2][NBUF][BUF_QW];
static void hw_failure(const char *why) { fprintf(stderr, "unexpected GS failure: %s\n", why); abort(); }
#define MOCKS(tag) \
static void tag##_vu_chunk_flush(void) { tag##_V.chunk_open = 0; tag##_H.vu_flushes++; } \
static void tag##_pk_flush(void) { tag##_H.cur = (tag##_H.cur + 1) % NBUF; tag##_H.wr = 1; tag##_H.flushes++; }
MOCKS(base)
MOCKS(cur)
#include "base-production.inc"
#include "cur-production.inc"

static u32 rng = 0x31415926u;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static u32 lookups, conversions, row_cases, packet_cases;
static int fail(const char *why)
{
    printf("GS NEXT3 FAIL %s lookups=%u conversions=%u rows=%u packets=%u\n", why, lookups, conversions, row_cases, packet_cases);
    return 1;
}
static void init(u32 wr)
{
    memset(&base_H, 0, sizeof base_H); memset(&cur_H, 0, sizeof cur_H);
    memset(rings, 0xa5, sizeof rings);
    for (int i = 0; i < NBUF; i++) { base_H.ring[i] = rings[0][i]; cur_H.ring[i] = rings[1][i]; }
    base_H.wr = cur_H.wr = wr; base_H.pool_base = cur_H.pool_base = 1;
    base_V.chunk_open = cur_V.chunk_open = 0;
}
static void setpal(const u32 *pal)
{
    memcpy(base_H.pal, pal, sizeof base_H.pal); memcpy(cur_H.pal, pal, sizeof cur_H.pal);
    base_pal_build(); cur_pal_build();
}
static int reference_lookup(u32 rgb)
{
    for (int i = 0; i < 256; i++) if (base_H.pal[i] == rgb) return i;
    return -1;
}
static int check_lookup(u32 rgb)
{
    int a = base_pal_lookup(rgb), b = cur_pal_lookup(rgb), c = reference_lookup(rgb);
    lookups++;
    return a == b && a == c;
}
static int palette_tests(void)
{
    u32 pal[256], src[256];
    u8 a[272], b[272];
#ifdef GS_NEXT3_EE
    const int random_cases = 32;
#else
    const int random_cases = 1024;
#endif
    const int real_count = (int)(sizeof real_palettes / sizeof real_palettes[0]);
    for (int pass = -real_count; pass < random_cases; pass++)
    {
        if (pass < 0) memcpy(pal, real_palettes[pass + real_count], sizeof pal);
        else for (int i = 0; i < 256; i++) pal[i] = rnd() & 0xffffffu;
        if (pass == 0) memset(pal, 0, sizeof pal);
        if (pass == 1) for (int i = 0; i < 256; i++) pal[i] = 0xffffffu;
        if (pass == 2) { pal[255] = 0xffffffu; pal[0] = 0; }
        if (pass == 3) { pal[255] = pal[127] = pal[0] = 0xffffffu; }
        if (pass == 4) for (int i = 0; i < 256; i++) pal[i] = (u32)i % 7u;
        if (pass == 5)
        {
            u32 n = 0;
            for (u32 c = 0; n < 256; c++) if (base_pal_hash(c) == 1023) pal[n++] = c;
        }
        setpal(pal);
        for (int i = 0; i < 256; i++) if (!check_lookup(pal[i])) return fail("palette hit/lowest duplicate");
        if (!check_lookup(0xffffffu) || !check_lookup(0)) return fail("sentinel/black");
        for (int i = 0; i < 2048; i++) if (!check_lookup(rnd() & 0xffffffu)) return fail("palette miss/collision");
        for (int group = 0; group < 8; group++)
        {
            for (int i = 0; i < 256; i++) src[i] = pal[rnd() & 255] | 0xff000000u;
            switch (group)
            {
                case 1: src[255] = pal[255]; break;
                case 2: src[0] = pal[255]; break;
                case 3: src[127] = (rnd() & 0xffffffu) | 0xff000000u; break;
                case 4: src[0] = (pal[255] ^ 1u); break;
                case 5: src[127] = (src[127] & 0xffffffu) | 0x80000000u; break;
                case 6: for (int i = 1; i < 256; i++) src[i] = src[0]; break;
                case 7: for (int i = 0; i < 256; i++) src[i] = pal[255]; break;
            }
            memset(a, 0xa5, sizeof a); memset(b, 0xa5, sizeof b);
            int ah = 123, au = 123, bh = 123, bu = 123;
            int av = base_rgba_to_indices(src, 256, a + 8, &ah, &au), bv = cur_rgba_to_indices(src, 256, b + 8, &bh, &bu);
            conversions++;
            if (av != bv || ah != bh || au != bu || memcmp(a, b, sizeof a)) return fail("RGBA classification/index conversion");
        }
    }
    return 0;
}
static int rows_and_packets(void)
{
    static u32 source[1024 * 257];
    static u32 a[1024 + 32], b[1024 + 32];
    const u32 widths[] = {1, 2, 3, 4, 7, 13, 16, 31, 63, 64, 127, 257, 1024};
    typedef void (*fill_t)(void *, u32, u8 *);
    fill_t base_fill[] = {base_fill_ap88_ct32, base_fill_ct32, base_fill_ia88};
    fill_t cur_fill[] = {cur_fill_ap88_ct32, cur_fill_ct32, cur_fill_ia88};
    for (u32 i = 0; i < sizeof source / sizeof source[0]; i++) source[i] = rnd();
    for (u32 i = 0; i < 256; i++) base_H.pal[i] = cur_H.pal[i] = rnd() & 0xffffffu;
    for (u32 i = 0; i < 256; i++)
    {
        if (base_a8_to_gs(i) != cur_a8_to_gs(i)) return fail("alpha rounding");
        source[i] = i | ((255u - i) << 8) | (i << 24);
    }
    for (int kind = 0; kind < 3; kind++)
        for (u32 wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
            for (u32 dx = 0; dx <= 5; dx++)
                for (u32 dy = 0; dy <= 3; dy++)
                    for (u32 off = 0; off < 4; off++)
                    {
                        u32 w = widths[wi];
                        if ((w << dx) > 1024) continue;
                        fillctx_t c;
                        memset(&c, 0, sizeof c);
                        c.src = (const u8 *)source + off * 4; c.sw = 1024; c.sh = 257;
                        c.W2 = w; c.H2 = 17; c.dx = dx; c.dy = dy;
                        for (u32 row = 0; row < 17; row += 8)
                        {
                            memset(a, 0xa5, sizeof a); memset(b, 0xa5, sizeof b);
                            base_fill[kind](&c, row, (u8 *)(a + off + 4)); cur_fill[kind](&c, row, (u8 *)(b + off + 4));
                            row_cases++;
                            if (memcmp(a, b, sizeof a)) return fail("CT32 row/padding");
                        }
                    }
    for (int kind = 0; kind < 3; kind++)
        for (int wi = 0; wi < 7; wi++)
        {
            u32 w = widths[wi * 2], h = wi & 1 ? 257u : 17u;
            for (u32 remaining = 1; remaining <= 8; remaining++)
            {
                init(BUF_QW - remaining);
                for (u32 i = 0; i < 256; i++) base_H.pal[i] = cur_H.pal[i] = rnd() & 0xffffffu;
                base_V.chunk_open = cur_V.chunk_open = (remaining & 1) != 0;
                fillctx_t c;
                memset(&c, 0, sizeof c); c.src = (const u8 *)source;
                c.sw = 1024; c.sh = 257; c.W2 = w; c.H2 = h;
                base_upload_bands(1024, (w + 63) / 64, PSM_CT32, w, h, 4, base_fill[kind], &c);
                cur_upload_bands(1024, (w + 63) / 64, PSM_CT32, w, h, 4, cur_fill[kind], &c);
                packet_cases++;
                if (base_H.wr != cur_H.wr || base_H.cur != cur_H.cur || base_H.serial != cur_H.serial
                    || base_H.flushes != cur_H.flushes || base_H.vu_flushes != cur_H.vu_flushes || memcmp(rings[0], rings[1], sizeof rings[0]))
                    return fail("complete upload packets/ring rotation");
            }
        }
    return 0;
}

#ifdef GS_NEXT3_EE
static u32 ticks(void) { u32 x; __asm__ volatile("mfc0 %0,$9" : "=r"(x)); return x; }
static volatile u32 sink;
static void benchmark(void)
{
    static u32 source[512 * 128], out[512];
    typedef void (*fill_t)(void *, u32, u8 *);
    fill_t volatile fillers[2][3] = {
        {base_fill_ap88_ct32, base_fill_ct32, base_fill_ia88},
        {cur_fill_ap88_ct32, cur_fill_ct32, cur_fill_ia88}};
    for (u32 i = 0; i < sizeof source / sizeof source[0]; i++) source[i] = rnd();
    for (u32 i = 0; i < 256; i++) base_H.pal[i] = cur_H.pal[i] = real_palettes[0][i];
    setpal(real_palettes[0]);
    fillctx_t c;
    memset(&c, 0, sizeof c); c.src = (const u8 *)source; c.sw = c.W2 = 512; c.sh = c.H2 = 128;
    for (int kind = 0; kind < 9; kind++)
        for (int rep = 0; rep < 9; rep++)
        {
            c.W2 = kind < 3 ? 512u : kind < 6 ? 17u : 1u;
            u32 times[2];
            for (int j = 0; j < 2; j++)
            {
                int tag = j ^ (rep & 1);
                fill_t fill = fillers[tag][kind % 3];
                u32 t = ticks();
                for (int i = 0; i < 1024; i++)
                {
                    fill(&c, (u32)i & 127, (u8 *)out);
                    sink ^= out[0];
                }
                times[tag] = ticks() - t;
            }
            printf("GSNEXT3BENCH kind=%d width=%u rep=%d rows=1024 base=%u current=%u\n", kind, c.W2, rep, times[0], times[1]);
        }
}
#endif

int main(void)
{
    init(1);
    if (palette_tests() || rows_and_packets()) return 1;
    printf("GS NEXT3 PASS lookups=%u rgba_conversions=%u row_cases=%u complete_packets=%u alpha_values=256 reverse_table_bytes=%u->%u\n",
        lookups, conversions, row_cases, packet_cases, (unsigned)(sizeof base_pal_key + sizeof base_pal_val),
        (unsigned)(sizeof cur_pal_key + sizeof cur_pal_val));
#ifdef GS_NEXT3_EE
    benchmark();
#endif
    printf("GS NEXT3 DONE\n");
    return 0;
}
