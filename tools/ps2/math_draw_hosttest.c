/* Actual tilted drawer bodies are extracted by math_draw_hosttest.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m_fixed.h"
static void I_Error(const char *message, ...) { fprintf(stderr, "%s\n", message); exit(2); }
#include "libdivide.h"

#define MAXVIDWIDTH 320
#define MAXLIGHTSCALE 48
#define BASEVIDWIDTH 320
#define TRANSPARENTPIXEL 255
#define SPANSIZE 16
#define INVSPAN 0.0625f

#ifdef PS2_OPT_SLOPE
typedef float slopereal_t;
#define SLOPE_U32(x) R_SlopeToU32(x)
#else
typedef double slopereal_t;
#define SLOPE_U32(x) ((UINT32)(INT64)(x))
#endif
typedef struct { slopereal_t x, y, z; } vec_t;
static vec_t ds_su, ds_sv, ds_sz, ds_slopelight;
static double zeroheight = 16.0;
static float ds_lightscale;
static fixed_t fovtan = FRACUNIT;
static struct { int width; } vid = {320};
static int centerx = 160, centery = 100;
static int ds_x1, ds_x2, ds_y, ds_bgofs;
static int nflatxshift = 26, nflatyshift = 20;
static UINT32 nflatmask = 4032;
static UINT32 ds_flatwidth = 63, ds_flatheight = 47;
static UINT8 framebuffer[64000], background[64000], flat[4096], maps[48 * 256], trans[65536], translation[256];
static UINT16 sprite[4096];
static UINT8 *screens[] = {framebuffer, background};
static UINT8 *topleft = framebuffer, *ds_source, *colormaps = maps, *ds_colormap = maps;
static UINT8 *ds_transmap = trans, *ds_translation = translation, *planezlight[48];

/* Original drawers intentionally narrow coordinates and slope lighting. */
#pragma warning(push)
#pragma warning(disable: 4244)
#include "drawers.inc"
#pragma warning(pop)

static UINT32 state = 12345;
static UINT32 random32(void) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
static double signed_value(void) { return ((double)random32() - 2147483648.0) / 65536.0; }

int main(int argc, char **argv)
{
	static const int widths[] = {1, 2, 15, 16, 17, 31, 32, 33, 319, 320};
	if (argc != 3) return 2;
	const int cases = atoi(argv[2]);
	FILE *file = fopen(argv[1], "wb");
	if (!file) return 2;
	for (int i = 0; i < 4096; i++)
	{
		flat[i] = (UINT8)(random32() >> 24);
		sprite[i] = (UINT16)(flat[i] | ((i % 7) ? 0xFF00 : 0));
	}
	for (int i = 0; i < 48 * 256; i++) maps[i] = (UINT8)((i * 13 + i / 256) & 255);
	for (int i = 0; i < 65536; i++) trans[i] = (UINT8)(((i >> 8) * 3 + (i & 255) * 5) / 8);
	for (int i = 0; i < 256; i++) translation[i] = (UINT8)(255 - i);
	for (int i = 0; i < 64000; i++) background[i] = (UINT8)(i * 17);
	for (int i = 0; i < 48; i++) planezlight[i] = maps + i * 256;
	ds_lightscale = (float)(320.0 / zeroheight / 21.0);
	for (int f = 0; f < 14; f++)
		for (int c = 0; c < cases; c++)
		{
			const int width = c < 10 ? widths[c] : 1 + (int)(random32() % 320);
			ds_x1 = (int)(random32() % (321 - width));
			const int startx = ds_x1;
			ds_x2 = ds_x1 + width - 1;
			ds_y = 50 + (int)(random32() % 100);
			ds_bgofs = (int)(random32() % 9) - 4;
			const double mult = f < 7 ? 1024.0 : 1.0;
			ds_su.x = (slopereal_t)(signed_value() * mult);
			ds_su.y = (slopereal_t)(signed_value() * mult);
			ds_su.z = (slopereal_t)(signed_value() * 65536.0 * mult);
			ds_sv.x = (slopereal_t)(signed_value() * mult);
			ds_sv.y = (slopereal_t)(signed_value() * mult);
			ds_sv.z = (slopereal_t)(signed_value() * 65536.0 * mult);
			ds_sz.x = (slopereal_t)0.0007;
			ds_sz.y = (slopereal_t)0.0004;
			ds_sz.z = (slopereal_t)2.0;
			ds_slopelight.x = (slopereal_t)0.001;
			ds_slopelight.y = (slopereal_t)0.0002;
			ds_slopelight.z = (slopereal_t)10.0;
			ds_source = is_sprite[f] ? (UINT8 *)sprite : flat;
			UINT8 *row = framebuffer + ds_y * 320;
			memset(row, 0xCC, 320);
			drawers[f]();
			for (int i = 0; i < 320; i++)
				if ((i < startx || i > ds_x2) && row[i] != 0xCC) { fclose(file); return 3; }
			if (fwrite(row, 1, 320, file) != 320) { fclose(file); return 2; }
		}
	if (fclose(file)) return 2;
	printf("drawers=14 cases_per_drawer=%d rows=%d bytes=%llu\n", cases, cases * 14, (unsigned long long)cases * 14 * 320);
	return 0;
}
