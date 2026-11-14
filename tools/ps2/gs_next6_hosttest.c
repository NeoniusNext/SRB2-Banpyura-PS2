/* Frozen/current production functions and this independent oracle use three non-LTO TUs. */
#include "common.h"

hwfixture_t H;
enum { DATA_CAP = 262144, OUT_CAP = 4160 };
static u8 source[DATA_CAP + 96] __attribute__((aligned(16)));
static u8 saved[sizeof source] __attribute__((aligned(16)));
static u8 output[2][OUT_CAP] __attribute__((aligned(16)));
static u8 expected[OUT_CAP];
static u32 rng = 0x58d72af1u, cases, pixels;
typedef void (*fill_t)(fillctx_t *, u32, u8 *);
static fill_t volatile functions[2][3] = {
    {base_fill_ap88_ct32, base_fill_ct32, base_fill_ia88},
    {cur_fill_ap88_ct32, cur_fill_ct32, cur_fill_ia88}
};

static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static u32 sbpp(u32 fmt) { return fmt == 1 ? 4 : 2; }
static int fail(const char *why, const fillctx_t *c, u32 fmt, u32 row, u32 dst)
{
    printf("GS NEXT6 FAIL %s case=%u format=%u width=%u sw=%u sh=%u dx=%u dy=%u mode=%d row=%u dst=%u\n",
           why, cases, fmt, c->W2, c->sw, c->sh, c->dx, c->dy, c->mode, row, dst);
    return 1;
}

/* Original independent 128/255 rounding, including both end points. */
static u32 oracle(const u8 *p, u32 fmt)
{
    u32 rgb, alpha;
    if (fmt == 1)
    {
        rgb = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
        alpha = p[3];
    }
    else
    {
        rgb = fmt == 0 ? H.pal[p[0]] : (u32)p[0] * 0x010101u;
        alpha = p[1];
    }
    return rgb | (((alpha * 128u + 127u) / 255u) << 24);
}

static int check(fillctx_t *c, u32 fmt, u32 row, u32 dstoff)
{
    size_t n = (size_t)c->sw * c->sh * sbpp(fmt) + (size_t)(c->src - source) + 16;
    u32 sy = (row << c->dy) + ((1u << c->dy) >> 1);
    u32 bias = ((1u << c->dx) >> 1);
    fillctx_t saved_ctx;
    hwfixture_t saved_pal = H;
    if (n > sizeof source || 32 + dstoff + c->W2 * 4 > sizeof expected) abort();
    memcpy(&saved_ctx, c, sizeof saved_ctx);
    memcpy(saved, source, n);
    memset(expected, 0xa5, sizeof expected);
    memset(output, 0xa5, sizeof output);
    for (u32 x = 0; x < c->W2; x++)
    {
        const u8 *texel = c->src + ((size_t)sy * c->sw + (x << c->dx) + bias) * sbpp(fmt);
        u32 value = oracle(texel, fmt);
        memcpy(expected + 32 + dstoff + 4 * x, &value, 4);
    }
    for (u32 tag = 0; tag < 2; tag++)
    {
        functions[tag][fmt](c, row, output[tag] + 32 + dstoff);
        if (memcmp(source, saved, n)) return fail("source write", c, fmt, row, dstoff);
        if (memcmp(c, &saved_ctx, sizeof *c)) return fail("context write", c, fmt, row, dstoff);
        if (memcmp(&H, &saved_pal, sizeof H)) return fail("palette write", c, fmt, row, dstoff);
        if (memcmp(output[tag], expected, sizeof expected))
            return fail(tag ? "current output/oracle/canary" : "baseline output/oracle/canary", c, fmt, row, dstoff);
    }
    cases++;
    pixels += c->W2;
    return 0;
}

static int setup(fillctx_t *c, u32 fmt, u32 width, u32 dx, u32 dy, u32 row, u32 srcoff, u32 pattern)
{
    memset(c, 0, sizeof *c);
    c->sw = (width << dx) + (rnd() & 3);
    c->sh = ((row + 1) << dy) + 1;
    c->W2 = width;
    c->H2 = row + 1;
    c->dx = dx;
    c->dy = dy;
    c->mode = (int)rnd();
    c->holes = (int)rnd();
    c->src = source + 32 + srcoff;
    size_t n = (size_t)c->sw * c->sh * sbpp(fmt);
    if (c->sw > 16384 || c->sh > 16384 || n > DATA_CAP) return 0;
    memset(source, 0xb6, n + 80);
    for (u32 i = 0; i < 256; i++) H.pal[i] = rnd() & 0x00ffffffu;
    for (size_t i = 0; i < n; i++)
    {
        u32 value = rnd();
        source[32 + srcoff + i] = pattern == 0 ? (u8)value : pattern == 1 ? 255 :
                                 pattern == 2 ? ((value & 256) ? 255 : 0) : pattern == 3 ? 0 : (u8)i;
    }
    return 1;
}

static int proofs(void)
{
    fillctx_t c;
    /* Every byte pair in each lane: complete (palette-index/grey,alpha),
       plus all byte combinations of R,G with every alpha in RGBA. */
    for (u32 fmt = 0; fmt < 3; fmt++)
    {
        if (!setup(&c, fmt, 4, 0, 0, 0, 0, 0)) abort();
        c.sw = c.W2 = 4;
        c.sh = c.H2 = 1;
        for (u32 value = 0; value < 65536; value++)
        {
            for (u32 lane = 0; lane < 4; lane++)
            {
                u32 pair = (value + 15427u * lane) & 65535;
                u8 *p = source + 32 + lane * sbpp(fmt);
                p[0] = (u8)pair;
                p[1] = (u8)(pair >> 8);
                if (fmt == 1) { p[2] = (u8)(pair ^ (pair >> 8)); p[3] = (u8)(pair >> 8); }
            }
            if (check(&c, fmt, 0, (value & 3) * 4)) return 1;
        }
    }
    static const u32 aligns[] = {0, 1, 2, 3, 4, 7, 8, 15};
    for (u32 fmt = 0; fmt < 3; fmt++)
    {
        for (u32 width = 1; width <= 1024; width++)
        {
#ifdef GS_NEXT6_EE
            if (width > 65 && width != 127 && width != 128 && width != 255 && width != 256 && width != 511 && width != 512 && width != 1023 && width != 1024) continue;
#endif
            for (u32 ai = 0; ai < sizeof aligns / sizeof aligns[0]; ai++)
            {
                u32 src = aligns[ai];
                if (fmt == 1 && (src & 3)) continue; /* Original RGBA u32-load requirement. */
                if (!setup(&c, fmt, width, 0, 0, 0, src, width % 5)) abort();
                for (u32 dst = 0; dst < 16; dst += 4)
                    if (check(&c, fmt, 0, dst)) return 1;
            }
        }
#ifdef GS_NEXT6_EE
        const u32 random_cases = 4096;
#else
        const u32 random_cases = 32768;
#endif
        for (u32 i = 0; i < random_cases; i++)
        {
            u32 dx = rnd() % 11, dy = rnd() % 11, row = rnd() % 4;
            u32 width = 1 + rnd() % (1024u >> (dx > 3 ? dx - 3 : 0));
            u32 src = rnd() & 15;
            if (fmt == 1) src &= ~3u;
            if (!setup(&c, fmt, width, dx, dy, row, src, rnd() % 5)) continue;
            if (check(&c, fmt, row, (rnd() & 3) * 4)) return 1;
        }
        /* Largest metadata shifts reachable from 16384-texel input at 16-texel cap. */
        for (u32 dx = 0; dx <= 10; dx++)
            for (u32 dy = 0; dy <= 10; dy++)
                if (setup(&c, fmt, 1, dx, dy, 0, 0, 0) && check(&c, fmt, 0, 0)) return 1;
        /* Same context address is reused with new widths, source, strides, shifts,
           mode and palette. The second row also catches a snapshot kept across rows. */
        for (u32 i = 0; i < 64; i++)
        {
            if (!setup(&c, fmt, 1 + i, i & 3, (i >> 2) & 3, 1, (i & 3) * 4, i % 5)) abort();
            if (check(&c, fmt, 0, 0) || check(&c, fmt, 1, 4)) return 1;
        }
    }
    printf("GS NEXT6 PASS cases=%u pixels=%u\n", cases, pixels);
    return 0;
}

#ifdef GS_NEXT6_EE
static u32 ticks(void) { u32 x; __asm__ volatile("mfc0 %0,$9" : "=r"(x)); return x; }
static volatile u32 sink;
static int benchmark(void)
{
    static const u32 widths[] = {1, 2, 3, 4, 7, 16, 31, 64, 127, 256, 511, 1024};
    u32 kind = 0;
    for (u32 fmt = 0; fmt < 3; fmt++)
        for (u32 scenario = 0; scenario < 42; scenario++, kind++)
        {
            u32 width = widths[scenario % 12], dx = 0, dy = 0, row = 0, src = 0, dstoff = 0;
            if (scenario >= 12 && scenario < 24) { src = fmt == 1 ? 4 : 1; dstoff = 4; }
            if (scenario >= 24 && scenario < 36) { dx = 1; dy = 1; row = 1; }
            if (scenario >= 36) { width = widths[(scenario - 36) * 2]; dx = 3; dy = 2; row = 2; src = fmt == 1 ? 12 : 7; dstoff = 12; }
            fillctx_t c;
            if (!setup(&c, fmt, width, dx, dy, row, src, 0)) abort();
            if (check(&c, fmt, row, dstoff)) return 1;
            u32 calls = 262144u / width;
            if (calls > 16384) calls = 16384;
            for (u32 rep = 0; rep < 9; rep++)
            {
                u32 times[2];
                for (u32 j = 0; j < 2; j++)
                {
                    u32 tag = j ^ (rep & 1);
                    fill_t f = functions[tag][fmt];
                    u8 *dst = output[tag] + 32 + dstoff;
                    u32 t = ticks();
                    for (u32 i = 0; i < calls; i++) { f(&c, row, dst); sink ^= dst[i & 3]; }
                    times[tag] = ticks() - t;
                }
                if (memcmp(output[0], expected, sizeof expected) || memcmp(output[1], expected, sizeof expected))
                    return fail("timed output/canary", &c, fmt, row, dstoff);
                printf("GSNEXT6BENCH kind=%u format=%u width=%u src=%u dst=%u dx=%u dy=%u row=%u calls=%u rep=%u base=%u current=%u\n",
                       kind, fmt, width, src, dstoff, dx, dy, row, calls, rep, times[0], times[1]);
            }
        }
    return 0;
}
#endif

int main(void)
{
    if (proofs()) return 1;
#ifdef GS_NEXT6_EE
    if (benchmark()) return 1;
#endif
    puts("GS NEXT6 DONE");
    return 0;
}
