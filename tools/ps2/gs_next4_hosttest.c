/* Actual frozen/current production functions are compiled in two separate TUs. */
#include <stdio.h>
#include <string.h>
#include "common.h"

static const int formats[] = {PSM_CT32, PSM_CT16, PSM_CT16S, PSM_T8};
static u32 footprint_cases, plan_cases, block_cases, rng = 0x31415926u;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int compare_footprint(int psm, u32 w, u32 h, u32 tbw)
{
    u32 a = base_footprint(psm, w, h, tbw), b = cur_footprint(psm, w, h, tbw);
    footprint_cases++;
    if (a != b)
    {
        printf("GS NEXT4 FAIL footprint psm=%x w=%u h=%u tbw=%u base=%u current=%u\n", psm, w, h, tbw, a, b);
        return 1;
    }
    return 0;
}
static int footprints(void)
{
    static const u32 edges[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 23, 24, 25, 31, 32, 33, 39, 40, 41,
        47, 48, 49, 55, 56, 57, 63, 64, 65, 79, 95, 96, 97, 111, 127, 128, 129, 255, 256, 257,
        511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097, 8191, 8192, 8193, 16383, 16384,
        16385, 32767, 32768, 32769, 65534, 65535};
    static const u32 strides[] = {0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 127, 128, 255, 256, 511, 512, 1023, 1024};
    for (u32 p = 0; p < 4; p++)
    {
#ifndef GS_NEXT4_EE
        /* Every exact size accepted by a single GS image, with production TBW. */
        for (u32 w = 1; w <= 1024; w++)
            for (u32 h = 1; h <= 1024; h++)
            {
                u32 tbw = formats[p] == PSM_T8 ? ((w + 127) / 128) * 2 : (w + 63) / 64;
                if (compare_footprint(formats[p], w, h, tbw)) return 1;
            }
#endif
        /* All last-page/block residues, including CT16S rows 4/5 and odd T8 TBW. */
        for (u32 w = 1; w <= 128; w++)
            for (u32 h = 1; h <= 64; h++)
#ifdef GS_NEXT4_EE
                for (u32 t = 0; t < sizeof strides / sizeof strides[0]; t++)
                {
                    if (compare_footprint(formats[p], w, h, strides[t])) return 1;
                }
#else
                for (u32 tbw = 0; tbw < 64; tbw++)
                    if (compare_footprint(formats[p], w, h, tbw)) return 1;
#endif
        /* Uploads reject sources over 16384, but virtual frame-plan dimensions
           can still span every positive GLMipmap_t u16 size before that check. */
        for (u32 w = 0; w < sizeof edges / sizeof edges[0]; w++)
            for (u32 h = 0; h < sizeof edges / sizeof edges[0]; h++)
                for (u32 t = 0; t < sizeof strides / sizeof strides[0]; t++)
                    if (compare_footprint(formats[p], edges[w], edges[h], strides[t])) return 1;
    }
#ifdef GS_NEXT4_EE
    const u32 random_count = 8192;
#else
    const u32 random_count = 100000;
#endif
    for (u32 i = 0; i < random_count; i++)
    {
        int psm = formats[rnd() & 3];
        u32 w = rnd() % 65535u + 1, h = rnd() % 65535u + 1;
        u32 tbw = psm == PSM_T8 ? ((w + 127) / 128) * 2 : (w + 63) / 64;
        if (compare_footprint(psm, w, h, tbw) || compare_footprint(psm, w, h, rnd() % 1025u)) return 1;
    }
    return 0;
}
static int compare_plan(int psm, u32 w, u32 h, u32 cap, int mode, u32 rows)
{
    u32 ax = 0xaaaaaaaau, ay = 0xbbbbbbbbu, bx = ax, by = ay;
    int a = base_plan(psm, w, h, cap, mode, rows, &ax, &ay);
    int b = cur_plan(psm, w, h, cap, mode, rows, &bx, &by);
    plan_cases++;
    if (a != b || ax != bx || ay != by)
    {
        printf("GS NEXT4 FAIL plan psm=%x w=%u h=%u cap=%u mode=%d rows=%u result=%d/%d dx=%u/%u dy=%u/%u\n",
            psm, w, h, cap, mode, rows, a, b, ax, bx, ay, by);
        return 1;
    }
    if (a)
    {
        u32 ac = base_plan_blocks(psm, w, h, ax, ay), bc = cur_plan_blocks(psm, w, h, bx, by);
        block_cases++;
        if (ac != bc)
        {
            printf("GS NEXT4 FAIL plan_blocks psm=%x w=%u h=%u dx=%u dy=%u base=%u current=%u\n", psm, w, h, ax, ay, ac, bc);
            return 1;
        }
    }
    return 0;
}
static int plans(void)
{
    static const u32 sizes[] = {1, 3, 8, 16, 17, 32, 33, 40, 48, 64, 65, 96, 127, 128, 129,
        255, 256, 257, 512, 1023, 1024, 1025, 2048, 4096, 8192, 16384};
    static const u32 caps[] = {0, 1, 2, 3, 4, 15, 16, 17, 31, 32, 33, 63, 64, 127, 128, 256, 512, 1024, 4096, 16384};
    for (u32 p = 0; p < 4; p++)
        for (u32 w = 0; w < sizeof sizes / sizeof sizes[0]; w++)
            for (u32 h = 0; h < sizeof sizes / sizeof sizes[0]; h++)
                for (u32 c = 0; c < sizeof caps / sizeof caps[0]; c++)
                    if (compare_plan(formats[p], sizes[w], sizes[h], caps[c], (int)(w + h + c) & 3, 1024)) return 1;
#ifdef GS_NEXT4_EE
    const u32 random_count = 8192;
#else
    const u32 random_count = 100000;
#endif
    for (u32 i = 0; i < random_count; i++)
    {
        int psm = formats[rnd() & 3];
        u32 w = (rnd() & 16383) + 1, h = (rnd() & 16383) + 1;
        if (i & 1) { w &= ~1u; h &= ~1u; if (!w) w = 2; if (!h) h = 2; }
        u32 cap = rnd() & 32767, rows = 64u << (rnd() % 5);
        if (compare_plan(psm, w, h, cap, (int)rnd() & 3, rows)) return 1;
    }
    return 0;
}

#ifdef GS_NEXT4_EE
static u32 ticks(void) { u32 x; __asm__ volatile("mfc0 %0,$9" : "=r"(x)); return x; }
static volatile u32 sink;
typedef u32 (*foot_t)(int, u32, u32, u32);
typedef int (*plan_t)(int, u32, u32, u32, int, u32, u32 *, u32 *);
typedef u32 (*blocks_t)(int, u32, u32, u32, u32);
static void benchmark(void)
{
    static const u32 widths[] = {64, 128, 256, 512, 1024, 33, 65, 257};
    static const u32 heights[] = {64, 128, 256, 512, 1024, 40, 48, 129};
    foot_t volatile foot[2] = {base_footprint, cur_footprint};
    plan_t volatile plan[2] = {base_plan, cur_plan};
    blocks_t volatile blocks[2] = {base_plan_blocks, cur_plan_blocks};
    for (int kind = 0; kind < 16; kind++)
        for (int rep = 0; rep < 9; rep++)
        {
            u32 times[2], dx = 0, dy = 0;
            int psm = formats[kind & 3];
            for (int j = 0; j < 2; j++)
            {
                int tag = j ^ (rep & 1);
                foot_t f = foot[tag];
                plan_t p = plan[tag];
                blocks_t b = blocks[tag];
                u32 t = ticks();
                for (u32 i = 0; i < 4096; i++)
                {
                    u32 w = widths[i & 7], h = heights[(i >> 3) & 7];
                    if (kind < 4)
                        sink ^= f(psm, w, h, psm == PSM_T8 ? ((w + 127) / 128) * 2 : (w + 63) / 64);
                    else if (kind < 8)
                        sink ^= f(psm, (w & 7) + 1, (h & 7) + 1, 2);
                    else if (kind < 12)
                    {
                        sink ^= (u32)p(psm, w * 4, h * 4, 16u << (i & 7), 2, 1024, &dx, &dy);
                        sink ^= dx + dy;
                    }
                    else
                        sink ^= b(psm, w * 4, h * 4, 2, 2);
                }
                times[tag] = ticks() - t;
            }
            printf("GSNEXT4BENCH kind=%d psm=%d calls=4096 rep=%d base=%u current=%u\n", kind, psm, rep, times[0], times[1]);
        }
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (footprints() || plans()) return 1;
    printf("GS NEXT4 PASS footprints=%u plans=%u plan_blocks=%u\n", footprint_cases, plan_cases, block_cases);
#ifdef GS_NEXT4_EE
    benchmark();
#endif
    printf("GS NEXT4 DONE\n");
    return 0;
}
