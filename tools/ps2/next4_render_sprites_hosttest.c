/* Complete frozen/current drawers are compiled in a separate translation unit,
 * without LTO or interprocedural specialization. Only the driver uses fixtures.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef NEXT4_SPRITES_EE
#include <time.h>
#endif
#include "next4_render_sprites_stub.h"
#define FB_HEIGHT 9
#define FB_GUARD 64
#define FB_BYTES (MAXVIDWIDTH * (FB_HEIGHT + 1) + 2 * FB_GUARD)
#define ALIAS_BYTES (65536 + FB_BYTES)
#ifdef NEXT4_SPRITES_EE
#define SOURCE_PIXELS (256 * 256)
#else
#define SOURCE_PIXELS (2048 * 2048)
#endif
next4_vid_t vid;
INT32 ds_y, ds_x1, ds_x2;
UINT8 *topleft, *screens[2];
UINT8 *ds_source, *ds_colormap, *ds_translation, *ds_transmap;
fixed_t ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
UINT32 nflatxshift, nflatyshift, nflatmask, nflatshiftup;

typedef struct { const char *name; void (*draw[4])(void); int sprite; } drawer_t;
#define DRAWER(n, s) { #n, { reference_##n, candidate_##n, hoists_##n, advance_##n }, s }
static const drawer_t drawers[] = {
	DRAWER(R_DrawSplat_8, 0), DRAWER(R_DrawTranslucentSplat_8, 0),
	DRAWER(R_DrawFloorSprite_8, 1), DRAWER(R_DrawTranslucentFloorSprite_8, 1),
};
#define NDRAWERS (sizeof drawers / sizeof drawers[0])
static UINT8 frame_reference[ALIAS_BYTES] __attribute__((aligned(16)));
static UINT8 frame_candidate[ALIAS_BYTES] __attribute__((aligned(16)));
static UINT8 source8[2][SOURCE_PIXELS];
static UINT16 source16[2][SOURCE_PIXELS];
static UINT8 cmaps[2][256], transmaps[2][65536], translations[2][256];
#ifndef NEXT4_SPRITES_EE
static UINT64 cases, compared_bytes, hash = UINT64_C(1469598103934665603);
#endif

static void initialize(void)
{
	size_t i, set;
	for (set = 0; set < 2; set++)
	{
		for (i = 0; i < sizeof cmaps[set]; i++) cmaps[set][i] = (UINT8)((i * 29 + set * 37) ^ (i >> 3));
		for (i = 0; i < sizeof transmaps[set]; i++) transmaps[set][i] = (UINT8)((i >> 8) * 23 + (i & 255) * 71 + set * 13);
		for (i = 0; i < sizeof translations[set]; i++) translations[set][i] = (UINT8)(i * 47 + 19 + set * 41);
		for (i = 0; i < SOURCE_PIXELS; i++)
		{
			source8[set][i] = i % 3 == 0 ? TRANSPARENTPIXEL : (UINT8)(i * 37 + i / 11 + set * 41);
			source16[set][i] = (UINT16)((i % 5 == 0 ? 0 : 0xFF00) | ((i * 41 + i / 13 + set * 17) & 255));
		}
	}
}

static void parameters(int fw, int first, int length, unsigned mode, unsigned bits)
{
	static const fixed_t starts[] = {INT32_MIN, INT32_MIN+1, -0x10001, -0x10000, -1,
		0, 1, 0xFFFF, 0x10000, 0x10001, 0x7FFEFFFF, INT32_MAX};
	static const fixed_t steps[] = {INT32_MIN, -0x70000000, -0x10001, -0x10000, -1,
		0, 1, 0xFFFF, 0x10000, 0x10001, 0x70000000, INT32_MAX};
	vid.width = vid.rowbytes = fw;
	vid.height = FB_HEIGHT;
	ds_y = 2 + (INT32)(mode % 5);
	ds_x1 = first; ds_x2 = first + length - 1;
	nflatshiftup = 16 - bits;
	nflatxshift = 16 + nflatshiftup; nflatyshift = nflatxshift - bits;
	nflatmask = ((1u << bits) - 1) << bits;
	ds_xfrac = starts[mode % (sizeof starts / sizeof starts[0])];
	ds_yfrac = starts[(mode / 3 + 5) % (sizeof starts / sizeof starts[0])];
	ds_xstep = steps[(mode / 12) % (sizeof steps / sizeof steps[0])];
	ds_ystep = steps[(mode / 7 + 5) % (sizeof steps / sizeof steps[0])];
}

/* Content aliases include actual writes into source/table bytes; no framebuffer
 * ever overlaps the storage of renderer pointer globals or dimension globals.
 */
static void bind_frame(UINT8 *frame, size_t d, unsigned mode, unsigned alias)
{
	const unsigned set = mode & 1;
	screens[0] = topleft = frame + FB_GUARD;
	screens[1] = NULL;
	ds_colormap = alias == 2 || alias == 5 ? topleft : cmaps[set];
	ds_source = alias == 1 || alias == 5 ? topleft : (alias == 6 ? topleft + 32 :
		(drawers[d].sprite ? (UINT8 *)source16[set] : source8[set]));
	ds_translation = alias == 3 || alias == 5 ? topleft : translations[set];
	ds_transmap = alias == 4 || alias == 5 ? frame : transmaps[set];
}

#ifndef NEXT4_SPRITES_EE
static int compare(size_t d, int fw, int first, int length, unsigned mode,
	unsigned bits, unsigned alias, unsigned edge, int broken)
{
	size_t i, bytes = alias ? ALIAS_BYTES : (size_t)fw * FB_HEIGHT + 2 * FB_GUARD + MAXVIDWIDTH;
	unsigned variant;
	for (i = 0; i < bytes; i++) frame_reference[i] = (UINT8)(i * 31 + (i >> 4) + mode);
	parameters(fw, first, length, mode, bits);
	if (alias) ds_y = 0;
	if (edge) ds_y = FB_HEIGHT - 1 + (INT32)(edge == 2);
	bind_frame(frame_reference, d, mode, alias);
	drawers[d].draw[0]();
	if (ds_x1 != first) return 1;
	for (variant = 1; variant < 4; variant++)
	{
		for (i = 0; i < bytes; i++) frame_candidate[i] = (UINT8)(i * 31 + (i >> 4) + mode);
		parameters(fw, first, length, mode, bits);
		if (alias) ds_y = 0;
		if (edge) ds_y = FB_HEIGHT - 1 + (INT32)(edge == 2);
		bind_frame(frame_candidate, d, mode, alias);
		drawers[d].draw[variant]();
		if (broken && variant == 1) topleft[ds_y * fw + first] ^= 1;
		if (memcmp(frame_reference, frame_candidate, bytes) || ds_x1 != first)
		{
			printf("SPRITES FAIL drawer=%s variant=%u size=%u frame=%d x=%d n=%d mode=%u alias=%u edge=%u\n",
				drawers[d].name, variant, 1u << bits, fw, first, length, mode, alias, edge);
			return 1;
		}
		compared_bytes += bytes;
	}
	cases++;
	for (i = 0; length > 0 && i < (size_t)length; i++)
		hash = (hash ^ topleft[ds_y * fw + first + i]) * UINT64_C(1099511628211);
	return 0;
}

static int equality(int broken)
{
	static const int widths[] = {1,7,16,31,319,320,512,640};
	static const int lengths[] = {0,1,2,3,7,8,9,15,16,17,31,32,33,63,64,65,100,127,319,320,321,512,639,640};
	size_t f, n, d;
	unsigned bits, mode, alias, edge;
	for (bits = 1; bits <= 11; bits++)
	for (f = 0; f < sizeof widths / sizeof widths[0]; f++)
	for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
	for (mode = 0; mode < 144; mode++)
	{
		const int length = lengths[n], fw = widths[f];
		int first;
		if (length > fw) continue;
		first = mode % 3 == 0 ? 0 : (mode % 3 == 1 ? fw - length : (fw - length) / 2);
		for (d = 0; d < NDRAWERS; d++)
			if (compare(d, fw, first, length, mode, bits, 0, 0, broken)) return 1;
	}
	for (alias = 1; alias <= 6; alias++)
	for (mode = 0; mode < 144; mode++)
	for (n = 0; n < 12; n++)
	for (d = 0; d < NDRAWERS; d++)
		if (compare(d, 320, (INT32)(mode % 3) * 61, lengths[n], mode, 4, alias, 0, broken)) return 1;
	/* Preserve the legacy inclusive remainder bound, even at deststop and when
	 * an eight-pixel group crosses it. Spare real storage keeps every access valid.
	 */
	for (edge = 1; edge <= 2; edge++)
	for (n = 0; n < 18; n++)
	for (d = 0; d < NDRAWERS; d++)
	for (mode = 0; mode < 12; mode++)
		if (compare(d, 31, edge == 1 ? 30 : 0, lengths[n], mode, 4, 0, edge, broken)) return 1;
	printf("SPRITES PASS cases=%llu compared_bytes=%llu hash=%016llx aliases=%u variants=3\n",
		(unsigned long long)cases, (unsigned long long)compared_bytes, (unsigned long long)hash, 6u);
	return 0;
}
#endif

static UINT64 now(void)
{
#ifdef NEXT4_SPRITES_EE
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
	static const unsigned bitcounts[] = {4,8};
	static const int lengths[] = {1,7,8,16,32,100,320};
	size_t d, b, n, i;
	unsigned dense, set;
	const unsigned calls = 1024;
	for (dense = 0; dense < 2; dense++)
	{
		for (set = 0; set < 2; set++)
		for (i = 0; i < SOURCE_PIXELS; i++)
		{
			source8[set][i] = dense || i % 3 ? (UINT8)((i * 37 + i / 11 + set * 41) % 255) : TRANSPARENTPIXEL;
			source16[set][i] = (UINT16)((dense || i % 5 ? 0xFF00 : 0) | ((i * 41 + i / 13 + set * 17) & 255));
		}
		for (d = 0; d < NDRAWERS; d++)
		for (b = 0; b < sizeof bitcounts / sizeof bitcounts[0]; b++)
		for (n = 0; n < sizeof lengths / sizeof lengths[0]; n++)
		{
			UINT64 best[4] = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
			unsigned order, rep, variant;
			memset(frame_reference, 0x73, sizeof frame_reference);
			parameters(320, 0, lengths[n], 109, bitcounts[b]); bind_frame(frame_reference, d, 109, 0);
			drawers[d].draw[0]();
			for (variant = 1; variant < 4; variant++)
			{
				memset(frame_candidate, 0x73, sizeof frame_candidate);
				parameters(320, 0, lengths[n], 109, bitcounts[b]); bind_frame(frame_candidate, d, 109, 0);
				drawers[d].draw[variant]();
				if (memcmp(frame_reference, frame_candidate, sizeof frame_reference) || ds_x1 != 0)
				{
					printf("SPRITES EE FAIL drawer=%s variant=%u n=%d bits=%u\n", drawers[d].name, variant, lengths[n], bitcounts[b]);
					return 1;
				}
			}
			for (rep = 0; rep < 3; rep++)
			for (order = 0; order < 4; order++)
			{
				void (*draw)(void);
				UINT64 start, elapsed;
				variant = (order + rep) % 4;
				draw = drawers[d].draw[variant];
				parameters(320, 0, lengths[n], 109, bitcounts[b]); bind_frame(frame_candidate, d, 109, 0);
				draw();
				start = now();
				for (i = 0; i < calls; i++) draw();
#ifdef NEXT4_SPRITES_EE
				elapsed = (UINT32)((UINT32)now() - (UINT32)start);
#else
				elapsed = now() - start;
#endif
				if (elapsed < best[variant]) best[variant] = elapsed;
			}
			printf("SPRITEBENCH %s size=%u n=%d dense=%u calls=%u base=%llu cand=%llu hoists=%llu advance=%llu unit=%s\n",
				drawers[d].name, 1u << bitcounts[b], lengths[n], dense, calls,
				(unsigned long long)(best[0] * 10 / calls), (unsigned long long)(best[1] * 10 / calls),
				(unsigned long long)(best[2] * 10 / calls), (unsigned long long)(best[3] * 10 / calls),
#ifdef NEXT4_SPRITES_EE
				"cycles_x10"
#else
				"ns_x10"
#endif
			);
		}
	}
	puts("SPRITES DONE");
	return 0;
}

int main(int argc, char **argv)
{
	initialize();
#ifdef NEXT4_SPRITES_EE
	(void)argc; (void)argv;
	return benchmark();
#else
	if (argc > 1 && !strcmp(argv[1], "bench")) return benchmark();
	return equality(argc > 1 && !strcmp(argv[1], "broken-pixel"));
#endif
}
