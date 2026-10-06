/* Hardware test for the PS2 video layer (src/ps2/i_video.c + src/ps2/ps2_gs.c + src/ps2/ps2_vmodes.h).
 * Links the real files plus minimal stand-ins for the engine symbols they use; no game logic here.
 * Everything is checked by reading VRAM back through the GS (local -> host) and comparing with the expected pixels.
 *   args (PCSX2 -gameargs; the leading dash is optional): ntsc | pal | 480p | 720p | ...   output format of the first part
 *         noswap                   run with the CLUT bit 3/4 swap disabled: the test must turn red
 *         frames=N                 length of the paced soak run (default 700)
 *         matrix=0                 skip the output format x internal mode matrix
 *         only=A,B                 run the matrix for those output ids only
 *         dump=1                   write the picture of every matrix cell to host: (video-<out>-<w>x<h>-<fit>.bin)
 */
#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "s_sound.h"
#include "i_video.h"
#include "console.h"
#include "command.h"
#include "netcode/d_netcmd.h"
#include "netcode/tic_command.h"
#include "ps2_gs.h"

#include <tamtypes.h>
#include <kernel.h>
#include <screenshot.h>
#include <rom0_info.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stand-ins for engine symbols used by i_video.c ---- */
viddef_t vid;
UINT8 *screens[5];
INT32 setmodeneeded;
UINT8 setrenderneeded;
boolean dedicated;
boolean netgame;
INT32 consoleplayer;
INT32 serverplayer;
tic_t simulated_lag;
marathonmode_t marathonmode;
boolean WipeInAction;
gamestate_t gamestate;
char *I_GetEnv(const char *name) { (void)name; return "host:"; }
void I_Quit(void) { for (;;) SleepThread(); }
void D_PostEvent(const event_t *event) { (void)event; }
CV_PossibleValue_t CV_OnOff[] = {{0, "Off"}, {1, "On"}, {0, NULL}};
consvar_t cv_ticrate, cv_showping, cv_closedcaptioning;

INT32 myargc;
char **myargv;
static int g_argc;
static char **g_argv;
static int overlay_calls, regs, drawfuncs, fps_calls, cons_alerts, commands;

INT32 M_CheckParm(const char *check)
{
	int i;
	for (i = 0; i < g_argc; i++) /* PCSX2 -gameargs: the first token arrives as argv[0] */
		if (!strcmp(g_argv[i], check) || !strcmp(g_argv[i], check + (check[0] == '-')) || !strcmp(g_argv[i], check + 2 * (check[0] == '-' && check[1] == '-')))
			return i + 1;
	return 0;
}
boolean M_IsNextParm(void) { return false; }
const char *M_GetNextParm(void) { return ""; }
void I_Error(const char *error, ...)
{
	va_list ap;
	printf("V0 I_Error: ");
	va_start(ap, error);
	vprintf(error, ap);
	va_end(ap);
	printf("\n");
	for (;;)
		SleepThread();
}
void CONS_Printf(const char *fmt, ...)
{
	va_list ap;
	printf("V0 con: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
void CONS_Alert(alerttype_t level, const char *fmt, ...)
{
	va_list ap;
	(void)level;
	cons_alerts++;
	printf("V0 alert: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
void CV_RegisterVar(consvar_t *variable) { (void)variable; regs++; }
void CV_StealthSetValue(consvar_t *var, INT32 value) { var->value = value; }
void CV_Set(consvar_t *var, const char *value) { (void)var; (void)value; }
void COM_AddCommand(const char *name, com_func_t func, com_flags_t flags) { (void)name; (void)func; (void)flags; commands++; }
size_t COM_Argc(void) { return 0; }
const char *COM_Argv(size_t arg) { (void)arg; return ""; }
boolean Playing(void) { return false; }
void SCR_SetDrawFuncs(void) { drawfuncs++; }
void SCR_CalculateFPS(void) { fps_calls++; }
void SCR_ClosedCaptions(void) { overlay_calls++; }
void SCR_DisplayTicRate(void) { overlay_calls++; }
void SCR_DisplayLocalPing(void) { overlay_calls++; }
void SCR_DisplayMarathonInfo(void) { overlay_calls++; }
void VID_BlitLinearScreen(const UINT8 *srcptr, UINT8 *destptr, INT32 width, INT32 height, size_t srcrowbytes, size_t destrowbytes)
{
	INT32 y;
	for (y = 0; y < height; y++)
		memcpy(destptr + y * destrowbytes, srcptr + y * srcrowbytes, width);
}

/* ---- helpers ---- */
static u32 count(void)
{
	u32 v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}
static unsigned vbl(void)
{
	ps2gs_stats_t s;
	ps2gs_get_stats(&s);
	return s.vblanks;
}

static int failures;
#define CHECK(name, cond, ...) do { int ok_ = (cond) ? 1 : 0; failures += !ok_; printf("V0 %s %s ", ok_ ? "PASS" : "FAIL", name); printf(__VA_ARGS__); printf("\n"); } while (0)

static UINT32 palA[256], palB[256];
static RGBA_t rgbaA[256], rgbaB[256];
static u32 *readback;
static UINT8 *fbuf;

static void make_palettes(void)
{
	unsigned i;
	for (i = 0; i < 256; i++)
	{
		/* both are bijections per channel: every index has its own colour, so a wrong index can not hide */
		palA[i] = i | ((i ^ 0x55u) << 8) | ((255u - i) << 16);
		palB[i] = ((i * 7u) & 255u) | (((i * 13u + 5u) & 255u) << 8) | (((i * 29u + 101u) & 255u) << 16);
		rgbaA[i].s.red = palA[i] & 255; rgbaA[i].s.green = (palA[i] >> 8) & 255; rgbaA[i].s.blue = (palA[i] >> 16) & 255; rgbaA[i].s.alpha = 255;
		rgbaB[i].s.red = palB[i] & 255; rgbaB[i].s.green = (palB[i] >> 8) & 255; rgbaB[i].s.blue = (palB[i] >> 16) & 255; rgbaB[i].s.alpha = 255;
	}
}

static const unsigned char glyph[][7] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
	{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
	{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
	{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
	{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
	{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
	{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
	{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
	{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
	{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
	{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
	{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
};
static int glyph_of(char c)
{
	static const char order[] = " SRB2PVIDEOT";
	const char *p = strchr(order, c);
	return p ? (int)(p - order) : 0;
}
static int pw = 320, ph = 200; /* size of the frame the pattern is drawn for */
static void put(UINT8 *b, int x, int y, UINT8 v)
{
	if (x >= 0 && x < pw && y >= 0 && y < ph)
		b[y * pw + x] = v;
}
static void text(UINT8 *b, int x, int y, int scale, UINT8 v, const char *s)
{
	int gx, gy, sx, sy;
	for (; *s; s++, x += 6 * scale)
		for (gy = 0; gy < 7; gy++)
			for (gx = 0; gx < 5; gx++)
				if (glyph[glyph_of(*s)][gy] & (0x10 >> gx))
					for (sy = 0; sy < scale; sy++)
						for (sx = 0; sx < scale; sx++)
							put(b, x + gx * scale + sx, y + gy * scale + sy, v);
}

/* known index pattern for any size (w*h = the current mode); frame != 0 moves the gradient and a box so every soak frame differs.
 * Round things stay round only when the picture is not stretched unevenly: a circle makes a wrong aspect visible. */
static void draw_pattern(UINT8 *b, int w, int h, int frame)
{
	int x, y, bx, s = h / 100 ? h / 100 : 1, r = h / 4;
	pw = w;
	ph = h;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			UINT8 v;
			if (y < h * 24 / 100) v = (UINT8)((x * 256) / w + frame);                        /* gradient, every index */
			else if (y < h * 48 / 100) v = (x < w / 2) ? (((x / 8 + y / 8) & 1) ? 0x08 : 0x10)  /* checker: bits 3/4 swap-sensitive */
				: (((x / 8 + y / 8) & 1) ? 0x18 : 0xE7);
			else if (y < h * 64 / 100) v = (UINT8)(((y - h * 48 / 100) * 8 + (x >> 6) * 3) & 255);      /* vertical gradient */
			else v = 0x20;
			b[y * w + x] = v;
		}
	/* a circle, diagonals */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			int dx = x - w / 2, dy = y - h * 3 / 4;
			if (dx * dx + dy * dy < r * r && dx * dx + dy * dy > (r - 2 * s) * (r - 2 * s)) b[y * w + x] = 0x77;
			if (x * h / w == y || (w - 1 - x) * h / w == y) b[y * w + x] = (UINT8)(0xC0 + (x & 15));
		}
	text(b, 8 * s, h * 66 / 100, 2 * s, 0xF0, "SRB2 PS2");
	text(b, 8 * s, h * 66 / 100 + 18 * s, 2 * s, 0x0F, "VIDEO TEST");
	for (x = 0; x < w; x++) { b[x] = 0xFF; b[(h - 1) * w + x] = 0xFF; }
	for (y = 0; y < h; y++) { b[y * w] = 0xFF; b[y * w + w - 1] = 0xFF; }
	b[1 * w + 1] = 1; b[1 * w + w - 2] = 2; b[(h - 2) * w + 1] = 3; b[(h - 2) * w + w - 2] = 4;
	bx = w * 47 / 100 + (frame * 3) % (w * 47 / 100);
	for (y = h * 85 / 100; y < h * 85 / 100 + 16 * s && y < h - 1; y++)
		for (x = bx; x < bx + 16 * s && x < w - 1; x++)
			b[y * w + x] = (UINT8)(0x40 + frame);
}

/* expected RGB of screen pixel (x,y) for dest rect (dx,dy,dw,dh): nearest texel, black outside */
static u32 expect(int x, int y, const UINT8 *idx, int sw, int sh, const UINT32 *pal, int dx, int dy, int dw, int dh)
{
	int u, v;
	if (x < dx || x >= dx + dw || y < dy || y >= dy + dh)
		return 0;
	/* GS pixel centres sit on integer coordinates: texel = floor((x - dx) * sw / dw) (a +0.5 here would be wrong) */
	u = ((x - dx) * sw) / dw;
	v = ((y - dy) * sh) / dh;
	return pal[idx[v * sw + u]];
}

static int fw, fh, fstride; /* the output in use */
static void geometry(void)
{
	fw = ps2gs_fb_width();
	fh = ps2gs_fb_height();
	fstride = ps2vm_fb_stride(fw);
}

static int read_fb(int index)
{
	int r = 1, y, n;
	/* ps2_screenshot takes the row length from the width it is given (TBW = w / 64): read whole rows of the stride */
	memset(readback, 0xCD, (size_t)fstride * fh * 4); /* a stale buffer must not pass */
	SyncDCache(readback, (u8 *)readback + (size_t)fstride * fh * 4);
	for (y = 0; y < fh; y += 96) /* ps2_screenshot moves at most 64K pixels per call */
	{
		n = (fh - y) < 96 ? (fh - y) : 96;
		r &= ps2_screenshot(readback + (size_t)y * fstride, ps2gs_fb_block(index), 0, y, fstride, n, 0) == 1;
	}
	SyncDCache(readback, (u8 *)readback + (size_t)fstride * fh * 4); /* drop cached lines of the DMA target */
	return r;
}

/* returns the number of mismatching pixels; first mismatch is printed */
static unsigned compare(const UINT8 *idx, int sw, int sh, const UINT32 *pal, int dx, int dy, int dw, int dh, const char *what)
{
	int x, y;
	unsigned bad = 0;
	int fx = -1, fy = -1;
	u32 fgot = 0, fexp = 0;
	for (y = 0; y < fh; y++)
		for (x = 0; x < fw; x++)
		{
			u32 got = readback[y * fstride + x] & 0xFFFFFF, want = expect(x, y, idx, sw, sh, pal, dx, dy, dw, dh);
			if (got != want)
			{
				if (!bad) { fx = x; fy = y; fgot = got; fexp = want; }
				bad++;
			}
		}
	if (bad)
		printf("V0 info %s first mismatch at (%d,%d) got=%06lx expected=%06lx\n", what, fx, fy, (unsigned long)fgot, (unsigned long)fexp);
	return bad;
}

static char hostname[64];

static void dump_fb(const char *name)
{
	char path[160];
	FILE *f;
	int y;
	snprintf(path, sizeof path, "%s-%s.bin", hostname, name);
	f = fopen(path, "wb");
	if (!f) { printf("V0 info cannot open %s\n", path); return; }
	for (y = 0; y < fh; y++)
		fwrite(readback + (size_t)y * fstride, 4, (size_t)fw, f);
	fclose(f);
}

/* present through the real engine entry point, then let the vblank flip happen */
static void show(void)
{
	I_FinishUpdate();
	ps2gs_wait_vblank(3);
}

static void verify(const char *name, const UINT32 *pal, int dx, int dy, int dw, int dh, unsigned expect_bad_min, int dump)
{
	int r;
	unsigned bad;
	r = read_fb(ps2gs_displayed());
	bad = compare(screens[0], vid.width, vid.height, pal, dx, dy, dw, dh, name);
	if (dump) dump_fb(name);
	if (expect_bad_min)
		CHECK(name, r == 1 && bad >= expect_bad_min, "readback=%d mismatch=%u (must be >= %u) of %d", r, bad, expect_bad_min, fw * fh);
	else
		CHECK(name, r == 1 && bad == 0, "readback=%d mismatch=%u of %d displayed=%d pending=%d", r, bad, fw * fh, ps2gs_displayed(), ps2gs_flip_pending());
}

static int aligned_screens(void)
{
	int i, ok = 1;
	for (i = 0; i < NUMSCREENS; i++)
		ok &= !((u32)screens[i] & 63);
	return ok;
}

/* switch the output format the way the menu does: cvar + the callback, applied by the next I_FinishUpdate */
static void select_output(int outid)
{
	cv_vidoutput.value = outid + 1;
	cv_vidoutput.func();
	I_FinishUpdate();
	ps2gs_wait_vblank(2);
	geometry();
}

static void select_fit(int fit)
{
	cv_vidfit.value = fit;
	cv_vidfit.func();
}

static int select_mode(int mode)
{
	int r = VID_SetMode(mode);
	if (r)
		SCR_SetDrawFuncs();
	return r;
}

static int arg_only(const char *list, int id)
{
	char buf[16];
	const char *p = list;
	if (!list)
		return 1;
	snprintf(buf, sizeof buf, "%d", id);
	while (*p)
	{
		if (atoi(p) == id)
			return 1;
		while (*p && *p != ',') p++;
		if (*p == ',') p++;
	}
	return 0;
}

static const char *fitname[] = {"fit43", "stretch", "square", "integer"};

int main(int argc, char **argv)
{
	ps2gs_stats_t st;
	int i, noswap, frames = 700, dy, region = 0, matrix = 1, dump = 0;
	const char *only = NULL;
	u32 t;

	g_argc = argc;
	g_argv = argv;
	myargc = argc;
	myargv = argv;
	for (i = 0; i < argc; i++)
	{
		if (!strncmp(argv[i], "frames=", 7))
			frames = atoi(argv[i] + 7);
		if (!strncmp(argv[i], "matrix=", 7))
			matrix = atoi(argv[i] + 7);
		if (!strncmp(argv[i], "only=", 5))
			only = argv[i] + 5;
		if (!strncmp(argv[i], "dump=", 5))
			dump = atoi(argv[i] + 5);
	}
	noswap = M_CheckParm("--noswap") != 0;
	ps2gs_dbg_noswap = noswap; /* negative control: every pattern test below has to fail with it */

	printf("V0 hello argc=%d noswap=%d frames=%d matrix=%d\n", argc, noswap, frames, matrix);
	for (i = 0; i < argc; i++)
		printf("V0 info argv[%d]=%s\n", i, argv[i]);
	{
		char rom[16];
		memset(rom, 0, sizeof rom);
		GetRomName(rom);
		printf("V0 info rom0:ROMVER=%s (region letter '%c' -> %s) bios=%d\n", rom, rom[4], rom[4] == 'E' ? "PAL" : "NTSC", ps2gs_bios_version());
	}
	readback = memalign(64, 704 * 576 * 4);
	make_palettes();
	cv_ticrate.value = 1; /* make the overlay hooks run */
	cv_closedcaptioning.value = 1;
	marathonmode = MA_RUNNING;

	/* 1. startup through the engine entry point */
	I_StartupGraphics();
	fbuf = screens[0];
	CHECK("startup", rendermode == render_soft && chosenrendermode == render_soft && graphics_started == 1 && drawfuncs >= 1 && commands == 5,
		"rendermode=%d chosen=%d started=%d cvars=%d drawfuncs=%d commands=%d", rendermode, chosenrendermode, graphics_started, regs, drawfuncs, commands);
	CHECK("vid", vid.width == 320 && vid.height == 200 && vid.bpp == 1 && vid.rowbytes == 320 && vid.direct == NULL && vid.modenum == 0,
		"%dx%dx%d rowbytes=%d", vid.width, vid.height, vid.bpp, (int)vid.rowbytes);
	CHECK("screens_aligned64", aligned_screens() && NUMSCREENS == 5, "screens[0]=%08lx [4]=%08lx stride=%d", (unsigned long)screens[0], (unsigned long)screens[4], (int)(screens[1] - screens[0]));
	CHECK("modes", VID_NumModes() == 11 && VID_GetModeForSize(1280, 800) == -1 && VID_GetModeForSize(320, 200) == 0 && VID_GetModeForSize(640, 480) == 9
		&& !strcmp(VID_GetModeName(0), "320x200") && !strcmp(VID_GetModeName(10), "640x512") && VID_GetModeName(11) == NULL && I_GetRefreshRate() == 35,
		"nummodes=%d forsize(1280,800)=%d name0=%s name10=%s refresh=%u", VID_NumModes(), VID_GetModeForSize(1280, 800), VID_GetModeName(0), VID_GetModeName(10), (unsigned)I_GetRefreshRate());
	geometry();
	region = ps2gs_mode();
	{
		unsigned fb0, fb1, tex, cl;
		ps2gs_get_layout(&fb0, &fb1, &tex, &cl);
		printf("V0 info output=%s id=%d fb=%dx%d stride=%d fb0=%08lx fb1=%08lx tex=%08lx clut=%08lx\n", ps2vm_output(region)->name, region, fw, fh, fstride,
			(unsigned long)fb0, (unsigned long)fb1, (unsigned long)tex, (unsigned long)cl);
	}
	snprintf(hostname, sizeof hostname, "host:video-%s", ps2vm_output(region)->arg);

	/* the clear done at init: both buffers black */
	{
		int r = read_fb(0), nz = 0;
		for (i = 0; i < fw * fh; i++) nz += (readback[(i / fw) * fstride + (i % fw)] & 0xFFFFFF) != 0;
		CHECK("init_clear", r == 1 && nz == 0, "readback=%d nonblack=%d", r, nz);
	}

	/* 2. exact 2x placement (integer scale, letterboxed): nearest sampling is unambiguous */
	dy = (fh - 400) / 2;
	ps2gs_set_dest(0, dy, 640, 400);
	draw_pattern(fbuf, 320, 200, 0);
	I_SetPalette(rgbaA);
	overlay_calls = 0;
	show();
	CHECK("overlay_hooks", overlay_calls == 3 && fps_calls == 1, "overlay_calls=%d (marathon, captions, ticrate) fps_calls=%d", overlay_calls, fps_calls);
	verify("pattern_2x", palA, 0, dy, 640, 400, 0, 1);

	/* 3. default placement: the whole frame buffer (stretched) */
	ps2gs_set_fit(PS2VM_FIT_43);
	show();
	{
		int x, y, w, h;
		ps2gs_get_dest(&x, &y, &w, &h);
		CHECK("fit43_default", x == 0 && y == 0 && w == fw && h == fh, "dest %d,%d %dx%d of %dx%d", x, y, w, h, fw, fh);
	}
	verify("pattern_stretch", palA, 0, 0, fw, fh, 0, 1);

	/* 4. palette change only: the CLUT must be reloaded, pixels untouched */
	I_SetPalette(rgbaB);
	show();
	verify("palette_change", palB, 0, 0, fw, fh, 0, 0);
	I_SetPalette(rgbaA);
	show();
	verify("palette_back", palA, 0, 0, fw, fh, 0, 0);
	{
		ps2gs_stats_t before, after;
		ps2gs_get_stats(&before);
		for (i = 0; i < 8; i++)
		{
			I_SetPalette(rgbaA); // an unchanged engine palette must preserve the loaded CLUT
			show();
		}
		ps2gs_get_stats(&after);
		CHECK("palette_unchanged", after.clut_uploads == before.clut_uploads,
			"unchanged palette: uploads %u -> %u", before.clut_uploads, after.clut_uploads);
		CHECK("packet_reused", after.packet_builds == before.packet_builds,
			"eight frames: packet builds %u -> %u", before.packet_builds, after.packet_builds);
		verify("palette_cached_readback", palA, 0, 0, fw, fh, 0, 0);
	}

	/* 5. negative control: without the bit 3/4 swap the comparison has to go red */
	ps2gs_dbg_noswap = 1;
	I_SetPalette(rgbaA);
	show();
	verify("negative_control_noswap", palA, 0, 0, fw, fh, 1000, 0);
	ps2gs_dbg_noswap = noswap;
	I_SetPalette(rgbaA);
	show();
	verify("swap_restored", palA, 0, 0, fw, fh, 0, 0);

	/* 6. I_ReadScreen */
	{
		static UINT8 copy[64000] __attribute__((aligned(64)));
		I_ReadScreen(copy);
		CHECK("read_screen", !memcmp(copy, screens[0], 64000), "64000 bytes");
	}

	/* 6b. a source buffer that is not 64-byte aligned goes through the internal aligned copy */
	{
		UINT8 *raw = memalign(64, 64000 + 64);
		memcpy(raw + 1, screens[0], 64000);
		FlushCache(0);
		ps2gs_present(raw + 1, 0);
		ps2gs_wait_vblank(3);
		verify("unaligned_source", palA, 0, 0, fw, fh, 0, 0);
		{
			struct mallinfo before = mallinfo(), after;
			ps2gs_shutdown();
			after = mallinfo();
			CHECK("staging_released", before.uordblks - after.uordblks >= 64000,
				"GS shutdown released %d heap bytes", before.uordblks - after.uordblks);
			CHECK("GS_restart", ps2gs_init(region) == PS2GS_OK, "restart output %d", region);
			ps2gs_set_dest(0, 0, fw, fh);
			I_SetPalette(rgbaA);
			show();
			verify("restart_palette", palA, 0, 0, fw, fh, 0, 0);
		}
		free(raw);
	}

	/* 6c. optional bilinear filter (vid_filter): colours blend after the CLUT lookup, flat areas stay exact */
	if (!noswap)
	{
		unsigned bad, total = (unsigned)(fw * fh);
		int r;
		cv_vidfilter.value = 1;
		cv_vidfilter.func();
		show();
		r = read_fb(ps2gs_displayed());
		bad = compare(screens[0], 320, 200, palA, 0, 0, fw, fh, "linear");
		CHECK("filter_linear", r == 1 && bad > 0 && bad < total / 2, "readback=%d pixels differing from nearest=%u of %u (edges blend, must be >0 and <50%%)", r, bad, total);
		cv_vidfilter.value = 0;
		cv_vidfilter.func();
		show();
		verify("filter_nearest_restored", palA, 0, 0, fw, fh, 0, 0);
	}

	/* 7. blocking behaviour: back-to-back calls with vid_wait on and off, never more than one vblank per call */
	{
		unsigned maxd = 0, d, v0;
		u32 maxc = 0, c, sumc = 0;
		ps2gs_get_stats(&st);
		for (i = 0; i < 40; i++)
		{
			draw_pattern(fbuf, 320, 200, i);
			cv_vidwait.value = 1;
			v0 = vbl(); t = count();
			I_FinishUpdate();
			c = count() - t; d = vbl() - v0;
			if (d > maxd) maxd = d;
			if (c > maxc) maxc = c;
			sumc += c;
		}
		CHECK("burst_vidwait_on", maxd <= 1, "40 back-to-back calls: max vblanks per call=%u max cycles=%lu avg cycles=%lu (vblank=%u cycles)", maxd, (unsigned long)maxc, (unsigned long)(sumc / 40), 294912000u / ((region == PS2GS_MODE_PAL) ? 50u : 60u));
		maxd = 0; maxc = 0; sumc = 0;
		for (i = 0; i < 40; i++)
		{
			draw_pattern(fbuf, 320, 200, i);
			v0 = vbl(); t = count();
			I_UpdateNoVsync();
			c = count() - t; d = vbl() - v0;
			if (d > maxd) maxd = d;
			if (c > maxc) maxc = c;
			sumc += c;
		}
		CHECK("burst_novsync", maxd <= 1, "40 back-to-back I_UpdateNoVsync: max vblanks per call=%u max cycles=%lu avg cycles=%lu", maxd, (unsigned long)maxc, (unsigned long)(sumc / 40));
		cv_vidwait.value = 1;
		ps2gs_wait_vblank(3);
		ps2gs_get_stats(&st);
		printf("V0 info burst stats: frames=%u dropped=%u flips=%u waited=%u timeouts=%u\n", st.frames, st.dropped, st.flips, st.waited, st.timeouts);
	}

	/* 8. paced soak: logic at 35 Hz against a 59.94 Hz vblank, verified every 64th frame; also the timing of I_FinishUpdate */
	{
		unsigned done = 0, v_start, v_start0, vhz = (region == PS2GS_MODE_PAL) ? 50u : 60u, v, maxd = 0, d, v0, verified = 0, bad_total = 0;
		u32 maxc = 0, minc = 0xFFFFFFFFu, c, sumc = 0, nc = 0, maxc_clut = 0, sumc_clut = 0, nclut = 0;
		int frame = 0;
		ps2gs_get_stats(&st);
		v_start = vbl();
		v_start0 = v_start;
		while (done < (unsigned)frames)
		{
			ps2gs_wait_vblank(1);
			v = vbl() - v_start;
			if ((v * 35u) / vhz < done)
				continue;
			draw_pattern(fbuf, 320, 200, frame);
			if (frame % 50 == 25) I_SetPalette(rgbaB);
			if (frame % 50 == 0) I_SetPalette(rgbaA);
			v0 = vbl(); t = count();
			I_FinishUpdate();
			c = count() - t; d = vbl() - v0;
			if (d > maxd) maxd = d;
			if (c > maxc) maxc = c;
			if (c < minc) minc = c;
			sumc += c; nc++;
			if (frame % 50 == 25 || frame % 50 == 0) { sumc_clut += c; nclut++; if (c > maxc_clut) maxc_clut = c; }
			done++;
			frame++;
			if ((frame & 63) == 0)
			{
				int r;
				unsigned bad;
				ps2gs_wait_vblank(3);
				r = read_fb(ps2gs_displayed());
				bad = compare(screens[0], 320, 200, (((frame - 1) % 50) >= 25) ? palB : palA, 0, 0, fw, fh, "soak");
				verified++;
				bad_total += bad + (r != 1);
			}
		}
		ps2gs_wait_vblank(3);
		{
			int r = read_fb(ps2gs_displayed());
			unsigned bad = compare(screens[0], 320, 200, (((frame - 1) % 50) >= 25) ? palB : palA, 0, 0, fw, fh, "soak_final");
			verified++;
			bad_total += bad + (r != 1);
		}
		ps2gs_get_stats(&st);
		CHECK("soak_frames", done >= 600 && done == (unsigned)frames, "frames=%u (>=600 required)", done);
		CHECK("soak_verified", bad_total == 0, "%u full-screen readbacks compared, mismatches=%u", verified, bad_total);
		CHECK("soak_no_long_block", maxd <= 1, "max vblanks spent inside one I_FinishUpdate=%u", maxd);
		CHECK("soak_timeouts", st.timeouts == 0, "bounded spins that ran out=%u", st.timeouts);
		printf("V0 timing I_FinishUpdate cycles: n=%lu min=%lu avg=%lu max=%lu (vblank=%u cycles, 35Hz tic=%u cycles); with CLUT upload: n=%lu avg=%lu max=%lu\n",
			(unsigned long)nc, (unsigned long)minc, (unsigned long)(sumc / nc), (unsigned long)maxc, 294912000u / vhz, 294912000u / 35u,
			(unsigned long)nclut, (unsigned long)(nclut ? sumc_clut / nclut : 0), (unsigned long)maxc_clut);
		printf("V0 timing internals (EE cycles): flush+build max=%lu dma_wait max=%lu present max=%lu\n", (unsigned long)st.flush_max, (unsigned long)st.dma_wait_max, (unsigned long)st.present_max);
		printf("V0 packet stats: frames=%u builds=%u CLUT uploads=%u\n", st.frames, st.packet_builds, st.clut_uploads);
		printf("V0 stats: frames=%u dropped=%u flips=%u vblanks=%u waited=%u timeouts=%u; soak: %u calls in %u vblanks (%u.%u calls/s at %u Hz)\n", st.frames, st.dropped, st.flips, st.vblanks, st.waited, st.timeouts,
				done, vbl() - v_start0, (done * vhz * 10u / (vbl() - v_start0)) / 10u, (done * vhz * 10u / (vbl() - v_start0)) % 10u, vhz);
	}

	/* 9. the internal modes through the engine entry point: buffer, slices, alignment, no leak over many switches */
	{
		int m, w, h, ok = 1, ok_al = 1, ok_zero = 1, okv = 1, changed;
		struct mallinfo before, after;
		before = mallinfo();
		for (i = 0; i < 4 * PS2VM_NUMMODES; i++)
		{
			m = (i * 7) % PS2VM_NUMMODES;
			ps2vm_mode_size(m, &w, &h);
			changed = (w != vid.width || h != vid.height);
			ok &= select_mode(m) == 1;
			okv &= vid.width == w && vid.height == h && vid.rowbytes == (size_t)w && vid.modenum == m && vid.bpp == 1;
			if (!(aligned_screens() && screens[1] == screens[0] + (size_t)w * h && screens[4] == screens[0] + 4 * (size_t)w * h))
			{
				ok_al = 0;
				printf("V0 info mode %d %dx%d slices: screens[0]=%08lx [1]=%08lx [4]=%08lx\n", m, w, h, (unsigned long)screens[0], (unsigned long)screens[1], (unsigned long)screens[4]);
			}
			if (changed && !(screens[4][(size_t)w * h - 1] == 0 && screens[0][0] == 0))
			{
				ok_zero = 0;
				printf("V0 info mode %d %dx%d not zero: %02x %02x\n", m, w, h, screens[4][(size_t)w * h - 1], screens[0][0]);
			}
			memset(screens[0], 0xAA, (size_t)w * h); /* touch the whole frame: a short allocation would trample the heap */
			memset(screens[4], 0x55, (size_t)w * h);
			I_FinishUpdate();
		}
		select_mode(0);
		after = mallinfo();
		CHECK("modes_switch", ok && okv, "%d switches through VID_SetMode, vid fields right", 4 * PS2VM_NUMMODES);
		CHECK("modes_slices", ok_al && ok_zero, "64-byte aligned slices, zeroed on a size change");
		CHECK("modes_no_leak", after.uordblks <= before.uordblks + 64, "malloc in use %d -> %d bytes", before.uordblks, after.uordblks);
		CHECK("mode_bad", VID_SetMode(11) == 0 && VID_SetMode(-3) == 0 && cons_alerts >= 2, "out-of-range modes refused with a message (%d alerts)", cons_alerts);
		select_mode(0);
	}

	/* 10. output format x internal mode matrix: every format the table has, readback of the displayed buffer against the model */
	if (matrix)
	{
		static const int src_modes[] = {0, 4, 7, 9, 10}; /* 320x200, 400x300, 640x400, 640x480, 640x512 */
		int o, k, f, cells = 0, cell_bad = 0, refused = 0, bios = ps2gs_bios_version();
		for (o = 0; o < PS2VM_NUMOUT; o++)
		{
			const ps2vm_output_t *od = ps2vm_output(o);
			int expect_r = ((od->flags & PS2VM_NEEDS_BIOS220) && bios < 220) ? PS2GS_E_BIOS : PS2GS_OK;
			int r = ps2gs_check(o, 640, 512);
			if (!arg_only(only, o))
				continue;
			CHECK("check_output", r == expect_r, "%d %s: ps2gs_check=%d expected=%d (bios %d)", o, od->name, r, expect_r, bios);
			if (r)
			{
				int previous = ps2gs_mode();
				refused++;
				select_output(o); /* the menu path: must refuse without leaving the old output */
				CHECK("refuse_output", ps2gs_mode() == previous, "stays on %s after asking for %s", ps2vm_output(ps2gs_mode())->name, od->name);
				select_output(region);
				continue;
			}
			select_output(o);
			CHECK("select_output", ps2gs_mode() == o && fw == od->fbw && fh == od->fbh, "%d %s: now %d, fb %dx%d (table %dx%d)", o, od->name, ps2gs_mode(), fw, fh, od->fbw, od->fbh);
			{
				ps2vm_crtc_t c, want;
				ps2gs_get_crtc(&c);
				ps2vm_crtc(od, 0, 0, &want);
				printf("V0 info %s crtc dx=%d dy=%d magh=%d magv=%d dw=%d dh=%d\n", od->name, c.dx, c.dy, c.magh, c.magv, c.dw, c.dh);
			}
			for (k = 0; k < (int)(sizeof src_modes / sizeof src_modes[0]); k++)
			{
				int w = 0, h = 0;
				ps2vm_mode_size(src_modes[k], &w, &h);
				if (select_mode(src_modes[k]) != 1)
				{
					CHECK("matrix_mode", 0, "%s %dx%d: VID_SetMode refused", od->name, w, h);
					continue;
				}
				draw_pattern(screens[0], w, h, 3);
				for (f = 0; f < PS2VM_FIT_COUNT; f++)
				{
					int x, y, dw_, dh_, whole, bad, rr;
					char name[96];
					unsigned total;
					if (k > 1 && f != PS2VM_FIT_43 && f != PS2VM_FIT_INTEGER)
						continue; /* the big sources: two fits are enough, the logic is the same */
					select_fit(f);
					I_SetPalette(rgbaA);
					show();
					ps2gs_get_dest(&x, &y, &dw_, &dh_);
					whole = (x == 0 && y == 0 && dw_ == fw && dh_ == fh);
					rr = read_fb(ps2gs_displayed());
					bad = (int)compare(screens[0], w, h, palA, x, y, dw_, dh_, "matrix");
					total = (unsigned)(dw_ * dh_);
					snprintf(name, sizeof name, "m_%s_%dx%d_%s", od->arg, w, h, fitname[f]);
					/* exact when the picture is not scaled down by more than 2 (the model rounds like the GS then);
					 * down-scaling by a lot picks other texels from the same column/row, so some mismatch is allowed there */
					{
						int down = (w > dw_ * 2 || h > dh_ * 2);
						unsigned limit = down ? total / 4 : 0;
						cells++;
						if (!(rr == 1 && (unsigned)bad <= limit))
							cell_bad++;
						CHECK(name, rr == 1 && (unsigned)bad <= limit, "dest %d,%d %dx%d (whole=%d) mismatch=%d of %u limit=%u", x, y, dw_, dh_, whole, bad, total, limit);
					}
					if (dump && ((k == 0 && f == PS2VM_FIT_43) || (k == 4 && f == PS2VM_FIT_43)))
					{
						snprintf(hostname, sizeof hostname, "host:video");
						dump_fb(name);
					}
				}
			}
			select_mode(0);
			select_fit(PS2VM_FIT_43);
		}
		printf("V0 info matrix: %d cells, %d outside the limit, %d outputs refused\n", cells, cell_bad, refused);
		select_output(region);
		CHECK("matrix_back", ps2gs_mode() == region, "back on output %d", ps2gs_mode());
		ps2gs_get_stats(&st);
		CHECK("matrix_timeouts", st.timeouts == 0, "bounded spins that ran out=%u", st.timeouts);
	}

	I_ShutdownGraphics();
	CHECK("shutdown", graphics_started == 0 && rendermode == render_none, "started=%d rendermode=%d", graphics_started, rendermode);
	printf("V0 COMPLETE failures=%d\n", failures);
	for (;;)
		SleepThread();
	return failures;
}
