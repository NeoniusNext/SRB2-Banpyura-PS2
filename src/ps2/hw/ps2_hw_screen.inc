// PS2 GS hardware renderer driver: screen textures, wipes, post-processing and read-back (included by ps2_hwd.c).
//
// A screen texture is a full-size copy of the frame buffer region (internal resolution, frame buffer format) made with a
// GS local-to-local transfer: no loss, no resampling. GENERIC2, the final picture, needs no copy at all: it is the last
// completed frame. Explicit captures always retain their own slot identity, including when spilled to EE RAM.

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
	r = &H.rec[ri];
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
	int ri;
	u8 pinned = saved_tex != NOREC ? H.rec[saved_tex].pin : 0;

	if (saved_tex != NOREC)
		H.rec[saved_tex].pin = 1;
	ri = scr_resident(slot);
	if (saved_tex != NOREC)
		H.rec[saved_tex].pin = pinned;

	if (ri == NOREC)
	{
		hw_limit(HW_MISSING, "screen draw slot unavailable; draw rejected");
		return;
	}
	if (!surf && flags == PF_NoDepthTest && xoff == 1.0f && yoff == 1.0f)
	{
		// Identity presentation is an exact GS transfer: no bilinear triangle-interpolation rounding and no fill cost.
		qw_t *p;
		u64 s, d;
		ov_flush_all();
		frame_prepare(0);
		s = (u64)H.rec[ri].blk | ((u64)H.rec[ri].tbw << 16) | ((u64)H.fbpsm << 24);
		d = (u64)(H.fb_addr[H.target] / 256) | ((u64)(H.fbw / 64) << 16) | ((u64)H.fbpsm << 24);
		p = ad_alloc(5);
		ad_set(&p[0], s | (d << 32), GSR_BITBLTBUF);
		ad_set(&p[1], 0, GSR_TRXPOS);
		ad_set(&p[2], (u64)H.vw | ((u64)H.vh << 32), GSR_TRXREG);
		ad_set(&p[3], 2, GSR_TRXDIR);
		ad_set(&p[4], 0, GSR_TEXFLUSH);
		H.rec[ri].stamp = H.frame_no;
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
	int ri;

	if (!H.up)
		return;
	pinned = saved_tex != NOREC ? H.rec[saved_tex].pin : 0;
	if (saved_tex != NOREC) H.rec[saved_tex].pin = 1;
	ri = scr_resident(slot);
	if (saved_tex != NOREC) H.rec[saved_tex].pin = pinned;
	if (ri == NOREC)
	{
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
	sprite_tex(&H.rec[bri], 0, frame_reg(H.target, 0), ALPHA_TRANS, 0, 0, 0, H.vw, H.vh, 0.0f, 0.0f, (float)H.vw, (float)H.vh, H.vw, H.vh, 0);
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

			sprite_tex(mask, 0, frame_reg(H.target, ~(0xFFu << (8 * ch))), (polyFlags & PF_Additive) ? ALPHA_ADD : ALPHA_RSUB, 1, 0, 0, H.vw, H.vh,
				0.0f, 0.0f, (float)mask->w, (float)mask->h, H.vw, H.vh, clut);
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
		sprite_tex(mask, 0, frame_reg(H.target, 0x00FFFFFFu), ALPHA_TRANS, 0, 0, 0, H.vw, H.vh, 0.0f, 0.0f, (float)mask->w, (float)mask->h, H.vw, H.vh, 0);
		// 2. the end picture where the mask says so
		sprite_tex(&H.rec[eri], 0, frame_reg(H.target, 0), ALPHA_AD, 1, 0, 0, H.vw, H.vh, 0.0f, 0.0f, (float)H.vw, (float)H.vh, H.vw, H.vh, 0);
	}
	screen_mode_end(&sv);
	H.rec[maskri].pin = 0;
	H.cur_tex = maskri;
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

		snprintf(message, sizeof message, "screen capture slot %d does not fit the GS pool (%u/%u blocks used)", (int)tex, H.used_blocks, H.pool_blocks);
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
	static u8 band[8 * 1024 * 4] __attribute__((aligned(64))); // OPT5: when the whole picture does not fit into RAM it is read 8 source rows at a time

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
		if (H.scr_rec[tex] == NOREC && !H.scr_data[tex])
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
	if (!pixels && stride * bpp * 8 <= (int)sizeof band)
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
