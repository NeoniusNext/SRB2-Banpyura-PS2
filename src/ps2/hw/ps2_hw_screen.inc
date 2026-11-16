// PS2 GS hardware renderer driver: screen textures, wipes, post-processing and read-back (included by ps2_hwd.c).
//
// A screen texture is a full-size copy of the frame buffer region (internal resolution, frame buffer format) made with a
// GS local-to-local transfer, or retained in EE RAM when it exceeds the pool: no loss, no resampling.
// GENERIC2, the final picture, needs no copy at all: it is the last completed frame. Explicit captures always retain their own slot identity, including when spilled to EE RAM.

// the view state the 2D screen draws replace (identity projection over the whole frame)
typedef struct
{
	float mv[16], proj[16], mvp[16];
	int dirty;
	float vpx, vpy, vpw, vph;
	u32 sx0, sy0, sx1, sy1;
	int gx, gy;
} view_save_t;

static void screen_mode_begin(view_save_t *sv)
{
	memcpy(sv->mv, H.mv, sizeof sv->mv);
	memcpy(sv->proj, H.proj, sizeof sv->proj);
	memcpy(sv->mvp, H.mvp, sizeof sv->mvp);
	sv->dirty = H.mat_dirty;
	sv->vpx = H.vpx;
	sv->vpy = H.vpy;
	sv->vpw = H.vpw;
	sv->vph = H.vph;
	sv->sx0 = H.sciss_x0;
	sv->sy0 = H.sciss_y0;
	sv->sx1 = H.sciss_x1;
	sv->sy1 = H.sciss_y1;
	sv->gx = H.guard_x;
	sv->gy = H.guard_y;
	viewport_set(0, 0, H.scr_w, H.scr_h);
	m_identity(H.mvp);
	H.mat_dirty = 0;
}

static void screen_mode_end(const view_save_t *sv)
{
	memcpy(H.mv, sv->mv, sizeof H.mv);
	memcpy(H.proj, sv->proj, sizeof H.proj);
	memcpy(H.mvp, sv->mvp, sizeof H.mvp);
	H.mat_dirty = sv->dirty;
	H.vpx = sv->vpx;
	H.vpy = sv->vpy;
	H.vpw = sv->vpw;
	H.vph = sv->vph;
	H.sciss_x0 = sv->sx0;
	H.sciss_y0 = sv->sy0;
	H.sciss_x1 = sv->sx1;
	H.sciss_y1 = sv->sy1;
	H.guard_x = sv->gx;
	H.guard_y = sv->gy;
}

// copy the frame (or the part of it that is drawn so far) into screen texture `tex`
static int scr_capture(int tex)
{
	int ri;
	texrec_t *r;
	qw_t *p;
	u64 s, d;

	if (OV.n)
		ov_flush();
	frame_prepare(0);
	ri = scr_tex_get(tex);
	if (ri == NOREC)
		return 0;
	tile_invalidate(); // a same-slot recapture can retain its EE pointer and record identity
	r = &H.rec[ri];
	if (r->tiled)
	{
		if (!scr_backing_read(tex, H.fb_addr[H.target] / 256, (u32)(H.fbw / 64)))
		{
			if (!H.scr_data[tex])
				tex_drop(ri, 0);
			return 0;
		}
		r->stamp = H.frame_no;
		return 1;
	}
	scr_backing_free(tex); // this capture replaces the older snapshot, including any spill backing
	r->stamp = H.frame_no;
	s = (u64)(H.fb_addr[H.target] / 256) | ((u64)(H.fbw / 64) << 16) | ((u64)H.fbpsm << 24);
	d = (u64)r->blk | ((u64)r->tbw << 16) | ((u64)H.fbpsm << 24);
	p = ad_alloc(5);
	ad_set(&p[0], s | (d << 32), GSR_BITBLTBUF);
	ad_set(&p[1], 0, GSR_TRXPOS);
	ad_set(&p[2], (u64)H.vw | ((u64)H.vh << 32), GSR_TRXREG);
	ad_set(&p[3], 2, GSR_TRXDIR); // local -> local
	ad_set(&p[4], 0, GSR_TEXFLUSH);
	return 1;
}

// Transfer one source rectangle verbatim, including all CT32 alpha bits used by wipes.
static void screen_copy_rect(const texrec_t *r, int sx, int sy, int dx, int dy, int w, int h)
{
	u64 s = (u64)r->blk | ((u64)r->tbw << 16) | ((u64)H.fbpsm << 24);
	u64 d = (u64)(H.fb_addr[H.target] / 256) | ((u64)(H.fbw / 64) << 16) | ((u64)H.fbpsm << 24);
	qw_t *p = ad_alloc(5);

	ad_set(&p[0], s | (d << 32), GSR_BITBLTBUF);
	ad_set(&p[1], (u64)(u32)sx | ((u64)(u32)sy << 16) | ((u64)(u32)dx << 32) | ((u64)(u32)dy << 48), GSR_TRXPOS);
	ad_set(&p[2], (u64)(u32)w | ((u64)(u32)h << 32), GSR_TRXREG);
	ad_set(&p[3], 2, GSR_TRXDIR);
	ad_set(&p[4], 0, GSR_TEXFLUSH);
}

// The source record is copied: the first tile upload can grow the record table.
static int screen_copy(const texrec_t *source)
{
	texrec_t r = *source, tile;
	int x, y;

	if (!r.tiled)
	{
		screen_copy_rect(&r, 0, 0, 0, 0, H.vw, H.vh);
		return 1;
	}
	for (y = 0; y < H.vh; y += TILE_CORE)
		for (x = 0; x < H.vw; x += TILE_CORE)
		{
			int w = H.vw - x < TILE_CORE ? H.vw - x : TILE_CORE;
			int h = H.vh - y < TILE_CORE ? H.vh - y : TILE_CORE;

			if (!tile_load(&r, x, y, 0, 0, &tile))
				return 0;
			screen_copy_rect(&tile, 1, 1, x, y, w, h);
		}
	return 1;
}

// Choose whole destination pixels whose original affine mapping lies in this tile's core.
// UV endpoints are evaluated from the original rectangle, preserving the phase under scaling.
static int screen_tile_span(int d0, int d1, float t0, float t1, int c0, int c1, int clip,
	int *lo, int *hi, float *uv0, float *uv1)
{
	float dt = t1 - t0, scale, a, b;
	int l, h;

	if (d1 <= d0 || clip <= 0 || dt == 0.0f)
		return 0;
	scale = (float)(d1 - d0) / dt;
	a = (float)d0 + ((float)c0 - t0) * scale;
	b = (float)d0 + ((float)c1 - t0) * scale;
	if (dt > 0.0f)
	{
		l = (int)ceilf(a);
		h = (int)ceilf(b);
	}
	else
	{
		l = (int)floorf(b) + 1;
		h = (int)floorf(a) + 1;
	}
	if (l < d0) l = d0;
	if (h > d1) h = d1;
	if (l < 0) l = 0;
	if (h > clip) h = clip;
	if (l >= h)
		return 0;
	*lo = l;
	*hi = h;
	*uv0 = t0 + (float)(l - d0) * dt / (float)(d1 - d0) - (float)c0 + 1.0f;
	*uv1 = t0 + (float)(h - d0) * dt / (float)(d1 - d0) - (float)c0 + 1.0f;
	return 1;
}

// Sprites used by wipes retain their exact frame mask, blend equation and CLUT override.
// A one-texel halo permits bilinear reads at a core edge without clamping a neighbouring sample.
static int sprite_screen(texrec_t *source, int linear, u64 frame, u64 alpha, int abe,
	int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1,
	int sciss_w, int sciss_h, u32 clut_blk)
{
	texrec_t r = *source, tile;
	int x, y, sw = r.w, sh = r.h;

	if (!r.tiled)
	{
		sprite_tex(&r, linear, frame, alpha, abe, x0, y0, x1, y1, u0, v0, u1, v1, sciss_w, sciss_h, clut_blk);
		return 1;
	}
	for (y = 0; y < sh; y += TILE_CORE)
		for (x = 0; x < sw; x += TILE_CORE)
		{
			int dx0, dy0, dx1, dy1;
			int cw = sw - x < TILE_CORE ? sw - x : TILE_CORE;
			int ch = sh - y < TILE_CORE ? sh - y : TILE_CORE;
			float tu0, tv0, tu1, tv1;

			if (!screen_tile_span(x0, x1, u0, u1, x, x + cw, sciss_w, &dx0, &dx1, &tu0, &tu1)
				|| !screen_tile_span(y0, y1, v0, v1, y, y + ch, sciss_h, &dy0, &dy1, &tv0, &tv1))
				continue;
			if (!tile_load(&r, x, y, 0, 0, &tile))
				return 0;
			sprite_tex(&tile, linear, frame, alpha, abe, dx0, dy0, dx1, dy1, tu0, tv0, tu1, tv1, sciss_w, sciss_h, clut_blk);
		}
	return 1;
}

// full-screen quad in NDC with the texture coordinates of a screen texture (row 0 at the top of the picture)
static void screen_quad(FOutVector *q, float xoff, float yoff)
{
	q[0].x = -xoff; q[0].y = -yoff; q[0].s = 0.0f; q[0].t = 1.0f;
	q[1].x = -xoff; q[1].y = yoff; q[1].s = 0.0f; q[1].t = 0.0f;
	q[2].x = xoff; q[2].y = yoff; q[2].s = 1.0f; q[2].t = 0.0f;
	q[3].x = xoff; q[3].y = -yoff; q[3].s = 1.0f; q[3].t = 1.0f;
	q[0].z = q[1].z = q[2].z = q[3].z = 0.0f;
}

// draw a screen texture over the whole frame buffer; surf may be NULL
static void draw_screen_tex(int slot, FSurfaceInfo *surf, u32 flags, float xoff, float yoff)
{
	FOutVector q[4];
	view_save_t sv;
	int saved_tex = H.cur_tex, saved_missing = H.cur_missing;
	int ri, saved_img = saved_tex != NOREC && H.rec[saved_tex].used ? img_of(saved_tex) : NOREC;
	u8 pinned = saved_img != NOREC && H.rec[saved_img].used ? H.rec[saved_img].pin : 0;

	if (saved_img != NOREC && H.rec[saved_img].used)
		H.rec[saved_img].pin = 1;
	ri = scr_resident(slot);

	if (ri == NOREC)
	{
		if (saved_img != NOREC && H.rec[saved_img].used)
			H.rec[saved_img].pin = pinned;
		hw_limit(HW_MISSING, "screen draw slot unavailable; draw rejected");
		return;
	}
	if (!surf && flags == PF_NoDepthTest && xoff == 1.0f && yoff == 1.0f)
	{
		// Identity presentation is an exact GS transfer: no bilinear triangle-interpolation rounding and no fill cost.
		ov_flush_all();
		frame_prepare(0);
		if (!screen_copy(&H.rec[ri]))
			hw_limit(HW_MISSING, "screen presentation tile upload failed");
		H.rec[ri].stamp = H.frame_no;
		if (saved_img != NOREC && H.rec[saved_img].used)
			H.rec[saved_img].pin = pinned;
		return;
	}
	screen_quad(q, xoff, yoff);
	H.cur_tex = ri;
	H.cur_missing = 0;
	H.rec[H.cur_tex].stamp = H.frame_no;
	screen_mode_begin(&sv);
	if (begin_draw(flags & ~(u32)(PF_ColorMapped | PF_Occlude), surf))
		emit_fan(q, NULL, 4, NULL);
	screen_mode_end(&sv);
	if (saved_img != NOREC && H.rec[saved_img].used)
		H.rec[saved_img].pin = pinned;
	H.cur_tex = saved_tex;
	H.cur_missing = saved_missing;
}

static void hw_PostImgRedraw(float points[SCREENVERTS][SCREENVERTS][2])
{
	int x, y, slot = HWD_SCREENTEXTURE_GENERIC1;
	view_save_t sv;
	FOutVector q[4];
	int saved_tex = H.cur_tex, saved_missing = H.cur_missing;
	FSurfaceInfo black_surf;
	u8 pinned;
	int ri, saved_img;

	if (!H.up)
		return;
	saved_img = saved_tex != NOREC && H.rec[saved_tex].used ? img_of(saved_tex) : NOREC;
	pinned = saved_img != NOREC && H.rec[saved_img].used ? H.rec[saved_img].pin : 0;
	if (saved_img != NOREC && H.rec[saved_img].used) H.rec[saved_img].pin = 1;
	ri = scr_resident(slot);
	if (ri == NOREC)
	{
		if (saved_img != NOREC && H.rec[saved_img].used) H.rec[saved_img].pin = pinned;
		hw_limit(HW_MISSING, "post-image capture unavailable; draw rejected");
		return;
	}
	H.rec[H.scr_rec[slot]].stamp = H.frame_no;
	screen_mode_begin(&sv);
	// a black square behind the screen texture so that nothing shows through at the edges
	q[0].x = -1.0f; q[0].y = -1.0f;
	q[1].x = -1.0f; q[1].y = 1.0f;
	q[2].x = 1.0f; q[2].y = 1.0f;
	q[3].x = 1.0f; q[3].y = -1.0f;
	q[0].z = q[1].z = q[2].z = q[3].z = 0.0f;
	q[0].s = q[0].t = q[1].s = q[1].t = q[2].s = q[2].t = q[3].s = q[3].t = 0.0f;
	memset(&black_surf, 0, sizeof black_surf);
	black_surf.PolyColor.s.alpha = 255;
	H.cur_tex = NOREC;
	if (begin_draw(PF_NoTexture | PF_Modulated | PF_NoDepthTest, &black_surf))
		emit_fan(q, NULL, 4, NULL);
	H.cur_tex = H.scr_rec[slot];
	H.cur_missing = 0;
	if (begin_draw(PF_NoDepthTest, NULL))
	{
		for (x = 0; x < SCREENVERTS - 1; x++)
		{
			for (y = 0; y < SCREENVERTS - 1; y++)
			{
				// the grid of the engine is in the units of the 3D view at z = 4.4: back to NDC
				const float k = 1.0f / 4.4f;
				float s0 = (float)x / (float)(SCREENVERTS - 1), s1 = (float)(x + 1) / (float)(SCREENVERTS - 1);
				float t0 = (float)y / (float)(SCREENVERTS - 1), t1 = (float)(y + 1) / (float)(SCREENVERTS - 1);

				q[0].x = points[x][y][0] * k;         q[0].y = points[x][y][1] * k;         q[0].s = s0; q[0].t = 1.0f - t0;
				q[1].x = points[x][y + 1][0] * k;     q[1].y = points[x][y + 1][1] * k;     q[1].s = s0; q[1].t = 1.0f - t1;
				q[2].x = points[x + 1][y + 1][0] * k; q[2].y = points[x + 1][y + 1][1] * k; q[2].s = s1; q[2].t = 1.0f - t1;
				q[3].x = points[x + 1][y][0] * k;     q[3].y = points[x + 1][y][1] * k;     q[3].s = s1; q[3].t = 1.0f - t0;
				emit_fan(q, NULL, 4, NULL);
			}
		}
	}
	screen_mode_end(&sv);
	if (saved_img != NOREC && H.rec[saved_img].used)
		H.rec[saved_img].pin = pinned;
	H.cur_tex = saved_tex;
	H.cur_missing = saved_missing;
}

static void hw_FlushScreenTextures(void)
{
	int i;

	if (!H.up)
		return;
	for (i = 0; i < NSCR; i++)
	{
		if (H.scr_rec[i] != NOREC)
			tex_drop(H.scr_rec[i], 0);
		scr_backing_free(i);
	}
}

// A wipe. Normal: the end picture over the start picture with the fade mask (an ALPHA_8 texture, the current texture) as the
// alpha of each pixel: the mask goes into the frame buffer's alpha channel (the CLUT of ALPHA_8 data scales it to 0..0x80), then
// the end picture is blended with the destination alpha as the factor. Continuous, like the OpenGL driver's two texture units.
// Tinted (surf != NULL): the base picture, then per colour channel the shader's min(1, colour * 24.875 * mask) added / subtracted.
static void hw_DoScreenWipe(int wipeStart, int wipeEnd, FSurfaceInfo *surf, FBITFIELD polyFlags)
{
	view_save_t sv;
	texrec_t *mask;
	int first, maskri, bri, eri;

	if (!H.up)
		return;
	if (OV.n)
		ov_flush();
	frame_prepare(1);
	do_clear(1, 1, NULL);
	first = (surf && surf->PolyColor.s.alpha == 255) ? wipeEnd : wipeStart;
	if (wipeStart < 0 || wipeStart >= NSCR || wipeEnd < 0 || wipeEnd >= NSCR || H.cur_tex == NOREC)
	{
		hw_failure("wipe mask/start/end capture unavailable; frame rejected");
		return;
	}
	maskri = H.cur_tex;
	mask = &H.rec[maskri];
	if (mask->clut != 3)
	{
		hw_limit(HW_WIPE, "wipe requires an ALPHA_8 mask; end draw rejected");
		return;
	}
	H.rec[maskri].pin = 1; // restoring a capture must not evict the selected engine mask
	bri = scr_resident(first);
	if (bri == NOREC)
	{
		H.rec[maskri].pin = 0;
		hw_limit(HW_MISSING, "wipe base capture cannot be made resident");
		return;
	}
	H.rec[bri].stamp = H.frame_no;
	screen_mode_begin(&sv);
	// the base picture
	if (!sprite_screen(&H.rec[bri], 0, frame_reg(H.target, 0), ALPHA_TRANS, 0, 0, 0, H.vw, H.vh, 0.0f, 0.0f, (float)H.vw, (float)H.vh, H.vw, H.vh, 0))
		goto tile_failed;
	mask = &H.rec[maskri]; // the record table can grow while restoring a capture
	if (!H.fb32)
		hw_limit(HW_WIPE, "CT16S frame buffer has a 1-bit alpha: the fade mask is thresholded");
	if (surf)
	{
		int ch;
		u32 pc[3];

		pc[0] = surf->PolyColor.s.red;
		pc[1] = surf->PolyColor.s.green;
		pc[2] = surf->PolyColor.s.blue;
		for (ch = 0; ch < 3; ch++)
		{
			u32 clut = clut_get(CK_WIPE, (u32)ch | ((surf->PolyColor.s.alpha >= 128 ? 1u : 0u) << 2) | (pc[ch] << 8), 0);

			mask = &H.rec[maskri]; // tile cache creation can grow the record table
			if (!sprite_screen(mask, 0, frame_reg(H.target, ~(0xFFu << (8 * ch))), (polyFlags & PF_Additive) ? ALPHA_ADD : ALPHA_RSUB, 1, 0, 0, H.vw, H.vh,
				0.0f, 0.0f, (float)mask->w, (float)mask->h, H.vw, H.vh, clut))
				goto tile_failed;
		}
	}
	else
	{
		eri = scr_resident(wipeEnd); // the base has already been drawn, so it can spill safely
		if (eri == NOREC)
		{
			screen_mode_end(&sv);
			H.rec[maskri].pin = 0;
			hw_limit(HW_MISSING, "wipe end capture cannot be made resident");
			return;
		}
		mask = &H.rec[maskri];
		H.rec[eri].stamp = H.frame_no;
		// 1. the mask into the alpha channel only
		if (!sprite_screen(mask, 0, frame_reg(H.target, 0x00FFFFFFu), ALPHA_TRANS, 0, 0, 0, H.vw, H.vh, 0.0f, 0.0f, (float)mask->w, (float)mask->h, H.vw, H.vh, 0))
			goto tile_failed;
		// 2. the end picture where the mask says so
		if (!sprite_screen(&H.rec[eri], 0, frame_reg(H.target, 0), ALPHA_AD, 1, 0, 0, H.vw, H.vh, 0.0f, 0.0f, (float)H.vw, (float)H.vh, H.vw, H.vh, 0))
			goto tile_failed;
	}
	screen_mode_end(&sv);
	H.rec[maskri].pin = 0;
	H.cur_tex = maskri;
	return;

tile_failed:
	screen_mode_end(&sv);
	H.rec[maskri].pin = 0;
	H.cur_tex = maskri;
	hw_limit(HW_MISSING, "wipe tile upload failed; remaining passes rejected");
}

static void hw_DrawScreenTexture(int tex, FSurfaceInfo *surf, FBITFIELD polyflags)
{
	if (!H.up)
		return;
	do_clear(1, 1, NULL);
	draw_screen_tex(tex, surf, surf ? (u32)polyflags : (u32)PF_NoDepthTest, 1.0f, 1.0f);
}

static void hw_MakeScreenTexture(int tex)
{
	if (!H.up)
		return;
	if (tex < 0 || tex >= NSCR)
	{
		hw_failure("capture requested for an invalid screen slot");
		return;
	}
	if (!scr_capture(tex))
	{
		char message[160];

		snprintf(message, sizeof message, "screen capture slot %d unavailable (GS %u/%u blocks, EE %u bytes)", (int)tex, H.used_blocks, H.pool_blocks, H.screen_bytes);
		hw_limit(HW_MISSING, message);
	}
}

// OpenGL driver: viewport = the window (width x height), the picture is letterboxed to keep the engine's aspect
static void hw_DrawScreenFinalTexture(int tex, int width, int height)
{
	FRGBAFloat black = {0.0f, 0.0f, 0.0f, 1.0f};
	float origaspect, newaspect, xoff = 1.0f, yoff = 1.0f;

	if (!H.up)
		return;
	origaspect = (float)H.scr_w / (float)H.scr_h;
	newaspect = width > 0 && height > 0 ? (float)width / (float)height : origaspect;
	if (origaspect < newaspect)
		xoff = origaspect / newaspect;
	else if (origaspect > newaspect)
		yoff = newaspect / origaspect;
	if (tex == HWD_SCREENTEXTURE_GENERIC2 && H.scr_rec[tex] == NOREC && !H.scr_data[tex])
	{
		if (xoff == 1.0f && yoff == 1.0f)
			return; // zero-copy presentation: do not clear the picture we are presenting
		if (!scr_capture(tex)) // save the picture BEFORE clearing for letterbox bars
		{
			hw_limit(HW_MISSING, "final aspect correction capture failed");
			return;
		}
	}
	if (scr_resident(tex) == NOREC)
	{
		hw_limit(HW_MISSING, "final screen slot unavailable");
		return;
	}
	do_clear(1, 0, &black);
	draw_screen_tex(tex, NULL, PF_NoDepthTest, xoff, yoff);
}

// ---- read-back (screenshots, tests) ----

// Reads the selected capture without changing the current frame or renderer state. Uncaptured GENERIC2 is the last completed
// frame (screenshots after FinishUpdate see the picture just presented). The picture is resampled by bilinear pixel-centre sampling when
// the internal size differs from the engine's size. FALSE leaves *dst_data* unchanged.
boolean PS2HWD_ReadScreenRGB(INT32 tex, UINT8 *dst_data)
{
	void *pixels;
	u32 blk, tbw;
	int bpp = H.fb32 ? 4 : 2, rows = H.vh, stride, dy, dx, owned = 1, banded = 0, bb0 = 0, bn = 0;
	float ry, rx;
	static u8 *band; // OPT5: when the whole picture does not fit into RAM it is read 8 source rows at a time (PS2-171: taken at the first need, was a 32 KB .bss array)
#define BAND_BYTES (8 * 1024 * 4)

	if (!H.up || !dst_data)
	{
		hw_failure("screen readback without active driver/destination");
		return false;
	}
	if (tex < 0 || tex >= NSCR)
	{
		hw_failure("readback of an invalid screen slot; destination untouched");
		return false;
	}
	if (tex == HWD_SCREENTEXTURE_GENERIC2 && H.scr_rec[tex] == NOREC && !H.scr_data[tex])
	{
		if (!H.have_drawn)
		{
			hw_failure("no frame was completed yet; destination untouched");
			return false;
		}
		blk = H.fb_addr[H.last_drawn] / 256;
		tbw = (u32)(H.fbw / 64);
	}
	else
	{
		if (!H.scr_data[tex] && (H.scr_rec[tex] == NOREC || H.rec[H.scr_rec[tex]].tiled))
		{
			hw_failure("readback screen slot unavailable; destination untouched");
			return false;
		}
		blk = H.scr_rec[tex] == NOREC ? 0 : H.rec[H.scr_rec[tex]].blk;
		tbw = (u32)(H.fbw / 64);
	}
	stride = (int)tbw * 64;
	if (H.scr_data[tex])
	{
		pixels = H.scr_data[tex];
		owned = 0;
	}
	else
		pixels = memalign(64, (size_t)stride * (size_t)rows * (size_t)bpp);
	if (!pixels && !band && stride * bpp * 8 <= BAND_BYTES)
		band = memalign(64, BAND_BYTES);
	if (!pixels && band && stride * bpp * 8 <= BAND_BYTES)
	{
		banded = 1; // no room for the whole picture: PS2-HW-21
		pixels = band;
		owned = 0;
	}
	if (!pixels)
	{
		hw_failure("screen readback RAM allocation failed");
		return false;
	}
	if (owned && PS2HWD_ReadVram(pixels, blk, tbw, H.fbpsm, 0, 0, stride, rows) != 0)
	{
		free(pixels);
		hw_failure("screen readback failed; destination untouched");
		return false;
	}
	ry = (float)H.vh / (float)H.scr_h;
	rx = (float)H.vw / (float)H.scr_w;
	for (dy = 0; dy < H.scr_h; dy++)
	{
		float fy = clampf(((float)dy + 0.5f) * ry - 0.5f, 0.0f, (float)(rows - 1));
		int y0 = (int)fy, y1 = y0 + 1 < rows ? y0 + 1 : y0;
		float wy = fy - (float)y0;
		for (dx = 0; dx < H.scr_w; dx++)
		{
			float fx = clampf(((float)dx + 0.5f) * rx - 0.5f, 0.0f, (float)(H.vw - 1)), wx;
			int x0 = (int)fx, x1 = x0 + 1 < H.vw ? x0 + 1 : x0, k;
			u32 c[4], at[4];

			wx = fx - (float)x0;
			at[0] = (u32)(y0 * stride + x0); at[1] = (u32)(y0 * stride + x1);
			at[2] = (u32)(y1 * stride + x0); at[3] = (u32)(y1 * stride + x1);
			if (banded)
			{
				if (y0 < bb0 || y1 >= bb0 + bn) // the two source rows are not in the band: read the next 8 rows from y0
				{
					bb0 = y0;
					bn = rows - y0 < 8 ? rows - y0 : 8;
					if (PS2HWD_ReadVram(band, blk, tbw, H.fbpsm, 0, bb0, stride, bn) != 0)
					{
						hw_failure("screen readback failed; destination untouched");
						return false;
					}
				}
				for (k = 0; k < 4; k++)
					at[k] -= (u32)(bb0 * stride);
			}
			for (k = 0; k < 4; k++)
				c[k] = expand_pixel(bpp == 4 ? ((u32 *)pixels)[at[k]] : ((u16 *)pixels)[at[k]], bpp == 4);
			for (k = 0; k < 3; k++)
			{
				float a = (float)((c[0] >> (8 * k)) & 255), b = (float)((c[1] >> (8 * k)) & 255);
				float d = (float)((c[2] >> (8 * k)) & 255), e = (float)((c[3] >> (8 * k)) & 255);
				float top = a + (b - a) * wx, bottom = d + (e - d) * wx;
				*dst_data++ = (UINT8)(top + (bottom - top) * wy + 0.5f);
			}
		}
	}
	if (owned)
		free(pixels);
	return true;
}

static void hw_ReadScreenTexture(int tex, UINT8 *dst_data)
{
	if (!PS2HWD_ReadScreenRGB(tex, dst_data))
	{
		// hw_failure aborted the engine; standalone tests continue: never hand back stale pixels
		if (dst_data && H.up)
			memset(dst_data, 0, (size_t)H.scr_w * (size_t)H.scr_h * 3);
	}
}

boolean PS2HWD_CaptureScreen(INT32 tex)
{
	if (!H.up || tex < 0 || tex >= NSCR)
	{
		hw_failure("capture requested without active driver/valid slot");
		return false;
	}
	return scr_capture(tex);
}
