// SONIC ROBO BLAST 2 (PS2 port)
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_memhud.c
/// \brief OPT11-MEM: showmem, the RAM / VRAM counter drawn like the FPS counter. See ps2_memhud.h.

#include "../doomdef.h"
#include "../doomstat.h"
#include "../command.h"
#include "../v_video.h"
#include "../screen.h"
#include "../i_time.h"
#include "../i_video.h"
#include "../i_system.h"
#include "../m_argv.h"
#include "../g_game.h"
#include "../netcode/d_clisrv.h"
#include "../netcode/tic_command.h"
#include "../r_main.h"
#include "../r_patch.h"
#include "../hu_stuff.h"
#include "../z_zone.h"
#include "ps2_mem.h"
#include "ps2_gs.h"
#include "ps2_memhud.h"
#ifdef HWRENDER
#include "hw/ps2_hwd_dbg.h"
#endif

static CV_PossibleValue_t showmem_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "RAM"}, {3, "VRAM"}, {0, NULL}};
static void showmem_onchange(void);
consvar_t cv_showmem = CVAR_INIT ("showmem", "Off", CV_SAVE|CV_CALL, showmem_cons_t, showmem_onchange);

#define SAMPLE_TICS 9 // 35 tics a second: the numbers are read about every 0.26 s (the text does not flicker)
#define RAM_RED_KIB (1024u) // less free zone memory than this: red
#define RAM_YELLOW_KIB (3072u) // less than this: yellow
#define VRAM_YELLOW_PCT 90u // software renderer: the GS memory is fuller than this (percent): yellow
#define VRAM_WS_YELLOW_PCT 85u // hardware renderer: the working set of the frame (percent of the texture pool): yellow
#define VRAM_WS_RED_PCT 95u // red

// ---------------------------------------------------------------------------------------------------------------------------------------
// the counters
// ---------------------------------------------------------------------------------------------------------------------------------------

void PS2MemHud_Sample(ps2memhud_t *o)
{
	zastats_t st;
	size_t ram = PS2Mem_RamBytes(), libc, zfree = 0;
	size_t freeb;
	int zone = ZA_Ready();

	memset(o, 0, sizeof *o);
	if (zone)
	{
		ZA_Stats(&st);
		zfree = st.freebytes;
		o->zone_big = (unsigned)(st.largestfree >> 10);
		o->zone_peak = (unsigned)(st.globalpeak >> 10);
	}
	{
		static long bias = -1; // -showmembias KiB: pretend that much of the zone is used (to see the colours of the pressure without filling the memory)

		if (bias < 0)
			bias = M_CheckParm("-showmembias") && M_IsNextParm() ? atol(M_GetNextParm()) : 0;
		if (bias && zone)
		{
			const size_t b = (size_t)bias << 10;

			zfree = zfree > b ? zfree - b : 0;
		}
	}
	libc = PS2Mem_LibcFree(); // the C heap can still grow to the stack, and holds free chunks: what malloc can still give
	freeb = zfree + libc;
	if (freeb > ram)
		freeb = ram;
	o->ram_total = (unsigned)(ram >> 10);
	o->ram_free = (unsigned)(freeb >> 10);
	o->ram_used = o->ram_total - o->ram_free;
	o->zone_free = (unsigned)(zfree >> 10);
	o->libc_free = (unsigned)(libc >> 10);
	o->ram_level = o->zone_free < RAM_RED_KIB ? 2 : o->zone_free < RAM_YELLOW_KIB ? 1 : 0; // the zone is what runs out: the C heap's room does not help a zone allocation

	o->vram_total = 4096;
#ifdef HWRENDER
	if (rendermode == render_opengl)
	{
		ps2hwd_vram_t v;

		PS2HWD_VramStats(&v);
		if (v.up)
		{
			o->hw = 1;
			o->vram_fb = v.fb_blocks / 4; // 256-byte blocks
			o->vram_z = v.z_blocks / 4;
			o->vram_clut = v.clut_blocks / 4;
			o->vram_tex_total = v.pool_blocks / 4;
			o->vram_tex_used = v.pool_used_blocks / 4;
			o->vram_ws = v.ws_blocks / 4;
			o->vram_used = o->vram_fb + o->vram_z + o->vram_clut + o->vram_tex_used;
			o->vram_level = 0;
			if (v.pool_blocks)
			{
				const unsigned long pool = v.pool_blocks;

				// the pool is a cache and is nearly full in normal play (the textures of earlier frames stay until the room is needed), so occupation is information, not
				// pressure: the colour follows the working set of the last frame (the textures it drew), which the texture planner keeps under 3/4 of the pool by lowering
				// their detail: yellow when the frame needs most of the pool, red when it needs nearly all of it (textures are being re-uploaded every frame)
				if (v.ws_blocks * 100ul >= pool * VRAM_WS_RED_PCT)
					o->vram_level = 2;
				else if (v.ws_blocks * 100ul >= pool * VRAM_WS_YELLOW_PCT)
					o->vram_level = 1;
			}
			return;
		}
	}
#endif
	if (ps2gs_is_up())
	{
		unsigned fb0, fb1, tex, clut;

		ps2gs_get_layout(&fb0, &fb1, &tex, &clut);
		(void)fb0;
		o->vram_fb = (2u * fb1) >> 10; // the frame buffers are the two first areas of the same size
		o->vram_tex_total = o->vram_tex_used = (clut - tex) >> 10; // the 8 bit source picture of the software renderer (it is drawn by the GS as one textured sprite)
		o->vram_clut = 8; // one page for the palette
		o->vram_used = (clut + 8192u) >> 10; // the layout is contiguous from address 0
		o->vram_level = o->vram_used * 100u >= o->vram_total * VRAM_YELLOW_PCT ? 1 : 0;
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// text: integer arithmetic only, static buffers
// ---------------------------------------------------------------------------------------------------------------------------------------

static char *put_u(char *p, unsigned v)
{
	char t[12];
	int n = 0;

	do
	{
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v);
	while (n)
		*p++ = t[--n];
	return p;
}

static char *put_s(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

// KiB as MiB with one decimal ("27.3")
static char *put_mb(char *p, unsigned kib)
{
	const unsigned t = (kib * 10u + 512u) / 1024u;

	p = put_u(p, t / 10u);
	*p++ = '.';
	*p++ = (char)('0' + t % 10u);
	return p;
}

typedef struct
{
	char label[8], main[28], detail[40];
	int lw, mw, dw; // widths in base units
	int level;
} memline_t;

#define MEM_LINEH 8 // the thin font: 7 pixels high, one line every 8 (the half size menu font is 4 pixels high and not legible at 320x200)
#define MEM_CASE V_ALLOWLOWERCASE // the thin font has lowercase letters

static void line_ram(memline_t *l, const ps2memhud_t *m, int full)
{
	char *p;

	strcpy(l->label, "RAM");
	p = l->main;
	*p++ = ' ';
	p = put_mb(p, m->ram_used);
	*p++ = '/';
	p = put_mb(p, m->ram_total);
	p = put_s(p, " MB");
	*p = 0;
	p = l->detail;
	if (!full)
	{
		p = put_s(p, "  free ");
		p = put_mb(p, m->zone_free);
		p = put_s(p, " big ");
		p = put_mb(p, m->zone_big);
	}
	else
	{
		p = put_s(p, "  free ");
		p = put_mb(p, m->ram_free);
	}
	*p = 0;
	l->level = m->ram_level;
}

static void line_ram2(memline_t *l, const ps2memhud_t *m)
{
	char *p = l->detail;

	l->label[0] = 0;
	l->main[0] = 0;
	p = put_s(p, "zone free ");
	p = put_mb(p, m->zone_free);
	p = put_s(p, " big ");
	p = put_mb(p, m->zone_big);
	p = put_s(p, " peak ");
	p = put_mb(p, m->zone_peak);
	p = put_s(p, " libc ");
	p = put_mb(p, m->libc_free);
	*p = 0;
	l->level = m->ram_level;
}

static void line_vram(memline_t *l, const ps2memhud_t *m)
{
	char *p;

	strcpy(l->label, "VRAM");
	p = l->main;
	*p++ = ' ';
	p = put_mb(p, m->vram_used);
	*p++ = '/';
	p = put_mb(p, m->vram_total);
	p = put_s(p, " MB");
	*p = 0;
	p = l->detail;
	if (m->hw)
	{
		p = put_s(p, "  tex ");
		p = put_mb(p, m->vram_tex_used);
		*p++ = '/';
		p = put_mb(p, m->vram_tex_total);
		p = put_s(p, " ws ");
		p = put_mb(p, m->vram_ws);
	}
	else
	{
		p = put_s(p, "  fb ");
		p = put_mb(p, m->vram_fb);
		p = put_s(p, " src ");
		p = put_mb(p, m->vram_tex_used);
	}
	*p = 0;
	l->level = m->vram_level;
}

static void line_vram2(memline_t *l, const ps2memhud_t *m)
{
	char *p = l->detail;

	l->label[0] = 0;
	l->main[0] = 0;
	p = put_s(p, "fb ");
	p = put_u(p, m->vram_fb);
	if (m->hw)
	{
		p = put_s(p, " z ");
		p = put_u(p, m->vram_z);
		p = put_s(p, " clut ");
		p = put_u(p, m->vram_clut);
		p = put_s(p, " pool ");
		p = put_u(p, m->vram_tex_used);
		*p++ = '/';
		p = put_u(p, m->vram_tex_total);
		p = put_s(p, " ws ");
		p = put_u(p, m->vram_ws);
	}
	else
	{
		p = put_s(p, " src ");
		p = put_u(p, m->vram_tex_used);
		p = put_s(p, " pal ");
		p = put_u(p, m->vram_clut);
	}
	p = put_s(p, " KB");
	*p = 0;
	l->level = m->vram_level;
}

// the width of a string in the thin font, as V_DrawFontStringAtFixed advances (base units)
static int str_w(const char *s)
{
	int w = 0;

	for (; *s; s++)
	{
		const int c = (int)(UINT8)*s - FONTSTART;

		if (c < 0 || c >= FONTSIZE || !tny_font.chars[c])
			w += (int)tny_font.spacewidth;
		else
			w += tny_font.chars[c]->width + tny_font.kerning;
	}
	return w;
}

static void measure(memline_t *l)
{
	l->lw = l->label[0] ? str_w(l->label) : 0;
	l->mw = l->main[0] ? str_w(l->main) : 0;
	l->dw = l->detail[0] ? str_w(l->detail) : 0;
}

static memline_t mem_lines[2], prev_lines[2];
static int nlines, shown_mode = -1;
static tic_t last_sample;

// profile
static unsigned prof_sample_cyc, prof_sample_n, prof_draw_cyc, prof_draw_n, prof_plate_cyc, prof_compose_cyc, prof_compose_n, prof_fallback_n;
static int prof_on = -1;
static unsigned long tot_draw_cyc, tot_draw_n, tot_compose_cyc, tot_compose_n, tot_sample_cyc, tot_sample_n; // since the start (the profile window prints and clears the ones above)

// ---------------------------------------------------------------------------------------------------------------------------------------
// the picture of the text: both lines are drawn into one small bitmap when the text changes (a few times a second) and shown as ONE patch every
// frame. Drawn glyph by glyph the 70 characters cost ~190000 cycles a frame in the hardware renderer (a texture switch for each), the patch ~10000.
// The patch is an ordinary zone block owned through tpatch (the zone clears it when the block goes: eviction, level change); when there is no room
// for it the text is drawn glyph by glyph this frame and the picture is tried again at the next reading.
// ---------------------------------------------------------------------------------------------------------------------------------------

#define BMP_W 288 // pixels (base units) of the widest line
#define BMP_H (2 * MEM_LINEH)
static UINT8 bmp[BMP_H * BMP_W], cov[BMP_H * BMP_W]; // palette indices of the picture / is the pixel drawn
static UINT8 doc[8 + 4 * BMP_W + 41 * BMP_W] __attribute__((aligned(16))); // the picture as a Doom patch (the worst column: 8 posts of 1 pixel and the end mark = 41 bytes)
static patch_t *tpatch;
static int tpatch_w;
static tic_t compose_retry;

static void blit_glyph(const patch_t *g, int x0, int y0, const UINT8 *cmap)
{
	int cx;

	for (cx = 0; cx < g->width; cx++)
	{
		const column_t *col = &g->columns[cx];
		const int px = x0 + cx - g->leftoffset;
		unsigned i, k;

		if (px < 0 || px >= BMP_W)
			continue;
		for (i = 0; i < col->num_posts; i++)
		{
			const post_t *po = &col->posts[i];

			for (k = 0; k < po->length; k++)
			{
				const int py = y0 + (int)po->topdelta + (int)k - g->topoffset;
				UINT8 v;

				if (py < 0 || py >= BMP_H)
					continue;
				v = col->pixels[po->data_offset + k];
				bmp[py * BMP_W + px] = cmap ? cmap[v] : v;
				cov[py * BMP_W + px] = 1;
			}
		}
	}
}

static int blit_string(const char *s, int x, int y, const UINT8 *cmap)
{
	for (; *s; s++)
	{
		const int c = (int)(UINT8)*s - FONTSTART;

		if (c < 0 || c >= FONTSIZE || !tny_font.chars[c])
		{
			x += (int)tny_font.spacewidth;
			continue;
		}
		blit_glyph(tny_font.chars[c], x, y, cmap);
		x += tny_font.chars[c]->width + tny_font.kerning;
	}
	return x;
}

static INT32 level_color(int level)
{
	return level >= 2 ? V_REDMAP : level == 1 ? V_YELLOWMAP : V_GREENMAP;
}

static INT32 detail_color(const memline_t *l)
{
	return l->level >= 1 ? level_color(l->level) : V_GRAYMAP;
}

static int line_w(const memline_t *l)
{
	return l->lw + l->mw + l->dw;
}

// the lines into the bitmap, the bitmap into a patch. 1: tpatch is ready
static int compose(void)
{
	int i, x, pw = 0, y;
	UINT8 *p;
	softwarepatch_t *dp = (softwarepatch_t *)doc;
	INT32 *colofs = (INT32 *)(void *)(doc + 8);
	unsigned c0 = prof_on ? PS2Mem_Cycles() : 0;
	patch_t *np;

	for (i = 0; i < nlines; i++)
		if (line_w(&mem_lines[i]) > pw)
			pw = line_w(&mem_lines[i]);
	if (pw > BMP_W)
		pw = BMP_W;
	if (pw < 1)
		return 0;
	memset(bmp, 0, sizeof bmp);
	memset(cov, 0, sizeof cov);
	for (i = 0; i < nlines; i++)
	{
		const memline_t *l = &mem_lines[i];

		x = pw - line_w(l); // the lines end at the same column
		if (l->lw)
			x = blit_string(l->label, x, i * MEM_LINEH, V_GetStringColormap(MENUCOLOR));
		if (l->mw)
			x = blit_string(l->main, x, i * MEM_LINEH, V_GetStringColormap(level_color(l->level)));
		if (l->dw)
			blit_string(l->detail, x, i * MEM_LINEH, V_GetStringColormap(detail_color(l)));
	}
	// the Doom patch: width, height, offsets, the column offsets, then per column the posts (top, length, pad, pixels, pad) and 0xFF
	dp->width = (INT16)pw;
	dp->height = BMP_H;
	dp->leftoffset = 0;
	dp->topoffset = 0;
	p = doc + 8 + 4 * pw;
	for (x = 0; x < pw; x++)
	{
		colofs[x] = (INT32)(p - doc);
		y = 0;
		while (y < BMP_H)
		{
			int start, k;

			if (!cov[y * BMP_W + x])
			{
				y++;
				continue;
			}
			start = y;
			while (y < BMP_H && cov[y * BMP_W + x])
				y++;
			*p++ = (UINT8)start;
			*p++ = (UINT8)(y - start);
			*p++ = 0;
			for (k = start; k < y; k++)
				*p++ = bmp[k * BMP_W + x];
			*p++ = 0;
		}
		*p++ = 0xFF;
	}
	if (tpatch)
		Patch_Free(tpatch); // the zone clears tpatch (its owner pointer)
	np = Patch_TryCreateFromDoomPatch(dp);
	if (prof_on)
	{
		prof_compose_cyc += PS2Mem_Cycles() - c0;
		prof_compose_n++;
		tot_compose_cyc += PS2Mem_Cycles() - c0;
		tot_compose_n++;
	}
	if (!np)
		return 0;
	Z_SetUser(np, (void **)&tpatch);
	tpatch_w = pw;
	return 1;
}

static void refresh(int mode)
{
	ps2memhud_t m;
	unsigned c0 = 0;

	if (prof_on)
		c0 = PS2Mem_Cycles();
	PS2MemHud_Sample(&m);
	memset(mem_lines, 0, sizeof mem_lines); // (the strings are compared as they are to see whether the text changed)
	switch (mode)
	{
		case 2: // RAM: two lines
			line_ram(&mem_lines[0], &m, 1);
			line_ram2(&mem_lines[1], &m);
			nlines = 2;
			break;
		case 3: // VRAM: two lines
			line_vram(&mem_lines[0], &m);
			line_vram2(&mem_lines[1], &m);
			nlines = 2;
			break;
		default: // RAM and VRAM: one line each
			line_ram(&mem_lines[0], &m, 0);
			line_vram(&mem_lines[1], &m);
			nlines = 2;
			break;
	}
	measure(&mem_lines[0]);
	measure(&mem_lines[1]);
	if (prof_on)
	{
		prof_sample_cyc += PS2Mem_Cycles() - c0;
		prof_sample_n++;
		tot_sample_cyc += PS2Mem_Cycles() - c0;
		tot_sample_n++;
	}
}

// the lower right corner: the FPS counter takes the last line (8 pixels) when it is on, the ping display (HU_drawPing in SCR_DisplayLocalPing) sits above it
// at y = 180 (FPS on) or 189 (FPS off); the block stands on whatever is lowest and free
static int bottom_edge(void)
{
	INT32 y = BASEVIDHEIGHT;

	if (cv_ticrate.value)
		y -= 8;
	if (cv_showping.value && ((netgame && consoleplayer != serverplayer) || (simulated_lag != 0 && consoleplayer == serverplayer && Playing())))
	{
		const UINT32 ping = playerpingtable[consoleplayer];

		if (cv_showping.value == 1 || (cv_showping.value == 2 && servermaxping && ping > servermaxping))
			y = cv_ticrate.value ? 180 : 189; // the top edge of the ping drawing
	}
	return (int)y;
}

static void showmem_onchange(void)
{
	if (!cv_showmem.value && tpatch)
		Patch_Free(tpatch); // Off: nothing stays allocated
	shown_mode = -1;
}

void PS2MemHud_Draw(void)
{
	const INT32 base = V_SNAPTORIGHT | V_SNAPTOBOTTOM | MEM_CASE | V_USERHUDTRANS;
	const int mode = cv_showmem.value;
	const tic_t now = I_GetTime();
	unsigned c0 = 0;
	int i, bottom, pw = 0, py;

	if (gamestate == GS_NULL || mode <= 0)
		return;
	if (prof_on < 0)
		prof_on = M_CheckParm("-ps2prof") != 0;
	if (prof_on)
		c0 = PS2Mem_Cycles();
	if (shown_mode != mode || now - last_sample >= SAMPLE_TICS || (!tpatch && now >= compose_retry))
	{
		last_sample = now;
		shown_mode = mode;
		refresh(mode);
		if (!tpatch || memcmp(mem_lines, prev_lines, sizeof mem_lines)) // the text changed (or the zone dropped the picture)
		{
			memcpy(prev_lines, mem_lines, sizeof mem_lines);
			if (!compose())
				compose_retry = now + SAMPLE_TICS; // no room: glyph by glyph meanwhile
		}
	}
	for (i = 0; i < nlines; i++)
		if (line_w(&mem_lines[i]) > pw)
			pw = line_w(&mem_lines[i]);
	bottom = bottom_edge();
	py = bottom - nlines * MEM_LINEH;
	{
		// a dark plate behind the text: the numbers stay legible over the green grass and the brown walls
		unsigned p0 = prof_on ? PS2Mem_Cycles() : 0;

		V_DrawFill(BASEVIDWIDTH - 3 - pw, py, pw + 3, nlines * MEM_LINEH, 31 | V_SNAPTORIGHT | V_SNAPTOBOTTOM | V_40TRANS);
		if (prof_on)
			prof_plate_cyc += PS2Mem_Cycles() - p0;
	}
	if (tpatch && tpatch_w == pw)
		V_DrawScaledPatch(BASEVIDWIDTH - 2 - pw, py, base & ~MEM_CASE, tpatch); // (V_ALLOWLOWERCASE is V_FLIP for a patch)
	else
	{
		if (prof_on)
			prof_fallback_n++;
		for (i = 0; i < nlines; i++)
		{
			const memline_t *l = &mem_lines[i];
			const int y = py + i * MEM_LINEH;
			int x = BASEVIDWIDTH - 2 - line_w(l);

			if (l->lw)
			{
				V_DrawThinString(x, y, base | MENUCOLOR, l->label);
				x += l->lw;
			}
			if (l->mw)
			{
				V_DrawThinString(x, y, base | level_color(l->level), l->main);
				x += l->mw;
			}
			if (l->dw)
				V_DrawThinString(x, y, base | detail_color(l), l->detail);
		}
	}
	if (prof_on)
	{
		prof_draw_cyc += PS2Mem_Cycles() - c0;
		prof_draw_n++;
		tot_draw_cyc += PS2Mem_Cycles() - c0;
		tot_draw_n++;
	}
}

void PS2MemHud_ProfLine(unsigned frames)
{
	if (!prof_sample_n && !prof_draw_n)
		return;
	if (!frames)
		frames = 1;
	I_OutputMsg("HWPROF60 showmem: drawn=%u of %u frames draw=%u (plate %u) cycles/frame, glyph by glyph in %u | samples=%u sample=%u cycles each | pictures=%u made=%u cycles each\n", prof_draw_n, frames,
		prof_draw_n ? prof_draw_cyc / prof_draw_n : 0, prof_draw_n ? prof_plate_cyc / prof_draw_n : 0, prof_fallback_n, prof_sample_n, prof_sample_n ? prof_sample_cyc / prof_sample_n : 0,
		prof_compose_n, prof_compose_n ? prof_compose_cyc / prof_compose_n : 0);
	prof_sample_cyc = prof_sample_n = prof_draw_cyc = prof_draw_n = prof_plate_cyc = prof_compose_cyc = prof_compose_n = prof_fallback_n = 0;
}

// the end of a -zquit run (ps2_mem.c, right after "ZSTAT"): the same numbers the display shows, read fresh, to be compared with the "[zmem] final" and ZSTAT lines
void PS2MemHud_Check(void)
{
	ps2memhud_t m;

	if (!cv_showmem.value)
		return;
	PS2MemHud_Sample(&m);
	I_OutputMsg("SHOWMEM now (KiB): ram total=%u used=%u free=%u | zone free=%u big=%u peak=%u | libc free=%u | vram total=%u used=%u fb=%u z=%u clut=%u tex=%u/%u ws=%u hw=%d levels=%d/%d\n",
		m.ram_total, m.ram_used, m.ram_free, m.zone_free, m.zone_big, m.zone_peak, m.libc_free, m.vram_total, m.vram_used, m.vram_fb, m.vram_z, m.vram_clut,
		m.vram_tex_used, m.vram_tex_total, m.vram_ws, m.hw, m.ram_level, m.vram_level);
	if (tot_draw_n)
		I_OutputMsg("SHOWMEM cost (-ps2prof, whole run): drawn %lu frames, %lu cycles a frame on average (the readings and the pictures made inside it included); readings %lu, %lu cycles each; pictures %lu, %lu cycles each\n",
			tot_draw_n, tot_draw_cyc / tot_draw_n, tot_sample_n, tot_sample_n ? tot_sample_cyc / tot_sample_n : 0, tot_compose_n, tot_compose_n ? tot_compose_cyc / tot_compose_n : 0);
}
