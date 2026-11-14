/* Actual frozen/current AP_88 fillers are built in two other non-LTO translation units. */
#include "common.h"
#include <stdarg.h>

u32 ps2hwd_fx2, ps2hwd_dbg_flags;
enum { DATA_CAP = 131072, OUT_CAP = 1104 };
static u8 source[DATA_CAP + 96] __attribute__((aligned(16)));
static u8 saved[sizeof source] __attribute__((aligned(16)));
static u8 output[2][OUT_CAP] __attribute__((aligned(16)));
static u8 expected[OUT_CAP];
static u32 rng = 0x72915643u, cases, pixels, debug_calls;

int gs_next5_print(const char *fmt, ...)
{
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (!strstr(text, ", 0 differ"))
    {
        printf("GS NEXT5 FAIL production debug: %s", text);
        exit(1);
    }
    debug_calls++;
    return 0;
}

static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int fail(const char *why, const fillctx_t *c, u32 row, u32 dst)
{
    printf("GS NEXT5 FAIL %s case=%u width=%u sw=%u sh=%u dx=%u dy=%u mode=%d row=%u dst=%u fx=%u dbg=%u\n",
           why, cases, c->W2, c->sw, c->sh, c->dx, c->dy, c->mode, row, dst, ps2hwd_fx2, ps2hwd_dbg_flags);
    return 1;
}

static int check(fillctx_t *c, u32 row, u32 dstoff)
{
    size_t n = (size_t)c->sw * c->sh * 2 + (size_t)(c->src - source) + 16;
    u32 sy = (row << c->dy) + ((1u << c->dy) >> 1);
    u32 bias = ((1u << c->dx) >> 1);
    if (n > sizeof source || 32 + dstoff + c->W2 > sizeof expected) abort();
    memcpy(saved, source, n);
    memset(expected, 0xa5, sizeof expected);
    memset(output, 0xa5, sizeof output);
    for (u32 x = 0; x < c->W2; x++)
    {
        const u8 *texel = c->src + ((size_t)sy * c->sw + (x << c->dx) + bias) * 2;
        expected[32 + dstoff + x] = c->mode == 0 && texel[1] == 0 ? 255 : texel[0];
    }
    base_fill(c, row, output[0] + 32 + dstoff);
    if (memcmp(source, saved, n)) return fail("baseline source write", c, row, dstoff);
    cur_fill(c, row, output[1] + 32 + dstoff);
    cases++;
    pixels += c->W2;
    if (memcmp(source, saved, n)) return fail("current source write", c, row, dstoff);
    if (memcmp(output[0], expected, sizeof expected)) return fail("baseline output/oracle/canary", c, row, dstoff);
    if (memcmp(output[1], expected, sizeof expected)) return fail("current output/oracle/canary", c, row, dstoff);
    return 0;
}

static int setup(fillctx_t *c, u32 width, u32 dx, u32 dy, u32 row, u32 srcoff, u32 pattern)
{
    memset(c, 0, sizeof *c);
    c->sw = (width << dx) + (rnd() & 3);
    c->sh = ((row + 1) << dy) + 1;
    c->W2 = width;
    c->H2 = row + 1;
    c->dx = dx;
    c->dy = dy;
    c->src = source + 32 + srcoff;
    size_t n = (size_t)c->sw * c->sh * 2;
    if (c->sw > 16384 || n > DATA_CAP) return 0;
    memset(source, 0xb6, n + 80);
    for (size_t i = 0; i < n; i += 2)
    {
        u32 v = rnd();
        source[32 + srcoff + i] = (u8)v;
        source[32 + srcoff + i + 1] = pattern == 0 ? (u8)(v >> 8) : pattern == 1 ? 255 :
                                      pattern == 2 ? ((v & 256) ? 255 : 0) : pattern == 3 ? 0 : (u8)(i >> 1);
    }
    return 1;
}

static int proofs(void)
{
    fillctx_t c;
    /* Every possible (index, alpha) pair appears in each of the four packed lanes. */
    memset(&c, 0, sizeof c);
    c.src = source + 32;
    c.sw = c.W2 = 4;
    c.sh = c.H2 = 1;
    memset(source, 0xb6, sizeof source);
    for (u32 v = 0; v < 65536; v++)
    {
        for (u32 lane = 0; lane < 4; lane++)
        {
            u32 pair = (v + 15427u * lane) & 65535;
            source[32 + 2 * lane] = (u8)pair;
            source[33 + 2 * lane] = (u8)(pair >> 8);
        }
        for (c.mode = 0; c.mode < 2; c.mode++)
            if (check(&c, 0, v & 7)) return 1;
    }
    static const u32 alignments[] = {0, 1, 2, 4, 6, 8, 15};
    for (u32 width = 1; width <= 1024; width++)
    {
#ifdef GS_NEXT5_EE
        if (width > 65 && width != 127 && width != 128 && width != 255 && width != 256 && width != 511 && width != 512 && width != 1023 && width != 1024) continue;
#endif
        for (u32 ai = 0; ai < sizeof alignments / sizeof alignments[0]; ai++)
        {
            if (!setup(&c, width, 0, 0, 0, alignments[ai], width % 5)) abort();
            for (u32 dst = 0; dst < 8; dst++)
                for (c.mode = 0; c.mode < 2; c.mode++)
                {
                    ps2hwd_fx2 = (width + dst) & 1;
                    ps2hwd_dbg_flags = ((width + dst) & 7) == 0 ? HWDBG_COMPOSE : 0;
                    if (check(&c, 0, dst)) return 1;
                }
        }
    }
#ifdef GS_NEXT5_EE
    const u32 random_cases = 2048;
#else
    const u32 random_cases = 16384;
#endif
    for (u32 i = 0; i < random_cases; i++)
    {
        u32 dx = rnd() % 8, dy = rnd() % 5, row = rnd() % 3;
        u32 width = 1 + rnd() % (1024u >> (dx > 3 ? dx - 3 : 0));
        if (!setup(&c, width, dx, dy, row, rnd() & 15, rnd() % 5)) continue;
        c.mode = (int)(rnd() % 5); /* Every nonzero mode ignores alpha, including arbitrary accepted int values. */
        ps2hwd_fx2 = rnd() & 1;
        ps2hwd_dbg_flags = (rnd() & 7) == 0 ? HWDBG_COMPOSE : 0;
        if (check(&c, row, rnd() & 7)) return 1;
    }
    printf("GS NEXT5 PASS cases=%u pixels=%u debug_calls=%u\n", cases, pixels, debug_calls);
    return 0;
}

#ifdef GS_NEXT5_EE
static u32 ticks(void) { u32 x; __asm__ volatile("mfc0 %0,$9" : "=r"(x)); return x; }
static volatile u32 sink;
typedef void (*fill_t)(fillctx_t *, u32, u8 *);
static int benchmark(void)
{
    static const u32 widths[] = {4, 16, 64, 256, 1024};
    static const u32 tail_widths[] = {5, 7, 31, 127};
    fill_t volatile functions[2] = {base_fill, cur_fill};
    int kind = 0;
    for (u32 scenario = 0; scenario < 23; scenario++)
    {
        u32 width, dstoff = 0, srcoff = 0, dx = 0, fx = 0;
        if (scenario < 10)
        {
            width = widths[scenario / 2];
            dstoff = scenario & 1;
        }
        else if (scenario == 10) width = 3;
        else if (scenario == 11) { width = 127; srcoff = 1; }
        else if (scenario == 12) { width = 64; dx = 1; }
        else if (scenario == 13) { width = 128; fx = FX3_NOFILL; }
        else if (scenario == 14) { width = 127; fx = FX3_NOFILL; }
        else { width = tail_widths[(scenario - 15) / 2]; dstoff = (scenario - 15) & 1; }
        fillctx_t c;
        if (!setup(&c, width, dx, 0, 0, srcoff, 2)) abort();
        /* Exact source alignment for every timed contiguous row. */
        c.sw = width << dx;
        c.sh = 1;
        ps2hwd_fx2 = fx;
        ps2hwd_dbg_flags = 0;
        for (c.mode = 0; c.mode < 2; c.mode++, kind++)
        {
            if (check(&c, 0, dstoff)) return 1;
            u32 calls = 262144u / width;
            if (calls > 16384) calls = 16384;
            for (u32 rep = 0; rep < 9; rep++)
            {
                u32 times[2];
                for (u32 j = 0; j < 2; j++)
                {
                    u32 tag = j ^ (rep & 1);
                    fill_t f = functions[tag];
                    u8 *dst = output[tag] + 32 + dstoff;
                    u32 t = ticks();
                    for (u32 i = 0; i < calls; i++)
                    {
                        f(&c, 0, dst);
                        sink ^= dst[i & 1];
                    }
                    times[tag] = ticks() - t;
                }
                if (memcmp(output[0], expected, sizeof expected) || memcmp(output[1], expected, sizeof expected))
                    return fail("timed output/canary", &c, 0, dstoff);
                printf("GSNEXT5BENCH kind=%d width=%u mode=%d dst=%u src=%u dx=%u fx=%u calls=%u rep=%u base=%u current=%u\n",
                       kind, width, c.mode, dstoff, srcoff, dx, fx, calls, rep, times[0], times[1]);
            }
        }
    }
    return 0;
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (proofs()) return 1;
#ifdef GS_NEXT5_EE
    if (benchmark()) return 1;
#endif
    puts("GS NEXT5 DONE");
    return 0;
}
