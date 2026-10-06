/* Compare actual drawers with frozen pre-edit implementations, including canaries. */
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
static UINT8 source[65536], cmap[256], tmap[65536], translation[256];

int main(int argc, char **argv)
{
	static const INT32 heights[] = {1, 2, 3, 7, 24, 64, 72, 100, 128, 16383, 16384};
	static const INT32 lengths[] = {1, 3, 4, 7, 8, 9, 15, 16, 31, 32, 33, 63, 199};
	void (*const refs[])(void) = {Ref_R_DrawTranslucentColumn_8,
		Ref_R_DrawTranslucentColumnClamped_8, Ref_R_DrawTranslatedTranslucentColumn_8,
		Ref_R_Draw2sMultiPatchColumn_8, Ref_R_Draw2sMultiPatchTranslucentColumn_8};
	void (*const cands[])(void) = {R_DrawTranslucentColumn_8,
		R_DrawTranslucentColumnClamped_8, R_DrawTranslatedTranslucentColumn_8,
		R_Draw2sMultiPatchColumn_8, R_Draw2sMultiPatchTranslucentColumn_8};
	unsigned long cases = 0;
	unsigned h, n, step, start, mode, p, i;
	(void)argv;
	for (i = 0; i < sizeof source; ++i) source[i] = (UINT8)(i * 97 + 31);
	for (i = 0; i < sizeof cmap; ++i) cmap[i] = (UINT8)(i * 13 + 7);
	for (i = 0; i < sizeof translation; ++i) translation[i] = (UINT8)(i * 23 + 51);
	for (i = 0; i < sizeof tmap; ++i) tmap[i] = (UINT8)((i >> 8) * 19 + i * 3);
	dc_source = source; dc_colormap = cmap; dc_transmap = tmap; dc_translation = translation;
	for (h = 0; h < sizeof heights / sizeof heights[0]; ++h)
	{
		for (i = 0; i < sizeof source; ++i)
			source[i] = (h & 1) && !(i % 5) ? TRANSPARENTPIXEL : (UINT8)(i * 97 + 31);
		const fixed_t period = heights[h] << FRACBITS;
		const fixed_t steps[] = {0, 1, FRACUNIT / 2, FRACUNIT, 7 * FRACUNIT,
			period / 4 - 1, period / 4, period / 4 + 1, period / 2, period - 1, period};
		const fixed_t starts[] = {0, 1, period - 1, period, -period, -period + 1,
			-3 * (period / 4), 2 * period - 1, period - 3 * (period / 4)};
		const INT32 posts[] = {0, 1, heights[h] / 2, heights[h] - 1, heights[h], heights[h] + 3};
		dc_texheight = heights[h];
		for (n = 0; n < sizeof lengths / sizeof lengths[0]; ++n)
		for (p = 0; p < sizeof posts / sizeof posts[0]; ++p)
		for (step = 0; step < sizeof steps / sizeof steps[0]; ++step)
		for (start = 0; start < sizeof starts / sizeof starts[0]; ++start)
		for (mode = 0; mode < sizeof refs / sizeof refs[0]; ++mode)
		{
			dc_yl = 200 - lengths[n]; dc_yh = 199; dc_x = cases % 320;
			dc_postlength = posts[p]; dc_iscale = steps[step];
			dc_texturemid = starts[start] - FixedMul((dc_yl << FRACBITS) - centeryfrac, dc_iscale);
			for (i = 0; i < sizeof reference; ++i) reference[i] = (UINT8)(i * 41 + 17);
			memcpy(candidate, reference, sizeof reference);
			topleft = reference; screens[0] = reference;
			refs[mode]();
			topleft = candidate; screens[0] = candidate;
			cands[mode]();
			if (argc > 1) candidate[dc_yl * vid.width + dc_x] ^= 1;
			if (memcmp(reference, candidate, sizeof reference))
			{
				fprintf(stderr, "OCT4 COLUMN FAIL h=%d n=%d post=%d step=%d start=%d mode=%u\n",
					dc_texheight, lengths[n], posts[p], steps[step], starts[start], mode);
				return 1;
			}
			++cases;
		}
	}
	printf("OCT4 COLUMN PASS cases=%lu bytes_per_case=%u\n", cases, (unsigned)sizeof reference);
	return 0;
}
