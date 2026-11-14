/* Actual frozen/current PSMT8 callbacks live in two other non-LTO TUs. */
#include "common.h"
#include <stdarg.h>

u32 ps2hwd_fx2, ps2hwd_dbg_flags;
enum { DATA_CAP = 1048576, OUT_CAP = 1104 };
static u8 source[DATA_CAP + 128] __attribute__((aligned(16)));
static u8 saved[sizeof source] __attribute__((aligned(16)));
static u8 output[2][OUT_CAP] __attribute__((aligned(16)));
static u8 expected[OUT_CAP];
static u32 rng = 0x71954362u, cases, pixels, debug_calls;
typedef void (*fill_t)(fillctx_t *, u32, u8 *);
static fill_t const functions[2][2] = {
    {base_fill_idx, cur_fill_idx}, {base_fill_ap88, cur_fill_ap88}
};

int gs_next7_print(const char *fmt, ...)
{
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (!strstr(text, ", 0 differ"))
    {
        printf("GS NEXT7 FAIL production debug: %s", text);
        exit(1);
    }
    debug_calls++;
    return 0;
}

static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int fail(const char *why, u32 format, const fillctx_t *c, u32 row, u32 dst)
{
    printf("GS NEXT7 FAIL %s case=%u format=%u width=%u sw=%u sh=%u dx=%u dy=%u mode=%d row=%u dst=%u fx=%u dbg=%u\n",
           why, cases, format, c->W2, c->sw, c->sh, c->dx, c->dy, c->mode, row, dst, ps2hwd_fx2, ps2hwd_dbg_flags);
    return 1;
}

static int check(u32 format, fillctx_t *c, u32 row, u32 dstoff)
{
    const u32 bpp = format + 1;
    size_t n = (size_t)c->sw * c->sh * bpp + (size_t)(c->src - source) + 16;
    u32 sy = (row << c->dy) + ((1u << c->dy) >> 1);
    u32 bias = ((1u << c->dx) >> 1);
    fillctx_t original = *c;
    if (n > sizeof source || 32 + dstoff + c->W2 > sizeof expected) abort();
    memcpy(saved, source, n);
    memset(expected, 0xa5, sizeof expected);
    memset(output, 0xa5, sizeof output);
    for (u32 x = 0; x < c->W2; x++)
    {
        const u8 *texel = c->src + ((size_t)sy * c->sw + (x << c->dx) + bias) * bpp;
        expected[32 + dstoff + x] = format && c->mode == 0 && texel[1] == 0 ? 255 : texel[0];
    }
    for (u32 tag = 0; tag < 2; tag++)
    {
        functions[format][tag](c, row, output[tag] + 32 + dstoff);
        if (memcmp(c, &original, sizeof original)) return fail("context write", format, c, row, dstoff);
        if (memcmp(source, saved, n)) return fail("source write", format, c, row, dstoff);
        if (memcmp(output[tag], expected, sizeof expected))
            return fail(tag ? "current output/oracle/canary" : "baseline output/oracle/canary", format, c, row, dstoff);
    }
    cases++;
    pixels += c->W2;
    return 0;
}

static int setup(u32 format, fillctx_t *c, u32 width, u32 dx, u32 dy, u32 row, u32 srcoff, u32 pattern)
{
    memset(c, 0, sizeof *c);
    c->sw = (width << dx) + (rnd() & 3);
    c->sh = ((row + 1) << dy) + 1;
    c->W2 = width;
    c->H2 = row + 1;
    c->dx = dx;
    c->dy = dy;
    c->src = source + 32 + srcoff;
    size_t n = (size_t)c->sw * c->sh * (format + 1);
    if (c->sw > 16384 || n > DATA_CAP) return 0;
    memset(source, 0xb6, n + 112);
    for (size_t i = 0; i < n; i += format + 1)
    {
        u32 v = rnd();
        source[32 + srcoff + i] = (u8)v;
        if (format)
            source[33 + srcoff + i] = pattern == 0 ? (u8)(v >> 8) : pattern == 1 ? 255 :
                pattern == 2 ? ((v & 256) ? 255 : 0) : pattern == 3 ? 0 : (u8)(i >> 1);
    }
    return 1;
}

static int proofs(void)
{
    fillctx_t c;
    /* Every possible AP88 pair occurs in every packed lane, in both modes. */
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
            if (check(1, &c, 0, v & 15)) return 1;
    }
    /* All widths and source/destination byte residues modulo 16. */
    for (u32 format = 0; format < 2; format++)
        for (u32 width = 1; width <= 1024; width++)
        {
#ifdef GS_NEXT7_EE
            if (width > 65 && width != 127 && width != 128 && width != 255 && width != 256 && width != 511 && width != 512 && width != 1023 && width != 1024) continue;
#endif
            for (u32 src = 0; src < 16; src++)
            {
                if (!setup(format, &c, width, 0, 0, 0, src, width % 5)) abort();
                for (u32 dst = 0; dst < 16; dst++)
                    for (c.mode = 0; c.mode <= (int)format; c.mode++)
                    {
                        ps2hwd_fx2 = (width + dst) & 1 ? FX3_NOFILL : 0;
                        ps2hwd_dbg_flags = ((width + dst) & 7) == 0 ? HWDBG_COMPOSE : 0;
                        if (check(format, &c, 0, dst)) return 1;
                    }
            }
        }
#ifdef GS_NEXT7_EE
    const u32 random_cases = 4096;
#else
    const u32 random_cases = 32768;
#endif
    /* Fresh contexts, rows, strides, source contents and modes on every call. */
    for (u32 format = 0; format < 2; format++)
        for (u32 i = 0; i < random_cases; i++)
        {
            u32 dx = rnd() % 11, dy = rnd() % 11, row = rnd() % 4;
            u32 width = 1 + rnd() % (1024u >> (dx > 3 ? dx - 3 : 0));
            if (!setup(format, &c, width, dx, dy, row, rnd() & 15, rnd() % 5)) continue;
            c.mode = (int)(rnd() % 7) - 3;
            ps2hwd_fx2 = rnd() & 1 ? FX3_NOFILL : 0;
            ps2hwd_dbg_flags = (rnd() & 7) == 0 ? HWDBG_COMPOSE : 0;
            if (check(format, &c, row, rnd() & 15)) return 1;
        }
    printf("GS NEXT7 PASS cases=%u pixels=%u debug_calls=%u\n", cases, pixels, debug_calls);
    return 0;
}

#ifdef GS_NEXT7_EE
static u32 ticks(void) { u32 x; __asm__ volatile("mfc0 %0,$9" : "=r"(x)); return x; }
static volatile u32 sink;
static int benchmark(void)
{
    static const u32 widths[] = {1, 2, 3, 4, 7, 16, 31, 64, 127, 256, 511, 1024};
    fill_t volatile selected[2][2] = {{base_fill_idx, cur_fill_idx}, {base_fill_ap88, cur_fill_ap88}};
    u32 kind = 0;
    for (u32 format = 0; format < 2; format++)
        for (u32 scenario = 0; scenario < 6; scenario++)
            for (u32 wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
            {
                u32 width = widths[wi], dstoff = 0, srcoff = 0, dx = 0, dy = 0, row = 0, fx = 0;
                if (scenario == 1) { srcoff = 1; dstoff = 3; }
                if (scenario == 2) { dstoff = 1; }
                if (scenario == 3) { dx = 1; dy = 1; row = 2; }
                if (scenario == 4) { dx = 3; row = 1; srcoff = 7; dstoff = 7; }
                if (scenario == 5) { fx = FX3_NOFILL; }
                fillctx_t c;
                if (!setup(format, &c, width, dx, dy, row, srcoff, 2)) abort();
                c.sw = width << dx;
                ps2hwd_fx2 = fx;
                ps2hwd_dbg_flags = 0;
                for (c.mode = 0; c.mode <= (int)format; c.mode++, kind++)
                {
                    if (check(format, &c, row, dstoff)) return 1;
                    u32 calls = 131072u / width;
                    if (calls > 8192) calls = 8192;
                    for (u32 rep = 0; rep < 9; rep++)
                    {
                        u32 times[2];
                        for (u32 j = 0; j < 2; j++)
                        {
                            u32 tag = j ^ (rep & 1);
                            fill_t f = selected[format][tag];
                            u8 *dst = output[tag] + 32 + dstoff;
                            u32 t = ticks();
                            for (u32 i = 0; i < calls; i++)
                            {
                                f(&c, row, dst);
                                sink ^= dst[i & (width > 1)];
                            }
                            times[tag] = ticks() - t;
                        }
                        if (memcmp(output[0], expected, sizeof expected) || memcmp(output[1], expected, sizeof expected))
                            return fail("timed output/canary", format, &c, row, dstoff);
                        printf("GSNEXT7BENCH kind=%u format=%u width=%u mode=%d dst=%u src=%u dx=%u dy=%u row=%u fx=%u calls=%u rep=%u base=%u current=%u\n",
                               kind, format, width, c.mode, dstoff, srcoff, dx, dy, row, fx, calls, rep, times[0], times[1]);
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
#ifdef GS_NEXT7_EE
    if (benchmark()) return 1;
#endif
    puts("GS NEXT7 DONE");
    return 0;
}
