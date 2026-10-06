/* Host test of src/ps2/ps2_vmodes.h (the video mode tables and arithmetic of the PS2 port).
 * Independent checks: the CRTC values against a straight copy of gsKit_set_buffer_attributes (gsKit 1.5.1), the VRAM budget of
 * every (output format x internal mode) pair, the picture placement properties, the alignment rules of the internal modes.
 * Prints "T <what> ..." lines; "OUT"/"MODE" lines carry the table values for the documentation check in video_hosttest.py.
 *   mutate=N   deliberately wrong expectation N (1..6): the test must report FAIL for exactly that group
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps2_vmodes.h"

static int failures, mutate;
static int group_fail[16];

#define CHECK(grp, cond, ...) do { int ok_ = (cond) ? 1 : 0; if (!ok_) { failures++; group_fail[grp]++; printf("T FAIL g%d ", grp); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- reference: gsKit_set_buffer_attributes (gsInit.c, gsKit 1.5.1), mode numbers instead of the GS_MODE_* names ---- */
typedef struct { int Mode, Interlace, Field, Width, Height, StartX, StartY, StartXOffset, StartYOffset, MagH, MagV, DW, DH; } refgs_t;

static void ref_attributes(refgs_t *g)
{
	g->StartXOffset = 0;
	g->StartYOffset = 0;
	switch (g->Mode)
	{
		case 0x02: g->StartX = 492; g->StartY = 34; g->DW = 2880; g->DH = 480; break;
		case 0x03: g->StartX = 520; g->StartY = 40; g->DW = 2880; g->DH = 576; break;
		case 0x1A: g->StartX = 280; g->StartY = 18; g->DW = 1280; g->DH = 480; break;
		case 0x1B: g->StartX = 330; g->StartY = 18; g->DW = 1280; g->DH = 480; break;
		case 0x1C: g->StartX = 360; g->StartY = 18; g->DW = 1280; g->DH = 480; break;
		case 0x1D: g->StartX = 260; g->StartY = 18; g->DW = 1280; g->DH = 480; break;
		case 0x2A: g->StartX = 450; g->StartY = 25; g->DW = 1600; g->DH = 600; break;
		case 0x2B:
		case 0x2C: g->StartX = 465; g->StartY = 25; g->DW = 1600; g->DH = 600; break;
		case 0x2D: g->StartX = 510; g->StartY = 25; g->DW = 1600; g->DH = 600; break;
		case 0x2E: g->StartX = 500; g->StartY = 25; g->DW = 1600; g->DH = 600; break;
		case 0x3B: g->StartX = 580; g->StartY = 30; g->DW = 2048; g->DH = 768; break;
		case 0x3C: g->StartX = 266; g->StartY = 30; g->DW = 1024; g->DH = 768; break;
		case 0x3D: g->StartX = 260; g->StartY = 30; g->DW = 1024; g->DH = 768; break;
		case 0x3E: g->StartX = 290; g->StartY = 30; g->DW = 1024; g->DH = 768; break;
		case 0x4A:
		case 0x4B: g->StartX = 350; g->StartY = 40; g->DW = 1280; g->DH = 1024; break;
		case 0x50: g->StartX = 232; g->StartY = 35; g->DW = 1440; g->DH = 480; break;
		case 0x53: g->StartX = 255; g->StartY = 44; g->DW = 1440; g->DH = 576; break;
		case 0x52: g->StartX = 306; g->StartY = 24; g->DW = 1280; g->DH = 720; break;
		case 0x51: g->StartX = 236; g->StartY = 38; g->DW = 1920; g->DH = 1080; break;
	}
	if ((g->Mode == 0x02 || g->Mode == 0x03) && g->Interlace == 0)
	{
		g->StartY /= 2;
		g->DH /= 2;
	}
	g->MagH = (g->DW / g->Width) - 1;
	g->MagV = (g->DH / g->Height) - 1;
	g->StartX += (g->DW - ((g->MagH + 1) * g->Width)) / 2;
	g->StartY += (g->DH - ((g->MagV + 1) * g->Height)) / 2;
	if (g->Interlace == 1)
		g->StartY &= ~1;
	g->DW = (g->MagH + 1) * g->Width;
	g->DH = (g->MagV + 1) * g->Height;
	if (g->Interlace == 1 && g->Field == 1)
		g->MagV--;
}

static int ipow2(int n) { int l = 0; while ((1 << l) < n) l++; return l; }

int main(int argc, char **argv)
{
	int i, j, m, w, h, x, y, dw, dh;
	int g;

	for (i = 1; i < argc; i++)
		if (!strncmp(argv[i], "mutate=", 7))
			mutate = atoi(argv[i] + 7);

	/* group 1: internal modes */
	CHECK(1, PS2VM_NUMMODES == 11 && PS2VM_MAXW == 640 && PS2VM_MAXH == 512, "constants");
	{
		static const int want[PS2VM_NUMMODES][2] = {{320, 200}, {320, 224}, {320, 240}, {320, 256}, {400, 300}, {512, 384}, {512, 448}, {640, 400}, {640, 448}, {640, 480}, {640, 512}};
		int maxw = 0, maxh = 0;
		for (m = 0; m < PS2VM_NUMMODES; m++)
		{
			int dup, ok = ps2vm_mode_size(m, &w, &h);
			CHECK(1, ok && w == want[m][0] + (mutate == 1 && m == 3) && h == want[m][1], "mode %d is %dx%d, documented %dx%d", m, w, h, want[m][0], want[m][1]);
			CHECK(1, (w & 7) == 0 && ((w * h) & 63) == 0 && w >= 320 && h >= 200, "mode %d %dx%d breaks the DMA/dup rules", m, w, h);
			CHECK(1, w <= PS2VM_MAXW && h <= PS2VM_MAXH, "mode %d exceeds MAXVID", m);
			CHECK(1, ps2vm_mode_for_size(w, h) == m, "mode_for_size(%d,%d) = %d, expected %d", w, h, ps2vm_mode_for_size(w, h), m);
			dup = (w * 65536 / 320 < h * 65536 / 200) ? w / 320 : h / 200; /* V_Recalc */
			printf("MODE %d %d %d %d %d %d %d\n", m, w, h, w * h, 5 * w * h, dup, ps2vm_tex_pages(w, h));
			if (w > maxw) maxw = w;
			if (h > maxh) maxh = h;
		}
		CHECK(1, maxw == PS2VM_MAXW && maxh == PS2VM_MAXH, "the biggest mode is %dx%d, MAXVID is %dx%d (not minimal)", maxw, maxh, PS2VM_MAXW, PS2VM_MAXH);
		CHECK(1, !ps2vm_mode_size(-1, &w, &h) && !ps2vm_mode_size(PS2VM_NUMMODES, &w, &h) && ps2vm_mode_for_size(1280, 800) == -1 && ps2vm_mode_for_size(320, 201) == -1, "out-of-range lookups");
	}

	/* group 2: output formats: CRTC against gsKit, framebuffer size and VRAM */
	CHECK(2, PS2VM_NUMOUT == 26 && ps2vm_output(-1) == 0 && ps2vm_output(PS2VM_NUMOUT) == 0, "table size");
	for (i = 0; i < PS2VM_NUMOUT; i++)
	{
		const ps2vm_output_t *o = ps2vm_output(i);
		refgs_t r;
		ps2vm_crtc_t c;
		int pages, worst = 0;
		memset(&r, 0, sizeof r);
		r.Mode = o->gsmode;
		r.Interlace = o->interlace;
		r.Field = o->ffmd;
		r.Width = o->fbw;
		r.Height = o->fbh;
		ref_attributes(&r);
		ps2vm_crtc(o, 0, 0, &c);
		CHECK(2, c.dx == r.StartX && c.dy == r.StartY + (mutate == 2 && i == 9) && c.magh == r.MagH && c.magv == r.MagV && c.dw == r.DW && c.dh == r.DH,
			"%s: crtc dx=%d dy=%d magh=%d magv=%d dw=%d dh=%d, gsKit says %d %d %d %d %d %d", o->name, c.dx, c.dy, c.magh, c.magv, c.dw, c.dh,
			r.StartX, r.StartY, r.MagH, r.MagV, r.DW, r.DH);
		CHECK(2, o->fbw > 0 && o->fbh > 0 && o->fbw <= 1024 && o->fbh <= 1024, "%s: frame buffer %dx%d", o->name, o->fbw, o->fbh);
		CHECK(2, c.magh >= 0 && c.magh <= 15 && c.magv >= 0 && c.magv <= 3, "%s: MAGH/MAGV out of the register range", o->name);
		/* the visible picture: (magh+1)*fbw VCK units by the rasters of the field/frame must lie inside the mode window */
		CHECK(2, (c.magh + 1) * o->fbw == c.dw, "%s: DW", o->name);
		CHECK(2, o->dar_w * 9 == o->dar_h * 16 || o->dar_w * 3 == o->dar_h * 4 || o->dar_w * 4 == o->dar_h * 5, "%s: display aspect %d:%d", o->name, o->dar_w, o->dar_h);
		/* group 3: every pair fits into 512 pages */
		for (m = 0; m < PS2VM_NUMMODES; m++)
		{
			ps2vm_mode_size(m, &w, &h);
			pages = ps2vm_vram_pages(o, w, h);
			if (pages > worst) worst = pages;
			CHECK(3, pages <= PS2VM_VRAM_PAGES - (mutate == 3 ? 120 : 0) && pages > 0, "%s with %dx%d needs %d pages", o->name, w, h, pages);
		}
		CHECK(3, 2 * ps2vm_fb_pages(o->fbw, o->fbh) * 8192 <= 4194304, "%s: two buffers alone exceed the 4 MB", o->name);
		printf("OUT %d|%s|%s|0x%02X|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d\n", i, o->name, o->arg, o->gsmode, o->interlace, o->ffmd, o->fbw, o->fbh,
			c.magh + 1, c.magv + 1, c.dw, c.dh, ps2vm_fb_pages(o->fbw, o->fbh), 2 * ps2vm_fb_pages(o->fbw, o->fbh),
			2 * ps2vm_fb_pages(o->fbw, o->fbh) + ps2vm_tex_pages(640, 512) + 1, worst, o->dar_w, o->dar_h, o->flags);
	}
	/* the first three keep the old PS2GS_MODE_ ids and sizes */
	CHECK(2, ps2vm_output(0)->fbw == 640 && ps2vm_output(0)->fbh == 448 && ps2vm_output(1)->fbh == 512 && ps2vm_output(2)->fbh == 480 && ps2vm_output(0)->gsmode == 0x02
		&& ps2vm_output(1)->gsmode == 0x03 && ps2vm_output(2)->gsmode == 0x50 && ps2vm_output(0)->interlace == 1 && ps2vm_output(0)->ffmd == 0 && ps2vm_output(2)->interlace == 0, "legacy outputs 0..2");
	{
		ps2vm_crtc_t c;
		/* what the old code programmed through gsKit: NTSC 640x448 field -> dx 652 dy 50 magh 3 magv 0 */
		ps2vm_crtc(ps2vm_output(0), 0, 0, &c);
		CHECK(2, c.dx == 652 && c.dy == 50 && c.magh == 3 && c.magv == 0 && c.dw == 2560 && c.dh == 448, "NTSC crtc");
		ps2vm_crtc(ps2vm_output(1), 0, 0, &c);
		CHECK(2, c.dx == 680 && c.dy == 72 && c.magh == 3 && c.magv == 0 && c.dw == 2560 && c.dh == 512, "PAL crtc");
		ps2vm_crtc(ps2vm_output(2), 0, 0, &c);
		CHECK(2, c.dx == 312 && c.dy == 35 && c.magh == 1 && c.magv == 0 && c.dw == 1280 && c.dh == 480, "480p crtc");
	}

	/* group 4: picture placement */
	for (i = 0; i < PS2VM_NUMOUT; i++)
	{
		const ps2vm_output_t *o = ps2vm_output(i);
		for (m = 0; m < PS2VM_NUMMODES; m++)
		{
			ps2vm_mode_size(m, &w, &h);
			for (g = 0; g < PS2VM_FIT_COUNT; g++)
			{
				ps2vm_dest(g, w, h, o->fbw, o->fbh, o->dar_w, o->dar_h, &x, &y, &dw, &dh);
				CHECK(4, x >= 0 && y >= 0 && dw > 0 && dh > 0 && x + dw <= o->fbw && y + dh <= o->fbh, "%s %dx%d fit %d: dest %d,%d %dx%d outside the %dx%d buffer", o->name, w, h, g, x, y, dw, dh, o->fbw, o->fbh);
				CHECK(4, (dw & 1) == 0 || dw == o->fbw || g == PS2VM_FIT_INTEGER, "%s %dx%d fit %d: odd width %d", o->name, w, h, g, dw);
				if (g == PS2VM_FIT_43 || g == PS2VM_FIT_SQUARE)
				{
					int tw = g == PS2VM_FIT_43 ? 4 : w, th = g == PS2VM_FIT_43 ? 3 : h;
					/* displayed aspect of the rectangle = (dw/dh) * (display aspect of the buffer / (fbw/fbh)) must be tw:th within the rounding */
					long long lhs = (long long)dw * o->dar_w * o->fbh * th, rhs = (long long)dh * o->dar_h * o->fbw * tw;
					double err = (double)(lhs - rhs) / (double)rhs;
					CHECK(4, err > -0.02 && err < 0.02, "%s %dx%d fit %d: aspect error %.4f (dest %dx%d)", o->name, w, h, g, err, dw, dh);
				}
				if (g == PS2VM_FIT_STRETCH)
					CHECK(4, x == 0 && y == 0 && dw == o->fbw && dh == o->fbh, "stretch is the whole buffer");
				if (g == PS2VM_FIT_INTEGER)
				{
					int whole = (dw % w == 0 && dh % h == 0 && dw / w == dh / h);
					int x2, y2, w2, h2;
					ps2vm_dest(PS2VM_FIT_43, w, h, o->fbw, o->fbh, o->dar_w, o->dar_h, &x2, &y2, &w2, &h2);
					CHECK(4, whole || (x == x2 && y == y2 && dw == w2 && dh == h2), "%s %dx%d: integer fit is neither a whole multiple nor the 4:3 fallback", o->name, w, h);
				}
			}
		}
	}
	/* the default must be the old behaviour: 320x200, 4:3 output (and 16:10 from 4:3 with square pixels is not the same) */
	for (i = 0; i < PS2VM_NUMOUT; i++)
	{
		const ps2vm_output_t *o = ps2vm_output(i);
		ps2vm_dest(PS2VM_FIT_43, 320, 200, o->fbw, o->fbh, o->dar_w, o->dar_h, &x, &y, &dw, &dh);
		if (o->dar_w * 3 == o->dar_h * 4)
			CHECK(5, x == 0 && y == 0 && dw == o->fbw + (mutate == 5 && i == 0) && dh == o->fbh, "%s: Fit 4:3 on a 4:3 output must be the whole buffer, got %d,%d %dx%d", o->name, x, y, dw, dh);
	}
	ps2vm_dest(PS2VM_FIT_43, 320, 200, 640, 360, 16, 9, &x, &y, &dw, &dh);
	CHECK(5, x == 80 && y == 0 && dw == 480 && dh == 360, "720p 4:3 column: %d,%d %dx%d", x, y, dw, dh);
	ps2vm_dest(PS2VM_FIT_INTEGER, 320, 200, 640, 448, 4, 3, &x, &y, &dw, &dh);
	CHECK(5, x == 0 && y == 24 && dw == 640 && dh == 400, "NTSC integer 2x: %d,%d %dx%d", x, y, dw, dh);
	ps2vm_dest(PS2VM_FIT_SQUARE, 320, 200, 640, 448, 4, 3, &x, &y, &dw, &dh);
	CHECK(5, x == 0 && dw == 640 && dh == 372 && y == 38, "NTSC square pixels 16:10: %d,%d %dx%d", x, y, dw, dh);

	/* group 6: bookkeeping helpers */
	CHECK(6, ps2vm_fb_stride(640) == 640 && ps2vm_fb_stride(400) == 448 && ps2vm_fb_stride(704) == 704 && ps2vm_fb_stride(1) == 64, "stride");
	CHECK(6, ps2vm_fb_pages(640, 448) == 140 && ps2vm_fb_pages(400, 300) == 70 && ps2vm_fb_pages(640, 224) == 70 + (mutate == 6), "fb pages %d %d %d", ps2vm_fb_pages(640, 448), ps2vm_fb_pages(400, 300), ps2vm_fb_pages(640, 224));
	CHECK(6, ps2vm_tex_pages(320, 200) == 12 && ps2vm_tex_pages(640, 512) == 40 && ps2vm_tex_tbw(320) == 6 && ps2vm_tex_tbw(640) == 10 && ps2vm_tex_tbw(400) == 8, "texture pages / tbw");
	CHECK(6, ipow2(320) == ps2vm_log2ceil(320) && ps2vm_log2ceil(320) == 9 && ps2vm_log2ceil(200) == 8 && ps2vm_log2ceil(512) == 9 && ps2vm_log2ceil(640) == 10, "log2");
	(void)j;

	printf("T summary failures=%d (g1=%d g2=%d g3=%d g4=%d g5=%d g6=%d)\n", failures, group_fail[1], group_fail[2], group_fail[3], group_fail[4], group_fail[5], group_fail[6]);
	return failures ? 1 : 0;
}
