/* Complete source-extracted decimators; independent byte/palette/division oracle. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

texture_state_t H;
enum { SOURCE_BYTES = 1024 * 1024, OUTPUT_BYTES = SOURCE_BYTES, GUARD = 32 };
static u8 source[SOURCE_BYTES + 96], saved_source[sizeof source];
static u8 output[OUTPUT_BYTES + 96], wanted[sizeof output];
static u32 cases, pixels, digest = 2166136261u, rng = 0x6a31b29du;
#ifndef TEXTURE_NATIVE_PALETTE
#define TEXTURE_NATIVE_PALETTE() ((void)0)
#define TEXTURE_NATIVE_CHECK() 1
#endif

static u32 random_word(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
}

static void palette(u32 kind)
{
    u32 i;
    static const u8 edges[15] = {0,1,2,3,63,64,65,127,128,129,191,192,193,254,255};
    for (i = 0; i < 256; i++)
    {
        u32 v = kind == 0 ? i : kind == 1 ? (i < 15 ? edges[i] : 0) : kind == 2 ? 127 : random_word();
        H.pal[i] = kind < 3 ? v * 0x010101u : v & 0xFFFFFFu;
    }
    TEXTURE_NATIVE_PALETTE();
}

static void oracle(const u8 *s, u32 sw, u32 sh, u32 lv, u8 *dest)
{
    u32 x, y, width = sw >> lv, height = sh >> lv, side = 1u << lv;
    u32 low = side / 4, high = side - low - 1;
    for (y = 0; y < height; y++) for (x = 0; x < width; x++)
    {
        u32 index[4], light[4], k, count = 0, sum = 0, mean, holes = 0;
        u32 best = 0, distance = 0, found = 0;
        index[0] = s[(size_t)(y * side + low) * sw + x * side + low];
        index[1] = s[(size_t)(y * side + low) * sw + x * side + high];
        index[2] = s[(size_t)(y * side + high) * sw + x * side + low];
        index[3] = s[(size_t)(y * side + high) * sw + x * side + high];
        for (k = 0; k < 4; k++)
        {
            u32 color = H.pal[index[k]];
            light[k] = ((color % 256) * 77 + ((color / 256) % 256) * 150 + ((color / 65536) % 256) * 29) / 256;
            if (index[k] == 255) holes++;
            else { count++; sum += light[k]; }
        }
        if (holes >= 2) { dest[(size_t)y * width + x] = 255; continue; }
        mean = (sum + count / 2) / count;
        for (k = 0; k < 4; k++) if (index[k] != 255)
        {
            u32 delta = mean < light[k] ? light[k] - mean : mean - light[k];
            if (!found || delta > distance) { found = 1; distance = delta; best = index[k]; }
        }
        dest[(size_t)y * width + x] = (u8)best;
    }
}

static int check(u32 sw, u32 sh, u32 lv, u32 sa, u32 da, const char *label)
{
    texture_fn functions[3] = {base_dec_level, cur_dec_level, truth_dec_level};
    texture_state_t initial = H;
    u32 tag, n = (sw >> lv) * (sh >> lv), i;
    u8 *src = source + GUARD + sa, *dst = output + GUARD + da;
#ifdef NEXT13_TEXTURE_EE
    const size_t source_bytes = GUARD + sa + (size_t)sw * sh + GUARD;
    const size_t output_bytes = GUARD + da + n + GUARD;
#else
    const size_t source_bytes = sizeof source, output_bytes = sizeof output;
#endif
    if ((size_t)sw * sh > SOURCE_BYTES || n > OUTPUT_BYTES) abort();
    memcpy(saved_source, source, source_bytes);
    memset(wanted, 0xA6, output_bytes);
    oracle(src, sw, sh, lv, wanted + GUARD + da);
    for (tag = 0; tag < 3; tag++)
    {
        memset(output, 0xA6, output_bytes);
        functions[tag](src, sw, sh, lv, dst);
        if (memcmp(output, wanted, output_bytes) || memcmp(saved_source, source, source_bytes) || memcmp(&initial, &H, sizeof H) || !TEXTURE_NATIVE_CHECK())
        {
            for (i = 0; i < output_bytes && output[i] == wanted[i]; i++) {}
            printf("TEXTURE NEXT13 FAIL tag=%u case=%s sw=%u sh=%u lv=%u sa=%u da=%u at=%u got=%u wanted=%u\n",
                tag, label, sw, sh, lv, sa, da, i, i < sizeof output ? output[i] : 0, i < sizeof output ? wanted[i] : 0);
            return 0;
        }
    }
    for (i = 0; i < n; i++) digest = (digest ^ wanted[GUARD + da + i]) * 16777619u;
    cases++; pixels += n; return 1;
}

static void samples(u32 at, const u8 v[4])
{
    u32 x = at % 256, y = at / 256;
    source[GUARD + (y * 2) * 512 + x * 2] = v[0];
    source[GUARD + (y * 2) * 512 + x * 2 + 1] = v[1];
    source[GUARD + (y * 2 + 1) * 512 + x * 2] = v[2];
    source[GUARD + (y * 2 + 1) * 512 + x * 2 + 1] = v[3];
}

static int sample_matrix(void)
{
    u32 kind, first, second, fixed, i, k;
    u8 v[4];
    for (kind = 0; kind < 7; kind++)
    {
        palette(kind);
        for (first = 0; first < 4; first++) for (second = first + 1; second < 4; second++)
        for (fixed = 0; fixed < 4; fixed++)
        {
            memset(source, 0x6D, sizeof source);
            for (i = 0; i < 65536; i++)
            {
                for (k = 0; k < 4; k++) v[k] = fixed == 0 ? 0 : fixed == 1 ? 254 : fixed == 2 ? 255 : (u8)(k * 71);
                v[first] = (u8)i; v[second] = (u8)(i >> 8); samples(i, v);
            }
            if (!check(512,512,1,0,0,"all-index-pairs")) return 0;
        }
    }
    palette(1);
    for (i = 0; i < 65536; i++)
    {
        for (k = 0; k < 4; k++) { u8 t = (u8)((i >> (k * 4)) & 15); v[k] = t == 15 ? 255 : t; }
        samples(i, v);
    }
    if (!check(512,512,1,0,0,"boundary-luminances-holes-ties")) return 0;
    for (kind = 0; kind < 16; kind++)
    {
        palette(3);
        for (i = 0; i < 65536; i++)
        {
            u32 word = random_word();
            for (k = 0; k < 4; k++) v[k] = (u8)(word >> (k * 8));
            samples(i, v);
        }
        if (!check(512,512,1,0,0,"random-quadruples")) return 0;
    }
    return 1;
}

static int dimensions(void)
{
    static const u32 widths[] = {1,2,3,4,5,7,8,9,15,16,17,31,32,33,63,64,65,127,128,129,255,256,257,511,512,513,1023,1024};
    u32 i, j, lv, a;
    palette(3);
    for (i = 0; i < sizeof source; i++) source[i] = (u8)random_word();
    for (i = 0; i < sizeof widths / sizeof widths[0]; i++)
    for (j = 0; j < sizeof widths / sizeof widths[0]; j += 3)
    for (lv = 0; lv <= 10; lv++)
    {
        if (!check(widths[i],widths[j],lv,(i + j) & 15,(i * 3 + j) & 15,"dimensions-stride-level")) return 0;
    }
    for (a = 0; a < 16; a++) for (j = 0; j < 16; j++) for (lv = 0; lv <= 5; lv++)
        if (!check(65,67,lv,a,j,"all-byte-alignments")) return 0;
    return 1;
}

int main(void)
{
    u32 a, b;
    for (a = 0; a < 256; a++) for (b = 0; b < 256; b++)
    {
        u32 d = a - b;
        d = (d ^ (0u - (d >> 31))) + (d >> 31);
        if (d != (a > b ? a - b : b - a)) return 1;
    }
    if (!sample_matrix() || !dimensions()) return 1;
    printf("TEXTURE NEXT13 PASS cases=%u pixels=%u digest=%08x\n",cases,pixels,digest);
    return 0;
}
