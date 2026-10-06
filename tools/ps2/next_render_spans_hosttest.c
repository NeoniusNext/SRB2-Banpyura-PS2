/* Actual NPOT fast spans, including the original inclusive framebuffer bound. */
#include "rdraw_stub.h"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4267 4018 4146 4305 4204 4100 4101 4189)
#endif
#include "lighting.inc"
#include "r_draw8.c"
#include "r_draw8_npo2.c"
#include "reference.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define FBSIZE (320 * 200)
static UINT8 reference[FBSIZE + 1024], candidate[FBSIZE + 1024], background[FBSIZE + 1024];
static UINT16 source[65536];
static UINT8 cmap[256], tmap[65536];
typedef void (*drawer_t)(void);
static const drawer_t drawers[] = {R_DrawSpan_NPO2_8, R_DrawTranslucentSpan_NPO2_8,
	R_DrawWaterSpan_NPO2_8, R_DrawSplat_NPO2_8, R_DrawTranslucentSplat_NPO2_8,
	R_DrawFloorSprite_NPO2_8, R_DrawTranslucentFloorSprite_NPO2_8};
static const drawer_t references[] = {Ref_R_DrawSpan_NPO2_8, Ref_R_DrawTranslucentSpan_NPO2_8,
	Ref_R_DrawWaterSpan_NPO2_8, Ref_R_DrawSplat_NPO2_8, Ref_R_DrawTranslucentSplat_NPO2_8,
	Ref_R_DrawFloorSprite_NPO2_8, Ref_R_DrawTranslucentFloorSprite_NPO2_8};

int main(int argc, char **argv)
{
	static const INT32 counts[] = {1, 7, 8, 9, 31, 32, 33, 319, 320, 321};
	static const INT32 rows[] = {0, 198, 199};
	static const INT32 columns[] = {0, 309, 310, 312, 313, 319};
	static const fixed_t steps[] = {-FRACUNIT, -1, 0, 1, FRACUNIT};
	size_t d, n, row, col, step, i;
	unsigned long cases = 0;
	(void)argv;
	for (i = 0; i < sizeof source / sizeof source[0]; ++i) source[i] = (UINT16)(0xFF00 | (i * 97 + 31) % 256);
	for (i = 0; i < sizeof cmap; ++i) cmap[i] = (UINT8)(i * 13 + 7);
	for (i = 0; i < sizeof tmap; ++i) tmap[i] = (UINT8)((i >> 8) * 19 + i * 3);
	for (i = 0; i < sizeof background; ++i) background[i] = (UINT8)(i * 73 + 31);
	ds_source = (UINT8 *)source; ds_colormap = cmap; ds_translation = cmap; ds_transmap = tmap;
	ds_flatwidth = 63; ds_flatheight = 47; ds_bgofs = 0; screens[1] = background;
	for (d = 0; d < sizeof drawers / sizeof drawers[0]; ++d)
	for (n = 0; n < sizeof counts / sizeof counts[0]; ++n)
	for (row = 0; row < sizeof rows / sizeof rows[0]; ++row)
	for (col = 0; col < sizeof columns / sizeof columns[0]; ++col)
	for (step = 0; step < sizeof steps / sizeof steps[0]; ++step)
	{
		ds_y = rows[row]; ds_x1 = columns[col]; ds_x2 = ds_x1 + counts[n] - 1;
		ds_xfrac = 62 * FRACUNIT + 65535; ds_yfrac = 0;
		ds_xstep = steps[step]; ds_ystep = -steps[step];
		for (i = 0; i < sizeof reference; ++i) reference[i] = (UINT8)(i * 41 + 17);
		memcpy(candidate, reference, sizeof reference);
		topleft = reference; screens[0] = reference; references[d]();
		topleft = candidate; screens[0] = candidate; drawers[d]();
		if (argc > 1) candidate[ds_y * vid.width + ds_x1] ^= 1;
		if (memcmp(reference, candidate, sizeof reference))
		{
			fprintf(stderr, "SPAN FAIL drawer=%zu count=%d y=%d x=%d step=%d\n", d, counts[n], ds_y, ds_x1, steps[step]);
			return 1;
		}
		++cases;
	}
	printf("SPAN PASS cases=%lu bytes_per_case=%u\n", cases, (unsigned)sizeof reference);
	return 0;
}
