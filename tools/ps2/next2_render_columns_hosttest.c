/* Frozen-current-baseline versus actual source. The driver extracts the complete
 * drawer functions; sampling, wrapping and fixed math are not reimplemented.
 * Drawers are compiled separately against external globals as in r_draw.c,
 * without LTO, so the benchmark cannot prove fixture-specific alias exclusions.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#ifndef NEXT2_COLUMNS_EE
#include <time.h>
#endif
#include "next2_render_columns_stub.h"

testvid_t vid;
UINT8 *topleft, *dc_source, *dc_translation, *dc_transmap, *colormaps;
lighttable_t *dc_colormap;
INT32 dc_x, dc_yl, dc_yh, dc_texheight;
fixed_t dc_iscale, dc_texturemid, centeryfrac;
void I_Error(const char *message, ...)
{
	va_list args;
	va_start(args, message);
	vfprintf(stderr, message, args);
	va_end(args);
	exit(2);
}

typedef struct { const char *name; void (*draw[2])(void); } drawer_t;
#define DRAWER(n) { #n, { reference_##n, candidate_##n } }
static const drawer_t drawers[] = {
	DRAWER(R_DrawShadeColumn_8),
	DRAWER(R_DrawTranslatedColumn_8),
	DRAWER(R_DrawTranslatedTranslucentColumn_8),
};
#define NDRAWERS (sizeof drawers / sizeof drawers[0])
#define FB_GUARD 64
#define MAX_WIDTH 640
#define MAX_HEIGHT 480
#define FB_BYTES (MAX_WIDTH * MAX_HEIGHT + 2 * FB_GUARD)
static UINT8 frame[2][FB_BYTES], pattern[FB_BYTES];
static UINT8 source[2][131072], translation[2][256], cmap[2][256];
static UINT8 transmap[2][65536], shademap[2][65536];
static UINT32 random_state = 0x12345678u;
static UINT32 random32(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static void initialize(void)
{
	size_t i;
	for (i = 0; i < sizeof pattern; i++) pattern[i] = (UINT8)random32();
	for (i = 0; i < sizeof source; i++) ((UINT8 *)source)[i] = (UINT8)random32();
	for (i = 0; i < sizeof translation; i++) ((UINT8 *)translation)[i] = (UINT8)random32();
	for (i = 0; i < sizeof cmap; i++) ((UINT8 *)cmap)[i] = (UINT8)random32();
	for (i = 0; i < sizeof transmap; i++) ((UINT8 *)transmap)[i] = (UINT8)random32();
	for (i = 0; i < sizeof shademap; i++) ((UINT8 *)shademap)[i] = (UINT8)random32();
}

static void tables(unsigned mask)
{
	dc_source = source[mask & 1] + 65536;
	dc_translation = translation[(mask >> 1) & 1];
	dc_colormap = cmap[(mask >> 2) & 1];
	dc_transmap = transmap[(mask >> 3) & 1];
	colormaps = shademap[(mask >> 4) & 1];
}

#ifndef NEXT2_COLUMNS_EE
static UINT64 cases, compared_bytes, hash = UINT64_C(1469598103934665603);
static int broken_pixel;
static int alias_buffers;

static int compare(size_t d)
{
	const size_t bytes = (size_t)vid.width * vid.height + 2 * FB_GUARD;
	unsigned variant;
	for (variant = 0; variant < 2; variant++)
	{
		memcpy(frame[variant], pattern, bytes);
		topleft = frame[variant] + FB_GUARD;
		if (alias_buffers)
		{
			/* Contents may alias the framebuffer; only the pointer globals are
			 * excluded by the engine's destination-buffer contract. */
			dc_source = topleft + 65536;
			dc_translation = topleft + 256;
			dc_colormap = topleft + 512;
			dc_transmap = colormaps = topleft + 768;
		}
		drawers[d].draw[variant]();
	}
	if (broken_pixel) frame[1][FB_GUARD] ^= 1;
	if (memcmp(frame[0], frame[1], bytes))
	{
		printf("FAIL %s width=%d h=%d x=%d yl=%d yh=%d tex=%d step=%d mid=%d case=%llu\n",
			drawers[d].name, vid.width, vid.height, dc_x, dc_yl, dc_yh,
			dc_texheight, dc_iscale, dc_texturemid, (unsigned long long)cases);
		return 1;
	}
	if (dc_yh >= dc_yl)
	{
		INT32 row;
		for (row = dc_yl; row <= dc_yh; row++)
			hash = (hash ^ frame[1][FB_GUARD + row * vid.width + dc_x]) * UINT64_C(1099511628211);
	}
	cases++;
	compared_bytes += bytes;
	return 0;
}

static int equality(void)
{
	static const INT32 widths[] = {1, 7, 16, 31, 319, 320, 512, 640};
	static const INT32 lengths[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100, 127, 128, 199, 200, 479, 480};
	static const INT32 heights[] = {0, 1, 2, 3, 5, 24, 63, 64, 72, 100, 128, 256, 1023, 1024, 16383, 16384, 32767};
	static const fixed_t starts[] = {INT32_MIN, INT32_MIN + 1, -0x10001, -0x10000, -1, 0, 1, 0xFFFF, 0x10000, 0x7FFEFFFF, INT32_MAX};
	static const fixed_t steps[] = {INT32_MIN, -0x70000000, -0x10001, -0x10000, -1, 0, 1, 0xFFFF, 0x10000, 0x10001, 0x70000000, INT32_MAX};
	size_t d, w, n, s, p, h;
	unsigned i;
	for (d = 0; d < NDRAWERS; d++)
	for (w = 0; w < sizeof widths / sizeof widths[0]; w++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (s = 0; s < sizeof starts / sizeof starts[0]; s++)
	for (p = 0; p < sizeof steps / sizeof steps[0]; p++)
	{
		/* The unmasked shade/translated drawers permit any signed frac with the
		 * padded source. For the wrapped drawer exercise power-of-two masking
		 * across every signed fixed-point overflow boundary as well. */
		vid.width = widths[w];
		vid.height = lengths[n] ? lengths[n] : 1;
		dc_x = (INT32)((s + p) % (unsigned)vid.width);
		dc_yl = 0;
		dc_yh = lengths[n] - 1;
		dc_iscale = steps[p];
		centeryfrac = (fixed_t)(random32() & 0xFFFFFF);
		dc_texturemid = (fixed_t)((UINT32)starts[s] - (UINT32)FixedMul(-centeryfrac, dc_iscale));
		dc_texheight = (INT32)(1u << ((s + p) % 15));
		tables(random32());
		if (compare(d)) return 1;
	}
	/* Selected NPOT sizes, exact periods and repeated positive/negative
	 * initial normalization. The original NPOT step wraps once, so constrain
	 * it to a safe range of that original path rather than masking a fault. */
	for (h = 0; h < sizeof heights / sizeof heights[0]; h++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (i = 0; i < 48; i++)
	{
		fixed_t start;
		const UINT32 period = heights[h] ? (UINT32)heights[h] << 16 : 1;
		if (!(heights[h] & (heights[h] - 1))) continue;
		vid.width = (i & 1) ? 320 : 640;
		vid.height = lengths[n] ? lengths[n] : 1;
		dc_x = (INT32)(i % (unsigned)vid.width);
		dc_yl = 0; dc_yh = lengths[n] - 1;
		dc_texheight = heights[h];
		/* Avoid signed overflow of frac += step in the unchanged legacy loop. */
		dc_iscale = (fixed_t)(random32() % (period < 0x40000000 ? period : 0x10000));
		switch (i % 6)
		{
		case 0: start = 0; break;
		case 1: start = (fixed_t)(period - 1); break;
		case 2: start = (fixed_t)period; break;
		case 3: start = -(fixed_t)period; break;
		case 4: start = INT32_MIN; break;
		default: start = INT32_MAX; break;
		}
		centeryfrac = 100 * FRACUNIT;
		dc_texturemid = (fixed_t)((UINT32)start - (UINT32)FixedMul(-centeryfrac, dc_iscale));
		tables(random32());
		if (compare(2)) return 1;
	}
	/* Random valid draws vary destination row, source/table identities, signed
	 * coordinates and clipping-empty ranges between consecutive calls. */
	for (i = 0; i < 30000; i++)
	{
		const INT32 count = (INT32)(random32() % 201);
		vid.width = widths[random32() % (sizeof widths / sizeof widths[0])];
		vid.height = count + 11;
		dc_x = (INT32)(random32() % (unsigned)vid.width);
		dc_yl = (INT32)(random32() % 11);
		dc_yh = dc_yl + count - 1;
		dc_iscale = (fixed_t)random32();
		centeryfrac = (fixed_t)(random32() & 0xFFFFFF);
		dc_texturemid = (fixed_t)random32();
		dc_texheight = (INT32)(1u << (random32() % 15));
		tables(random32());
		if (compare(random32() % NDRAWERS)) return 1;
	}
	alias_buffers = 1;
	for (i = 0; i < 3000; i++)
	{
		vid.width = 640; vid.height = 200;
		dc_x = (INT32)(random32() % 640);
		dc_yl = 0; dc_yh = (INT32)(random32() % 200);
		dc_iscale = (fixed_t)random32(); centeryfrac = 100*FRACUNIT;
		dc_texturemid = (fixed_t)random32(); dc_texheight = 128;
		if (compare(i % NDRAWERS)) return 1;
	}
	alias_buffers = 0;
	printf("COLUMNS PASS cases=%llu compared_bytes=%llu hash=%016llx\n",
		(unsigned long long)cases, (unsigned long long)compared_bytes, (unsigned long long)hash);
	return 0;
}
#endif

static UINT64 now(void)
{
#ifdef NEXT2_COLUMNS_EE
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
	static const INT32 lengths[] = {1, 8, 32, 100, 200, 480};
	static const INT32 heights[] = {64, 128, 72, 100};
	const unsigned calls = 1024;
	size_t d, n, h;
	for (d = 0; d < NDRAWERS; d++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (h = 0; h < sizeof heights / sizeof heights[0]; h++)
	{
		UINT64 best[2] = {UINT64_MAX, UINT64_MAX};
		unsigned variant, rep, i;
		vid.width = 320; vid.height = MAX_HEIGHT;
		dc_x = 171; dc_yl = 0; dc_yh = lengths[n] - 1;
		dc_texheight = heights[h];
		dc_iscale = FRACUNIT / 2;
		centeryfrac = 100 * FRACUNIT;
		dc_texturemid = 3 * FRACUNIT - FixedMul(-centeryfrac, dc_iscale);
		for (rep = 0; rep < 5; rep++)
		for (variant = 0; variant < 2; variant++)
		{
			UINT64 start, elapsed;
			void (*draw)(void) = drawers[d].draw[variant];
			tables(rep);
			topleft = frame[variant] + FB_GUARD;
			draw();
			start = now();
			for (i = 0; i < calls; i++) draw();
#ifdef NEXT2_COLUMNS_EE
			elapsed = (UINT32)((UINT32)now() - (UINT32)start);
#else
			elapsed = now() - start;
#endif
			if (elapsed < best[variant]) best[variant] = elapsed;
		}
		printf("COLBENCH %s n=%d tex=%d calls=%u base=%llu cand=%llu unit=%s\n",
			drawers[d].name, lengths[n], heights[h], calls,
			(unsigned long long)(best[0] * 10 / calls), (unsigned long long)(best[1] * 10 / calls),
#ifdef NEXT2_COLUMNS_EE
			"cycles_x10"
#else
			"ns_x10"
#endif
		);
	}
	puts("COLUMNS DONE");
}

int main(int argc, char **argv)
{
	initialize();
#ifdef NEXT2_COLUMNS_EE
	(void)argc; (void)argv;
	benchmark();
#else
	if (argc > 1 && !strcmp(argv[1], "bench")) benchmark();
	else
	{
		broken_pixel = argc > 1 && !strcmp(argv[1], "broken-pixel");
		return equality();
	}
#endif
	return 0;
}
