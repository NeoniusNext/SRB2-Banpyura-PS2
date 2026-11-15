/* Frozen/current complete NPOT perspective drawers live in another translation unit.
 * This driver sets globals, calls them, compares every framebuffer/canary byte,
 * and measures the same functions on the EE without cross-TU alias proofs.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#ifndef NEXT5_NPOT_EE
#include <time.h>
#endif
#include "next5_render_npot_stub.h"
#define FB_HEIGHT 9
#define FB_GUARD 32
#define FB_BYTES (MAXVIDWIDTH * FB_HEIGHT + 2 * FB_GUARD)
#define ALIAS_BYTES (65536 + FB_BYTES)
#define SOURCE_PIXELS (512 * 512)
next5_vid_t vid;
INT32 centerx, centery, ds_y, ds_x1, ds_x2, ds_bgofs;
UINT8 *topleft, *screens[2], *colormaps;
lighttable_t **planezlight;
UINT8 *ds_source, *ds_colormap, *ds_translation, *ds_transmap;
UINT16 ds_flatwidth, ds_flatheight;
next5_vector_t ds_su, ds_sv, ds_sz, ds_slopelight;
#ifdef PS2_OPT_SLOPE
float ds_lightscale;
#else
double zeroheight = 16;
fixed_t fovtan = FRACUNIT;
#endif

typedef struct { const char *name; void (*reference)(void), (*candidate)(void); int sprite; } drawer_t;
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
static UINT8 frame_reference[ALIAS_BYTES] __attribute__((aligned(16)));
static UINT8 frame_candidate[ALIAS_BYTES] __attribute__((aligned(16)));
static UINT8 background[FB_BYTES], source8[2][SOURCE_PIXELS];
static UINT16 source16[2][SOURCE_PIXELS];
static UINT8 cmaps[2][MAXLIGHTSCALE * 256 + 256], transmaps[2][65536], translations[2][256];
static lighttable_t *lightptrs[2][MAXLIGHTSCALE];
#ifndef NEXT5_NPOT_EE
static UINT64 cases, compared_bytes, hash = UINT64_C(1469598103934665603);
#endif

void I_Error(const char *message, ...)
{
	va_list args;
	va_start(args, message);
	vfprintf(stderr, message, args);
	va_end(args);
	exit(2);
}

static const UINT16 dimensions[][2] = {
	{1,1}, {1,3}, {3,1}, {3,5}, {63,47}, {64,47}, {47,64}, {257,17},
	{1024,3}, {16383,1}, {1,16384}, {32767,1}, {65535,1}, {1,65535},
	{319,241}, {512,512}, {2,2}, {2048,3}, {72,100}, {100,72}, {255,257}, {127,129},
};
#define NDIMENSIONS (sizeof dimensions / sizeof dimensions[0])

static void initialize(void)
{
	size_t i, set;
	for (set = 0; set < 2; set++)
	{
		for (i = 0; i < sizeof cmaps[set]; i++) cmaps[set][i] = (UINT8)((i * 29 + i / 256 * 17 + set * 37) ^ (i >> 3));
		for (i = 0; i < sizeof transmaps[set]; i++) transmaps[set][i] = (UINT8)((i >> 8) * 23 + (i & 255) * 71 + set * 13);
		for (i = 0; i < sizeof translations[set]; i++) translations[set][i] = (UINT8)(i * 47 + 19 + set * 41);
	}
	for (i = 0; i < sizeof background; i++) background[i] = (UINT8)(i * 53 + (i >> 7));
	for (set = 0; set < 2; set++)
	for (i = 0; i < SOURCE_PIXELS; i++)
	{
		source8[set][i] = i % 3 == 0 ? TRANSPARENTPIXEL : (UINT8)(i * 37 + i / 11 + set * 17);
		source16[set][i] = (UINT16)((i % 5 == 0 ? 0 : 0xFF00) | ((i * 41 + i / 13 + set * 23) & 255));
	}
}

static void parameters(int fw, int first, int length, unsigned mode, unsigned dim)
{
	static const double starts[] = {0, -1, 65535, -65536, 2147483648.0, -2147483648.0,
		4294967296.0, -4294967296.0, 1099511627776.0, -1099511627776.0,
		2147418112.0, -2147418112.0, 123456789.0, -987654321.0};
	const double start = starts[mode % (sizeof starts / sizeof starts[0])];
	vid.width = vid.rowbytes = fw;
	vid.height = FB_HEIGHT;
	centerx = fw / 2; centery = FB_HEIGHT / 2;
	ds_y = 2 + (INT32)(mode % 5);
	ds_x1 = first; ds_x2 = first + length - 1;
	ds_bgofs = (INT32)(mode % 3) - 1;
	ds_flatwidth = dimensions[dim][0]; ds_flatheight = dimensions[dim][1];
	ds_sz.x = mode & 1 ? -1.0 / 4096 : 1.0 / 4096;
	ds_sz.y = 1.0 / 8192; ds_sz.z = 1.25;
	ds_su.x = ((INT32)(mode % 5) - 2) * 195312.5;
	ds_su.y = -32768; ds_su.z = start;
	ds_sv.x = ((INT32)(mode % 7) - 3) * 37672.25;
	ds_sv.y = 65536; ds_sv.z = -start + (double)ds_flatheight * 65536;
	if (mode >= 14)
	{
		ds_sz.x = ds_sz.y = 0; ds_sz.z = 1;
		ds_su.x = ds_sv.x = ds_su.y = ds_sv.y = 0;
		ds_su.z = (double)ds_flatwidth * 65536 * ((INT32)mode - 17);
		ds_sv.z = (double)ds_flatheight * 65536 * (17 - (INT32)mode);
	}
	ds_slopelight.x = mode & 2 ? -0.125 : 0.125;
	ds_slopelight.y = 0.25; ds_slopelight.z = mode & 4 ? 60 : -10;
#ifdef PS2_OPT_SLOPE
	ds_lightscale = 1;
#endif
}

/* Aliases refer only to data bytes inside the destination allocation, never to
 * storage of renderer globals. Source and table contents must remain live reads.
 */
static void bind_frame(UINT8 *frame, size_t d, unsigned mode, unsigned alias)
{
	size_t i;
	const unsigned set = mode & 1;
	UINT8 *writebase, *alignedbase;
	screens[0] = topleft = frame + FB_GUARD;
	screens[1] = alias == 7 ? topleft : background + FB_GUARD;
	writebase = topleft + ds_y * vid.width + ds_x1;
	alignedbase = topleft + ((ds_y * vid.width + ds_x1) & ~1);
	colormaps = alias == 2 || alias == 5 ? writebase : cmaps[set];
	ds_colormap = colormaps + (mode % 2) * 256;
	planezlight = lightptrs[set];
	for (i = 0; i < MAXLIGHTSCALE; i++)
		planezlight[i] = colormaps + (alias == 2 || alias == 5 ? (i % 8) : i) * 256;
	ds_source = alias == 1 || alias == 5 ? alignedbase : (alias == 6 ? alignedbase + 32 : (drawers[d].sprite ? (UINT8 *)source16[set] : source8[set]));
	ds_translation = alias == 3 || alias == 5 ? writebase : translations[set];
	ds_transmap = alias == 4 || alias == 5 ? frame : transmaps[set];
}

#ifndef NEXT5_NPOT_EE
static int compare(size_t d, int fw, int first, int length, unsigned mode, unsigned dim, unsigned alias, int broken)
{
	size_t i, bytes = alias ? ALIAS_BYTES : FB_BYTES;
	INT32 expected_x, expected_light[MAXVIDWIDTH];
	for (i = 0; i < bytes; i++) frame_reference[i] = (UINT8)(i * 31 + (i >> 4) + mode);
	memcpy(frame_candidate, frame_reference, bytes);
	parameters(fw, first, length, mode, dim);
	bind_frame(frame_reference, d, mode, alias);
	memset(tiltlighting, 0xA5, sizeof tiltlighting);
	drawers[d].reference();
	expected_x = ds_x1;
	memcpy(expected_light, tiltlighting, sizeof tiltlighting);
	parameters(fw, first, length, mode, dim);
	bind_frame(frame_candidate, d, mode, alias);
	memset(tiltlighting, 0xA5, sizeof tiltlighting);
	drawers[d].candidate();
	if (broken) topleft[ds_y * fw + first] ^= 1;
	if (memcmp(frame_reference, frame_candidate, bytes) || ds_x1 != expected_x
		|| memcmp(expected_light, tiltlighting, sizeof tiltlighting))
	{
		printf("NPOT FAIL drawer=%s dim=%ux%u frame=%d x=%d n=%d mode=%u alias=%u x1=%d/%d\n",
			drawers[d].name, ds_flatwidth, ds_flatheight, fw, first, length, mode, alias, expected_x, ds_x1);
		return 1;
	}
	cases++; compared_bytes += bytes;
	for (i = 0; length > 0 && i < (size_t)length; i++) hash = (hash ^ topleft[ds_y * fw + first + i]) * UINT64_C(1099511628211);
	return 0;
}

static int equality(int broken, int alias_only)
{
	static const int widths[] = {1,7,16,31,319,320,512,640};
	static const int lengths[] = {-1,1,2,3,7,8,15,16,17,31,32,33,63,127,319,320,321,512,639,640};
	size_t f, n, d;
	unsigned dim, mode, alias;
	if (!alias_only)
	for (dim = 0; dim < NDIMENSIONS; dim++)
	for (f = 0; f < sizeof widths / sizeof widths[0]; f++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (mode = 0; mode < 21; mode++)
	{
		const int length = lengths[n], fw = widths[f];
		int first;
		if (length > fw) continue;
		first = length < 0 || mode % 3 == 0 ? 0 : (mode % 3 == 1 ? fw - length : (fw - length) / 2);
		for (d = 0; d < NDRAWERS; d++)
			if (compare(d, fw, first, length, mode, dim, 0, broken)) return 1;
	}
	for (alias = 1; alias <= 7; alias++)
	for (mode = 0; mode < 21; mode++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (d = 0; d < NDRAWERS; d++)
	{
		const INT32 first = (INT32)(mode % 3) * 61;
		if (first + lengths[n] > 640) continue;
		if (compare(d, 640, first, lengths[n], mode, 4, alias, broken)) return 1;
	}
	printf("NPOT PASS cases=%llu compared_bytes=%llu hash=%016llx aliases=%u\n",
		(unsigned long long)cases, (unsigned long long)compared_bytes, (unsigned long long)hash, 7u);
	return 0;
}
#endif

static UINT64 now(void)
{
#ifdef NEXT5_NPOT_EE
	UINT32 value;
	__asm__ volatile("mfc0 %0,$9" : "=r"(value));
	return value;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (UINT64)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

static int benchmark(void)
{
	static const unsigned dimindices[] = {4,5,7};
	static const int lengths[] = {1,8,16,32,100,320};
	size_t d, b, n, i;
	unsigned dense;
	const unsigned calls = 1024;
	for (dense = 0; dense < 2; dense++)
	{
		for (i = 0; i < SOURCE_PIXELS; i++)
		{
			source8[0][i] = dense || i % 3 ? (UINT8)((i * 37 + i / 11) % 255) : TRANSPARENTPIXEL;
			source16[0][i] = (UINT16)((dense || i % 5 ? 0xFF00 : 0) | ((i * 41 + i / 13) & 255));
		}
		for (d = 0; d < NDRAWERS; d++)
		for (b = 0; b < sizeof dimindices / sizeof dimindices[0]; b++)
		for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
		{
			UINT64 best[2] = {UINT64_MAX, UINT64_MAX};
			INT32 expected_light[MAXVIDWIDTH], expected_x;
			unsigned order, rep;
			memset(frame_reference, 0x73, sizeof frame_reference);
			memcpy(frame_candidate, frame_reference, sizeof frame_reference);
			parameters(320, 0, lengths[n], 12, dimindices[b]); bind_frame(frame_reference, d, 12, 0);
			memset(tiltlighting, 0xA5, sizeof tiltlighting);
			drawers[d].reference(); expected_x = ds_x1;
			memcpy(expected_light, tiltlighting, sizeof tiltlighting);
			parameters(320, 0, lengths[n], 12, dimindices[b]); bind_frame(frame_candidate, d, 12, 0);
			memset(tiltlighting, 0xA5, sizeof tiltlighting);
			drawers[d].candidate();
			if (memcmp(frame_reference, frame_candidate, sizeof frame_reference) || ds_x1 != expected_x
				|| memcmp(expected_light, tiltlighting, sizeof tiltlighting))
			{
				printf("NPOT EE FAIL drawer=%s n=%d dim=%u\n", drawers[d].name, lengths[n], dimindices[b]);
				return 1;
			}
			for (rep = 0; rep < 3; rep++)
			for (order = 0; order < 2; order++)
			{
				unsigned variant = (order + rep) & 1;
				void (*draw)(void) = variant ? drawers[d].candidate : drawers[d].reference;
				UINT64 start, elapsed;
				parameters(320, 0, lengths[n], 12, dimindices[b]); bind_frame(frame_candidate, d, 12, 0);
				draw();
				start = now();
				for (i = 0; i < calls; i++) { ds_x1 = 0; draw(); }
#ifdef NEXT5_NPOT_EE
				elapsed = (UINT32)((UINT32)now() - (UINT32)start);
#else
				elapsed = now() - start;
#endif
				if (elapsed < best[variant]) best[variant] = elapsed;
			}
			printf("NPOTBENCH %s dim=%ux%u n=%d dense=%u calls=%u base=%llu cand=%llu unit=%s\n",
				drawers[d].name, dimensions[dimindices[b]][0], dimensions[dimindices[b]][1], lengths[n], dense, calls,
				(unsigned long long)(best[0] * 10 / calls), (unsigned long long)(best[1] * 10 / calls),
#ifdef NEXT5_NPOT_EE
				"cycles_x10"
#else
				"ns_x10"
#endif
			);
		}
	}
	puts("NPOT DONE");
	return 0;
}

int main(int argc, char **argv)
{
	initialize();
#ifdef NEXT5_NPOT_EE
	(void)argc; (void)argv;
	return benchmark();
#else
	if (argc > 1 && !strcmp(argv[1], "bench")) return benchmark();
	if (argc > 1 && !strncmp(argv[1], "zero-", 5))
	{
		void (*draw)(void) = strstr(argv[1], "candidate") ? drawers[0].candidate : drawers[0].reference;
		parameters(320, 0, 1, 0, 4); bind_frame(frame_candidate, 0, 0, 0);
		draw();
		if (strstr(argv[1], "width")) ds_flatwidth = 0;
		else ds_flatheight = 0;
		ds_x1 = 0;
		draw();
		puts("NPOT FAIL zero dimension did not call I_Error");
		return 1;
	}
	return equality(argc > 1 && !strcmp(argv[1], "broken-pixel"),
		argc > 1 && !strcmp(argv[1], "alias-only"));
#endif
}
