/* Hardware test for the GS renderer driver (src/ps2/hw/ps2_hwd.c). Links the real driver plus stand-ins for CONS_*.
 * Everything is checked by reading the frame buffer / Z buffer back through the GS (local -> host) and comparing with
 * pixels computed on the EE by independent code (double precision reference pipeline, exact blend formulas).
 *   args (PCSX2 -gameargs, leading dash optional):
 *     fb16            run with the lossy CT16S frame buffers instead of the default CT32
 *     negctl=N        N = 1 ignore Z test, 2 ignore alpha test, 4 ignore blending, 8 never write Z, 16 no CLUT swap:
 *                     the tests that depend on the feature must turn red
 *     soak=N          frames of the soak run (default 600)
 *     dump            write host:hw-<name>.ppm of the interesting frames
 *     quick           skip soak and stress
 */
#include "doomdef.h"
#include "hardware/hw_drv.h"
#include "ps2/hw/ps2_hwd.h"
#include "ps2/hw/ps2_hwd_dbg.h"

#include <tamtypes.h>
#include <kernel.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stand-ins ---- */
void CONS_Printf(const char *fmt, ...)
{
	va_list ap;

	printf("H0 con: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
void CONS_Alert(alerttype_t level, const char *fmt, ...)
{
	va_list ap;

	printf("H0 alert(%d): ", (int)level);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* the engine hooks of the driver (hardware/hw_cache.c): the fixtures keep the data of their textures themselves */
void HWR_PS2_RegenerateMipmap(GLMipmap_t *m)
{
	(void)m;
}
void HWR_PS2_ReleaseMipmapData(GLMipmap_t *m)
{
	(void)m;
}

static int failures, checks;
#define CHECK(name, cond, ...) do { int ok_ = (cond) ? 1 : 0; failures += !ok_; checks++; printf("H0 %s %s ", ok_ ? "PASS" : "FAIL", name); printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_argc;
static char **g_argv;
static int arg(const char *name)
{
	int i;

	for (i = 0; i < g_argc; i++)
		if (!strcmp(g_argv[i], name) || (g_argv[i][0] == '-' && !strcmp(g_argv[i] + 1, name)))
			return 1;
	return 0;
}
static int argval(const char *name, int def)
{
	int i, n = (int)strlen(name);

	for (i = 0; i < g_argc; i++)
	{
		const char *a = g_argv[i] + (g_argv[i][0] == '-');

		if (!strncmp(a, name, n) && a[n] == '=')
			return atoi(a + n + 1);
	}
	return def;
}

static inline u32 cyc(void)
{
	u32 v;

	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

/* ---- driver, frame helpers ---- */
static struct hwdriver_s D;
static ps2hwd_info_t INFO;
static int FW, FH, FB32;
static u32 *fb; /* last read-back frame, 0x00BBGGRR */
static u32 *zb; /* last read-back depth, 24 bit */
static u32 *fb2;
#define SW 320
#define SH 200
static float SX, SY;
/* sample counts of the tests were chosen for 640x448 pixels */
static int scale_n(int n)
{
	return (int)((double)n * (double)FW * (double)FH / (640.0 * 448.0));
}
static int dump_on;

static u32 rgb(int r, int g, int b)
{
	return (u32)r | ((u32)g << 8) | ((u32)b << 16);
}
static int chan(u32 c, int i)
{
	return (int)((c >> (8 * i)) & 255);
}
/* what a colour looks like after a trip through the frame buffer */
static u32 quant(u32 c)
{
	int i;
	u32 o = 0;

	if (FB32)
		return c & 0xFFFFFF;
	for (i = 0; i < 3; i++)
	{
		int v = chan(c, i) >> 3;

		o |= (u32)((v << 3) | (v >> 2)) << (8 * i);
	}
	return o;
}
static int close_to(u32 a, u32 b, int tol)
{
	int i;

	for (i = 0; i < 3; i++)
		if (abs(chan(a, i) - chan(b, i)) > tol)
			return 0;
	return 1;
}

static void dump_ppm(const char *name, const u32 *img)
{
	char path[64];
	FILE *f;
	int i;
	u8 *row;

	if (!dump_on)
		return;
	snprintf(path, sizeof path, "host:hw-%s%s.ppm", name, FB32 ? "-32" : "");
	f = fopen(path, "wb");
	if (!f)
	{
		printf("H0 info cannot write %s\n", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", FW, FH);
	row = malloc((size_t)FW * 3);
	for (i = 0; i < FH; i++)
	{
		int x;

		for (x = 0; x < FW; x++)
		{
			u32 c = img[i * FW + x];

			row[x * 3] = (u8)c;
			row[x * 3 + 1] = (u8)(c >> 8);
			row[x * 3 + 2] = (u8)(c >> 16);
		}
		fwrite(row, 3, (size_t)FW, f);
	}
	free(row);
	fclose(f);
}

/* start a frame: full-screen view, 2D transform, clear colour + depth */
static void frame_start(u32 c)
{
	FRGBAFloat cc;

	cc.red = (float)chan(c, 0) / 255.0f;
	cc.green = (float)chan(c, 1) / 255.0f;
	cc.blue = (float)chan(c, 2) / 255.0f;
	cc.alpha = 1.0f;
	D.pfnGClipRect(0, 0, SW, SH, 0.9f);
	D.pfnSetTransform(NULL);
	D.pfnClearBuffer(1, 1, &cc);
}

static int frame_grab(void)
{
	int r;

	D.pfnFinishUpdate(0);
	r = PS2HWD_ReadFrame(fb);
	return r;
}

static int depth_grab(void)
{
	return PS2HWD_ReadDepth(zb);
}

/* 2D quad as the engine's HUD code makes it: NDC at z = 1, engine pixel coordinates (top-left origin) */
static void quad2d(FSurfaceInfo *s, float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1, u32 flags)
{
	FOutVector v[4];
	float nx0 = (x0 - SW / 2.0f) / (SW / 2.0f), nx1 = (x1 - SW / 2.0f) / (SW / 2.0f);
	float ny0 = (SH / 2.0f - y0) / (SH / 2.0f), ny1 = (SH / 2.0f - y1) / (SH / 2.0f); /* ny0 = top */

	v[0].x = nx0; v[0].y = ny1; v[0].z = 1.0f; v[0].s = s0; v[0].t = t1; /* bottom left */
	v[1].x = nx1; v[1].y = ny1; v[1].z = 1.0f; v[1].s = s1; v[1].t = t1;
	v[2].x = nx1; v[2].y = ny0; v[2].z = 1.0f; v[2].s = s1; v[2].t = t0;
	v[3].x = nx0; v[3].y = ny0; v[3].z = 1.0f; v[3].s = s0; v[3].t = t0;
	D.pfnDrawPolygon(s, v, 4, flags);
}

static FSurfaceInfo surf_rgba(int r, int g, int b, int a)
{
	FSurfaceInfo s;

	memset(&s, 0, sizeof s);
	s.PolyColor.s.red = (UINT8)r;
	s.PolyColor.s.green = (UINT8)g;
	s.PolyColor.s.blue = (UINT8)b;
	s.PolyColor.s.alpha = (UINT8)a;
	return s;
}

/* ---- palette and textures ---- */
static RGBA_t PAL[256], PAL2[256];
static void make_pals(void)
{
	unsigned i;

	for (i = 0; i < 256; i++)
	{
		/* a bijection per channel: every index has its own colour, so the index of a texel can be recovered */
		PAL[i].s.red = (UINT8)i;
		PAL[i].s.green = (UINT8)(i ^ 0x55u);
		PAL[i].s.blue = (UINT8)(255u - i);
		PAL[i].s.alpha = 255;
		PAL2[i].s.red = (UINT8)((i * 7u) & 255u);
		PAL2[i].s.green = (UINT8)((i * 13u + 5u) & 255u);
		PAL2[i].s.blue = (UINT8)((i * 29u + 101u) & 255u);
		PAL2[i].s.alpha = 255;
	}
}
static u32 palc(const RGBA_t *p, int i)
{
	return rgb(p[i].s.red, p[i].s.green, p[i].s.blue);
}

static GLMipmap_t *tex_new(int w, int h, GLTextureFormat_t fmt, UINT32 flags)
{
	GLMipmap_t *m = calloc(1, sizeof *m);
	size_t bpp = fmt == GL_TEXFMT_RGBA ? 4 : fmt == GL_TEXFMT_AP_88 || fmt == GL_TEXFMT_ALPHA_INTENSITY_88 ? 2 : 1;

	m->format = fmt;
	m->width = (UINT16)w;
	m->height = (UINT16)h;
	m->flags = flags;
	m->data = malloc((size_t)w * h * bpp);
	return m;
}
static void tex_free(GLMipmap_t *m)
{
	D.pfnDeleteTexture(m);
	free(m->data);
	free(m);
}
static void rgba_set(GLMipmap_t *m, int x, int y, const RGBA_t *p, int idx, int alpha)
{
	RGBA_t c = p[idx];

	c.s.alpha = (UINT8)alpha;
	((u32 *)m->data)[y * m->width + x] = c.rgba;
}

/* ====================================================================== tests */

static void test_layout(void)
{
	PS2HWD_GetInfo(&INFO);
	FW = INFO.vw;
	FH = INFO.vh;
	FB32 = INFO.fb32;
	SX = (float)FW / SW;
	SY = (float)FH / SH;
	printf("H0 info mode=%d fb=%dx%d %s fb0=%uK fb1=%uK z=%uK pool=%uK..%uK (%u pages) clut=%uK dma_irq=%d\n", INFO.mode, FW, FH, FB32 ? "CT32" : "CT16S",
		INFO.fb_block[0] / 4, INFO.fb_block[1] / 4, INFO.z_block / 4, INFO.pool_block / 4, (INFO.pool_block + INFO.pool_blocks) / 4, INFO.pool_blocks / 32, INFO.clut_block / 4, INFO.dma_irq);
	CHECK("layout_no_overlap", INFO.fb_block[1] > INFO.fb_block[0] && INFO.z_block > INFO.fb_block[1] && INFO.clut_block >= INFO.z_block + (unsigned)((FW / 64) * ((FH + 31) / 32) * 32)
		&& INFO.pool_block + INFO.pool_blocks == 16384, "fb0=%u fb1=%u z=%u clut=%u pool=%u+%u (blocks of 256 B)", INFO.fb_block[0], INFO.fb_block[1], INFO.z_block, INFO.clut_block, INFO.pool_block, INFO.pool_blocks);
}

static void test_clear(void)
{
	FRGBAFloat c = {0.25f, 0.5f, 0.75f, 1.0f};
	int i, bad = 0, zbad = 0, r, rz;
	u32 want = quant(rgb(64, 128, 191));

	D.pfnGClipRect(0, 0, SW, SH, 0.9f);
	D.pfnSetTransform(NULL);
	D.pfnClearBuffer(1, 1, &c);
	r = frame_grab();
	rz = depth_grab();
	for (i = 0; i < FW * FH; i++)
	{
		bad += fb[i] != want;
		zbad += zb[i] != 0;
	}
	CHECK("clear_colour", r == 0 && bad == 0, "readback=%d pixels != %06x: %d of %d (first pixel %06x)", r, (unsigned)want, bad, FW * FH, (unsigned)fb[0]);
	CHECK("clear_depth", rz == 0 && zbad == 0, "readback=%d nonzero Z: %d of %d", rz, zbad, FW * FH);
	dump_ppm("clear", fb);
}

/* coverage test: pixel (x, y) is inside [x0, x1) x [y0, y1) when its centre is. tol: pixels this close to an edge are skipped. */
static int rect_compare(u32 inside, u32 outside, float x0, float y0, float x1, float y1, float tol, int *skipped, int *n_inside)
{
	int x, y, bad = 0;

	*skipped = 0;
	*n_inside = 0;
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++)
		{
			float cx = x + 0.5f, cy = y + 0.5f;
			int in = cx >= x0 && cx < x1 && cy >= y0 && cy < y1;
			float d = fminf(fminf(fabsf(cx - x0), fabsf(cx - x1)), fminf(fabsf(cy - y0), fabsf(cy - y1)));

			if (d < tol)
			{
				(*skipped)++;
				continue;
			}
			*n_inside += in;
			bad += fb[y * FW + x] != (in ? inside : outside);
		}
	return bad;
}

static void test_flat(void)
{
	FSurfaceInfo s = surf_rgba(255, 0, 0, 255);
	u32 red = quant(rgb(255, 0, 0)), gray = quant(rgb(40, 40, 40));
	int skipped, nin, bad, r;

	/* edges on pixel boundaries: engine 40..100 x 50..100 -> frame buffer 80..200 x 112..224 (SY = 2.24) */
	frame_start(rgb(40, 40, 40));
	quad2d(&s, 40.0f, 50.0f, 100.0f, 100.0f, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
	r = frame_grab();
	bad = rect_compare(red, gray, 40.0f * SX, 50.0f * SY, 100.0f * SX, 100.0f * SY, 0.0f, &skipped, &nin);
	CHECK("flat_quad_exact_edges", r == 0 && bad == 0 && nin == (int)(60 * SX * 50 * SY), "readback=%d mismatches=%d (strict: every pixel), pixels inside=%d (want %d)", r, bad, nin, (int)(60 * SX * 50 * SY));
	dump_ppm("flat", fb);

	/* fractional edges */
	frame_start(rgb(40, 40, 40));
	quad2d(&s, 123.3f, 11.7f, 270.9f, 150.2f, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
	r = frame_grab();
	bad = rect_compare(red, gray, 123.3f * SX, 11.7f * SY, 270.9f * SX, 150.2f * SY, 0.08f, &skipped, &nin);
	CHECK("flat_quad_fraction_edges", r == 0 && bad == 0, "readback=%d mismatches=%d skipped (within 0.08 px of an edge)=%d inside=%d", r, bad, skipped, nin);
}

/* the texture texel under frame buffer pixel (x, y) for a quad (x0..x1, y0..y1) in frame buffer pixels with nearest sampling */
static int tex_texel(float x0, float x1, float p, int n)
{
	return (int)floorf((p - x0) / (x1 - x0) * (float)n);
}

static void test_tex_map(const char *name, int wrapmode)
{
	GLMipmap_t *m = tex_new(64, 64, GL_TEXFMT_RGBA, wrapmode ? TF_WRAPXY : 0);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	int x, y, r, bad = 0, skipped = 0, n = 0;
	float X0 = 20.0f * SX, X1 = 148.0f * SX, Y0 = 20.0f * SY, Y1 = 84.0f * SY;

	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++)
			rgba_set(m, x, y, PAL, (x * 3 + y * 5) % 255, 255);
	D.pfnSetTexturePalette(PAL);
	frame_start(rgb(10, 20, 30));
	D.pfnSetTexture(m);
	quad2d(&s, 20.0f, 20.0f, 148.0f, 84.0f, 0, 0, 1, 1, PF_Masked | PF_Modulated | PF_Occlude);
	r = frame_grab();
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++)
		{
			float cx = x + 0.5f, cy = y + 0.5f;
			u32 want;
			float u, v, fu, fv;
			int tu, tv;

			if (cx < X0 || cx >= X1 || cy < Y0 || cy >= Y1)
			{
				if (cx < X0 - 0.1f || cx > X1 + 0.1f || cy < Y0 - 0.1f || cy > Y1 + 0.1f)
					bad += fb[y * FW + x] != quant(rgb(10, 20, 30));
				continue;
			}
			u = (cx - X0) / (X1 - X0) * 64.0f;
			v = (cy - Y0) / (Y1 - Y0) * 64.0f;
			fu = u - floorf(u);
			fv = v - floorf(v);
			if (cx < X0 + 0.1f || cx > X1 - 0.1f || cy < Y0 + 0.1f || cy > Y1 - 0.1f || fu < 0.06f || fu > 0.94f || fv < 0.06f || fv > 0.94f)
			{
				skipped++;
				continue;
			}
			tu = (int)u;
			tv = (int)v;
			want = quant(palc(PAL, (tu * 3 + tv * 5) % 255));
			n++;
			bad += fb[y * FW + x] != want;
		}
	CHECK(name, r == 0 && bad == 0 && n > scale_n(20000), "readback=%d mismatching pixels=%d of %d compared (%d near texel/quad edges skipped), downloaded=%u", r, bad, n, skipped, (unsigned)m->downloaded);
	(void)tex_texel;
	dump_ppm(name, fb);
	tex_free(m);
}

/* all 256 palette entries through a 16x16 texture: catches a wrong CLUT bit 3/4 layout */
static void test_clut(const char *name, int expect_fail)
{
	GLMipmap_t *m = tex_new(16, 16, GL_TEXFMT_RGBA, 0);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	int x, y, r, bad = 0;
	float X0 = 16.0f * SX, X1 = 144.0f * SX, Y0 = 16.0f * SY, Y1 = 144.0f * SY;

	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			rgba_set(m, x, y, PAL, y * 16 + x == 255 ? 254 : y * 16 + x, 255);
	D.pfnSetTexturePalette(PAL);
	frame_start(rgb(0, 0, 0));
	D.pfnSetTexture(m);
	quad2d(&s, 16.0f, 16.0f, 144.0f, 144.0f, 0, 0, 1, 1, PF_Masked | PF_Modulated);
	r = frame_grab();
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
		{
			int px = (int)(X0 + (x + 0.5f) * (X1 - X0) / 16.0f), py = (int)(Y0 + (y + 0.5f) * (Y1 - Y0) / 16.0f);
			u32 want = quant(palc(PAL, y * 16 + x == 255 ? 254 : y * 16 + x));

			bad += fb[py * FW + px] != want;
		}
	if (expect_fail)
		CHECK(name, r == 0 && bad >= 100, "negative control (CLUT bit 3/4 swap off): %d of 256 palette cells wrong (must be >= 100)", bad);
	else
		CHECK(name, r == 0 && bad == 0, "readback=%d wrong palette cells: %d of 256", r, bad);
	dump_ppm(name, fb);
	tex_free(m);
}

static void test_keyed(void)
{
	GLMipmap_t *m = tex_new(16, 16, GL_TEXFMT_RGBA, 0);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	int x, y, r, rz, bad = 0, zbad = 0, n = 0;
	float X0 = 32.0f * SX, X1 = 96.0f * SX, Y0 = 32.0f * SY, Y1 = 96.0f * SY;
	u32 bg = quant(rgb(10, 20, 30));

	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			rgba_set(m, x, y, PAL, 100 + x, x < 8 ? 0 : 255); /* left half: holes (alpha 0) */
	D.pfnSetTexturePalette(PAL);
	frame_start(rgb(10, 20, 30));
	D.pfnSetTexture(m);
	quad2d(&s, 32.0f, 32.0f, 96.0f, 96.0f, 0, 0, 1, 1, PF_Masked | PF_Modulated | PF_Occlude);
	r = frame_grab();
	rz = depth_grab();
	for (y = (int)Y0 + 3; y < (int)Y1 - 3; y++)
		for (x = (int)X0 + 3; x < (int)X1 - 3; x++)
		{
			float u = (x + 0.5f - X0) / (X1 - X0) * 16.0f;
			int tu = (int)u;
			float fu = u - floorf(u);

			if (fu < 0.1f || fu > 0.9f)
				continue;
			n++;
			if (tu < 8)
			{
				bad += fb[y * FW + x] != bg;
				zbad += zb[y * FW + x] != 0; /* alpha-tested out: no depth write either */
			}
			else
			{
				bad += fb[y * FW + x] != quant(palc(PAL, 100 + tu));
				zbad += zb[y * FW + x] == 0;
			}
		}
	CHECK("keyed_holes_alpha_test", r == 0 && rz == 0 && bad == 0 && zbad == 0 && n > 3000, "colour mismatches=%d depth mismatches=%d (holes must leave Z untouched, solid texels write it) of %d", bad, zbad, n);
	tex_free(m);
}

/* exact GS blend of an 8-bit source over the destination pixel as the frame buffer holds it */
static int blend_ch(int cs, int cd, int as, int mode)
{
	int d = FB32 ? cd : ((cd >> 3) << 3); /* a 16-bit pixel is widened by shifting */
	int v;

	switch (mode)
	{
		case 0: v = (((cs - d) * as) >> 7) + d; break; /* translucent */
		case 1: v = ((cs * as) >> 7) + d; break; /* additive */
		default: v = d - ((cs * as) >> 7); break; /* reverse subtract */
	}
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void test_blend(const char *name, u32 flags, int mode, int alpha, int cs_r, int cs_g, int cs_b, int expect_fail)
{
	FSurfaceInfo s = surf_rgba(cs_r, cs_g, cs_b, alpha);
	int r, x, y, bad = 0, n = 0, as = (alpha * 128 + 127) / 255;
	u32 bg = rgb(200, 100, 50), want;
	int wr, wg, wb;

	frame_start(bg);
	quad2d(&s, 50.0f, 50.0f, 150.0f, 100.0f, 0, 0, 0, 0, flags | PF_NoTexture | PF_Modulated | PF_NoDepthTest);
	r = frame_grab();
	wr = blend_ch(cs_r, 200, as, mode);
	wg = blend_ch(cs_g, 100, as, mode);
	wb = blend_ch(cs_b, 50, as, mode);
	want = FB32 ? rgb(wr, wg, wb) : quant(rgb(wr, wg, wb));
	for (y = (int)(55 * SY); y < (int)(95 * SY); y++)
		for (x = (int)(55 * SX); x < (int)(145 * SX); x++)
		{
			n++;
			bad += !close_to(fb[y * FW + x], want, FB32 ? 1 : 8);
		}
	if (expect_fail)
		CHECK(name, r == 0 && bad > n / 2, "negative control: %d of %d pixels differ from the blended result (must be > half); got %06x want %06x", bad, n, (unsigned)fb[(int)(75 * SY) * FW + (int)(100 * SX)], (unsigned)want);
	else
		CHECK(name, r == 0 && bad == 0, "readback=%d pixels off by more than the 16-bit step: %d of %d; got %06x want %06x", r, bad, n, (unsigned)fb[(int)(75 * SY) * FW + (int)(100 * SX)], (unsigned)want);
}

/* ---- reference 3D pipeline in double precision ---- */
typedef struct
{
	double m[16]; /* column-major mvp */
	double vx, vy, vw, vh; /* viewport in frame buffer pixels */
} refcam_t;

static void rm_mul(double *o, const double *a, const double *b)
{
	double t[16];
	int c, r;

	for (c = 0; c < 4; c++)
		for (r = 0; r < 4; r++)
			t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] + a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
	memcpy(o, t, sizeof t);
}
static void rm_id(double *m)
{
	int i;

	for (i = 0; i < 16; i++)
		m[i] = i % 5 == 0;
}
static void rm_rot(double *m, double deg, double x, double y, double z)
{
	double t[16], r = deg * M_PI / 180.0, c = cos(r), s = sin(r), l = sqrt(x * x + y * y + z * z), ic;

	x /= l; y /= l; z /= l; ic = 1 - c;
	rm_id(t);
	t[0] = x * x * ic + c; t[1] = y * x * ic + z * s; t[2] = z * x * ic - y * s;
	t[4] = x * y * ic - z * s; t[5] = y * y * ic + c; t[6] = z * y * ic + x * s;
	t[8] = x * z * ic + y * s; t[9] = y * z * ic - x * s; t[10] = z * z * ic + c;
	rm_mul(m, m, t);
}
static void rm_scale(double *m, double x, double y, double z)
{
	double t[16];

	rm_id(t);
	t[0] = x; t[5] = y; t[10] = z;
	rm_mul(m, m, t);
}
static void rm_trans(double *m, double x, double y, double z)
{
	double t[16];

	rm_id(t);
	t[12] = x; t[13] = y; t[14] = z;
	rm_mul(m, m, t);
}
static void rm_persp(double *m, double fovy, double aspect, double zn, double zf)
{
	double p[16] = {0}, rad = fovy / 2 * M_PI / 180, cot = cos(rad) / sin(rad);

	p[0] = cot / aspect;
	p[5] = cot;
	p[10] = -(zf + zn) / (zf - zn);
	p[11] = -1;
	p[14] = -2 * zn * zf / (zf - zn);
	rm_mul(m, m, p);
}

static void ref_setup(refcam_t *c, const FTransform *t, double znear, int vminx, int vminy, int vmaxx, int vmaxy)
{
	double mv[16], pr[16];

	rm_id(mv);
	rm_scale(mv, t->scalex, t->scaley, -t->scalez);
	rm_rot(mv, t->anglex, 1, 0, 0);
	rm_rot(mv, t->angley + 270.0, 0, 1, 0);
	rm_trans(mv, -t->x, -t->z, -t->y);
	rm_id(pr);
	rm_persp(pr, t->fovxangle, 1.0, znear, 32768.0);
	rm_mul(c->m, pr, mv);
	c->vx = vminx * (double)SX;
	c->vy = vminy * (double)SY;
	c->vw = (vmaxx - vminx) * (double)SX;
	c->vh = (vmaxy - vminy) * (double)SY;
}

/* convex polygon -> window coordinates, clipped in clip space against the six planes; returns the vertex count */
static int ref_clip(const refcam_t *c, const double (*p)[3], int n, double (*out)[3])
{
	double a[32][4], b[32][4];
	int cnt = n, i, k, pl;
	double (*in)[4] = a, (*o)[4] = b;

	for (i = 0; i < n; i++)
	{
		const double *m = c->m;

		in[i][0] = m[0] * p[i][0] + m[4] * p[i][1] + m[8] * p[i][2] + m[12];
		in[i][1] = m[1] * p[i][0] + m[5] * p[i][1] + m[9] * p[i][2] + m[13];
		in[i][2] = m[2] * p[i][0] + m[6] * p[i][1] + m[10] * p[i][2] + m[14];
		in[i][3] = m[3] * p[i][0] + m[7] * p[i][1] + m[11] * p[i][2] + m[15];
	}
	for (pl = 0; pl < 6; pl++)
	{
		int m2 = 0;

		for (i = 0; i < cnt; i++)
		{
			double *v0 = in[i], *v1 = in[(i + 1) % cnt], d0, d1;
			int ax = pl / 2, sgn = (pl & 1) ? -1 : 1;

			d0 = v0[3] + sgn * v0[ax];
			d1 = v1[3] + sgn * v1[ax];
			if (d0 >= 0)
				memcpy(o[m2++], v0, sizeof(double[4]));
			if ((d0 >= 0) != (d1 >= 0))
			{
				double t = d0 / (d0 - d1);

				for (k = 0; k < 4; k++)
					o[m2][k] = v0[k] + (v1[k] - v0[k]) * t;
				m2++;
			}
		}
		cnt = m2;
		{
			double (*t)[4] = in;

			in = o;
			o = t;
		}
		if (!cnt)
			return 0;
	}
	for (i = 0; i < cnt; i++)
	{
		out[i][0] = c->vx + (in[i][0] / in[i][3] * 0.5 + 0.5) * c->vw;
		out[i][1] = c->vy + (-in[i][1] / in[i][3] * 0.5 + 0.5) * c->vh;
		out[i][2] = 0;
	}
	return cnt;
}

/* window space convex polygon: is the pixel centre inside, and how far from the nearest edge (pixels) */
typedef struct
{
	float x, y, ex, ey, inv;
} refedge_t;

/* the per-pixel part runs in single precision (hardware FPU): the matrices and the clipping above stay in double */
static int ref_edges(const double (*w)[3], int n, refedge_t *e)
{
	int i, m = 0;

	for (i = 0; i < n; i++)
	{
		const double *a = w[i], *b = w[(i + 1) % n];
		double ex = b[0] - a[0], ey = b[1] - a[1], len = sqrt(ex * ex + ey * ey);

		if (len < 1e-9)
			continue;
		e[m].x = (float)a[0];
		e[m].y = (float)a[1];
		e[m].ex = (float)ex;
		e[m].ey = (float)ey;
		e[m].inv = (float)(1.0 / len);
		m++;
	}
	return m;
}

static int edges_inside(const refedge_t *e, int n, float px, float py, float *edge_dist)
{
	float best = 1e30f;
	int i, inside_pos = 1, inside_neg = 1;

	for (i = 0; i < n; i++)
	{
		float cr = (e[i].ex * (py - e[i].y) - e[i].ey * (px - e[i].x)) * e[i].inv;

		if (fabsf(cr) < best)
			best = fabsf(cr);
		if (cr < 0)
			inside_pos = 0;
		if (cr > 0)
			inside_neg = 0;
	}
	*edge_dist = best;
	return inside_pos || inside_neg;
}

static FTransform cam_default(void)
{
	FTransform t;

	memset(&t, 0, sizeof t);
	t.x = 0.0f;
	t.y = 0.0f;
	t.z = 20.0f;
	t.anglex = 0.0f;
	t.angley = 0.0f;
	t.scalex = 1.0f;
	t.scaley = (float)SW / (float)SH;
	t.scalez = 1.0f;
	t.fovxangle = 90.0f;
	return t;
}

/* polygons in 3D, flat coloured; compare the coverage with the reference pipeline */
static void draw_poly3d(const double (*p)[3], int n, u32 colour, u32 flags)
{
	FOutVector v[16];
	FSurfaceInfo s = surf_rgba(chan(colour, 0), chan(colour, 1), chan(colour, 2), 255);
	int i;

	for (i = 0; i < n; i++)
	{
		v[i].x = (float)p[i][0];
		v[i].y = (float)p[i][1];
		v[i].z = (float)p[i][2];
		v[i].s = v[i].t = 0;
	}
	D.pfnDrawPolygon(&s, v, n, flags | PF_NoTexture | PF_Modulated);
}

static int compare_coverage(const refcam_t *c, const double (*polys)[8][3], const int *counts, const u32 *cols, int npoly, u32 bg, double tol, int *skipped, int *compared)
{
	int x, y, bad = 0, i, nw[8], ne[8];
	double w[8][16][3];
	refedge_t ed[8][16];

	for (i = 0; i < npoly; i++)
	{
		nw[i] = ref_clip(c, (const double (*)[3])polys[i], counts[i], w[i]);
		ne[i] = nw[i] >= 3 ? ref_edges((const double (*)[3])w[i], nw[i], ed[i]) : 0;
	}
	*skipped = *compared = 0;
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++)
		{
			u32 want = bg;
			float minedge = 1e30f;
			int skip = 0;

			for (i = 0; i < npoly; i++)
			{
				float d;

				if (ne[i] < 3)
					continue;
				if (edges_inside(ed[i], ne[i], (float)x + 0.5f, (float)y + 0.5f, &d))
					want = cols[i]; /* later polygons overdraw (no depth test in these scenes) */
				if (d < minedge)
					minedge = d;
			}
			if (minedge < (float)tol)
				skip = 1;
			if (skip)
			{
				(*skipped)++;
				continue;
			}
			(*compared)++;
			bad += fb[y * FW + x] != quant(want);
		}
	return bad;
}

static void test_transform(const char *name, int shear_like, float angley, float anglex)
{
	FTransform t = cam_default();
	refcam_t c;
	double polys[8][8][3];
	int counts[8], npoly = 0, r, bad, skipped, compared;
	u32 cols[8];

	(void)shear_like;
	t.angley = angley;
	t.anglex = anglex;
	ref_setup(&c, &t, 0.9, 0, 0, SW, SH);
	/* a triangle ahead of the camera, a quad crossing the near plane (camera inside it), a big quad crossing every screen edge */
	{
		/* view direction for angley=0 is +x (world), up is +y; the camera sits at (0, 20, 0) in world coordinates (x, z, y) */
		double a[3][3] = {{100, 0, -60}, {100, 60, 0}, {140, 10, 70}};
		double b[4][3] = {{-5, 5, -60}, {-5, 5, 60}, {300, 5, 60}, {300, 5, -60}}; /* the ground ahead: crosses the near plane under the camera */
		double d[4][3] = {{60, -400, -400}, {60, 400, -400}, {60, 400, 400}, {60, -400, 400}};
		int i, k;

		for (i = 0; i < 3; i++)
			for (k = 0; k < 3; k++)
				polys[0][i][k] = a[i][k];
		counts[0] = 3;
		cols[0] = rgb(255, 0, 0);
		for (i = 0; i < 4; i++)
			for (k = 0; k < 3; k++)
			{
				polys[1][i][k] = b[i][k];
				polys[2][i][k] = d[i][k];
			}
		counts[1] = 4;
		cols[1] = rgb(0, 255, 0);
		counts[2] = 4;
		cols[2] = rgb(0, 0, 255);
		npoly = 3;
	}
	frame_start(rgb(0, 0, 0));
	D.pfnSetTransform(&t);
	{
		/* painter's order: the far wall first, then the ground, then the triangle */
		draw_poly3d((const double (*)[3])polys[2], counts[2], cols[2], PF_NoDepthTest);
		draw_poly3d((const double (*)[3])polys[1], counts[1], cols[1], PF_NoDepthTest);
		draw_poly3d((const double (*)[3])polys[0], counts[0], cols[0], PF_NoDepthTest);
	}
	{
		/* the reference draws in the same order: index 2, 1, 0 */
		double pp[3][8][3];
		int cc[3];
		u32 col[3];

		memcpy(pp[0], polys[2], sizeof pp[0]); cc[0] = counts[2]; col[0] = cols[2];
		memcpy(pp[1], polys[1], sizeof pp[1]); cc[1] = counts[1]; col[1] = cols[1];
		memcpy(pp[2], polys[0], sizeof pp[2]); cc[2] = counts[0]; col[2] = cols[0];
		r = frame_grab();
		bad = compare_coverage(&c, (const double (*)[8][3])pp, cc, col, 3, rgb(0, 0, 0), 0.2, &skipped, &compared);
	}
	CHECK(name, r == 0 && bad == 0 && compared > FW * FH / 2, "readback=%d pixels differing from the double precision reference: %d of %d (%d within 0.2 px of an edge skipped)", r, bad, compared, skipped);
	dump_ppm(name, fb);
	D.pfnSetTransform(NULL);
	(void)npoly;
}

/* two overlapping quads at different depths, drawn both ways round: the nearer one must win */
static void test_depth(const char *name, int expect_fail)
{
	FTransform t = cam_default();
	double nearq[4][3] = {{80, 10, -40}, {80, 10, 40}, {80, 70, 40}, {80, 70, -40}};
	double farq[4][3] = {{160, -20, -60}, {160, -20, 60}, {160, 100, 60}, {160, 100, -60}};
	u32 cn = rgb(255, 0, 0), cf = rgb(0, 255, 0);
	int order, bad_total = 0, r = 0, n = 0, x, y;
	refcam_t c;
	double w[16][3];
	refedge_t ed[16];
	int nw, ne;

	ref_setup(&c, &t, 0.9, 0, 0, SW, SH);
	nw = ref_clip(&c, (const double (*)[3])nearq, 4, w);
	ne = ref_edges((const double (*)[3])w, nw, ed);
	for (order = 0; order < 2; order++)
	{
		int bad = 0;

		frame_start(rgb(0, 0, 0));
		D.pfnSetTransform(&t);
		if (order == 0)
		{
			draw_poly3d((const double (*)[3])nearq, 4, cn, PF_Occlude);
			draw_poly3d((const double (*)[3])farq, 4, cf, PF_Occlude);
		}
		else
		{
			draw_poly3d((const double (*)[3])farq, 4, cf, PF_Occlude);
			draw_poly3d((const double (*)[3])nearq, 4, cn, PF_Occlude);
		}
		r |= frame_grab();
		for (y = 0; y < FH; y++)
			for (x = 0; x < FW; x++)
			{
				float d;

				if (edges_inside(ed, ne, (float)x + 0.5f, (float)y + 0.5f, &d) && d > 1.0f)
				{
					n++;
					bad += fb[y * FW + x] != quant(cn);
				}
			}
		bad_total += bad;
		if (order == 0)
			dump_ppm(name, fb);
	}
	D.pfnSetTransform(NULL);
	if (expect_fail)
		CHECK(name, r == 0 && bad_total > 1000, "negative control: wrong pixels in the overlap with the depth feature disabled: %d (must be > 1000)", bad_total);
	else
		CHECK(name, r == 0 && bad_total == 0 && n > scale_n(2000), "readback=%d overlap pixels showing the far quad: %d of %d (both draw orders)", r, bad_total, n);
}

/* DrawIndexedTriangles must give the same picture as DrawPolygon (batching path) */
static void test_indexed(void)
{
	GLMipmap_t *m = tex_new(32, 32, GL_TEXFMT_RGBA, TF_WRAPXY);
	FSurfaceInfo s = surf_rgba(200, 220, 255, 255);
	FOutVector fan[3][5];
	static FOutVector verts[64];
	static UINT32 idx[64];
	int i, x, y, r1, r2, bad = 0, ni = 0, nv = 0;

	for (y = 0; y < 32; y++)
		for (x = 0; x < 32; x++)
			rgba_set(m, x, y, PAL, ((x >> 2) + (y >> 2) * 8) % 255, 255);
	D.pfnSetTexturePalette(PAL);
	/* three convex polygons (5 vertices) with s,t beyond 1 (repeat) and one that leaves the screen */
	for (i = 0; i < 3; i++)
	{
		float cx = -0.5f + 0.5f * (float)i, cy = i == 2 ? 0.9f : 0.0f; /* the last polygon crosses the top edge */
		int k;

		for (k = 0; k < 5; k++)
		{
			float a = (float)k * 6.2831853f / 5.0f;

			fan[i][k].x = cx + 0.28f * cosf(a);
			fan[i][k].y = cy + 0.4f * sinf(a);
			fan[i][k].z = 1.0f;
			fan[i][k].s = 3.0f + 2.0f * cosf(a) + (float)i;
			fan[i][k].t = -2.0f + 2.0f * sinf(a);
		}
	}
	frame_start(rgb(5, 5, 5));
	D.pfnSetTexture(m);
	for (i = 0; i < 3; i++)
		D.pfnDrawPolygon(&s, fan[i], 5, PF_Masked | PF_Modulated | PF_NoDepthTest);
	r1 = frame_grab();
	memcpy(fb2, fb, (size_t)FW * FH * 4);
	for (i = 0; i < 3; i++)
	{
		int k, first = nv;

		for (k = 0; k < 5; k++)
			verts[nv++] = fan[i][k];
		for (k = 1; k < 4; k++)
		{
			idx[ni++] = (UINT32)first;
			idx[ni++] = (UINT32)(first + k);
			idx[ni++] = (UINT32)(first + k + 1);
		}
	}
	frame_start(rgb(5, 5, 5));
	D.pfnSetTexture(m);
	D.pfnDrawIndexedTriangles(&s, verts, (FUINT)ni, PF_Masked | PF_Modulated | PF_NoDepthTest, idx);
	r2 = frame_grab();
	for (i = 0; i < FW * FH; i++)
		bad += fb[i] != fb2[i];
	CHECK("indexed_equals_polygon", r1 == 0 && r2 == 0 && bad == 0, "readback=%d/%d pixels that differ between DrawPolygon fans and DrawIndexedTriangles: %d of %d", r1, r2, bad, FW * FH);
	dump_ppm("indexed", fb);
	tex_free(m);
}

static void test_palette_change(void)
{
	GLMipmap_t *m = tex_new(16, 16, GL_TEXFMT_RGBA, 0);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	int x, y, r, bad = 0;
	ps2hwd_stats_t st0, st1;

	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			rgba_set(m, x, y, PAL, y * 16 + x == 255 ? 254 : y * 16 + x, 255);
	D.pfnSetTexturePalette(PAL);
	frame_start(rgb(0, 0, 0));
	D.pfnSetTexture(m);
	quad2d(&s, 16.0f, 16.0f, 144.0f, 144.0f, 0, 0, 1, 1, PF_Masked | PF_Modulated);
	frame_grab();
	PS2HWD_GetStats(&st0, 0);
	D.pfnSetTexturePalette(PAL2); /* the engine flushes its cache; the driver only has to change the CLUT images */
	frame_start(rgb(0, 0, 0));
	D.pfnSetTexture(m);
	quad2d(&s, 16.0f, 16.0f, 144.0f, 144.0f, 0, 0, 1, 1, PF_Masked | PF_Modulated);
	r = frame_grab();
	PS2HWD_GetStats(&st1, 0);
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
		{
			int px = (int)((16.0f + (x + 0.5f) * 8.0f) * SX), py = (int)((16.0f + (y + 0.5f) * 8.0f) * SY);

			bad += fb[py * FW + px] != quant(palc(PAL2, y * 16 + x == 255 ? 254 : y * 16 + x));
		}
	CHECK("palette_change_clut_only", r == 0 && bad == 0 && st1.uploads == st0.uploads, "readback=%d cells with the old palette: %d of 256; texture uploads during the change: %u (must be 0)", r, bad, st1.uploads - st0.uploads);
	D.pfnSetTexturePalette(PAL);
	tex_free(m);
}

/* many textures, far more than the pool holds: LRU eviction, texture handles cleared, re-upload on demand */
static void test_eviction(void)
{
	enum { N = 240, TS = 128 }; // 3.75 MiB of indexed storage exceeds both CT32 and CT16S fixture pools
	GLMipmap_t **t = calloc(N, sizeof *t);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	int i, pass, r = 0, bad = 0, evicted = 0;
	ps2hwd_stats_t st0, st1;
	int cols = 20;

	D.pfnSetTexturePalette(PAL);
	for (i = 0; i < N; i++)
	{
		int x, y;

		t[i] = tex_new(TS, TS, GL_TEXFMT_RGBA, 0);
		for (y = 0; y < TS; y++)
			for (x = 0; x < TS; x++)
				rgba_set(t[i], x, y, PAL, i % 255, 255); /* one colour per texture: index i */
	}
	PS2HWD_GetStats(&st0, 0);
	for (pass = 0; pass < 3; pass++)
	{
		frame_start(rgb(0, 0, 0));
		for (i = 0; i < N; i++)
		{
			int cx = (i % cols) * 16, cy = (i / cols) * 16;

			D.pfnSetTexture(t[i]);
			quad2d(&s, (float)cx, (float)cy, (float)(cx + 15), (float)(cy + 15), 0, 0, 1, 1, PF_Masked | PF_Modulated);
		}
		r |= frame_grab();
		for (i = 0; i < N; i++)
		{
			int cx = (i % cols) * 16, cy = (i / cols) * 16;
			int px = (int)((cx + 7.5f) * SX), py = (int)((cy + 7.5f) * SY);

			bad += fb[py * FW + px] != quant(palc(PAL, i % 255));
		}
		for (i = 0; i < N; i++)
			evicted += t[i]->downloaded == 0;
		if (pass == 0)
			dump_ppm("evict", fb);
	}
	PS2HWD_GetStats(&st1, 0);
	CHECK("texture_eviction", r == 0 && bad == 0 && st1.evictions > st0.evictions, "readback=%d wrong cells=%d of %d (3 passes); evictions=%u uploads=%u textures without a VRAM copy after the last pass=%d", r, bad, N,
		st1.evictions - st0.evictions, st1.uploads - st0.uploads, evicted);
	PS2HWD_GetInfo(&INFO);
	printf("H0 info pool used %u of %u blocks, texture bytes reported %d\n", INFO.pool_used_blocks, INFO.pool_blocks, (int)D.pfnGetTextureUsed());
	for (i = 0; i < N; i++)
		tex_free(t[i]);
	free(t);
}


/* the new frame starts as a copy of the previous one (the engine relies on that after a page flip): draw a little on top */
static void test_keep_frame(void)
{
	FSurfaceInfo sr = surf_rgba(255, 0, 0, 255), sb = surf_rgba(0, 0, 255, 255);
	int r, x, y, bad = 0, n = 0;

	frame_start(rgb(0, 0, 0));
	quad2d(&sr, 0, 0, 320, 200, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
	D.pfnFinishUpdate(0);
	D.pfnGClipRect(0, 0, SW, SH, 0.9f);
	D.pfnSetTransform(NULL);
	quad2d(&sb, 100, 50, 200, 120, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest); /* no clear: the red must still be there */
	r = frame_grab();
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++)
		{
			float cx = (x + 0.5f) / SX, cy = (y + 0.5f) / SY;
			u32 want;

			if (fabsf(cx - 100) < 1 || fabsf(cx - 200) < 1 || fabsf(cy - 50) < 1 || fabsf(cy - 120) < 1)
				continue;
			want = (cx >= 100 && cx < 200 && cy >= 50 && cy < 120) ? rgb(0, 0, 255) : rgb(255, 0, 0);
			n++;
			bad += fb[y * FW + x] != quant(want);
		}
	CHECK("keep_previous_frame", r == 0 && bad == 0, "readback=%d pixels that are not the previous frame plus the new quad: %d of %d", r, bad, n);
	/* and a frame that starts with a colour clear must not show the old picture */
	frame_start(rgb(0, 255, 0));
	r = frame_grab();
	for (y = 0, bad = 0; y < FH * FW; y++)
		bad += fb[y] != quant(rgb(0, 255, 0));
	CHECK("clear_replaces_previous_frame", r == 0 && bad == 0, "pixels not green after a clear: %d", bad);
}

/* bilinear versus nearest on a two-colour texture */
static void test_filter(void)
{
	GLMipmap_t *m = tex_new(16, 16, GL_TEXFMT_RGBA, 0);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	int x, y, r, mid_nearest = 0, mid_linear = 0, bad_far = 0;
	float X0 = 32.0f * SX, X1 = 160.0f * SX;
	u32 ca = palc(PAL, 10), cb = palc(PAL, 200);

	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			rgba_set(m, x, y, PAL, x < 8 ? 10 : 200, 255);
	D.pfnSetTexturePalette(PAL);
	for (r = 0; r < 2; r++)
	{
		int mid = 0;

		D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, r ? HWD_SET_TEXTUREFILTER_BILINEAR : HWD_SET_TEXTUREFILTER_POINTSAMPLED);
		frame_start(rgb(0, 0, 0));
		D.pfnSetTexture(m);
		quad2d(&s, 32, 32, 160, 96, 0, 0, 1, 1, PF_Masked | PF_Modulated);
		frame_grab();
		for (y = (int)(40 * SY); y < (int)(88 * SY); y++)
			for (x = (int)X0 + 4; x < (int)X1 - 4; x++)
			{
				float u = (x + 0.5f - X0) / (X1 - X0) * 16.0f;
				u32 c = fb[y * FW + x];

				if (u < 6.5f || u > 9.5f)
				{
					if (!(u < 6.5f ? close_to(c, quant(ca), 1) : close_to(c, quant(cb), 1)))
						bad_far++;
				}
				else if (c != quant(ca) && c != quant(cb))
					mid++;
			}
		if (r)
			mid_linear = mid;
		else
			mid_nearest = mid;
	}
	D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, HWD_SET_TEXTUREFILTER_POINTSAMPLED);
	CHECK("filter_nearest_vs_bilinear", mid_nearest == 0 && mid_linear > scale_n(500) && bad_far == 0, "pixels between the two colours near the border: nearest %d (must be 0), bilinear %d (must be > 500); wrong pixels away from it: %d", mid_nearest, mid_linear, bad_far);
	tex_free(m);
}

/* the Z buffer holds larger values for nearer surfaces, with enough resolution for 1.5 units at a distance of 3000 */
/* the VU0 vertex transform against the scalar one (driver hook PS2HWD_TestVU0): outcodes, X/Y/Z integers, 1/w; and the cycles of both */
static void test_vu0(void)
{
	ps2hwd_vu0test_t r;
	unsigned int seed, n = 0, ocd = 0, dxn = 0, dyn = 0, dzn = 0, cs = 0, cv = 0;
	int mdx = 0, mdy = 0, mdz = 0, have = 0;
	float mq = 0.0f;

	for (seed = 1; seed <= 12; seed++)
	{
		PS2HWD_TestVU0(4096, seed, &r);
		have = r.have_vu0;
		n += r.n;
		ocd += r.oc_diff;
		dxn += r.dx_nonzero; dyn += r.dy_nonzero; dzn += r.dz_nonzero;
		if (r.max_dx > mdx) mdx = r.max_dx;
		if (r.max_dy > mdy) mdy = r.max_dy;
		if (r.max_dz > mdz) mdz = r.max_dz;
		if (r.max_q_rel > mq) mq = r.max_q_rel;
		cs += r.cyc_scalar;
		cv += r.cyc_vu0;
	}
	printf("H0 vu0 n=%u outcode_diff=%u dx_nonzero=%u dy_nonzero=%u dz_nonzero=%u max_dx=%d max_dy=%d max_dz=%d max_q_rel=%g cycles/vertex scalar=%u vu0=%u\n",
		n, ocd, dxn, dyn, dzn, mdx, mdy, mdz, (double)mq, cs / (n ? n : 1), cv / (n ? n : 1));
	CHECK("vu0_transform", have && n >= 40000 && ocd * 2000 <= n && mdx <= 1 && mdy <= 1 && mdz <= 8 && mq < 2e-6f,
		"%u vertices, %u outcode differences, max dX %d dY %d dZ %d, max 1/w relative error %g", n, ocd, mdx, mdy, mdz, (double)mq);
}

static void test_depth_resolution(void)
{
	FTransform t = cam_default();
	double a[4][3] = {{3000, -200, -200}, {3000, -200, 200}, {3000, 200, 200}, {3000, 200, -200}};
	double b[4][3] = {{3001.5, -200, -200}, {3001.5, -200, 200}, {3001.5, 200, 200}, {3001.5, 200, -200}};
	int order, bad = 0, r = 0, zs_ok = 0;

	for (order = 0; order < 2; order++)
	{
		frame_start(rgb(0, 0, 0));
		D.pfnSetTransform(&t);
		if (order == 0)
		{
			draw_poly3d((const double (*)[3])b, 4, rgb(0, 255, 0), PF_Occlude);
			draw_poly3d((const double (*)[3])a, 4, rgb(255, 0, 0), PF_Occlude);
		}
		else
		{
			draw_poly3d((const double (*)[3])a, 4, rgb(255, 0, 0), PF_Occlude);
			draw_poly3d((const double (*)[3])b, 4, rgb(0, 255, 0), PF_Occlude);
		}
		r |= frame_grab();
		r |= depth_grab();
		bad += fb[(FH / 2) * FW + FW / 2] != quant(rgb(255, 0, 0)); /* the nearer one wins in both orders */
		zs_ok += zb[(FH / 2) * FW + FW / 2] > 0;
	}
	D.pfnSetTransform(NULL);
	CHECK("depth_resolution_1p5_at_3000", r == 0 && bad == 0 && zs_ok == 2, "readback=%d wrong winner in %d of 2 orders; Z value at the centre %u (24 bit)", r, bad, (unsigned)zb[(FH / 2) * FW + FW / 2]);
}

/* textures of very different shapes and formats packed back to back: each one must come out intact (footprint, alignment) */
static void test_pool_pack(void)
{
	static const int sizes[][2] = {{16, 16}, {32, 16}, {16, 32}, {64, 16}, {16, 64}, {128, 32}, {32, 128}, {256, 16}, {16, 256}, {64, 64}, {256, 64}, {64, 256}, {128, 128}, {512, 32}, {32, 512}, {1024, 16}, {16, 1024},
		{200, 50}, {48, 96}, {320, 200}};
	enum { NS = sizeof sizes / sizeof sizes[0] };
	GLMipmap_t *t[NS * 3];
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	int i, k, r, bad = 0, n = 0, total = 0, badk[3] = {0, 0, 0}, shown = 0, skipped = 0;

	D.pfnSetTexturePalette(PAL);
	for (k = 0; k < 3; k++)
		for (i = 0; i < NS; i++)
		{
			int w = sizes[i][0], h = sizes[i][1], x, y, id = k * NS + i;

			t[id] = tex_new(w, h, GL_TEXFMT_RGBA, TF_WRAPXY);
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
				{
					int idx = (x * 7 + y * 13 + id * 29) % 255;

					if (k == 0)
						rgba_set(t[id], x, y, PAL, idx, 255); /* palette colours: PSMT8 */
					else if (k == 1)
						((u32 *)t[id]->data)[y * w + x] = rgb((x * 5 + id) & 255, (y * 3 + id * 11) & 255, (x ^ y) & 255) | 0xFF000000u; /* arbitrary colours, opaque: CT16 */
					else
						((u32 *)t[id]->data)[y * w + x] = rgb((x * 3) & 255, (y * 5) & 255, (id * 7) & 255) | ((u32)((x + y) & 255) << 24); /* partial alpha: CT32 */
				}
		}
	frame_start(rgb(0, 0, 0));
	for (i = 0; i < NS * 3; i++)
	{
		int cx = (i % 10) * 32, cy = (i / 10) * 32;

		D.pfnSetTexture(t[i]);
		quad2d(&s, (float)cx, (float)cy, (float)(cx + 30), (float)(cy + 30), 0, 0, 1, 1, i >= 2 * NS ? PF_Translucent | PF_Modulated : PF_Masked | PF_Modulated);
	}
	r = frame_grab();
	for (i = 0; i < NS * 3; i++)
	{
		int k2 = i / NS, w = t[i]->width, h = t[i]->height, id = i, gx, gy;
		int cx = (i % 10) * 32, cy = (i / 10) * 32;

		for (gy = 0; gy < 5; gy++)
			for (gx = 0; gx < 5; gx++)
			{
				float fu = (gx + 0.5f) / 5.0f, fv = (gy + 0.5f) / 5.0f;
				int px = (int)((cx + 1.0f + fu * 28.0f) * SX), py = (int)((cy + 1.0f + fv * 28.0f) * SY);
				int tx, ty;
				u32 c = fb[py * FW + px], want;
				int ok;

				/* textures keep their exact size: the texel is the position in the quad times the size */
				{
					float u = ((px + 0.5f) / SX - (float)cx) / 30.0f * (float)w;
					float v = ((py + 0.5f) / SY - (float)cy) / 30.0f * (float)h;
					// GS vertex coordinates are 1/16 pixel: the sampling position is uncertain by that times the minification
					float mx = 0.06f + 0.04f * ((float)w / (30.0f * SX)), my = 0.06f + 0.04f * ((float)h / (30.0f * SY));

					mx = mx > 0.45f ? 0.45f : mx;
					my = my > 0.45f ? 0.45f : my;
					if (u - floorf(u) < mx || u - floorf(u) > 1.0f - mx || v - floorf(v) < my || v - floorf(v) > 1.0f - my)
					{
						skipped++;
						continue;
					}
					tx = (int)u; ty = (int)v;
				}

				if (k2 == 0)
				{
					want = quant(palc(PAL, (tx * 7 + ty * 13 + id * 29) % 255));
					ok = c == want;
				}
				else if (k2 == 1)
				{
					want = rgb((tx * 5 + id) & 255, (ty * 3 + id * 11) & 255, (tx ^ ty) & 255);
					ok = close_to(c, quant(want), FB32 ? 1 : 9); /* opaque direct colour: CT32 */
				}
				else
				{
					int a = (tx + ty) & 255, as = (a * 128 + 127) / 255, cc;
					u32 src = rgb((tx * 3) & 255, (ty * 5) & 255, (id * 7) & 255);

					ok = 1;
					for (cc = 0; cc < 3; cc++)
					{
						int cs = chan(src, cc), v = (cs * as) >> 7; /* over black */

						if (abs(chan(c, cc) - (FB32 ? v : (v & ~7))) > (FB32 ? 2 : 9))
							ok = 0;
					}
				}
				n++;
				bad += !ok;
				badk[k2] += !ok;
				if (!ok && shown < 8)
				{
					shown++;
					printf("H0 info pool_pack mismatch: texture %d (%dx%d, format %d) texel (%d,%d): got %06x alpha=%d expectedRGB=%d,%d,%d\n", i, w, h, k2, tx, ty, (unsigned)c,
						(tx+ty)&255, 0, 0, 0);
				}
			}
		total += (int)((u32)w * h);
	}
	PS2HWD_GetInfo(&INFO);
	CHECK("pool_pack_mixed_shapes", r == 0 && bad == 0, "readback=%d wrong samples %d of %d (PSMT8 %d, CT16 %d, CT32 %d) over %d textures (%d texels, sizes 16x16..1024x16, 320x200), pool used %u of %u blocks", r, bad, n, badk[0], badk[1], badk[2], NS * 3, total,
		INFO.pool_used_blocks, INFO.pool_blocks);
	printf("H0 info pool_pack skipped %d samples within 0.06 texel of boundaries\n", skipped);
	dump_ppm("pool", fb);
	for (i = 0; i < NS * 3; i++)
		tex_free(t[i]);
}

/* paced frames: the vblank handler has to flip every one of them */
static void test_flip(void)
{
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	ps2hwd_stats_t a, b;
	int i, d0, d1, alt = 0;

	PS2HWD_Sync();
	PS2HWD_GetStats(&a, 0);
	PS2HWD_GetInfo(&INFO);
	d0 = INFO.displayed;
	for (i = 0; i < 30; i++)
	{
		frame_start(rgb(i * 8, 0, 0));
		quad2d(&s, 10.0f + (float)i, 10, 40.0f + (float)i, 40, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
		D.pfnFinishUpdate(1); /* waits for a vblank */
		PS2HWD_GetInfo(&INFO);
		d1 = INFO.displayed;
		alt += d1 != d0;
		d0 = d1;
	}
	PS2HWD_GetStats(&b, 0);
	CHECK("flip_every_paced_frame", b.flips - a.flips >= 28 && alt >= 28 && b.dropped - a.dropped <= 1, "30 frames with FinishUpdate(vsync): flips=%u buffer changes seen=%d dropped=%u vblanks=%u", b.flips - a.flips, alt, b.dropped - a.dropped, b.vblanks - a.vblanks);
}

/* many tiny polygons: forces the DMA ring through all its buffers */
static void test_ring_stress(void)
{
	FSurfaceInfo s;
	int cx, cy, r, bad = 0, n = 0, bx = 100, by = 80;
	ps2hwd_stats_t st0, st1;
	u32 t0, t1;

	PS2HWD_GetStats(&st0, 0);
	frame_start(rgb(0, 0, 0));
	t0 = cyc();
	for (cy = 0; cy < by; cy++)
		for (cx = 0; cx < bx; cx++)
		{
			u32 h = (u32)(cx * 2654435761u) ^ (u32)(cy * 40503u);

			s = surf_rgba((int)(h & 255), (int)((h >> 8) & 255), (int)((h >> 16) & 255), 255);
			quad2d(&s, (float)cx * 3.2f, (float)cy * 2.5f, (float)cx * 3.2f + 3.2f, (float)cy * 2.5f + 2.5f, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
		}
	t1 = cyc();
	r = frame_grab();
	PS2HWD_GetStats(&st1, 0);
	for (cy = 0; cy < by; cy++)
		for (cx = 0; cx < bx; cx++)
		{
			u32 h = (u32)(cx * 2654435761u) ^ (u32)(cy * 40503u);
			int px = (int)(((float)cx * 3.2f + 1.6f) * SX), py = (int)(((float)cy * 2.5f + 1.25f) * SY);

			n++;
			bad += fb[py * FW + px] != quant(rgb((int)(h & 255), (int)((h >> 8) & 255), (int)((h >> 16) & 255)));
		}
	CHECK("ring_stress_8000_quads", r == 0 && bad == 0 && st1.dma_kicks - st0.dma_kicks >= 4, "readback=%d wrong cells=%d of %d; DMA kicks=%u qwords=%u EE cycles for the 8000 DrawPolygon calls=%u (%u per polygon)", r, bad, n,
		st1.dma_kicks - st0.dma_kicks, st1.qwords - st0.qwords, (unsigned)(t1 - t0), (unsigned)((t1 - t0) / (u32)n));
}

static void test_screen_textures(void)
{
	FSurfaceInfo sa = surf_rgba(255, 0, 0, 255), sb = surf_rgba(0, 0, 255, 255), sg = surf_rgba(0, 255, 0, 255);
	int r, bad = 0, n = 0, x, y;
	GLMipmap_t *mask = tex_new(64, 64, GL_TEXFMT_ALPHA_8, 0);

	/* picture 1: left half red, a green box; make it screen texture 0 (wipe start); picture 2: blue; wipe end */
	frame_start(rgb(0, 0, 0));
	quad2d(&sa, 0, 0, 320, 200, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
	quad2d(&sg, 60, 40, 140, 120, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
	D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_START);
	frame_grab();
	memcpy(fb2, fb, (size_t)FW * FH * 4);
	frame_start(rgb(0, 0, 0));
	quad2d(&sb, 0, 0, 320, 200, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
	D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_END);
	/* the screen texture comes back (DrawScreenTexture clears first): flat areas are exact, edges blend */
	D.pfnDrawScreenTexture(HWD_SCREENTEXTURE_WIPE_START, NULL, 0);
	r = frame_grab();
	for (y = 8; y < FH - 8; y++)
		for (x = 8; x < FW - 8; x++)
		{
			float cx = (x + 0.5f) / SX, cy = (y + 0.5f) / SY;
			u32 want;
			int near_edge = (fabsf(cx - 60) < 2 || fabsf(cx - 140) < 2) && cy > 38 && cy < 122;

			near_edge |= (fabsf(cy - 40) < 2 || fabsf(cy - 120) < 2) && cx > 58 && cx < 142;
			if (near_edge)
				continue;
			want = (cx >= 60 && cx < 140 && cy >= 40 && cy < 120) ? rgb(0, 255, 0) : rgb(255, 0, 0);
			n++;
			bad += !close_to(fb[y * FW + x], quant(want), FB32 ? 1 : 8);
		}
	CHECK("screen_texture_roundtrip", r == 0 && bad == 0 && n > scale_n(100000), "readback=%d pixels off the original picture: %d of %d (edges of the box skipped)", r, bad, n);
	{
		UINT8 *capture = malloc(SW * SH * 3);
		int slotbad = 0, statebad = 0;
		memcpy(fb2, fb, (size_t)FW * FH * 4);
		memset(capture, 0xA5, SW * SH * 3);
		D.pfnReadScreenTexture(HWD_SCREENTEXTURE_WIPE_END, capture);
		for (x = 0; x < SW * SH; x++)
			slotbad += capture[x * 3] != 0 || capture[x * 3 + 1] != 0 || capture[x * 3 + 2] != 255;
		D.pfnReadScreenTexture(HWD_SCREENTEXTURE_WIPE_START, capture);
		for (y = 8; y < SH - 8; y++)
			for (x = 8; x < SW - 8; x++)
			{
				u32 want = x > 62 && x < 138 && y > 42 && y < 118 ? rgb(0,255,0) : rgb(255,0,0);
				if (abs(x-60) < 3 || abs(x-140) < 3 || abs(y-40) < 3 || abs(y-120) < 3) continue;
				slotbad += capture[(y*SW+x)*3] != (UINT8)want || capture[(y*SW+x)*3+1] != (UINT8)(want>>8) || capture[(y*SW+x)*3+2] != (UINT8)(want>>16);
			}
		r = PS2HWD_ReadFrame(fb);
		for (x = 0; x < FW * FH; x++) statebad += fb[x] != fb2[x];
		CHECK("screen_readback_selected_slot", r == 0 && slotbad == 0 && statebad == 0,
			"two distinct saved slots: wrong RGB=%d; current framebuffer changed=%d of %d", slotbad, statebad, FW*FH);
		free(capture);
	}
	dump_ppm("screen", fb);
	/* wipe: mask left half set (255), right half 0: left shows the end picture (blue), right the start picture (red/green) */
	{
		int xx, yy;

		for (yy = 0; yy < 64; yy++)
			for (xx = 0; xx < 64; xx++)
				((UINT8 *)mask->data)[yy * 64 + xx] = xx < 32 ? 255 : 0;
	}
	frame_start(rgb(0, 0, 0));
	D.pfnSetTexture(mask);
	D.pfnDoScreenWipe(HWD_SCREENTEXTURE_WIPE_START, HWD_SCREENTEXTURE_WIPE_END, NULL, 0);
	r = frame_grab();
	bad = n = 0;
	for (y = 8; y < FH - 8; y++)
		for (x = 8; x < FW - 8; x++)
		{
			float cx = (x + 0.5f) / SX, cy = (y + 0.5f) / SY;
			u32 want;

			if (fabsf(cx - 160) < 4)
				continue;
			if (cx < 160)
				want = rgb(0, 0, 255);
			else
			{
				if ((fabsf(cx - 60) < 2 || fabsf(cx - 140) < 2) || (fabsf(cy - 40) < 2 || fabsf(cy - 120) < 2))
					continue;
				want = rgb(255, 0, 0);
			}
			n++;
			bad += !close_to(fb[y * FW + x], quant(want), FB32 ? 1 : 8);
		}
	CHECK("screen_wipe_mask", r == 0 && bad == 0 && n > 50000, "readback=%d pixels with the wrong side of the mask: %d of %d", r, bad, n);
	dump_ppm("wipe", fb);
	tex_free(mask);
	D.pfnFlushScreenTextures();
}

static void test_sky(void)
{
	/* a tiny dome: one textured strip of 4 quads around the view plus a coloured cap; just has to draw and clip without trouble */
	static gl_skyvertex_t data[32];
	static gl_skyloopdef_t loops[2];
	gl_sky_t sky;
	GLMipmap_t *m = tex_new(32, 32, GL_TEXFMT_RGBA, TF_WRAPXY);
	FTransform t = cam_default();
	int i, x, y, r, nz = 0;

	for (y = 0; y < 32; y++)
		for (x = 0; x < 32; x++)
			rgba_set(m, x, y, PAL, (x + y * 2) % 255, 255);
	memset(&sky, 0, sizeof sky);
	for (i = 0; i <= 8; i++)
	{
		float a = (float)i * 6.2831853f / 8.0f;

		data[i * 2].x = 5000.0f * cosf(a);
		data[i * 2].z = 5000.0f * sinf(a);
		data[i * 2].y = 0.0f;
		data[i * 2].u = (float)i * 0.5f;
		data[i * 2].v = 0.0f;
		data[i * 2 + 1] = data[i * 2];
		data[i * 2 + 1].y = 3000.0f;
		data[i * 2 + 1].v = 1.0f;
		data[i * 2].r = data[i * 2].g = data[i * 2].b = data[i * 2].a = 255;
		data[i * 2 + 1].r = data[i * 2 + 1].g = data[i * 2 + 1].b = data[i * 2 + 1].a = 255;
	}
	loops[0].mode = HWD_SKYLOOP_STRIP;
	loops[0].vertexcount = 18;
	loops[0].vertexindex = 0;
	loops[0].use_texture = true;
	sky.loops = loops;
	sky.loopcount = 1;
	sky.data = data;
	sky.vertex_count = 18;
	sky.height = 200;
	D.pfnSetTexturePalette(PAL);
	frame_start(rgb(0, 0, 0));
	D.pfnSetTransform(&t);
	D.pfnSetTexture(m);
	D.pfnSetBlend(PF_Translucent | PF_NoDepthTest | PF_Modulated);
	D.pfnRenderSkyDome(&sky);
	r = frame_grab();
	for (i = 0; i < FW * FH; i++)
		nz += fb[i] != 0;
	CHECK("sky_dome_draws", r == 0 && nz > FW * FH / 8, "readback=%d non-black pixels=%d of %d", r, nz, FW * FH);
	dump_ppm("sky", fb);
	// Reusing the same camera must not accumulate the dome's scale/rotation.
	frame_start(rgb(0, 0, 0)); D.pfnSetTransform(&t); D.pfnSetTexture(m);
	D.pfnSetBlend(PF_Translucent | PF_NoDepthTest | PF_Modulated);
	D.pfnRenderSkyDome(&sky); D.pfnClearBuffer(1, 1, NULL); D.pfnRenderSkyDome(&sky);
	memcpy(fb2, fb, (size_t)FW * FH * 4); r = frame_grab();
	for (i = 0, nz = 0; i < FW * FH; i++) nz += fb[i] != fb2[i];
	CHECK("sky_transform_restored", r == 0 && nz == 0, "second dome with unchanged camera: differing pixels=%d", nz);
	D.pfnSetTransform(NULL);
	tex_free(m);
}

static void test_index_collisions(void)
{
	static FOutVector v[513];
	FOutVector compact[3] = {{-0.6f,-0.6f,1,0,0}, {0.6f,-0.6f,1,0,0}, {0,0.6f,1,0,0}};
	UINT32 ci[3] = {0,1,2}, idx[3] = {0,256,512};
	FSurfaceInfo s = surf_rgba(255,80,20,255);
	int r, i, bad = 0, drawn = 0;
	v[0] = compact[0]; v[256] = compact[1]; v[512] = compact[2];
	frame_start(0); D.pfnDrawIndexedTriangles(&s, compact, 3, PF_NoTexture|PF_Modulated|PF_Occlude, ci);
	r = frame_grab(); memcpy(fb2, fb, (size_t)FW*FH*4);
	frame_start(0); D.pfnDrawIndexedTriangles(&s, v, 3, PF_NoTexture|PF_Modulated|PF_Occlude, idx);
	r |= frame_grab();
	for (i = 0; i < FW*FH; i++) { bad += fb[i] != fb2[i]; drawn += fb[i] != 0; }
	CHECK("indexed_cache_collision", r == 0 && bad == 0 && drawn > 10000,
		"indices 0/256/512 vs compact mesh: wrong pixels=%d of %d, drawn=%d", bad, FW*FH, drawn);
}

static void test_models(void)
{
	float verts[2][9] = {{-0.5f,-0.5f,0, 0.5f,-0.5f,0, 0,0.5f,0}, {-0.25f,-0.5f,0, 0.75f,-0.5f,0, 0.25f,0.5f,0}};
	short tinyverts[2][9]; float uvs[6] = {0}; unsigned short ix[3] = {0,1,2};
	mdlframe_t frames[2] = {{0}}; tinyframe_t tinyframes[2] = {{0}};
	mesh_t mesh = {0}; model_t model = {0};
	FSurfaceInfo s = surf_rgba(40,160,240,255);
	FTransform pos = {0}; FOutVector ref[3]; UINT32 indices[3] = {0,1,2};
	int it, j, k, bad = 0, r = 0, visible = 0;
	for (j = 0; j < 2; j++)
	{
		frames[j].vertices = verts[j]; tinyframes[j].vertices = tinyverts[j];
		for (k = 0; k < 9; k++) tinyverts[j][k] = (short)(verts[j][k]*64);
	}
	model.numMeshes = 1; model.meshes = &mesh;
	mesh.numFrames = 2; mesh.numVertices = 3; mesh.numTriangles = 1; mesh.uvs = uvs; mesh.indices = ix;
	s.PolyFlags = PF_NoTexture|PF_Modulated|PF_Occlude;
	pos.y = 1;
	for (it = 0; it < 8; it++)
	{
		int tiny = it & 1, flipped = (it >> 1) & 1, hflip = (it >> 2) & 1;
		mesh.frames = tiny ? NULL : frames; mesh.tinyframes = tiny ? tinyframes : NULL;
		frame_start(0); D.pfnCreateModelVBOs(&model);
		D.pfnDrawModel(&model, 0, 4, 2, 1, &pos, 2, 2, (UINT8)flipped, (UINT8)hflip, &s);
		r |= frame_grab(); memcpy(fb2, fb, (size_t)FW*FH*4);
		for (j = 0; j < 3; j++)
		{
			ref[j].x = (verts[0][j*3] + verts[1][j*3])*0.5f;
			ref[j].y = verts[0][j*3+1] * (flipped ? -1.0f : 1.0f);
			ref[j].z = 1; ref[j].s = ref[j].t = 0;
		}
		frame_start(0);
		if (!hflip) D.pfnDrawIndexedTriangles(&s, ref, 3, s.PolyFlags|PF_Masked, indices);
		r |= frame_grab();
		for (j = 0; j < FW*FH; j++) { bad += fb[j] != fb2[j]; visible += fb2[j] != 0; }
	}
	CHECK("models_float_tiny_interpolated", r == 0 && bad == 0 && visible > 20000,
		"8 float/tiny/flipped/cull cases vs independent expanded triangles: wrong pixels=%d; drawn samples=%d", bad, visible);
}

static void test_ap88(void)
{
	GLMipmap_t *m = tex_new(16,16,GL_TEXFMT_AP_88,0);
	FSurfaceInfo s = surf_rgba(255,255,255,255);
	int x, y, r, bad = 0, n = 0;
	u32 bg = rgb(20,40,60);
	D.pfnSetTexturePalette(PAL);
	for (y = 0; y < 16; y++) for (x = 0; x < 16; x++)
	{
		((UINT8 *)m->data)[(y*16+x)*2] = (UINT8)(x & 1 ? 255 : 20);
		((UINT8 *)m->data)[(y*16+x)*2+1] = (UINT8)(y*16+x);
	}
	frame_start(bg); D.pfnSetTexture(m);
	quad2d(&s,32,32,288,168,0,0,1,1,PF_Translucent|PF_Modulated);
	r = frame_grab();
	for (y = 0; y < 16; y++) for (x = 0; x < 16; x++)
	{
		int px = (int)((32+(x+0.5f)*16)*SX), py = (int)((32+(y+0.5f)*8.5f)*SY), ch;
		u32 src = palc(PAL,x & 1 ? 255 : 20), want = 0;
		for (ch = 0; ch < 3; ch++) want |= (u32)blend_ch(chan(src,ch),chan(bg,ch),((y*16+x)+1)/2,0) << (ch*8);
		// Replicated RGB555 has steps of both 8 and 9, not uniformly 8.
		if (!close_to(fb[py*FW+px],quant(want),FB32 ? 1 : 9))
		{
			if (bad < 4) printf("H0 info AP88 cell %d,%d alpha=%d got=%06x want=%06x\n",x,y,y*16+x,(unsigned)fb[py*FW+px],(unsigned)quant(want));
			bad++;
		}
		n++;
	}
	CHECK("ap88_partial_alpha", r == 0 && bad == 0, "0..255 alpha + opaque palette index 255: wrong cells=%d of %d",bad,n);
	tex_free(m);
}

/* timing: polygons per second the EE side can pack, and the frame time with heavy overdraw */
static void test_perf(void)
{
	GLMipmap_t *m = tex_new(64, 64, GL_TEXFMT_RGBA, TF_WRAPXY);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	FTransform t = cam_default();
	FOutVector v[4];
	static FOutVector bv[4 * 1000];
	static UINT32 bi[6 * 1000];
	int i, n = 2000, x, y;
	u32 t0, t1, t2, t3;
	ps2hwd_stats_t st0, st1;

	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++)
			rgba_set(m, x, y, PAL, (x ^ y) % 255, 255);
	D.pfnSetTexturePalette(PAL);
	frame_start(rgb(0, 0, 0));
	D.pfnSetTransform(&t);
	D.pfnSetTexture(m);
	PS2HWD_GetStats(&st0, 0);
	t0 = cyc();
	for (i = 0; i < n; i++)
	{
		float zz = 40.0f + (float)(i % 50) * 4.0f, yy = -30.0f + (float)(i / 50) * 1.5f;

		v[0].x = zz; v[0].y = yy; v[0].z = -20.0f; v[0].s = 0; v[0].t = 1;
		v[1].x = zz; v[1].y = yy; v[1].z = 20.0f; v[1].s = 3; v[1].t = 1;
		v[2].x = zz; v[2].y = yy + 8.0f; v[2].z = 20.0f; v[2].s = 3; v[2].t = 0;
		v[3].x = zz; v[3].y = yy + 8.0f; v[3].z = -20.0f; v[3].s = 0; v[3].t = 0;
		D.pfnDrawPolygon(&s, v, 4, PF_Masked | PF_Modulated | PF_Occlude);
	}
	t1 = cyc();
	PS2HWD_Sync();
	t2 = cyc();
	PS2HWD_GetStats(&st1, 0);
	printf("H0 perf %d textured quads (3D, clipped %u, rejected %u): EE %u cycles = %u per polygon (transform+clip+pack); wait for the GS afterwards %u cycles; GIF qwords %u\n", n, st1.clipped - st0.clipped, st1.rejected - st0.rejected,
		(unsigned)(t1 - t0), (unsigned)((t1 - t0) / (u32)n), (unsigned)(t2 - t1), st1.qwords - st0.qwords);
	/* same through the indexed path in batches of 1000 quads */
	for (i = 0; i < 1000; i++)
	{
		float zz = 40.0f + (float)(i % 50) * 4.0f, yy = -30.0f + (float)(i / 50) * 1.5f;
		FOutVector *q = &bv[i * 4];

		q[0].x = zz; q[0].y = yy; q[0].z = -20.0f; q[0].s = 0; q[0].t = 1;
		q[1].x = zz; q[1].y = yy; q[1].z = 20.0f; q[1].s = 3; q[1].t = 1;
		q[2].x = zz; q[2].y = yy + 8.0f; q[2].z = 20.0f; q[2].s = 3; q[2].t = 0;
		q[3].x = zz; q[3].y = yy + 8.0f; q[3].z = -20.0f; q[3].s = 0; q[3].t = 0;
		bi[i * 6 + 0] = (UINT32)(i * 4); bi[i * 6 + 1] = (UINT32)(i * 4 + 1); bi[i * 6 + 2] = (UINT32)(i * 4 + 2);
		bi[i * 6 + 3] = (UINT32)(i * 4); bi[i * 6 + 4] = (UINT32)(i * 4 + 2); bi[i * 6 + 5] = (UINT32)(i * 4 + 3);
	}
	frame_start(rgb(0, 0, 0));
	D.pfnSetTransform(&t);
	D.pfnSetTexture(m);
	PS2HWD_GetStats(&st0, 0);
	t0 = cyc();
	D.pfnDrawIndexedTriangles(&s, bv, 6000, PF_Masked | PF_Modulated | PF_Occlude, bi);
	D.pfnDrawIndexedTriangles(&s, bv, 6000, PF_Masked | PF_Modulated | PF_Occlude, bi);
	t1 = cyc();
	PS2HWD_Sync();
	t2 = cyc();
	PS2HWD_GetStats(&st1, 0);
	printf("H0 perf 4000 triangles of 2 indexed batches: EE %u cycles = %u per triangle; GS wait afterwards %u; qwords %u\n", (unsigned)(t1 - t0), (unsigned)((t1 - t0) / 4000u), (unsigned)(t2 - t1), st1.qwords - st0.qwords);
	/* overdraw: 20 full-screen textured quads */
	frame_start(rgb(0, 0, 0));
	D.pfnSetTexture(m);
	t0 = cyc();
	for (i = 0; i < 20; i++)
		quad2d(&s, 0, 0, 320, 200, 0, 0, 4, 4, PF_Masked | PF_Modulated | PF_NoDepthTest);
	t1 = cyc();
	PS2HWD_Sync();
	t2 = cyc();
	t3 = t2 - t1;
	printf("H0 perf 20 full-screen textured quads (%dx%d): EE submit %u cycles; GS time after submit %u cycles (%u us at 294.912 MHz, not a hardware measurement under PCSX2)\n", FW, FH, (unsigned)(t1 - t0), (unsigned)t3, (unsigned)(t3 / 295u));
	frame_grab();
	tex_free(m);
}

/* a few hundred frames of mixed content, a read-back compare of every Nth against an immediate re-render */
static void test_soak(int frames)
{
	GLMipmap_t *m[6];
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	FTransform t = cam_default();
	int f, i, mism = 0, checked = 0, x, y;
	u32 t0 = cyc(), vb0, maxf = 0;
	ps2hwd_stats_t st0, st1;

	for (i = 0; i < 6; i++)
	{
		m[i] = tex_new(64, 64, GL_TEXFMT_RGBA, TF_WRAPXY);
		for (y = 0; y < 64; y++)
			for (x = 0; x < 64; x++)
				rgba_set(m[i], x, y, i & 1 ? PAL2 : PAL, (x * (i + 1) + y * 3 + i * 17) % 255, (x + y + i) % 11 == 0 ? 0 : 255);
	}
	D.pfnSetTexturePalette(PAL);
	PS2HWD_GetStats(&st0, 0);
	vb0 = st0.vblanks;
	for (f = 0; f < frames; f++)
	{
		int render;

		/* every 25th frame is drawn twice (second time into the same buffer after the first was read): identical */
		for (render = 0; render < ((f % 25 == 24) ? 2 : 1); render++)
		{
			u32 f0 = cyc(), d;

			frame_start(rgb(f & 255, 20, 40));
			t.angley = (float)(f * 3);
			t.x = 10.0f * sinf((float)f * 0.05f);
			D.pfnSetTransform(&t);
			for (i = 0; i < 6; i++)
			{
				int k;

				D.pfnSetTexture(m[i]);
				for (k = 0; k < 8; k++)
				{
					FOutVector v[4];
					float zz = 60.0f + (float)i * 25.0f, yy = -30.0f + (float)k * 9.0f;

					v[0].x = zz; v[0].y = yy; v[0].z = -50.0f; v[0].s = 0; v[0].t = 1;
					v[1].x = zz; v[1].y = yy; v[1].z = 50.0f; v[1].s = 2; v[1].t = 1;
					v[2].x = zz; v[2].y = yy + 8.0f; v[2].z = 50.0f; v[2].s = 2; v[2].t = 0;
					v[3].x = zz; v[3].y = yy + 8.0f; v[3].z = -50.0f; v[3].s = 0; v[3].t = 0;
					D.pfnDrawPolygon(&s, v, 4, (i & 1 ? PF_Translucent : PF_Masked) | PF_Modulated | PF_Occlude);
				}
			}
			D.pfnSetTransform(NULL);
			D.pfnSetTexture(NULL);
			quad2d(&s, 4, 4, 30, 12, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
			D.pfnFinishUpdate(f % 3 == 0 ? 1 : 0);
			d = cyc() - f0;
			if (d > maxf)
				maxf = d;
			if (f % 25 == 24)
			{
				if (render == 0)
				{
					PS2HWD_ReadFrame(fb);
					memcpy(fb2, fb, (size_t)FW * FH * 4);
				}
				else
				{
					int q;

					PS2HWD_ReadFrame(fb);
					for (q = 0; q < FW * FH; q++)
						mism += fb[q] != fb2[q];
					checked++;
				}
			}
		}
	}
	PS2HWD_Sync();
	PS2HWD_GetStats(&st1, 0);
	CHECK("soak_frames", f == frames && st1.timeouts == st0.timeouts && mism == 0 && checked >= frames / 25 - 1, "frames=%d repeat-render compares=%d mismatching pixels=%d timeouts=%u dropped=%u frame_waits=%u dma_waits=%u", f, checked, mism,
		st1.timeouts - st0.timeouts, st1.dropped - st0.dropped, st1.frame_waits - st0.frame_waits, st1.dma_waits - st0.dma_waits);
	printf("H0 timing soak: %u EE cycles total for %d frames (%u vblanks), slowest frame submit+finish %u cycles; flips=%u uploads=%u evictions=%u\n", (unsigned)(cyc() - t0), f, st1.vblanks - vb0, (unsigned)maxf, st1.flips - st0.flips, st1.uploads - st0.uploads, st1.evictions - st0.evictions);
	for (i = 0; i < 6; i++)
		tex_free(m[i]);
}

#include "hw_test_ext.inc"

int main(int argc, char **argv)
{
	int negctl, soak, quick, cycles;
	ps2hwd_config_t c;
	ps2hwd_stats_t st;

	g_argc = argc;
	g_argv = argv;
	negctl = argval("negctl", 0);
	ps2hwd_dbg_flags = negctl;
	soak = argval("soak", 600);
	quick = arg("quick");
	dump_on = arg("dump");
	printf("H0 hello argc=%d fb16=%d negctl=%d soak=%d quick=%d dump=%d\n", argc, arg("fb16"), negctl, soak, quick, dump_on);
	make_pals();

	for (cycles = 0; cycles < 2; cycles++) /* the second round proves Shutdown/Init leaves nothing behind */
	{
		size_t heap0 = (size_t)mallinfo().uordblks;

		PS2HWD_GetConfig(&c);
		c.video = PS2HWD_VIDEO_NTSC;
		c.fb32 = arg("fb16") ? 0 : 1; /* CT32 is the default; CT16S is the opt-in lossy mode */
		c.dither = 0;
		PS2HWD_Configure(&c);
		PS2HWD_FillDriver(&D);
		CHECK("init", D.pfnInit() == true, "round %d", cycles);
		PS2HWD_SetScreenSize(SW, SH);
		PS2HWD_GetInfo(&INFO);
		FW = INFO.vw;
		FH = INFO.vh;
		FB32 = INFO.fb32;
		SX = (float)FW / SW;
		SY = (float)FH / SH;
		if (!fb)
		{
			fb = memalign(64, (size_t)FW * FH * 4);
			fb2 = memalign(64, (size_t)FW * FH * 4);
			zb = memalign(64, (size_t)FW * FH * 4);
		}
		if (cycles == 1)
		{
			test_clear();
			D.pfnShutdown();
			CHECK("shutdown_leak", (size_t)mallinfo().uordblks <= heap0 + 4096, "heap in use before the first Init %u, after the second Shutdown %u", (unsigned)heap0, (unsigned)mallinfo().uordblks);
			break;
		}
		test_layout();
		test_clear();
		test_flat();
		test_tex_map("tex_nearest_repeat", 1);
		test_tex_map("tex_nearest_clamp", 0);
		test_clut("clut_all_256", negctl == 16);
		if (negctl == 16)
		{
			/* with the swap disabled the normal test must fail; the negative-control line above already requires it */
		}
		test_keyed();
		test_blend("blend_translucent", PF_Translucent, 0, 128, 0, 0, 255, 0);
		test_blend("blend_translucent_25", PF_Translucent, 0, 64, 255, 255, 0, 0);
		test_blend("blend_additive", PF_Additive, 1, 128, 100, 50, 25, 0);
		test_blend("blend_reverse_subtract", PF_ReverseSubtract, 2, 255, 60, 30, 10, 0);
		test_blend("blend_translucent_ctl", PF_Translucent, 0, 128, 0, 0, 255, negctl == 4);
		test_transform("transform_view0", 0, 0.0f, 0.0f);
		test_transform("transform_view_yaw40_pitch10", 0, 40.0f, 10.0f);
		test_transform("transform_view_yaw200", 0, 200.0f, -5.0f);
		test_vu0();
		test_depth("depth_order", negctl == 1 || negctl == 8);
		test_depth_resolution();
		test_keep_frame();
		test_filter();
		test_indexed();
		test_index_collisions();
		test_models();
		test_ap88();
		test_palette_change();
		test_sky();
		test_screen_textures();
		test_final_screen();
		test_perspective_texture();
		test_capture_spill();
		if (FB32) test_continuous_wipes();
		if (arg("ext")) test_ext_all();
		test_pool_pack();
		test_eviction();
		test_flip();
		if (!quick)
		{
			test_ring_stress();
			test_perf();
			test_soak(soak);
		}
		PS2HWD_GetStats(&st, 0);
		printf("H0 stats frames=%u polys=%u verts_in=%u verts_out=%u clipped=%u rejected=%u qwords=%u state_writes=%u uploads=%u (%u bytes) evictions=%u tex_missing=%u dma_kicks=%u dma_waits=%u frame_waits=%u dropped=%u flips=%u vblanks=%u timeouts=%u\n",
			st.frames, st.polys, st.verts_in, st.verts_out, st.clipped, st.rejected, st.qwords, st.state_writes, st.uploads, st.upload_bytes, st.evictions, st.tex_missing, st.dma_kicks, st.dma_waits, st.frame_waits, st.dropped,
			st.flips, st.vblanks, st.timeouts);
		PS2HWD_GetInfo(&INFO);
		printf("H0 stats pool used %u of %u blocks\n", INFO.pool_used_blocks, INFO.pool_blocks);
		printf("H0 budget EE state=%u ring=%u records=%u scratch=%u sky=%u bytes; limitation calls=%u mask=0x%x\n",
			INFO.state_bytes,INFO.ring_bytes,INFO.record_bytes,INFO.scratch_bytes,INFO.sky_bytes,st.unsupported_calls,st.unsupported_mask);
		printf("H0 budget screen current=%u peak=%u spills=%u restores=%u\n", INFO.screen_bytes, INFO.screen_peak_bytes, st.screen_spills, st.screen_restores);
		CHECK("no_timeouts", st.timeouts == 0, "bounded waits that ran out: %u", st.timeouts);
		D.pfnShutdown();
	}
	printf("H0 COMPLETE checks=%d failures=%d\n", checks, failures);
	for (;;)
		SleepThread();
	return failures;
}
