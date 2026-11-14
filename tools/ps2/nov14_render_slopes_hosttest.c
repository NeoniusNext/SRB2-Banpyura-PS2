/* Actual frozen/current NPOT slope drawers: byte equality, cache lifecycle and
 * a same-binary timing fixture. Generated includes come from the Python driver.
 * No geometry, slope conversion, lighting or sampling code is reimplemented.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#ifndef NOV14_SLOPES_EE
#include <time.h>
#endif
#include "m_fixed.h"
static void I_Error(const char *message, ...)
{
	va_list args;
	va_start(args, message);
	vfprintf(stderr, message, args);
	va_end(args);
	exit(2);
}
#include "libdivide.h"

#define MAXVIDWIDTH 640
#define MAXLIGHTSCALE 48
#define BASEVIDWIDTH 320
#define TRANSPARENTPIXEL 255
#define SPANSIZE 16
#define INVSPAN 0.0625f
#define FB_HEIGHT 9
#define FB_GUARD 32
#define FB_BYTES (MAXVIDWIDTH * FB_HEIGHT + 2 * FB_GUARD)
#define SOURCE_BYTES (512 * 512)

typedef UINT8 lighttable_t;
static struct { INT32 width, height, rowbytes; } vid;
static INT32 centerx, centery;
#ifndef PS2_OPT_SLOPE
static fixed_t fovtan = FRACUNIT;
#endif
static UINT8 *topleft, *screens[2], *colormaps;
static lighttable_t *planezlight[MAXLIGHTSCALE];
static INT32 ds_y, ds_x1, ds_x2, ds_bgofs;
static UINT16 ds_flatwidth, ds_flatheight;
static UINT8 *ds_source, *ds_colormap, *ds_transmap, *ds_translation;
#ifdef PS2_OPT_SLOPE
typedef struct { float x, y, z; } testvector_t;
typedef float slopereal_t;
static float ds_lightscale;
#define SLOPE_U32(x) R_SlopeToU32(x)
#else
typedef struct { double x, y, z; } testvector_t;
typedef double slopereal_t;
static double zeroheight = 16.0;
#define SLOPE_U32(x) ((UINT32)(INT64)(x))
#endif
static testvector_t ds_su, ds_sv, ds_sz, ds_slopelight;
#include "lighting.inc"

#ifndef NOV14_SLOPES_EE
static unsigned reference_generated, candidate_generated;
static int broken_divider;
static struct libdivide_u32_t reference_gen(uint32_t divisor)
{
	reference_generated++;
	return libdivide_u32_gen(divisor);
}
static struct libdivide_u32_t candidate_gen(uint32_t divisor)
{
	candidate_generated++;
	if (broken_divider && divisor > 1)
		divisor--;
	return libdivide_u32_gen(divisor);
}
#define libdivide_u32_gen reference_gen
#endif
#include "reference.inc"
#ifndef NOV14_SLOPES_EE
#undef libdivide_u32_gen
#define libdivide_u32_gen candidate_gen
#endif
#include "candidate.inc"
#ifndef NOV14_SLOPES_EE
#undef libdivide_u32_gen
#endif

typedef struct
{
	const char *name;
	void (*reference)(void), (*candidate)(void);
	int sprite;
} drawer_t;
#define DRAWER(name, sprite) { #name, reference_##name, candidate_##name, sprite }
static const drawer_t drawers[] = {
	DRAWER(R_DrawTiltedSpan_NPO2_8, 0),
	DRAWER(R_DrawTiltedTranslucentSpan_NPO2_8, 0),
	DRAWER(R_DrawTiltedSplat_NPO2_8, 0),
	DRAWER(R_DrawTiltedTranslucentSplat_NPO2_8, 0),
	DRAWER(R_DrawTiltedFloorSprite_NPO2_8, 1),
	DRAWER(R_DrawTiltedTranslucentFloorSprite_NPO2_8, 1),
	DRAWER(R_DrawTiltedWaterSpan_NPO2_8, 0),
};
#define NDRAWERS (sizeof drawers / sizeof drawers[0])
static UINT8 frame_candidate[FB_BYTES];
#ifndef NOV14_SLOPES_EE
static UINT8 frame_reference[FB_BYTES];
#endif
static UINT8 background[MAXVIDWIDTH * FB_HEIGHT];
static UINT8 source8[SOURCE_BYTES], cmaps[MAXLIGHTSCALE * 256 + 256];
static UINT16 source16[SOURCE_BYTES];
static UINT8 transmap[65536], translation[256];
#ifndef NOV14_SLOPES_EE
static UINT64 case_count, compared_bytes, hash = UINT64_C(1469598103934665603);
#endif

static void initialize(void)
{
	size_t i;
	for (i = 0; i < sizeof cmaps; i++) cmaps[i] = (UINT8)((i * 29 + i / 256 * 17) ^ (i >> 3));
	for (i = 0; i < MAXLIGHTSCALE; i++) planezlight[i] = cmaps + i * 256;
	for (i = 0; i < sizeof transmap; i++) transmap[i] = (UINT8)((i >> 8) * 23 + (i & 255) * 71);
	for (i = 0; i < sizeof translation; i++) translation[i] = (UINT8)(i * 47 + 19);
	for (i = 0; i < sizeof background; i++) background[i] = (UINT8)(i * 53 + (i >> 7));
	for (i = 0; i < SOURCE_BYTES; i++)
	{
		source8[i] = (i % 3 == 0) ? TRANSPARENTPIXEL : (UINT8)(i * 37 + i / 11);
		source16[i] = (UINT16)((i % 5 == 0 ? 0 : 0xFF00) | ((i * 41 + i / 13) & 255));
	}
	colormaps = cmaps;
	ds_transmap = transmap;
	ds_translation = translation;
	screens[1] = background;
}

/* Includes signed fixed-coordinate boundaries, exact periods, many wraps, and
 * values whose conversion takes the preexisting large-coordinate helper.
 */
static void parameters(int framewidth, int first, int length, unsigned mode)
{
	static const double starts[] = {
		0, -1, 65535, -65536, 2147483648.0, -2147483648.0,
		4294967296.0, -4294967296.0, 1099511627776.0, -1099511627776.0,
		2147418112.0, -2147418112.0, 123456789.0, -987654321.0,
	};
	const double start = starts[mode % (sizeof starts / sizeof starts[0])];
	vid.width = vid.rowbytes = framewidth;
	vid.height = FB_HEIGHT;
	centerx = framewidth / 2;
	centery = FB_HEIGHT / 2;
	ds_y = 2 + (INT32)(mode % 5);
	ds_x1 = first;
	ds_x2 = first + length - 1;
	ds_bgofs = (INT32)(mode % 3) - 1;
	ds_colormap = cmaps + (mode % 2) * 256;
	ds_sz.x = (mode & 1) ? -1.0 / 4096 : 1.0 / 4096;
	ds_sz.y = 1.0 / 8192;
	ds_sz.z = 1.25;
	ds_su.x = ((INT32)(mode % 5) - 2) * 195312.5;
	ds_su.y = -32768;
	ds_su.z = start;
	ds_sv.x = ((INT32)(mode % 7) - 3) * 37672.25;
	ds_sv.y = 65536;
	ds_sv.z = -start + (double)ds_flatheight * 65536;
	if (mode >= 14)
	{
		/* Constant depth gives exact positive/negative dimension multiples. */
		ds_sz.x = ds_sz.y = 0;
		ds_sz.z = 1;
		ds_su.x = ds_sv.x = ds_su.y = ds_sv.y = 0;
		ds_su.z = (double)ds_flatwidth * 65536 * ((INT32)mode - 17);
		ds_sv.z = (double)ds_flatheight * 65536 * (17 - (INT32)mode);
	}
	ds_slopelight.x = (mode & 2) ? -0.125 : 0.125;
	ds_slopelight.y = 0.25;
	ds_slopelight.z = (mode & 4) ? 60 : -10;
#ifdef PS2_OPT_SLOPE
	ds_lightscale = 1.0;
#endif
}

#ifndef NOV14_SLOPES_EE
static unsigned expected_axis_generations(UINT16 dimension, UINT16 keys[2], unsigned *next)
{
	if (keys[0] == dimension || keys[1] == dimension) return 0;
	keys[*next] = dimension;
	*next ^= 1;
	return 1;
}

static int divider_equivalence(void)
{
#ifdef PS2_OPT_DRAW
	UINT32 dimension, repeat;
	for (dimension = 1; dimension <= 65535; dimension++)
	for (repeat = 0; repeat < 4; repeat++)
	{
		struct libdivide_u32_t x, y, expected_x, expected_y;
		ds_flatwidth = (UINT16)(repeat & 1 ? 65536 - dimension : dimension);
		ds_flatheight = (UINT16)dimension;
		expected_x = libdivide_u32_gen(ds_flatwidth);
		expected_y = libdivide_u32_gen(ds_flatheight);
		R_NPO2SlopeDividers(&x, &y);
		if (x.magic != expected_x.magic || x.more != expected_x.more
			|| y.magic != expected_y.magic || y.more != expected_y.more)
		{
			printf("FAIL divider dim=%u repeat=%u\n", dimension, repeat);
			return 1;
		}
	}
	puts("SLOPE DIVIDERS PASS dimensions=65535 descriptor_pairs=262140");
#endif
	return 0;
}

static int equality(int broken_pixel)
{
	static const UINT16 dimensions[][2] = {
		{1,3}, {3,1}, {3,5}, {63,47}, {64,47}, {47,64}, {1,1},
		{257,17}, {1024,3}, {16383,1}, {1,16384}, {32767,1},
		{65535,1}, {1,65535}, {319,241}, {512,512}, {63,47},
	};
	static const int framewidths[] = {1, 7, 16, 31, 319, 320, 512, 640};
	static const int lengths[] = {1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 127, 319, 320, 321, 512, 639, 640};
#ifdef PS2_OPT_DRAW
	UINT16 widthkeys[2] = {0, 0}, heightkeys[2] = {0, 0};
	unsigned nextwidth = 0, nextheight = 0;
#endif
	size_t dim, f, n, d, i;
	unsigned mode;
	for (dim = 0; dim < sizeof dimensions / sizeof dimensions[0]; dim++)
	for (f = 0; f < sizeof framewidths / sizeof framewidths[0]; f++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (mode = 0; mode < 21; mode++)
	{
		const int length = lengths[n], fw = framewidths[f];
		static const unsigned churn[] = {0, 1, 0, 1, 2, 0, 2};
		const size_t keydim = (dim + (mode < 14 ? 0 : churn[mode - 14]))
			% (sizeof dimensions / sizeof dimensions[0]);
		int first;
		if (length > fw) continue;
		first = mode % 3 == 0 ? 0 : (mode % 3 == 1 ? fw - length : (fw - length) / 2);
		ds_flatwidth = dimensions[keydim][0];
		ds_flatheight = dimensions[keydim][1];
		for (d = 0; d < NDRAWERS; d++)
		{
			INT32 expected_x1, expected_lighting[MAXVIDWIDTH];
			unsigned old_reference = reference_generated, old_candidate = candidate_generated;
			unsigned expected_generated;
			for (i = 0; i < FB_BYTES; i++) frame_reference[i] = (UINT8)(i * 31 + (i >> 4) + mode);
			memcpy(frame_candidate, frame_reference, sizeof frame_reference);
			parameters(fw, first, length, mode);
			ds_source = drawers[d].sprite ? (UINT8 *)source16 : source8;
			screens[0] = topleft = frame_reference + FB_GUARD;
			memset(tiltlighting, 0xA5, sizeof tiltlighting);
			drawers[d].reference();
			expected_x1 = ds_x1;
			memcpy(expected_lighting, tiltlighting, sizeof tiltlighting);
			parameters(fw, first, length, mode);
			screens[0] = topleft = frame_candidate + FB_GUARD;
			memset(tiltlighting, 0xA5, sizeof tiltlighting);
			drawers[d].candidate();
			if (broken_pixel) topleft[ds_y * fw + first] ^= 1;
#ifdef PS2_OPT_DRAW
			expected_generated = expected_axis_generations(ds_flatwidth, widthkeys, &nextwidth)
				+ expected_axis_generations(ds_flatheight, heightkeys, &nextheight);
#else
			expected_generated = 2;
#endif
			if (memcmp(frame_reference, frame_candidate, FB_BYTES) || ds_x1 != expected_x1
				|| memcmp(expected_lighting, tiltlighting, sizeof tiltlighting)
				|| reference_generated - old_reference != 2
				|| candidate_generated - old_candidate != expected_generated)
			{
				printf("FAIL drawer=%s dim=%ux%u frame=%d x=%d n=%d mode=%u gens=%u/%u expected=%u\n",
					drawers[d].name, ds_flatwidth, ds_flatheight, fw, first, length, mode,
					reference_generated-old_reference, candidate_generated-old_candidate, expected_generated);
				return 1;
			}
			case_count++;
			compared_bytes += FB_BYTES;
			for (i = 0; i < (size_t)length; i++) hash = (hash ^ topleft[ds_y * fw + first + i]) * UINT64_C(1099511628211);
		}
	}
	printf("SLOPES PASS cases=%llu compared_bytes=%llu hash=%016llx divider_generations=%u/%u\n",
		(unsigned long long)case_count, (unsigned long long)compared_bytes,
		(unsigned long long)hash, reference_generated, candidate_generated);
	return divider_equivalence();
}
#endif

static UINT64 now(void)
{
#ifdef NOV14_SLOPES_EE
	UINT32 value;
	__asm__ volatile("mfc0 %0,$9" : "=r"(value));
	return value;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (UINT64)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

static void benchmark(void)
{
	static const UINT16 dims[][2] = {{63,47}, {64,47}, {257,17}};
	static const int lengths[] = {1,16,100,320};
	size_t d, s, n;
	int churn;
	const unsigned calls = 1024;
	for (d = 0; d < NDRAWERS; d++)
	for (s = 0; s < sizeof dims / sizeof dims[0]; s++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (churn = 0; churn < 2; churn++)
	{
		UINT64 best[2] = {UINT64_MAX, UINT64_MAX};
		unsigned variant, rep, i;
		for (rep = 0; rep < 3; rep++)
		for (variant = 0; variant < 2; variant++)
		{
			void (*draw)(void) = variant ? drawers[d].candidate : drawers[d].reference;
			UINT64 start, elapsed;
			ds_flatwidth = dims[s][0];
			ds_flatheight = dims[s][1];
			parameters(320, 0, lengths[n], 12);
			ds_source = drawers[d].sprite ? (UINT8 *)source16 : source8;
			screens[0] = topleft = frame_candidate + FB_GUARD;
			draw();
			start = now();
			for (i = 0; i < calls; i++)
			{
				ds_x1 = 0;
				ds_flatwidth = (UINT16)(dims[s][0] + (churn && (i & 1)));
				ds_flatheight = (UINT16)(dims[s][1] + (churn && (i & 1)));
				draw();
			}
#ifdef NOV14_SLOPES_EE
			elapsed = (UINT32)((UINT32)now() - (UINT32)start);
#else
			elapsed = now() - start;
#endif
			if (elapsed < best[variant]) best[variant] = elapsed;
		}
		printf("SLOPEBENCH %s dim=%ux%u n=%d churn=%d calls=%u base=%llu cand=%llu unit=%s\n",
			drawers[d].name, dims[s][0], dims[s][1], lengths[n], churn, calls,
			(unsigned long long)(best[0] * 10 / calls), (unsigned long long)(best[1] * 10 / calls),
#ifdef NOV14_SLOPES_EE
			"cycles_x10"
#else
			"ns_x10"
#endif
		);
	}
	puts("SLOPES DONE");
}

int main(int argc, char **argv)
{
	initialize();
#ifdef NOV14_SLOPES_EE
	(void)argc; (void)argv;
	benchmark();
#else
	if (argc > 1 && !strcmp(argv[1], "bench")) benchmark();
	else if (argc > 1 && !strncmp(argv[1], "zero-", 5))
	{
		void (*draw)(void) = strstr(argv[1], "candidate") ? drawers[0].candidate : drawers[0].reference;
		ds_flatwidth = 63; ds_flatheight = 47;
		parameters(320, 0, 1, 0);
		ds_source = source8;
		screens[0] = topleft = frame_candidate + FB_GUARD;
		draw(); // Warm both cache keys before testing the invalid dimension.
		if (strstr(argv[1], "width")) ds_flatwidth = 0;
		else ds_flatheight = 0;
		ds_x1 = 0;
		draw();
		puts("FAIL zero dimension did not call I_Error");
		return 1;
	}
	else
	{
		broken_divider = argc > 1 && !strcmp(argv[1], "broken-divider");
		return equality(argc > 1 && !strcmp(argv[1], "broken-pixel"));
	}
#endif
	return 0;
}
