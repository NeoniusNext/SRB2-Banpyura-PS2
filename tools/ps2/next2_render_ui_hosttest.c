/* Full cropped-patch source, with real extracted flags/structs/helpers. Compare
 * all framebuffer/canary bytes and persistent lookup/hook/blend-table state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_globals.h"

viddef_t vid;
INT32 rendermode = render_soft, splitscreen, displayplayer, st_translucency;
INT32 ps2mh_recording;
testplayer_t players[4], *stplyr = players;
UINT8 *screens[5];
#define MAX_WIDTH 657
#define MAX_HEIGHT 512
#define FRAME_SEGMENT (MAX_WIDTH * MAX_HEIGHT)
#define FRAME_BYTES (3 * FRAME_SEGMENT + 128)
#define FRAME_START (FRAME_SEGMENT + 64)
#define PATCH_WIDTH 257
#define PATCH_HEIGHT 1024
static UINT8 frame[2][FRAME_BYTES], pattern[FRAME_BYTES];
static UINT8 pixels[PATCH_WIDTH * PATCH_HEIGHT], cmaps[2][256], blends[2][65536];
static post_t posts[PATCH_WIDTH][4];
static column_t columns[PATCH_WIDTH];
static patch_t patch;
static unsigned table_calls, menu_calls, hardware_calls;
static int last_style, last_alpha, menu_skip;
static UINT32 rng = 0x10293847;
static UINT32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

UINT8 *R_GetBlendTable(int style, INT32 alpha)
{
	table_calls++; last_style = style; last_alpha = alpha;
	return blends[(style + alpha) & 1];
}
boolean PS2MenuHints_NoteCropped(fixed_t x, fixed_t y, fixed_t hs, fixed_t vs, INT32 flags, const patch_t *p, fixed_t w, fixed_t h)
{
	(void)x; (void)y; (void)hs; (void)vs; (void)flags; (void)p; (void)w; (void)h;
	menu_calls++;
	return menu_skip;
}
void HWR_DrawCroppedPatch(patch_t *p, fixed_t x, fixed_t y, fixed_t hs, fixed_t vs, INT32 flags, const UINT8 *cm, fixed_t sx, fixed_t sy, fixed_t w, fixed_t h)
{
	(void)p; (void)x; (void)y; (void)hs; (void)vs; (void)flags; (void)cm; (void)sx; (void)sy; (void)w; (void)h;
	hardware_calls++;
}

typedef struct { fixed_t x, y, hs, vs, sx, sy, w, h; INT32 flags; const UINT8 *cm; } parameters_t;
static const parameters_t *active;
static void invoke(unsigned variant)
{
	if (variant) candidate_V_DrawCroppedPatch(active->x, active->y, active->hs, active->vs, active->flags,
		&patch, active->cm, active->sx, active->sy, active->w, active->h);
	else reference_V_DrawCroppedPatch(active->x, active->y, active->hs, active->vs, active->flags,
		&patch, active->cm, active->sx, active->sy, active->w, active->h);
}

static void initialize(void)
{
	size_t i;
	for (i = 0; i < sizeof pattern; i++) pattern[i] = (UINT8)rnd();
	for (i = 0; i < sizeof pixels; i++) pixels[i] = (UINT8)rnd();
	for (i = 0; i < sizeof cmaps; i++) ((UINT8 *)cmaps)[i] = (UINT8)rnd();
	for (i = 0; i < sizeof blends; i++) ((UINT8 *)blends)[i] = (UINT8)rnd();
	patch.columns = columns; patch.posts = (post_t *)posts; patch.pixels = pixels;
	for (i = 0; i < PATCH_WIDTH; i++)
	{
		columns[i].pixels = pixels + i * PATCH_HEIGHT;
		columns[i].posts = posts[i];
	}
}

static void configure_patch(int width, int height, unsigned style)
{
	int i;
	patch.width = (INT16)width; patch.height = (INT16)height;
	patch.leftoffset = (INT16)((style & 1) ? 3 : -2);
	patch.topoffset = (INT16)((style & 2) ? 5 : -3);
	for (i = 0; i < width; i++)
	{
		unsigned p;
		columns[i].num_posts = style % 3 == 0 ? 1 : ((unsigned)i % 5 == 0 ? 0 : 4);
		for (p = 0; p < columns[i].num_posts; p++)
		{
			posts[i][p].topdelta = columns[i].num_posts == 1 ? 0 : p * ((unsigned)height / 4);
			posts[i][p].length = columns[i].num_posts == 1 ? (unsigned)height : ((unsigned)i + p) % ((unsigned)height / 4 + 1);
			posts[i][p].data_offset = posts[i][p].topdelta;
		}
	}
}

#ifndef NEXT2_UI_EE
static UINT64 cases, compared, hash = 1469598103934665603ULL;
static int broken_pixel;
static int compare(const parameters_t *p)
{
	unsigned variant;
	unsigned calls[2][3];
	int style[2], alpha[2];
	active = p;
	for (variant = 0; variant < 2; variant++)
	{
		unsigned s;
		memcpy(frame[variant], pattern, FRAME_BYTES);
		for (s = 0; s < 5; s++) screens[s] = frame[variant] + FRAME_START;
		if (p->flags & 0x800) screens[p->flags & V_PARAMMASK] = NULL;
		table_calls = menu_calls = hardware_calls = 0;
		last_style = last_alpha = -1;
		invoke(variant);
		calls[variant][0] = table_calls; calls[variant][1] = menu_calls; calls[variant][2] = hardware_calls;
		style[variant] = last_style; alpha[variant] = last_alpha;
	}
	if (broken_pixel) frame[1][FRAME_START] ^= 1;
	if (memcmp(frame[0], frame[1], FRAME_BYTES) || memcmp(calls[0], calls[1], sizeof calls[0])
		|| style[0] != style[1] || alpha[0] != alpha[1]
		|| reference_ui_cmap() != candidate_ui_cmap() || reference_ui_trans() != candidate_ui_trans())
	{
		printf("UI FAIL case=%llu mode=%d split=%d flags=%08x patch=%dx%d x=%d y=%d scale=%d/%d crop=%d,%d,%d,%d\n",
			(unsigned long long)cases, rendermode, splitscreen, (unsigned)p->flags, patch.width, patch.height,
			p->x, p->y, p->hs, p->vs, p->sx, p->sy, p->w, p->h);
		return 1;
	}
	cases++; compared += FRAME_BYTES;
	hash = (hash ^ frame[1][FRAME_START + (cases * 7919 % ((size_t)vid.rowbytes * vid.height))]) * 1099511628211ULL;
	return 0;
}

static int equality(void)
{
	static const INT32 widths[] = {1, 7, 319, 320, 512, 640};
	static const INT32 heights[] = {1, 7, 199, 200, 400, 512};
	static const fixed_t scales[] = {FRACUNIT/16, FRACUNIT/4, FRACUNIT/2, FRACUNIT-1, FRACUNIT, FRACUNIT+1, 3*FRACUNIT/2, 2*FRACUNIT, 3*FRACUNIT};
	static const INT32 flags[] = {0, V_NOSCALESTART, V_NOSCALEPATCH, V_SMALLSCALEPATCH, V_MEDSCALEPATCH,
		V_SNAPTOTOP|V_SNAPTOLEFT, V_SNAPTOBOTTOM|V_SNAPTORIGHT, V_FLIP,
		V_PERPLAYER, V_PERPLAYER|V_NOSCALESTART, V_PERPLAYER|V_SNAPTOBOTTOM|V_SNAPTOLEFT};
	unsigned i;
	for (i = 0; i < 24000; i++)
	{
		parameters_t p;
		const unsigned pw = 1 + rnd() % PATCH_WIDTH, ph = 1 + rnd() % PATCH_HEIGHT;
		configure_patch((int)pw, (int)ph, i % 6);
		vid.width = widths[rnd() % (sizeof widths / sizeof widths[0])];
		vid.height = heights[rnd() % (sizeof heights / sizeof heights[0])];
		vid.rowbytes = (size_t)vid.width + (i % 3 == 0 ? 17 : 0);
		vid.dup = 1 + (INT32)(i % 3); vid.smalldup = 1; vid.meddup = 2;
		p.x = ((INT32)(rnd() % 500) - 100) * FRACUNIT + (INT32)(rnd() & 65535);
		p.y = ((INT32)(rnd() % 500) - 100) * FRACUNIT + (INT32)(rnd() & 65535);
		p.hs = scales[rnd() % (sizeof scales / sizeof scales[0])];
		p.vs = scales[rnd() % (sizeof scales / sizeof scales[0])];
		p.sx = (fixed_t)(rnd() % (pw * FRACUNIT + 1));
		p.sy = (fixed_t)(rnd() % (ph * FRACUNIT + 1));
		p.w = (i % 7 == 0) ? 0 : (fixed_t)(rnd() % ((pw + 1) * FRACUNIT));
		p.h = (i % 9 == 0) ? -1 : (fixed_t)(rnd() % ((ph + 1) * FRACUNIT));
		p.flags = (INT32)((UINT32)flags[i % (sizeof flags / sizeof flags[0])] | ((i % 16) << V_ALPHASHIFT)
			| (((i / 16) % 8) << V_BLENDSHIFT) | (i % 5));
		if (i % 100 == 0) p.flags |= 0x800; /* null screen; unused spacing bit */
		p.cm = i & 1 ? cmaps[(i >> 1) & 1] : NULL;
		splitscreen = (INT32)(i & 1); stplyr = players + ((i >> 1) & 1);
		st_translucency = (INT32)(i % 11);
		rendermode = i % 113 == 0 ? render_none : (i % 127 == 0 ? render_opengl : render_soft);
		ps2mh_recording = i % 31 == 0; menu_skip = i % 62 == 0;
		if (compare(&p)) return 1;
	}
	/* Dense, visible coverage exercises every lookup and alpha/HUD remap on
	 * both player boundaries rather than relying on randomly clipped draws. */
	for (i = 0; i < 2048; i++)
	{
		parameters_t p;
		configure_patch(63, 241, i % 6);
		vid.width = vid.rowbytes = 320; vid.height = 200; vid.dup = vid.smalldup = vid.meddup = 1;
		p.x = 100*FRACUNIT; p.y = ((INT32)(i % 4) - 1)*50*FRACUNIT;
		p.hs = p.vs = scales[i % (sizeof scales / sizeof scales[0])];
		p.sx = p.sy = (i & 1) ? 0 : FRACUNIT/2;
		p.w = 63*FRACUNIT; p.h = 241*FRACUNIT;
		p.flags = (INT32)(((i % 16) << V_ALPHASHIFT) | (((i / 16) % 8) << V_BLENDSHIFT));
		if (i & 256) p.flags |= V_PERPLAYER;
		p.cm = i & 128 ? cmaps[(i >> 9) & 1] : NULL;
		splitscreen = 1; stplyr = players + ((i >> 10) & 1); st_translucency = (INT32)(i % 11);
		rendermode = render_soft; ps2mh_recording = 0;
		if (compare(&p)) return 1;
	}
	printf("UI PASS cases=%llu compared_bytes=%llu hash=%016llx\n",
		(unsigned long long)cases, (unsigned long long)compared, (unsigned long long)hash);
	return 0;
}
#endif

#ifdef NEXT2_UI_EE
static UINT32 now(void) { UINT32 v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }
static int benchmark(void)
{
	static const INT32 sizes[][2] = {{8,8}, {32,64}, {63,127}, {160,100}};
	unsigned n, kind, sparse;
	for (n = 0; n < sizeof sizes / sizeof sizes[0]; n++)
	for (kind = 0; kind < 4; kind++)
	for (sparse = 0; sparse < 2; sparse++)
	{
		parameters_t p;
		UINT32 best[2] = {0xFFFFFFFFu,0xFFFFFFFFu};
		unsigned variant, rep, i;
		const unsigned calls = 32;
		configure_patch(sizes[n][0], sizes[n][1], sparse);
		vid.width = vid.rowbytes = 320; vid.height = 200; vid.dup = vid.smalldup = vid.meddup = 1;
		p.x = 80*FRACUNIT; p.y = 30*FRACUNIT; p.hs = p.vs = FRACUNIT;
		p.sx = p.sy = FRACUNIT/2; p.w = sizes[n][0]*FRACUNIT; p.h = sizes[n][1]*FRACUNIT;
		p.flags = kind >= 2 ? 5 << V_ALPHASHIFT : 0;
		p.cm = kind & 1 ? cmaps[0] : NULL;
		active = &p;
		for (variant = 0; variant < 2; variant++)
		{
			memcpy(frame[variant], pattern, FRAME_BYTES);
			screens[0] = frame[variant] + FRAME_START;
			invoke(variant);
		}
		if (memcmp(frame[0], frame[1], FRAME_BYTES)) { puts("UI EE PIXEL FAIL"); return 1; }
		for (rep = 0; rep < 5; rep++)
		for (variant = 0; variant < 2; variant++)
		{
			UINT32 start, elapsed;
			screens[0] = frame[variant] + FRAME_START;
			invoke(variant);
			start = now();
			for (i = 0; i < calls; i++) invoke(variant);
			elapsed = now() - start;
			if (elapsed < best[variant]) best[variant] = elapsed;
		}
		printf("UIBENCH dim=%dx%d kind=%u sparse=%u calls=%u base=%llu cand=%llu unit=cycles_x10\n",
			sizes[n][0], sizes[n][1], kind, sparse, calls,
			(unsigned long long)best[0]*10/calls, (unsigned long long)best[1]*10/calls);
	}
	puts("UI DONE");
	return 0;
}
#endif

int main(int argc, char **argv)
{
	initialize();
#ifdef NEXT2_UI_EE
	(void)argc; (void)argv;
	return benchmark();
#else
	broken_pixel = argc > 1 && !strcmp(argv[1], "broken-pixel");
	return equality();
#endif
}
