// PS2 GS hardware renderer driver: texture conformance self-test (included at the end of ps2_hwd.c). PS2-HW-69 (OPT10, HF).
//
// -hwtextest: after a level is loaded every map texture (both chroma key variants) and every level flat is made resident the way the engine does
// (HWR_GetTexture / HWR_GetLevelFlat -> hw_SetTexture) and drawn twice, then the frame buffer is read back and compared with the texels the engine
// handed to the driver:
//   A  1:1 copy with a GS sprite (tiles of 320x200): upload, TBW, block layout, CLUT, sizes up to 1024
//   B  the repeat path of walls and floors (hw_DrawPolygon with forced wrap, up to two repeats): NPOT cuts, region clamp, UV bases, ST scale
// It needs the frame buffer to be 320x200 (-hwfbh 200: one pixel per engine pixel) and CT32. Lines "TTFAIL ..." name every texture that differs and
// the first differing pixel, "TT done ..." summarises. A geometry-free, whole-game check of the texture path (what the user sees as "textures on walls
// and floors glitch"): PCSX2 and a real GS must agree with the texels.

#include "../../hardware/hw_glob.h"
#include "../../r_textures.h"
#include "../../p_setup.h"

static void hw_DrawPolygon(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags);
static void hw_GClipRect(INT32 minx, INT32 miny, INT32 maxx, INT32 maxy, float nearclip);

static struct
{
	int on;
	const GLMipmap_t *m;
	u8 *buf;
	size_t cap;
	u32 w, h, bpp;
	int fmt, flags, have;
	int rpsm; // PS2-HW-71: the GS format the texture was stored in (palette rendering: the CLUT shows the screen palette, a baked CT32 image the texture palette)
} TT;

// called by hw_SetTexture before anything is done with the texture: the texels the engine hands over (the driver may take the data over)
static void tt_capture(const GLMipmap_t *m)
{
	size_t n;
	u32 bpp;

	TT.m = m;
	TT.have = 0;
	if (!m->data || !m->width || !m->height)
		return;
	switch (m->format)
	{
		case GL_TEXFMT_P_8: bpp = 1; break;
		case GL_TEXFMT_AP_88: bpp = 2; break;
		case GL_TEXFMT_RGBA: bpp = 4; break;
		default: return;
	}
	n = (size_t)m->width * m->height * bpp;
	if (n > 4u * 1024u * 1024u)
		return;
	if (n > TT.cap)
	{
		u8 *b = realloc(TT.buf, n);

		if (!b)
			return;
		TT.buf = b;
		TT.cap = n;
	}
	memcpy(TT.buf, m->data, n);
	TT.w = m->width;
	TT.h = m->height;
	TT.bpp = bpp;
	TT.fmt = m->format;
	TT.flags = (int)m->flags;
	TT.have = 1;
}

// colour (0x00BBGGRR) the texel must show; 0 = a hole or a texel with partial alpha (not compared)
static int tt_expect(u32 x, u32 y, u32 *rgb)
{
	const u8 *p = TT.buf + ((size_t)y * TT.w + x) * TT.bpp;
	const u32 *op = out_pal();

	switch (TT.fmt)
	{
		case GL_TEXFMT_P_8:
			if ((TT.flags & TF_CHROMAKEYED) && p[0] == 255)
				return 0;
			*rgb = op[p[0]] & 0x00FFFFFFu;
			return 1;
		case GL_TEXFMT_AP_88:
			if (TT.flags & TF_CHROMAKEYED)
			{
				if (p[0] == 255)
					return 0;
			}
			else if (p[1] != 255)
			{
				return 0;
			}
			*rgb = (TT.rpsm == PSM_CT32 ? H.pal[p[0]] : op[p[0]]) & 0x00FFFFFFu;
			return 1;
		default:
		{
			u32 c = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);

			if (p[3] != 255)
				return 0;
			if (TT.rpsm == PSM_T8)
			{
				int k = pal_lookup(c);

				if (k >= 0)
					c = op[k] & 0x00FFFFFFu;
			}
			*rgb = c;
			return 1;
		}
	}
}

typedef struct
{
	u32 tested, bad, first_x, first_y, exp, got;
	u16 bx[8], by[8]; // the first mismatches
	u32 be[8], bg[8];
} ttres_t;

static u32 *tt_fb;

// for a failing 1:1 tile: which whole-texel shift of the expectation explains the read-back best (a systematic sampling offset)
static void tt_shift_report(const char *label, u32 tw, u32 th, u32 ox, u32 oy, u32 mx, u32 my)
{
	int sx, sy, bsx = 0, bsy = 0;
	u32 best = 0xFFFFFFFFu, x, y;

	for (sy = -2; sy <= 2; sy++)
		for (sx = -2; sx <= 2; sx++)
		{
			u32 bad = 0;

			for (y = 2; y < th - 2; y++)
				for (x = 2; x < tw - 2; x++)
				{
					u32 e, g = tt_fb[(size_t)y * H.fbw + x] & 0x00FFFFFFu;

					if (tt_expect((x + ox + (u32)sx) % mx, (y + oy + (u32)sy + my) % my, &e) && g != e)
						bad++;
				}
			if (bad < best)
			{
				best = bad;
				bsx = sx;
				bsy = sy;
			}
		}
	I_OutputMsg("TTSHIFT %s tile at %u,%u: best texel shift %d,%d leaves %u mismatches\n", label, ox, oy, bsx, bsy, best);
}

static int tt_read(u32 rows)
{
	if (!tt_fb)
		tt_fb = memalign(64, (size_t)H.fbw * 200 * 4);
	if (!tt_fb)
		return 0;
	return PS2HWD_ReadVram(tt_fb, H.fb_addr[H.target] / 256, (unsigned)(H.fbw / 64), H.fbpsm, 0, 0, H.fbw, (int)rows) == 0;
}

// frame buffer pixels (x, y) show texel (tx + x, ty + y) for the tile of tw x th pixels. The UV register cannot express 1024.0: the last texel column / row of a
// 1024 texel axis is drawn by sprites of one pixel whose U / V is the integer texel (the GS samples U0 + n at pixel n: sub-texel starts shift the picture)
static void tt_tile(texrec_t *r, u32 tx, u32 ty, u32 tw, u32 th)
{
	const u32 mw = tw - (tx + tw == 1024 ? 1u : 0u), mh = th - (ty + th == 1024 ? 1u : 0u);
	const u64 fr = frame_reg(H.target, 0);

	if (mw && mh)
		sprite_tex(r, 0, fr, 0, 0, 0, 0, (int)mw, (int)mh, (float)tx, (float)ty, (float)(tx + mw), (float)(ty + mh), H.vw, H.vh, 0);
	if (mw < tw && mh)
		sprite_tex(r, 0, fr, 0, 0, (int)mw, 0, (int)mw + 1, (int)mh, (float)(tx + mw), (float)ty, 1023.9375f, (float)(ty + mh), H.vw, H.vh, 0);
	if (mh < th && mw)
		sprite_tex(r, 0, fr, 0, 0, 0, (int)mh, (int)mw, (int)mh + 1, (float)tx, (float)(ty + mh), (float)(tx + mw), 1023.9375f, H.vw, H.vh, 0);
	if (mw < tw && mh < th)
		sprite_tex(r, 0, fr, 0, 0, (int)mw, (int)mh, (int)mw + 1, (int)mh + 1, (float)(tx + mw), (float)(ty + mh), 1023.9375f, 1023.9375f, H.vw, H.vh, 0);
}

static void tt_clear(void)
{
	frame_prepare(1);
	gs_fill_all(H.target, 1, 1, 0x80FF00FFu); // magenta: what a hole or a skipped draw leaves
}

// compare the read-back rows [0, ph) x [0, pw) with texel ((x + ox) % mx, (y + oy) % my)
static void tt_compare(ttres_t *res, u32 pw, u32 ph, u32 ox, u32 oy, u32 mx, u32 my)
{
	u32 x, y;

	for (y = 0; y < ph; y++)
		for (x = 0; x < pw; x++)
		{
			u32 e, g = tt_fb[(size_t)y * H.fbw + x] & 0x00FFFFFFu;

			if (!tt_expect((x + ox) % mx, (y + oy) % my, &e))
				continue;
			res->tested++;
			if (g != e)
			{
				if (res->bad < 8)
				{
					res->bx[res->bad] = (u16)x;
					res->by[res->bad] = (u16)y;
					res->be[res->bad] = e;
					res->bg[res->bad] = g;
				}
				if (!res->bad++)
				{
					res->first_x = x;
					res->first_y = y;
					res->exp = e;
					res->got = g;
				}
			}
		}
}

static void tt_one(const char *label, int *n_tested, int *n_failA, int *n_failB, int *n_skipped, int *n_missing, int *n_decim)
{
	texrec_t *r;
	u32 w = TT.w, h = TT.h, tx, ty;
	ttres_t A, B;
	char why[64] = "";

	memset(&A, 0, sizeof A);
	memset(&B, 0, sizeof B);
	if (!TT.have || H.cur_tex == NOREC || !H.rec[H.cur_tex].used)
	{
		if (!TT.have)
			(*n_skipped)++;
		else
		{
			(*n_missing)++;
			I_OutputMsg("TTFAIL %s %ux%u: not resident (%s)\n", label, w, h, H.cur_missing ? "skipped by the frame plan" : "no VRAM");
		}
		return;
	}
	r = &H.rec[H.cur_tex];
	if (r->dx || r->dy)
	{
		(*n_decim)++; // stored decimated (a texel axis over 1024): the 1:1 compare does not apply
		I_OutputMsg("TTDECIM %s %ux%u stored %ux%u (dx %u dy %u)\n", label, w, h, (unsigned)r->w, (unsigned)r->h, (unsigned)r->dx, (unsigned)r->dy);
		return;
	}
	if (r->owner && (r->owner->width != w || r->owner->height != h))
	{
		(*n_skipped)++; // the capture belongs to another texture
		return;
	}
	TT.rpsm = r->psm;
	if (r->owner)
		TT.flags = (int)r->owner->flags; // the variant (chroma keyed or not) shares the captured texels
	(*n_tested)++;
	// A: 1:1 tiles
	for (ty = 0; ty < h; ty += 200)
		for (tx = 0; tx < w; tx += 320)
		{
			u32 tw = w - tx < 320 ? w - tx : 320, th = h - ty < 200 ? h - ty : 200;

			tt_clear();
			tt_tile(r, tx, ty, tw, th);
			if (!tt_read(th))
			{
				snprintf(why, sizeof why, "read-back failed");
				break;
			}
			{
				u32 before = A.bad;

				tt_compare(&A, tw, th, tx, ty, w, h);
				if (A.bad > before && A.bad - before > 40 && th > 4 && tw > 4)
					tt_shift_report(label, tw, th, tx, ty, w, h);
			}
		}
	// B: the repeat path (up to two repeats, whatever fits the 320x200 screen)
	if (H.vw == 320 && H.vh == 200 && r->wrapx && r->wrapy)
	{
		u32 pw = 2 * w < 320 ? 2 * w : 320, ph = 2 * h < 200 ? 2 * h : 200;

		if (pw > w || ph > h)
		{
			FOutVector v[4];
			float x1 = -1.0f + 2.0f * (float)pw / 320.0f, y1 = 1.0f - 2.0f * (float)ph / 200.0f, s1 = (float)pw / (float)w, t1 = (float)ph / (float)h;

			hw_GClipRect(0, 0, 320, 200, NZCLIP_DEFAULT);
			v[0].x = v[3].x = -1.0f;
			v[1].x = v[2].x = x1;
			v[0].y = v[1].y = 1.0f;
			v[2].y = v[3].y = y1;
			v[0].z = v[1].z = v[2].z = v[3].z = 1.0f;
			v[0].s = v[3].s = 0.0f;
			v[1].s = v[2].s = s1;
			v[0].t = v[1].t = 0.0f;
			v[2].t = v[3].t = t1;
			tt_clear();
			hw_DrawPolygon(NULL, v, 4, PF_NoDepthTest | PF_ForceWrapX | PF_ForceWrapY);
			if (tt_read(ph))
				tt_compare(&B, pw, ph, 0, 0, w, h);
			else
				snprintf(why, sizeof why, "read-back failed");
		}
	}
	if (A.bad || B.bad || why[0])
	{
		if (A.bad)
			(*n_failA)++;
		if (B.bad)
			(*n_failB)++;
		I_OutputMsg("TTFAIL %s %ux%u psm=%d clut=%u tbw=%u wrap=%u/%u npot=%u/%u region=%u: A %u/%u (first %u,%u exp %06x got %06x) B %u/%u (first %u,%u exp %06x got %06x) %s\n",
			label, w, h, r->psm, r->clut, r->tbw, r->wrapx, r->wrapy, r->npot_x, r->npot_y, r->region, A.bad, A.tested, A.first_x, A.first_y, A.exp, A.got,
			B.bad, B.tested, B.first_x, B.first_y, B.exp, B.got, why);
		if (A.bad)
		{
			u32 q;

			I_OutputMsg("TTDETAIL %s A:", label);
			for (q = 0; q < A.bad && q < 8; q++)
				I_OutputMsg(" (%u,%u e%06x g%06x)", A.bx[q], A.by[q], A.be[q], A.bg[q]);
			I_OutputMsg("\n");
		}
	}
}

void PS2HWD_TextureTest(void)
{
	int i, nt = 0, fa = 0, fb = 0, sk = 0, ms = 0, nd = 0, ntex = 0, nfl = 0;
	char label[32];

	if (!H.up || !H.fb32)
	{
		I_OutputMsg("TT: needs the CT32 frame buffer\n");
		return;
	}
	if (H.vw != 320 || H.vh != 200)
		I_OutputMsg("TT: frame buffer %dx%d, not 320x200 (-hwfbh 200): the repeat test B is skipped\n", H.vw, H.vh);
	TT.on = 1;
	{
		// the map textures the level uses (sides, FOFs included), each once
		u8 *seen = calloc((size_t)numtextures + 1, 1);
		size_t k;
		int j;

		if (!seen)
		{
			I_OutputMsg("TT: no memory\n");
			return;
		}
		for (k = 0; k < numsides; k++)
		{
			INT32 t[3] = {sides[k].toptexture, sides[k].midtexture, sides[k].bottomtexture};

			for (j = 0; j < 3; j++)
				if (t[j] > 0 && t[j] < numtextures)
					seen[t[j]] = 1;
		}
		for (i = 1; i < numtextures; i++)
		{
			int ck;

			if (!seen[i] || !textures[i])
				continue;
			HWR_ClearAllTextures(); // the texture is made and uploaded afresh (the second, chroma keyed variant is a view of the same image)
			for (ck = 0; ck < 2; ck++)
			{
				HWR_GetTexture(texturetranslation[i], ck ? true : false);
				snprintf(label, sizeof label, "T:%.8s%s", textures[i]->name, ck ? "+k" : "");
				tt_one(label, &nt, &fa, &fb, &sk, &ms, &nd);
				ntex++;
			}
			if ((ntex & 63) == 62)
				I_OutputMsg("TT progress: %d textures, tested %d, fail A %d B %d\n", ntex, nt, fa, fb);
		}
		free(seen);
	}
	for (i = 0; i < (int)numlevelflats; i++)
	{
		int ck;

		HWR_ClearAllTextures();
		for (ck = 0; ck < 2; ck++)
		{
			HWR_GetLevelFlat(&levelflats[i], ck ? true : false);
			snprintf(label, sizeof label, "F:%.8s%s", levelflats[i].name, ck ? "+k" : "");
			tt_one(label, &nt, &fa, &fb, &sk, &ms, &nd);
			nfl++;
		}
	}
	TT.on = 0;
	I_OutputMsg("TT done: textures %d flats %d tested %d fail-A %d fail-B %d skipped(no data) %d not-resident %d decimated %d\n", ntex, nfl, nt, fa, fb, sk, ms, nd);
}
