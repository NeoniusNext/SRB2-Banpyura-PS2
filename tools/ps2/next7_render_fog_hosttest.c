/* Complete actual frozen/current drawers are in a separate external-global TU. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifndef NEXT7_FOG_EE
#include <time.h>
#endif
#include "next7_render_fog_stub.h"
#define FB_GUARD 32
#define FB_HEIGHT 9
#define FB_MAX ((MAXVIDWIDTH + 17) * (MAXVIDWIDTH + 2) + 2 * FB_GUARD + 65536)
next7_vid_t vid;
INT32 centerx, centery, ds_y, ds_x1, ds_x2, ds_bgofs;
INT32 dc_x, dc_yl, dc_yh;
UINT8 *topleft, *screens[5], *colormaps;
lighttable_t **planezlight;
UINT8 *ds_source, *ds_colormap, *ds_transmap, *dc_colormap;
next7_vector_t ds_slopelight;
#ifdef PS2_OPT_SLOPE
float ds_lightscale;
#else
double zeroheight = 16;
fixed_t fovtan = FRACUNIT;
#endif

typedef struct { const char *name; void (*reference)(void), (*candidate)(void); int column, tilted, bounded; } drawer_t;
#define DRAWER(name, column, tilted, bounded) { #name, reference_##name, candidate_##name, column, tilted, bounded }
static const drawer_t drawers[] = {
	DRAWER(R_DrawTiltedFogSpan_8, 0, 1, 0),
	DRAWER(R_DrawTransSolidColorSpan_8, 0, 0, 1),
	DRAWER(R_DrawTiltedSolidColorSpan_8, 0, 1, 0),
	DRAWER(R_DrawTiltedTransSolidColorSpan_8, 0, 1, 0),
	DRAWER(R_DrawWaterSolidColorSpan_8, 0, 0, 1),
	DRAWER(R_DrawTiltedWaterSolidColorSpan_8, 0, 1, 0),
	DRAWER(R_DrawFogColumn_8, 1, 0, 0),
};
#define NDRAWERS (sizeof drawers / sizeof drawers[0])
static UINT8 frame_reference[FB_MAX] __attribute__((aligned(16)));
static UINT8 frame_candidate[FB_MAX] __attribute__((aligned(16)));
static UINT8 background[FB_MAX], source[2][256];
static UINT8 cmaps[2][MAXLIGHTSCALE * 256 + 256], transmaps[2][65536];
static lighttable_t *lightptrs[2][MAXLIGHTSCALE];
static UINT64 cases, compared_bytes, hash = UINT64_C(1469598103934665603);

static void initialize(void)
{
	size_t i, set;
	for (set = 0; set < 2; set++)
	{
		for (i = 0; i < sizeof cmaps[set]; i++) cmaps[set][i] = (UINT8)((i * 29 + i / 256 * 17 + set * 37) ^ (i >> 3));
		for (i = 0; i < sizeof transmaps[set]; i++) transmaps[set][i] = (UINT8)((i >> 8) * 23 + (i & 255) * 71 + set * 13);
		for (i = 0; i < sizeof source[set]; i++) source[set][i] = (UINT8)(i * 37 + set * 91);
	}
	for (i = 0; i < sizeof background; i++) background[i] = (UINT8)(i * 53 + (i >> 7));
}

static void parameters(size_t d, int fw, int first, int length, unsigned mode, int boundary)
{
	static const float lightstarts[] = {-32760, -10, -1.25f, -0.25f, 0, 0.5f, 1, 24, 47, 47.5f, 48, 70, 32760};
	static const float lightsteps[] = {-32, -0.125f, -1.0f / 65536, 0, 1.0f / 65536, 0.125f, 32};
	vid.width = fw; vid.rowbytes = fw + (int)(mode % 3) * 7;
	vid.height = drawers[d].column ? MAXVIDWIDTH + 2 : FB_HEIGHT;
	centerx = fw / 2; centery = vid.height / 2;
	ds_y = boundary ? vid.height - 1 : 2 + (INT32)(mode % 5);
	ds_x1 = first; ds_x2 = first + length - 1;
	ds_bgofs = boundary ? -1 : (INT32)(mode % 3) - 1;
	dc_x = (INT32)(mode % (unsigned)fw); dc_yl = first; dc_yh = first + length - 1;
	ds_slopelight.x = lightsteps[mode % (sizeof lightsteps / sizeof lightsteps[0])];
	ds_slopelight.y = 0.25f; ds_slopelight.z = lightstarts[mode % (sizeof lightstarts / sizeof lightstarts[0])];
#ifdef PS2_OPT_SLOPE
	ds_lightscale = mode & 4 ? 0.5f : 1;
#endif
}

/* Aliases cover data bytes; the storage of renderer globals remains separate.
 * In particular, all lookup contents remain live reads after pixel stores.
 */
static void bind_frame(UINT8 *frame, unsigned mode, unsigned alias)
{
	size_t i;
	const unsigned set = mode & 1;
	screens[0] = topleft = frame + FB_GUARD;
	screens[1] = alias == 4 || alias == 5 ? topleft :
	             (alias == 6 ? topleft + 1 : (alias == 7 ? topleft - 1 : background + FB_GUARD));
	colormaps = alias == 1 || alias == 5 ? topleft : cmaps[set];
	ds_colormap = colormaps + (mode % 2) * 256;
	dc_colormap = colormaps + ((mode / 2) % MAXLIGHTSCALE) * 256;
	planezlight = lightptrs[set];
	for (i = 0; i < MAXLIGHTSCALE; i++) planezlight[i] = colormaps + i * 256;
	ds_source = alias == 2 || alias == 5 ? topleft + ds_y * vid.width + ds_x1 : source[set] + mode % 256;
	ds_transmap = alias == 3 || alias == 5 ? frame : transmaps[set];
}

static void initialize_lighting(unsigned mode)
{
	unsigned i;
	for (i = 0; i < MAXVIDWIDTH; i++) tiltlighting[i] = (INT32)((i * 13 + mode) % MAXLIGHTSCALE);
}

static int compare(size_t d, int fw, int first, int length, unsigned mode, unsigned alias, int boundary, int broken)
{
	size_t i, bytes;
	INT32 expected_x, expected_light[MAXVIDWIDTH];
	parameters(d, fw, first, length, mode, boundary);
	bytes = (size_t)vid.rowbytes * vid.height + 2 * FB_GUARD + (alias ? 65536 : 0);
	for (i = 0; i < bytes; i++) frame_reference[i] = (UINT8)(i * 31 + (i >> 4) + mode);
	memcpy(frame_candidate, frame_reference, bytes);
	bind_frame(frame_reference, mode, alias); initialize_lighting(mode);
	drawers[d].reference(); expected_x = ds_x1;
	memcpy(expected_light, tiltlighting, sizeof tiltlighting);
	parameters(d, fw, first, length, mode, boundary);
	bind_frame(frame_candidate, mode, alias); initialize_lighting(mode);
	drawers[d].candidate();
	if (broken) topleft[drawers[d].column ? dc_yl * fw + dc_x : ds_y * fw + first] ^= 1;
	if (memcmp(frame_reference, frame_candidate, bytes) || ds_x1 != expected_x
		|| memcmp(expected_light, tiltlighting, sizeof tiltlighting))
	{
		printf("FOG FAIL drawer=%s frame=%d x=%d n=%d mode=%u alias=%u boundary=%d x1=%d/%d\n",
		       drawers[d].name, fw, first, length, mode, alias, boundary, expected_x, ds_x1);
		return 1;
	}
	cases++; compared_bytes += bytes;
	for (i = 0; i < 32; i++) hash = (hash ^ frame_candidate[(bytes / 33) * i]) * UINT64_C(1099511628211);
	return 0;
}

#ifndef NEXT7_FOG_EE
static int equality(int broken, int aliases_only)
{
	static const int widths[] = {1,7,16,31,319,320,512,640};
	static const int lengths[] = {-17,-1,0,1,2,3,7,8,15,16,17,31,32,33,63,127,319,320,321,512,639,640};
	size_t f, n, d;
	unsigned mode, alias;
	if (!aliases_only)
	for (f = 0; f < sizeof widths / sizeof widths[0]; f++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (mode = 0; mode < 39; mode++)
	for (d = 0; d < NDRAWERS; d++)
	{
		const int length = lengths[n], fw = widths[f];
		const int extent = drawers[d].column ? MAXVIDWIDTH : fw;
		int first;
		if (length > extent || (length == 0 && drawers[d].tilted)) continue; /* Original helper divides by zero for a zero-length tilted call. */
		first = length < 1 || mode % 3 == 0 ? 0 : (mode % 3 == 1 ? extent - length : (extent - length) / 2);
		if (compare(d, fw, first, length, mode, 0, 0, broken)) return 1;
	}
	for (alias = 1; alias <= 7; alias++)
	for (mode = 0; mode < 39; mode++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (d = 0; d < NDRAWERS; d++)
	{
		const int length = lengths[n];
		if (length > 320 || (length == 0 && drawers[d].tilted)) continue;
		if (compare(d, 320, (length <= 0 ? 0 : (INT32)(mode % 3) * ((320 - length) / 2)),
		            length, mode, alias, 0, broken)) return 1;
	}
	/* The two ordinary drawers retain their original inclusive end condition,
	 * even for reversed counts and spans extending beyond the final row. */
	for (d = 0; d < NDRAWERS; d++) if (drawers[d].bounded)
	for (mode = 0; mode < 12; mode++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (alias = 0; alias <= 7; alias++)
		if (compare(d, 320, 318, lengths[n], mode, alias, 1, broken)) return 1;
	printf("FOG PASS cases=%llu compared_bytes=%llu hash=%016llx aliases=7\n",
	       (unsigned long long)cases, (unsigned long long)compared_bytes, (unsigned long long)hash);
	return 0;
}
#endif

static UINT64 now(void)
{
#ifdef NEXT7_FOG_EE
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
	static const int lengths[] = {1,2,7,8,16,32,100,320};
	size_t d, n, i;
	unsigned mode;
	const unsigned calls = 2048;
	for (d = 0; d < NDRAWERS; d++)
	for (mode = 0; mode < 3; mode++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	{
		UINT64 best[2] = {UINT64_MAX, UINT64_MAX};
		unsigned order, rep;
		if (compare(d, 320, 0, lengths[n], mode + 6, 0, 0, 0)) return 1;
		for (rep = 0; rep < 5; rep++)
		for (order = 0; order < 2; order++)
		{
			const unsigned variant = (order + rep) & 1;
			void (*draw)(void) = variant ? drawers[d].candidate : drawers[d].reference;
			UINT64 start, elapsed;
			parameters(d, 320, 0, lengths[n], mode + 6, 0);
			bind_frame(frame_candidate, mode + 6, 0); initialize_lighting(mode + 6);
			draw();
			start = now();
			for (i = 0; i < calls; i++) { ds_x1 = 0; draw(); }
#ifdef NEXT7_FOG_EE
			elapsed = (UINT32)((UINT32)now() - (UINT32)start);
#else
			elapsed = now() - start;
#endif
			if (elapsed < best[variant]) best[variant] = elapsed;
		}
		printf("FOGBENCH drawer=%s n=%d mode=%u calls=%u before=%llu after=%llu\n",
		       drawers[d].name, lengths[n], mode, calls, (unsigned long long)best[0], (unsigned long long)best[1]);
		fflush(stdout);
	}
	puts("FOG DONE");
	return 0;
}

int main(int argc, char **argv)
{
	initialize();
#ifdef NEXT7_FOG_EE
	(void)argc; (void)argv;
	return benchmark();
#else
	if (argc > 1 && !strcmp(argv[1], "bench")) return benchmark();
	return equality(argc > 1 && !strcmp(argv[1], "broken-pixel"), argc > 1 && !strcmp(argv[1], "aliases"));
#endif
}
