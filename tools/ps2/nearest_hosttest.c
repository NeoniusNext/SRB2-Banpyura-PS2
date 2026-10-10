/* PS2-LOAD-14: src/ps2/ps2_nearest.c against the original NearestPaletteColor loop (src/r_data.c), every colour (2^24) on several palettes.
 *   cc -O2 -DPS2_NEAREST_HOST -I src/ps2 -o build/hosttest/nearest_hosttest tools/ps2/nearest_hosttest.c src/ps2/ps2_nearest.c
 *   build/hosttest/nearest_hosttest [PLAYPAL.lmp ...]   (PLAYPAL-style files: 768 bytes, r g b per colour; made-up palettes are always tested)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps2_nearest.h"

typedef struct { UINT8 red, green, blue, alpha; } col_t;

/* r_data.c NearestPaletteColor, verbatim apart from the types */
static UINT8 Reference(UINT8 r, UINT8 g, UINT8 b, const col_t *palette)
{
	int dr, dg, db;
	int distortion, bestdistortion = 256 * 256 * 4, bestcolor = 0, i;

	for (i = 0; i < 256; i++)
	{
		dr = r - palette[i].red;
		dg = g - palette[i].green;
		db = b - palette[i].blue;
		distortion = dr*dr + dg*dg + db*db;
		if (distortion < bestdistortion)
		{
			if (!distortion)
				return (UINT8)i;

			bestdistortion = distortion;
			bestcolor = i;
		}
	}

	return (UINT8)bestcolor;
}

static unsigned long long seed = 88172645463325252ull;
static unsigned Rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (unsigned)(seed >> 16); }

static int Test(const char *name, const col_t *pal, int stride)
{
	static ps2nearest_t ctx;
	unsigned long long bad = 0, n = 0;
	unsigned r, g, b;

	memset(&ctx, 0, sizeof ctx);
	PS2Nearest_Build(&ctx, pal);
	if (!PS2Nearest_Same(&ctx, pal))
	{
		printf("%s: Same() says the palette it was built from changed\n", name);
		return 1;
	}
	for (r = 0; r < 256; r += stride)
		for (g = 0; g < 256; g += stride)
			for (b = 0; b < 256; b += stride)
			{
				const UINT8 a = Reference((UINT8)r, (UINT8)g, (UINT8)b, pal), c = PS2Nearest_Find(&ctx, (UINT8)r, (UINT8)g, (UINT8)b);

				n++;
				if (a != c && ++bad <= 5)
					printf("%s: (%u,%u,%u) reference %u, fast %u\n", name, r, g, b, a, c);
			}
	printf("%s: %llu colours, %llu differ\n", name, n, bad);
	return bad != 0;
}

int main(int argc, char **argv)
{
	col_t pal[256];
	int i, fail = 0, k;

	memset(pal, 0, sizeof pal);
	/* made-up palettes */
	for (i = 0; i < 256; i++) { pal[i].red = pal[i].green = pal[i].blue = (UINT8)i; pal[i].alpha = 255; }
	fail |= Test("grey ramp", pal, 1);
	for (i = 0; i < 256; i++) { pal[i].red = (UINT8)((i & 7) * 36); pal[i].green = (UINT8)(((i >> 3) & 7) * 36); pal[i].blue = (UINT8)(((i >> 6) & 3) * 85); }
	fail |= Test("3-3-2 cube", pal, 1);
	for (k = 0; k < 6; k++)
	{
		for (i = 0; i < 256; i++) { pal[i].red = (UINT8)Rnd(); pal[i].green = (UINT8)Rnd(); pal[i].blue = (UINT8)Rnd(); pal[i].alpha = (UINT8)Rnd(); }
		if (k == 1) for (i = 0; i < 256; i += 2) pal[i + 1] = pal[i]; /* duplicates */
		if (k == 2) for (i = 0; i < 256; i++) { pal[i].red &= 0x1F; pal[i].green &= 0x1F; pal[i].blue &= 0x1F; } /* all dark */
		if (k == 3) for (i = 0; i < 256; i++) { pal[i].red = (UINT8)(Rnd() % 3 * 127); pal[i].green = (UINT8)(Rnd() % 3 * 127); pal[i].blue = (UINT8)(Rnd() % 3 * 127); } /* few distinct colours, many ties */
		if (k == 4) for (i = 0; i < 256; i++) pal[i].red = pal[i].green = pal[i].blue = (UINT8)(i < 128 ? 0 : 255); /* two colours */
		if (k == 5) for (i = 0; i < 256; i++) { pal[i].red = 255; pal[i].green = 0; pal[i].blue = 0; } /* one colour */
		{ char name[32]; snprintf(name, sizeof name, "random %d", k); fail |= Test(name, pal, 1); }
	}
	for (i = 1; i < argc; i++)
	{
		FILE *f = fopen(argv[i], "rb");
		unsigned char buf[768];

		if (!f || fread(buf, 1, 768, f) != 768) { printf("%s: cannot read 768 bytes\n", argv[i]); fail = 1; if (f) fclose(f); continue; }
		fclose(f);
		for (k = 0; k < 256; k++) { pal[k].red = buf[k*3]; pal[k].green = buf[k*3+1]; pal[k].blue = buf[k*3+2]; pal[k].alpha = 255; }
		fail |= Test(argv[i], pal, 1);
	}
	puts(fail ? "FAILED" : "ALL EQUAL");
	return fail;
}
