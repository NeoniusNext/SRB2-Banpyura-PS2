/* Actual clamped drawers, complete and truncated posts, and fallback boundaries. */
#include "rdraw_stub.h"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4267 4018 4146 4305 4204 4100 4101 4189)
#endif
#include "lighting.inc"
#include "r_draw8.c"
#include "reference.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define FBSIZE (320 * 200)
static UINT8 reference[FBSIZE + 64], candidate[FBSIZE + 64];
static UINT8 source[32768], cmap[256], tmap[65536];

int main(int argc, char **argv)
{
	static const INT32 heights[] = {0, 1, 2, 3, 24, 64, 72, 100, 128, 16383, 16384};
	static const INT32 lengths[] = {1, 7, 8, 9, 31, 32, 63, 199};
	unsigned long cases = 0;
	unsigned h, n, p, step, start, mode, i;
	(void)argv;
	for (i = 0; i < sizeof source; ++i) source[i] = (UINT8)(i * 97 + 31);
	for (i = 0; i < sizeof cmap; ++i) cmap[i] = (UINT8)(i * 13 + 7);
	for (i = 0; i < sizeof tmap; ++i) tmap[i] = (UINT8)((i >> 8) * 19 + i * 3);
	dc_source = source;
	dc_colormap = cmap;
	dc_transmap = tmap;
	for (h = 0; h < sizeof heights / sizeof heights[0]; ++h)
	{
		const fixed_t period = heights[h] > 0 ? heights[h] << FRACBITS : 128 << FRACBITS;
		const fixed_t steps[] = {0, 1, FRACUNIT / 2, FRACUNIT, 7 * FRACUNIT,
			period - 1, period, period + 1, -1, -FRACUNIT};
		const fixed_t starts[] = {0, 1, period - 1, period, -period, -period + 1,
			-3 * (period / 4), 2 * period - 1};
		const INT32 posts[] = {-1, 0, 1, heights[h] / 2, heights[h] - 1, heights[h], heights[h] + 3};
		dc_texheight = heights[h];
		for (n = 0; n < sizeof lengths / sizeof lengths[0]; ++n)
		for (p = 0; p < sizeof posts / sizeof posts[0]; ++p)
		for (step = 0; step < sizeof steps / sizeof steps[0]; ++step)
		for (start = 0; start < sizeof starts / sizeof starts[0]; ++start)
		for (mode = 0; mode < 2; ++mode)
		{
			/* Post drawers with zero texheight index their source directly. */
			if (!dc_texheight && (starts[start] < 0 || steps[step] < 0)) continue;
			dc_yl = 200 - lengths[n]; dc_yh = 199; dc_x = 319;
			dc_postlength = posts[p]; dc_iscale = steps[step];
			dc_texturemid = starts[start] - FixedMul((dc_yl << FRACBITS) - centeryfrac, dc_iscale);
			for (i = 0; i < sizeof reference; ++i) reference[i] = (UINT8)(i * 41 + 17);
			memcpy(candidate, reference, sizeof reference);
			topleft = reference; screens[0] = reference;
			if (mode) Ref_R_DrawTranslucentColumnClamped_8(); else Ref_R_DrawColumnClamped_8();
			topleft = candidate; screens[0] = candidate;
			if (mode) R_DrawTranslucentColumnClamped_8(); else R_DrawColumnClamped_8();
			if (argc > 1) candidate[dc_yl * vid.width + dc_x] ^= 1;
			if (memcmp(reference, candidate, sizeof reference))
			{
				fprintf(stderr, "COLUMN FAIL h=%d n=%d post=%d step=%d start=%d blend=%u\n",
					dc_texheight, lengths[n], posts[p], steps[step], starts[start], mode);
				return 1;
			}
			++cases;
		}
	}
	printf("COLUMN PASS cases=%lu bytes_per_case=%u\n", cases, (unsigned)sizeof reference);
	return 0;
}
