/* Every comparison below covers the complete visible GS frame. */
_Static_assert(sizeof(GLMipmap_t) == 68, "native EE packed mipmap ABI");
void TN_Pool(unsigned int blocks);
int TN_Upload(GLMipmap_t *m, unsigned int *info);
int TN_Raw(unsigned int *dest);
int TN_Slot(int slot, unsigned int *info);
int TN_Spill(void);
int TN_AttributeFog(int model_light, int zwrite);
void TN_AttributeCell(int on);
#ifdef TN_DYNAMIC_TILE_PROBE
void TN_PhysicalTileInfo(u32 *out);
#endif
#ifdef TN_BORROW_PROBE
void TN_BorrowInfo(u32 *out);
int TN_BorrowCapture(int slot, u32 *out);
int TN_BorrowBlockPrefix(void);
int TN_BorrowReclaimOverwrite(u32 *out);
void TN_BorrowCleanup(void);
#endif
#ifdef TN_ONECORE_PROBE
void TN_OneCoreInfo(u32 *out);
#endif
#ifdef TN_STENCIL_PROBE
int TN_ZRaw(u32 *dest);
#endif

static u32 *tn_ref, *tn_a, *tn_b;
static u8 *tn_rgb;
static int tn_cell_case;
static const u32 tn_bg = 0x00352b1du;

static void tn_save(const char *name, const u32 *pixels)
{
	char path[96]; FILE *f;
	snprintf(path, sizeof path, "host:tn-%s-%dx%d.rgba", name, FW, FH);
	f = fopen(path, "wb");
	if (f) { fwrite(pixels, 4, (size_t)FW * FH, f); fclose(f); }
	dump_ppm(name, pixels);
}

static int tn_compare(const char *name, const u32 *got, const u32 *want, int alpha, int tolerance)
{
	int exact = 0, beyond = 0, maxerr = 0, uncovered = 0, expected = 0, i, c;
	unsigned long long totalerr = 0;
	for (i = 0; i < FW * FH; i++) {
		int e = 0;
		for (c = 0; c < (alpha ? 4 : 3); c++) {
			int d = abs((int)((got[i] >> (8 * c)) & 255) - (int)((want[i] >> (8 * c)) & 255));
			if (d > e) e = d;
			totalerr += (unsigned)d;
		}
		exact += e != 0; beyond += e > tolerance; if (e > maxerr) maxerr = e;
		expected += (want[i] & 0xffffffu) != tn_bg;
		uncovered += (got[i] & 0xffffffu) == tn_bg && (want[i] & 0xffffffu) != tn_bg;
	}
	printf("TN PIXELS name=%s size=%dx%d compared=%d rgba=%d exactdiff=%d tolerance=%d beyond=%d maxerr=%d totalerr=%llu uncovered=%d expected_coverage=%d\n",
		name, FW, FH, FW * FH, alpha, exact, tolerance, beyond, maxerr, totalerr, uncovered, expected);
	return beyond;
}

static void tn_init(int w, int h, int mode)
{
	ps2hwd_config_t c;
	PS2HWD_GetConfig(&c);
	c.video = mode; c.fbw = w; c.fbh = h; c.fb32 = 1;
	c.linear = 0; c.dither = 0; c.screen_w = w; c.screen_h = h;
	c.tex_cap_blocks = 0; c.tex_adapt = 0; c.tex_max_bytes = 0;
	c.screen_max_bytes = 8u * 1024u * 1024u;
	PS2HWD_Configure(&c); PS2HWD_FillDriver(&D);
	CHECK("tn_init", D.pfnInit(), "requested=%dx%d mode=%d", w, h, mode);
	D.pfnSetTexturePalette(PAL); PS2HWD_GetInfo(&INFO);
	FW = INFO.vw; FH = INFO.vh; FB32 = INFO.fb32; SX = (float)FW / SW; SY = (float)FH / SH;
	ps2hwd_dbg_flags = 0x2000000; /* established original-resolution mode */
	if (arg("commonpath")) ps2hwd_dbg_flags |= 64 | 128 | 8192;
	printf("TN MODE logical=%dx%d visible=%dx%d stride=%dx%d pool=%u reserve=%u\n", INFO.screen_w, INFO.screen_h, FW, FH, INFO.fbw, INFO.fbh, INFO.pool_blocks, INFO.pool_used_blocks);
}

static void tn_pattern(int seed)
{
	int x, y;
	frame_start(tn_bg);
	for (y = 0; y < 10; y++) for (x = 0; x < 16; x++) {
		FSurfaceInfo s = surf_rgba(24 + ((x * 11 + y * 13 + seed * 37) % 210),
			19 + ((x * 7 + y * 17 + seed * 61) % 210), 28 + ((x * 19 + y * 5 + seed * 43) % 210),
			16 + ((x * 31 + y * 23 + seed * 19) % 240));
		quad2d(&s, x * 20.0f, y * 20.0f, (x + 1) * 20.0f, (y + 1) * 20.0f,
			0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
	}
}

static int tn_grab(u32 *out)
{
	D.pfnFinishUpdate(0);
	return TN_Raw(out);
}

static GLMipmap_t *tn_texture(int kind, int w, int h)
{
	GLTextureFormat_t fmt = kind == 0 ? GL_TEXFMT_P_8 : kind == 1 ? GL_TEXFMT_AP_88 : GL_TEXFMT_RGBA;
	GLMipmap_t *m = tex_new(w, h, fmt, TF_WRAPXY | (kind == 0 ? TF_CHROMAKEYED : 0));
	int x, y;
	CHECK("tn_texture_alloc", m && m->data, "format=%d size=%dx%d", kind, w, h);
	for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
		const int idx = 20 + ((x / 3 + y / 5 + (x / 61) * 29 + (y / 61) * 37) % 210);
		const int hole = ((x / 19 + y / 17) % 13) == 0;
		if (kind == 0) ((u8 *)m->data)[y * w + x] = hole ? 255 : (u8)idx;
		else if (kind == 1) ((u16 *)m->data)[y * w + x] = (u16)(idx | ((hole ? 0 : (x % 13 == 0 ? 143 : 255)) << 8));
		else ((u32 *)m->data)[y * w + x] = (u32)(31 + (x * 3 + y * 7) % 191)
			| ((u32)(29 + (x * 5 + y * 3) % 193) << 8) | ((u32)(37 + (x * 7 + y * 11) % 181) << 16)
			| ((u32)(hole ? 0 : (x % 11 == 0 ? 137 : 255)) << 24);
	}
	return m;
}

static void tn_geometry(GLMipmap_t *m, int shape)
{
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	u32 flags = PF_Modulated | PF_NoDepthTest | (m->format == GL_TEXFMT_P_8 ? PF_Masked : PF_Translucent);
	if (shape == 0)
		quad2d(&s, 0, 0, 320, 200, 0, 0, 1, 1, flags);
	else if (shape == 1)
		quad2d(&s, -17.5f, -11.25f, 337.5f, 213.25f, -0.319f, -0.231f, 0.719f, 0.681f, flags);
	else if (shape == 2)
		quad2d(&s, 0, 0, 320, 200, 30.25f / m->width, 50.25f / m->height,
			(30.25f + 248.0f) / m->width, (50.25f + 124.0f) / m->height, flags);
	else if (shape == 5 || shape == 6)
		quad2d(&s, 36, 38, 284, 162, 0, 0, (shape == 5 ? 248.0f : 124.0f) / m->width,
			(shape == 5 ? 124.0f : 62.0f) / m->height, flags);
	else if (shape == 7)
		quad2d(&s, 152.3125f, 96.1875f, 168.3125f, 104.1875f,
			0.137f / m->width, 0.293f / m->height, 1 + 0.137f / m->width, 1 + 0.293f / m->height, flags);
	else if (shape == 9) {
		int phase;
		for (phase = 0; phase < 16; phase++) {
			float x = 10 + (phase % 8) * 38 + (float)phase / 16;
			float y = 50 + (phase / 8) * 64 + (float)((phase * 7) % 16) / 16;
			quad2d(&s, x, y, x + 16, y + 8, 0.137f / m->width, 0.293f / m->height,
				1 + 0.137f / m->width, 1 + 0.293f / m->height, flags);
		}
	}
	else {
		FTransform t; FOutVector v[4];
		if (shape == 4) {
			static u8 indices[32 * 256]; int row, index;
			for (row = 0; row < 32; row++) for (index = 0; index < 256; index++)
				indices[row * 256 + index] = (u8)((index + row * 7) % 255);
			s.LightTableId = PS2HWD_CreateIndexedLightTable(indices, PAL);
			s.LightInfo.light_level = 144; s.LightInfo.fade_start = 0; s.LightInfo.fade_end = 31;
			D.pfnSetSpecialState(HWD_SET_SHADERS, 1); D.pfnSetShader(0); flags |= PF_ColorMapped;
		}
		memset(&t, 0, sizeof t); t.scalex = t.scalez = 1; t.scaley = 1.6f; t.fovxangle = 90;
		D.pfnSetTransform(&t);
		v[0] = (FOutVector){80, -70, -100, -0.12f, 1.14f};
		v[1] = (FOutVector){520, -210, 410, 1.12f, 1.14f};
		v[2] = (FOutVector){520, 210, 410, 1.12f, -0.14f};
		v[3] = (FOutVector){80, 70, -100, -0.12f, -0.14f};
		if (shape == 8) {
			v[0] = (FOutVector){80, -4, -5, -0.2f, 1.2f};
			v[1] = (FOutVector){1600, -25, 40, 1.2f, 1.2f};
			v[2] = (FOutVector){1600, 25, 40, 1.2f, -0.2f};
			v[3] = (FOutVector){80, 4, -5, -0.2f, -0.2f};
		}
		if (arg("triangles")) {
			FOutVector second[3] = {v[0], v[2], v[3]};
			D.pfnDrawPolygon(&s, v, 3, flags); D.pfnDrawPolygon(&s, second, 3, flags);
		} else D.pfnDrawPolygon(&s, v, 4, flags);
		D.pfnSetTransform(NULL);
		if (shape == 4) { D.pfnSetShader(-1); D.pfnSetSpecialState(HWD_SET_SHADERS, 0); }
	}
}

static void tn_texture_pair(int kind, int w, int h, int shape, int linear)
{
	GLMipmap_t *m = tn_texture(kind, w, h); unsigned int info[8] = {0};
	char name[80]; int variant, beyond;
	if (kind == 0 && (linear || shape >= 7)) m->flags &= ~(u32)TF_CHROMAKEYED;
	snprintf(name, sizeof name, "texture-f%d-%dx%d-s%d-l%d", kind, w, h, shape, linear);
	for (variant = 0; variant < 2; variant++) {
		TN_Pool(variant ? 512 : 0);
		D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, linear ? HWD_SET_TEXTUREFILTER_BILINEAR : HWD_SET_TEXTUREFILTER_POINTSAMPLED);
		frame_start(tn_bg);
		CHECK("tn_upload", TN_Upload(m, info), "name=%s variant=%d", name, variant);
		printf("TN SOURCE name=%s variant=%d tiled=%u psm=%u size=%ux%u blocks=%u clut=%u level=%u/%u\n", name, variant, info[0], info[1], info[2], info[3], info[4], info[5], info[6], info[7]);
		CHECK("tn_admission", info[0] == (unsigned)variant && !info[6] && !info[7], "name=%s variant=%d", name, variant);
		tn_geometry(m, shape);
#ifdef TN_DYNAMIC_TILE_PROBE
		if (variant) {
			u32 physical[4]; TN_PhysicalTileInfo(physical);
			printf("TN TILEPHYSICAL name=%s valid=%u layout=%u extent=%u blocks=%u\n", name, physical[0], physical[1], physical[2], physical[3]);
			if (arg("indexed-halo")) CHECK("tn_halo_physical_extent", physical[0] && physical[2] == (kind == 0 ? 128u : 64u) && physical[3] == 64, "name=%s extent=%u blocks=%u", name, physical[2], physical[3]);
		}
#endif
		CHECK("tn_readback", tn_grab(variant ? fb : tn_ref) == 0, "name=%s variant=%d", name, variant);
		{ char image_name[96]; snprintf(image_name, sizeof image_name, "%s-v%d", name, variant); tn_save(image_name, variant ? fb : tn_ref); }
	}
	beyond = tn_compare(name, fb, tn_ref, 0, shape >= 5 ? 0 : linear ? 3 : 0);
	CHECK("tn_texture_parity", beyond == 0, "name=%s beyond=%d", name, beyond);
	if (shape == 2 || kind == 0) { tn_save(name, fb); }
	tex_free(m);
}

static void tn_sky_pair(void)
{
	static gl_skyvertex_t data[18];
	gl_skyloopdef_t loop = {HWD_SKYLOOP_STRIP, 18, 0, true};
	gl_sky_t sky; FTransform t; unsigned int info[8] = {0}; int i, variant;
	GLMipmap_t *m = tn_texture(0, 1024, 512);
	m->flags &= ~(u32)TF_CHROMAKEYED;
	memset(&sky, 0, sizeof sky); memset(&t, 0, sizeof t);
	t.scalex = t.scalez = 1; t.scaley = 1.6f; t.fovxangle = 90; t.z = 20;
	for (i = 0; i <= 8; i++) {
		float a = (float)i * 6.2831853f / 8;
		data[i * 2].x = 5000 * cosf(a); data[i * 2].z = 5000 * sinf(a); data[i * 2].y = -1200;
		data[i * 2].u = (float)i * 0.5f; data[i * 2].v = 0;
		data[i * 2].r = (u8)(180 + i * 7); data[i * 2].g = (u8)(240 - i * 9); data[i * 2].b = 223; data[i * 2].a = 255;
		data[i * 2 + 1] = data[i * 2]; data[i * 2 + 1].y = 3000; data[i * 2 + 1].v = 1;
		data[i * 2 + 1].r = 255; data[i * 2 + 1].g = 197; data[i * 2 + 1].b = 173;
	}
	sky.loops = &loop; sky.loopcount = 1; sky.data = data; sky.vertex_count = 18;
	sky.width = 1024; sky.height = 200;
	for (variant = 0; variant < 2; variant++) {
		TN_Pool(variant ? 512 : 0); frame_start(tn_bg);
		D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, HWD_SET_TEXTUREFILTER_BILINEAR);
		CHECK("tn_sky_upload", TN_Upload(m, info), "variant=%d tiled=%u", variant, info[0]);
		D.pfnSetTransform(&t); D.pfnSetBlend(PF_Translucent | PF_NoDepthTest | PF_Modulated);
		D.pfnRenderSkyDome(&sky);
		CHECK("tn_sky_read", tn_grab(variant ? fb : tn_ref) == 0, "variant=%d", variant);
		D.pfnSetTransform(NULL);
	}
	CHECK("tn_sky_parity", tn_compare("actual-sky-dome-colors", fb, tn_ref, 0, 3) == 0, "complete GS frame");
	tn_save("actual-sky-dome", fb); tex_free(m);
}

static void tn_post(void)
{
	float points[SCREENVERTS][SCREENVERTS][2]; int x, y;
	for (x = 0; x < SCREENVERTS; x++) for (y = 0; y < SCREENVERTS; y++) {
		float nx = (float)x / (SCREENVERTS - 1), ny = (float)y / (SCREENVERTS - 1);
		points[x][y][0] = (nx * 2 - 1 + sinf(ny * 6.2831853f) * 0.027f) * 4.4f;
		points[x][y][1] = (ny * 2 - 1 + sinf(nx * 6.2831853f) * 0.019f) * 4.4f;
	}
	D.pfnPostImgRedraw(points);
}

static GLMipmap_t *tn_mask(void)
{
	GLMipmap_t *m = tex_new(64, 64, GL_TEXFMT_ALPHA_8, 0); int x, y;
	for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) ((u8 *)m->data)[y * 64 + x] = (u8)((x * 37 + y * 17) & 255);
	return m;
}

static void tn_capture_pair(int proxy, u32 *refs[4])
{
	GLMipmap_t *mask = tn_mask(); unsigned int info[6] = {0}; int i;
	FSurfaceInfo tint = surf_rgba(53, 107, 179, 255);
	TN_Pool(proxy ? 128 : 0);
	tn_pattern(1); D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_START);
	D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1); CHECK("tn_capture_a", tn_grab(tn_a) == 0, "proxy=%d", proxy);
	tn_pattern(2); D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_END); CHECK("tn_capture_b", tn_grab(tn_b) == 0, "proxy=%d", proxy);
	for (i = 0; i < 3; i++) {
		int slot = i == 0 ? HWD_SCREENTEXTURE_WIPE_START : i == 1 ? HWD_SCREENTEXTURE_WIPE_END : HWD_SCREENTEXTURE_GENERIC1;
		CHECK("tn_saved_slot", TN_Slot(slot, info), "proxy=%d slot=%d", proxy, slot);
		printf("TN SLOT proxy=%d slot=%d tiled=%u blocks=%u backing=%u ee=%u gs=%u/%u\n", proxy, slot, info[0], info[1], info[2], info[3], info[4], info[5]);
	}
	if (!proxy) for (i = 0; i < 3; i++) CHECK("tn_actual_ee_spill", TN_Spill(), "capture=%d of 3", i);
	frame_start(tn_bg); D.pfnDrawScreenTexture(HWD_SCREENTEXTURE_WIPE_START, NULL, 0);
	CHECK("tn_capture_identity_read", tn_grab(fb) == 0, "proxy=%d", proxy);
	CHECK("tn_capture_identity_rgba", tn_compare("capture-identity-rgba", fb, tn_a, 1, 0) == 0, "proxy=%d", proxy);
	if (!proxy) memcpy(refs[0], fb, (size_t)FW * FH * 4); else CHECK("tn_capture_pair_identity", tn_compare("capture-resident-vs-proxy", fb, refs[0], 1, 0) == 0, "full-frame");
	frame_start(tn_bg); tn_post(); CHECK("tn_post_read", tn_grab(fb) == 0, "proxy=%d", proxy);
	if (!proxy) memcpy(refs[1], fb, (size_t)FW * FH * 4); else CHECK("tn_post_parity", tn_compare("post-resident-vs-proxy", fb, refs[1], 0, 3) == 0, "full-frame");
	frame_start(tn_bg); D.pfnSetTexture(mask); D.pfnDoScreenWipe(HWD_SCREENTEXTURE_WIPE_START, HWD_SCREENTEXTURE_WIPE_END, NULL, 0);
	CHECK("tn_wipe_read", tn_grab(fb) == 0, "proxy=%d", proxy);
	if (!proxy) memcpy(refs[2], fb, (size_t)FW * FH * 4); else CHECK("tn_wipe_parity", tn_compare("wipe-resident-vs-proxy", fb, refs[2], 1, 0) == 0, "full-frame");
	frame_start(tn_bg); D.pfnSetTexture(mask); D.pfnDoScreenWipe(HWD_SCREENTEXTURE_WIPE_START, HWD_SCREENTEXTURE_WIPE_END, &tint, PF_Additive);
	CHECK("tn_tinted_wipe_read", tn_grab(fb) == 0, "proxy=%d", proxy);
	if (!proxy) memcpy(refs[3], fb, (size_t)FW * FH * 4); else CHECK("tn_tinted_wipe_parity", tn_compare("tinted-wipe-resident-vs-proxy", fb, refs[3], 1, 0) == 0, "full-frame");
	tex_free(mask);
}

static void tn_highres(int w, int h, int mode)
{
	GLMipmap_t *sky, *mask; unsigned int info[8] = {0}; int i, bad, alpha_count = 0; u8 seen[256] = {0};
	char name[64];
	tn_init(w, h, mode);
	sky = tn_texture(0, 1024, 512); sky->flags &= ~(u32)TF_CHROMAKEYED;
	frame_start(tn_bg); CHECK("tn_highres_sky_upload", TN_Upload(sky, info), "size=%dx%d", w, h);
	CHECK("tn_highres_sky_tiled", info[0] == (h > 400 ? 1u : 0u) && info[2] == 1024 && info[3] == 512, "size=%dx%d source_footprint=512KiB tiled=%u", w, h, info[0]);
	tn_geometry(sky, 0); CHECK("tn_highres_sky_read", tn_grab(fb) == 0, "size=%dx%d", w, h);
	bad = 0; for (i = 0; i < FW * FH; i++) bad += (fb[i] & 0xffffffu) == tn_bg;
	CHECK("tn_highres_stage_coverage", bad == 0, "size=%dx%d uncovered=%d of %d", w, h, bad, FW * FH);
	snprintf(name, sizeof name, "highres-source-%dx%d", w, h); tn_save(name, fb); tex_free(sky);
	tn_pattern(3); D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_START); D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1);
	CHECK("tn_highres_a_read", tn_grab(tn_a) == 0, "size=%dx%d", w, h);
	tn_save("highres-original-a", tn_a);
	for (i = 0; i < FW * FH; i++) seen[tn_a[i] >> 24] = 1;
	for (i = 0; i < 256; i++) alpha_count += seen[i];
	CHECK("tn_highres_alpha_diversity", alpha_count > 8, "size=%dx%d distinct_alpha=%d", w, h, alpha_count);
	tn_pattern(4); D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_END);
	CHECK("tn_highres_b_read", tn_grab(tn_b) == 0, "size=%dx%d", w, h);
	tn_save("highres-original-b", tn_b);
	for (i = 0; i < 3; i++) {
		int slot = i == 0 ? HWD_SCREENTEXTURE_WIPE_START : i == 1 ? HWD_SCREENTEXTURE_WIPE_END : HWD_SCREENTEXTURE_GENERIC1;
		CHECK("tn_highres_slot", TN_Slot(slot, info) && info[0] == 1 && info[2], "size=%dx%d slot=%d proxy=%u backing=%u ee=%u gs=%u/%u", w, h, slot, info[0], info[2], info[3], info[4], info[5]);
	}
	frame_start(tn_bg); D.pfnDrawScreenTexture(HWD_SCREENTEXTURE_WIPE_START, NULL, 0);
	CHECK("tn_highres_identity_read", tn_grab(fb) == 0, "size=%dx%d", w, h);
	CHECK("tn_highres_identity_rgba", tn_compare("highres-identity-rgba", fb, tn_a, 1, 0) == 0, "size=%dx%d", w, h);
	tn_save("highres-restored-identity", fb);
	memset(tn_rgb, 0xa5, (size_t)FW * FH * 3);
	CHECK("tn_highres_rgb_read", PS2HWD_ReadScreenRGB(HWD_SCREENTEXTURE_WIPE_END, tn_rgb), "size=%dx%d", w, h);
	bad = 0; for (i = 0; i < FW * FH; i++) bad += tn_rgb[i * 3] != (u8)tn_b[i] || tn_rgb[i * 3 + 1] != (u8)(tn_b[i] >> 8) || tn_rgb[i * 3 + 2] != (u8)(tn_b[i] >> 16);
	CHECK("tn_highres_selected_rgb", bad == 0, "size=%dx%d wrong=%d of %d", w, h, bad, FW * FH);
	mask = tex_new(64, 64, GL_TEXFMT_ALPHA_8, 0); memset(mask->data, 128, 4096);
	frame_start(tn_bg); D.pfnSetTexture(mask); D.pfnDoScreenWipe(HWD_SCREENTEXTURE_WIPE_START, HWD_SCREENTEXTURE_WIPE_END, NULL, 0);
	CHECK("tn_highres_wipe_read", tn_grab(fb) == 0, "size=%dx%d", w, h);
	for (i = 0; i < FW * FH; i++) {
		int c; u32 want = 0x80000000u;
		for (c = 0; c < 3; c++) want |= (u32)((((tn_b[i] >> (8 * c)) & 255) * 64 + ((tn_a[i] >> (8 * c)) & 255) * 64) >> 7) << (8 * c);
		tn_ref[i] = want;
	}
	CHECK("tn_highres_wipe_alpha", tn_compare("highres-wipe-alpha64", fb, tn_ref, 1, 1) == 0, "size=%dx%d", w, h);
	tn_save("highres-independent-wipe-expected", tn_ref);
	snprintf(name, sizeof name, "highres-wipe-%dx%d", w, h); tn_save(name, fb);
	frame_start(tn_bg); tn_post(); CHECK("tn_highres_post_read", tn_grab(fb) == 0, "size=%dx%d", w, h);
	snprintf(name, sizeof name, "highres-post-%dx%d", w, h); tn_save(name, fb);
	{
		ps2hwd_stats_t st; PS2HWD_GetStats(&st, 0);
		CHECK("tn_highres_clean", !st.tex_missing && !st.timeouts && !st.tex_decimated && !st.unsupported_mask, "size=%dx%d missing=%u timeouts=%u decimated=%u limitations=%x", w, h, st.tex_missing, st.timeouts, st.tex_decimated, st.unsupported_mask);
	}
	tex_free(mask); D.pfnShutdown();
}

static void tn_coordinate_case(int shape)
{
	RGBA_t gray[256]; GLMipmap_t *m = tex_new(1024, 512, GL_TEXFMT_P_8, TF_WRAPXY);
	unsigned int info[8] = {0}; int component, variant, i, x, y;
	int changed = 0, missing = 0, extra = 0, inconsistent = 0, nongray = 0, invalid = 0;
	int maxdx = 0, maxdy = 0, beyond1 = 0, covered = 0;
	const int shifts[4] = {0, 8, 16, 24};
	memset(tn_a, 0, (size_t)FW * FH * 4); memset(tn_b, 0, (size_t)FW * FH * 4);
	for (i = 0; i < 256; i++) { gray[i].s.red = gray[i].s.green = gray[i].s.blue = (u8)i; gray[i].s.alpha = 255; }
	D.pfnSetTexturePalette(gray);
	D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, HWD_SET_TEXTUREFILTER_POINTSAMPLED);
	for (component = 0; component < 4; component++) {
		for (y = 0; y < 512; y++) for (x = 0; x < 1024; x++)
			((u8 *)m->data)[y * 1024 + x] = (u8)(component == 0 ? x : component == 1 ? x >> 8 : component == 2 ? y : y >> 8);
		for (variant = 0; variant < 2; variant++) {
			u32 *raw = variant ? fb : tn_ref, *coord = variant ? tn_b : tn_a;
			char name[96];
			TN_Pool(variant ? 512 : 0); frame_start(tn_bg);
			CHECK("tn_coord_upload", TN_Upload(m, info) && info[0] == (unsigned)variant && info[5] == 1,
				"shape=%d component=%d variant=%d tiled=%u psm=%u clut=%u", shape, component, variant, info[0], info[1], info[5]);
			tn_geometry(m, shape); CHECK("tn_coord_read", tn_grab(raw) == 0, "shape=%d component=%d variant=%d", shape, component, variant);
			for (i = 0; i < FW * FH; i++) {
				int cov = (raw[i] & 0xffffffu) != tn_bg;
				if (component == 0) coord[i] = cov ? 0x80000000u : 0;
				else inconsistent += cov != ((coord[i] >> 31) != 0);
				if (cov) {
					nongray += (u8)raw[i] != (u8)(raw[i] >> 8) || (u8)raw[i] != (u8)(raw[i] >> 16);
					coord[i] |= (raw[i] & 255u) << shifts[component];
				}
			}
			snprintf(name, sizeof name, "coordinate-shape%d-part%d-v%d", shape, component, variant); tn_save(name, raw);
		}
	}
	for (i = 0; i < FW * FH; i++) {
		int cr = (tn_a[i] >> 31) != 0, ct = (tn_b[i] >> 31) != 0;
		missing += cr && !ct; extra += ct && !cr;
		if (cr && ct) {
			int rx = (int)(tn_a[i] & 0xffffu), ry = (int)((tn_a[i] >> 16) & 0x7fffu);
			int tx = (int)(tn_b[i] & 0xffffu), ty = (int)((tn_b[i] >> 16) & 0x7fffu);
			int dx = abs(tx - rx), dy = abs(ty - ry);
			covered++; changed += dx != 0 || dy != 0;
			if (rx >= 1024 || tx >= 1024 || ry >= 512 || ty >= 512) { invalid++; beyond1++; continue; }
			if (dx > 512) dx = 1024 - dx;
			if (dy > 256) dy = 512 - dy;
			if (dx > maxdx) maxdx = dx;
			if (dy > maxdy) maxdy = dy;
			beyond1 += dx > 1 || dy > 1;
		}
	}
	printf("TN COORDS shape=%d source=1024x512 frame=%dx%d compared=%d covered=%d changed=%d max_circular_dx=%d max_circular_dy=%d beyond1=%d missing_coverage=%d extra_coverage=%d component_coverage_inconsistent=%d nongray=%d invalid=%d\n",
		shape, FW, FH, FW * FH, covered, changed, maxdx, maxdy, beyond1, missing, extra, inconsistent, nongray, invalid);
	CHECK("tn_coord_coverage", !missing && !extra && !inconsistent, "shape=%d missing=%d extra=%d inconsistent=%d", shape, missing, extra, inconsistent);
	CHECK("tn_coord_recipe", !nongray && !invalid, "shape=%d nongray=%d invalid=%d", shape, nongray, invalid);
	CHECK("tn_coord_bound", !beyond1, "shape=%d dx=%d dy=%d beyond_one_source_texel=%d", shape, maxdx, maxdy, beyond1);
	tex_free(m); D.pfnSetTexturePalette(PAL);
}

static void tn_attribute_case(int model_light, int zwrite)
{
	GLMipmap_t *m = tn_texture(0, 512, 512);
	unsigned int info[8] = {0}; int variant, badz = 0, i;
	char name[96];
	m->flags &= ~(u32)TF_CHROMAKEYED;
	snprintf(name, sizeof name, "attributes-model%d-zwrite%d", model_light, zwrite);
	if (tn_cell_case) snprintf(name, sizeof name, "attributes-cell%d-model%d-zwrite%d", tn_cell_case, model_light, zwrite);
	for (variant = 0; variant < 2; variant++) {
#ifdef TN_ONECORE_PROBE
		u32 before[4] = {0}, after[4] = {0};
#endif
		TN_Pool(variant ? 512 : 0); frame_start(tn_bg);
		D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, HWD_SET_TEXTUREFILTER_BILINEAR);
		CHECK("tn_attribute_upload", TN_Upload(m, info) && info[0] == (unsigned)variant, "model=%d zwrite=%d variant=%d", model_light, zwrite, variant);
#ifdef TN_ONECORE_PROBE
		if (arg("onecore")) TN_OneCoreInfo(before);
#endif
		CHECK("tn_attribute_emit", TN_AttributeFog(model_light, zwrite), "model=%d zwrite=%d variant=%d", model_light, zwrite, variant);
#ifdef TN_ONECORE_PROBE
		if (arg("onecore")) {
			TN_OneCoreInfo(after);
			int expected_fast = variant && tn_cell_case != 4;
			printf("TN ONECORE cell=%d model_light=%d zwrite=%d variant=%d fast_delta=%u masks=%u mask_sprites=%u workspace=%u\n", tn_cell_case, model_light, zwrite, variant, after[0] - before[0], after[1], after[2], after[3]);
			CHECK("tn_onecore_executed", expected_fast ? after[0] > before[0] : after[0] == before[0], "cell=%d model=%d zwrite=%d variant=%d delta=%u", tn_cell_case, model_light, zwrite, variant, after[0] - before[0]);
			CHECK("tn_onecore_workspace_route", variant && tn_cell_case == 4 ? after[1] > before[1] && after[3] > 0 : !after[1] && !after[2] && !after[3], "cell=%d model=%d zwrite=%d masks=%u sprites=%u workspace=%u", tn_cell_case, model_light, zwrite, after[1], after[2], after[3]);
		}
#endif
		CHECK("tn_attribute_read", tn_grab(variant ? fb : tn_ref) == 0, "model=%d zwrite=%d variant=%d", model_light, zwrite, variant);
#ifdef TN_STENCIL_PROBE
		CHECK("tn_attribute_depth_read", TN_ZRaw(variant ? tn_b : tn_a) == 0, "model=%d zwrite=%d variant=%d", model_light, zwrite, variant);
#endif
		{ char file[112]; snprintf(file, sizeof file, "%s-v%d", name, variant); tn_save(file, variant ? fb : tn_ref); }
	}
	CHECK("tn_attribute_RGBA_exact", tn_compare(name, fb, tn_ref, 1, 0) == 0, "NoDepth model=%d zwrite=%d", model_light, zwrite);
#ifdef TN_STENCIL_PROBE
	for (i = 0; i < FW * FH; i++) badz += (tn_a[i] & 0xffffffu) != (tn_b[i] & 0xffffffu);
	CHECK("tn_attribute_Zlow24_exact", !badz, "NoDepth model=%d zwrite=%d wrong_depth_pixels=%d", model_light, zwrite, badz);
#else
	(void)badz; (void)i;
#endif
	tex_free(m);
}

#ifdef TN_BORROW_PROBE
static void tn_borrow_case(int model_light, int zwrite, int screen)
{
	GLMipmap_t *m = screen ? NULL : tn_texture(0, 512, 512);
	const int order[4] = {0, 1, 3, 2};
	unsigned int info[8];
	if (m) m->flags &= ~(u32)TF_CHROMAKEYED;
	TN_AttributeCell(screen == 2 ? 8 : screen ? 7 : 6);
	for (int trial = 0; trial < (screen == 2 ? 2 : 4); trial++) {
		const int variant = order[trial];
		const int expect_wide = screen != 2 && (variant == 1 || variant == 3);
		u32 before[4], after[4], cache_before[8], cache_after[8], reclaimed[4] = {0};
		char name[96];
		TN_Pool(variant ? 512 : 0);
		D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, HWD_SET_TEXTUREFILTER_BILINEAR);
		if (variant == 2) CHECK("tn_borrow_occupied_prefix", TN_BorrowBlockPrefix(), "model=%d zwrite=%d", model_light, zwrite);
		if (screen) { tn_pattern(7); D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1); }
		frame_start(tn_bg);
		CHECK("tn_borrow_upload", (screen ? TN_BorrowCapture(HWD_SCREENTEXTURE_GENERIC1, info) : TN_Upload(m, info)) && info[0] == (u32)!!variant, "screen=%d model=%d zwrite=%d variant=%d", screen, model_light, zwrite, variant);
		TN_OneCoreInfo(before); TN_BorrowInfo(cache_before);
		CHECK("tn_borrow_emit", TN_AttributeFog(model_light, zwrite), "model=%d zwrite=%d variant=%d", model_light, zwrite, variant);
		TN_OneCoreInfo(after); TN_BorrowInfo(cache_after);
		printf("TN BORROW screen=%d model=%d zwrite=%d variant=%d borrowed=%u block=%u capacity=%u upload_delta=%u fast_delta=%u mask_delta=%u sprite_delta=%u workspace_before=%u workspace_after=%u used=%u queued_qwords=%u width=%u height=%u\n",
			screen, model_light, zwrite, variant, cache_after[0], cache_after[1], cache_after[2], cache_after[3] - cache_before[3], after[0] - before[0], after[1] - before[1], after[2] - before[2], before[3], after[3], cache_after[4], cache_after[5], cache_after[6], cache_after[7]);
		CHECK("tn_borrow_layout", expect_wide ? cache_after[0] == 64 && cache_after[2] == 128 && cache_after[3] - cache_before[3] == 32768 && cache_after[6] == 128 && cache_after[7] == (screen ? 64u : 256u) && cache_after[5] > 1 : !cache_after[0], "model=%d zwrite=%d variant=%d borrowed=%u capacity=%u upload=%u", model_light, zwrite, variant, cache_after[0], cache_after[2], cache_after[3] - cache_before[3]);
		CHECK("tn_borrow_route", expect_wide ? after[0] - before[0] == 1 && after[1] == before[1] && after[2] == before[2] && after[3] == before[3] : variant ? after[0] == before[0] && after[1] > before[1] : after[0] == before[0], "model=%d zwrite=%d variant=%d", model_light, zwrite, variant);
		if (variant == 3) {
			CHECK("tn_borrow_queued_reclaim_overwrite", TN_BorrowReclaimOverwrite(reclaimed), "model=%d zwrite=%d prefix=%u ordinary=%u borrowed_after=%u permanent_block=%u", model_light, zwrite, reclaimed[0], reclaimed[1], reclaimed[2], reclaimed[3]);
			printf("TN BORROWRECLAIM screen=%d model=%d zwrite=%d prefix=%u ordinary=%u ordinary_blocks=416 borrowed_after=%u permanent_block=%u overwritten_blocks=64 fixture_fence_before=0\n", screen, model_light, zwrite, reclaimed[0], reclaimed[1], reclaimed[2], reclaimed[3]);
		}
		CHECK("tn_borrow_read", tn_grab(variant ? fb : tn_ref) == 0, "model=%d zwrite=%d variant=%d", model_light, zwrite, variant);
		CHECK("tn_borrow_depth_read", TN_ZRaw(variant ? tn_b : tn_a) == 0, "model=%d zwrite=%d variant=%d", model_light, zwrite, variant);
		snprintf(name, sizeof name, "borrow-screen%d-model%d-zwrite%d-v%d", screen, model_light, zwrite, variant); tn_save(name, variant ? fb : tn_ref);
		if (variant) {
			int badz = 0;
			CHECK("tn_borrow_RGBA_exact", tn_compare(name, fb, tn_ref, 1, 0) == 0, "model=%d zwrite=%d variant=%d", model_light, zwrite, variant);
			for (int i = 0; i < FW * FH; i++) badz += (tn_a[i] & 0xffffffu) != (tn_b[i] & 0xffffffu);
			CHECK("tn_borrow_Zlow24_exact", !badz, "model=%d zwrite=%d variant=%d wrong_depth_pixels=%d", model_light, zwrite, variant, badz);
		}
		TN_BorrowCleanup();
	}
	if (m) tex_free(m);
	TN_AttributeCell(0);
}
#endif

#ifdef TN_STENCIL_PROBE
void TN_StencilSeed(const u32 *pixels);
void TN_StencilContext(int context);
void TN_StencilDims(int *width, int *height, int *stride);
void TN_StencilMask(int x0, int y0, int x1, int y1, unsigned int outside_high, unsigned int inside_high);
void TN_StencilDraw(u32 z32, u32 rgba, int zwrite);
int TN_ZRaw(u32 *dest);
void TN_StencilGradient(int z32, int nodepth, int zwrite, u32 rgba);
void TN_StencilFog(int xyzf2, u32 fog, u32 rgba);

static void tn_stencil_cases(void)
{
	const int x0 = 31, y0 = 15, x1 = FW - 29, y1 = FH - 17;
	const u32 paint = 0x314b6987u, sourcez = 0x00654321u;
	int trial, x, y, i;
	{
		int w, h, stride, baseline_bad = 0, covered = 0;
		TN_StencilDims(&w, &h, &stride);
		CHECK("tn_stencil_dimensions", w == FW && h == FH && stride == FW, "visible=%dx%d stride=%d", w, h, stride);
		TN_StencilContext(1); tn_pattern(5);
		CHECK("tn_stencil_original_fb", tn_grab(tn_a) == 0, "size=%dx%d", FW, FH);
		for (y = 0; y < FH; y++) for (x = 0; x < FW; x++)
			tn_b[y * FW + x] = ((u32)((x ^ y) & 255) << 24) | ((u32)(x * 123457u + y * 45673u + ((x ^ y) & 255) * 101u) & 0xffffffu);
		TN_StencilSeed(tn_b); TN_StencilMask(0, 0, FW, FH, 0, 0);
		TN_StencilDraw(sourcez | 0x01000000u, paint, 0);
		CHECK("tn_stencil_original_triangle_fb", tn_grab(fb) == 0, "size=%dx%d", FW, FH);
		CHECK("tn_stencil_original_triangle_z", TN_ZRaw(tn_ref) == 0, "size=%dx%d", FW, FH);
		for (i = 0; i < FW * FH; i++) {
			tn_rgb[i] = fb[i] == paint; covered += tn_rgb[i];
			baseline_bad += (!tn_rgb[i] && fb[i] != tn_a[i]) || (tn_ref[i] & 0xffffffu) != (tn_b[i] & 0xffffffu) || (tn_ref[i] >> 24) != 0;
		}
		CHECK("tn_stencil_original_triangle", !baseline_bad && covered > 0 && covered < FW * FH,
			"size=%dx%d actual_GS_covered=%d baseline_wrong_FB_or_Z=%d", FW, FH, covered, baseline_bad);
	}
	for (trial = 0; trial < 7; trial++) {
		int nodepth = trial >= 5 ? 1 : trial ? (trial - 1) & 1 : 0;
		int zwrite = trial >= 5 ? trial - 5 : trial ? (trial - 1) >> 1 : 0;
		int context = 1 + ((trial + (FW == 640)) & 1);
		int immediate = trial == 0 || !((trial + (FW == 640)) & 1), badmasklow = 0, badmaskhigh = 0;
		int badfb = 0, badalpha = 0, badlow = 0, badhigh = 0, passcount = 0, shown = 0;
		u32 drawlow = trial >= 5 ? 0xffffffu : sourcez;
		u32 z = drawlow | (nodepth ? 0x01000000u : 0);
		TN_StencilContext(context);
		tn_pattern(5); CHECK("tn_stencil_seed_fb", tn_grab(tn_a) == 0, "size=%dx%d trial=%d", FW, FH, trial);
		for (y = 0; y < FH; y++) for (x = 0; x < FW; x++)
			tn_b[y * FW + x] = ((u32)((x ^ y) & 255) << 24) | (trial >= 5 ? 0u : (u32)(x * 123457u + y * 45673u + ((x ^ y) & 255) * 101u) & 0xffffffu);
		TN_StencilSeed(tn_b); TN_StencilMask(x0, y0, x1, y1, 2, 0);
		if (immediate) {
			CHECK("tn_stencil_mask_read", TN_ZRaw(tn_ref) == 0, "size=%dx%d trial=%d", FW, FH, trial);
			for (y = 0; y < FH; y++) for (x = 0; x < FW; x++) {
				int at = y * FW + x, inside = x >= x0 && x < x1 && y >= y0 && y < y1;
				badmasklow += (tn_ref[at] & 0xffffffu) != (tn_b[at] & 0xffffffu);
				badmaskhigh += (tn_ref[at] >> 24) != (inside ? 0u : 2u);
			}
			CHECK("tn_stencil_alias_mask", !badmasklow && !badmaskhigh, "size=%dx%d trial=%d wrong_low24=%d wrong_mask_high=%d", FW, FH, trial, badmasklow, badmaskhigh);
		}
		if (trial) TN_StencilDraw(z, paint, zwrite);
		CHECK("tn_stencil_result_fb", tn_grab(fb) == 0, "size=%dx%d trial=%d", FW, FH, trial);
		CHECK("tn_stencil_result_z", TN_ZRaw(tn_ref) == 0, "size=%dx%d trial=%d", FW, FH, trial);
		for (y = 0; y < FH; y++) for (x = 0; x < FW; x++) {
			int at = y * FW + x, inside = x >= x0 && x < x1 && y >= y0 && y < y1;
			u32 oldlow = tn_b[at] & 0xffffffu;
			int pass = trial && inside && tn_rgb[at] && (nodepth || sourcez >= oldlow);
			u32 wantfb = pass ? paint : tn_a[at];
			u32 wantlow = pass && zwrite ? drawlow : oldlow;
			u32 wanthigh = pass && zwrite ? (u32)nodepth : inside ? 0u : 2u;
			passcount += pass; badfb += fb[at] != wantfb; badalpha += (fb[at] >> 24) != (wantfb >> 24);
			badlow += (tn_ref[at] & 0xffffffu) != wantlow; badhigh += (tn_ref[at] >> 24) != wanthigh;
			if (shown < 6 && (fb[at] != wantfb || (tn_ref[at] & 0xffffffu) != wantlow || (tn_ref[at] >> 24) != wanthigh)) {
				printf("TN STENCILBAD size=%dx%d trial=%d at=%d,%d fb=%08x/%08x z=%08x/%08x\n", FW, FH, trial, x, y, fb[at], wantfb, tn_ref[at], wantlow | (wanthigh << 24)); shown++;
			}
		}
		printf("TN STENCIL size=%dx%d trial=%d context=%d rect=%d,%d,%d,%d source_z=%08x noDepth=%d zwrite=%d immediate_read=%d compared=%d expected_drawn=%d bad_fb_rgba=%d bad_fb_alpha=%d bad_z_low24=%d bad_z_high=%d\n",
			FW, FH, trial, context, x0, y0, x1, y1, z, nodepth, zwrite, immediate, FW * FH, passcount, badfb, badalpha, badlow, badhigh);
		CHECK("tn_stencil_depth_and_rgba", !badfb && !badalpha && !badlow && !badhigh, "size=%dx%d trial=%d", FW, FH, trial);
	}
	for (trial = 0; trial < 3; trial++) {
		int nodepth = trial != 0, zwrite = trial != 1, variant;
		int badfb = 0, badz = 0, maxdelta = 0;
		for (variant = 0; variant < 2; variant++) {
			TN_StencilContext(1 + (FW == 640)); tn_pattern(5);
			D.pfnFinishUpdate(0);
			for (y = 0; y < FH; y++) for (x = 0; x < FW; x++)
				tn_ref[y * FW + x] = (u32)(x * 123457u + y * 45673u + ((x ^ y) & 255) * 101u) & 0xffffffu;
			TN_StencilSeed(tn_ref); TN_StencilMask(0, 0, FW, FH, 0, 0);
			TN_StencilGradient(variant, nodepth, zwrite, paint);
			CHECK("tn_stencil_gradient_fb", tn_grab(variant ? fb : tn_a) == 0, "size=%dx%d trial=%d variant=%d", FW, FH, trial, variant);
			CHECK("tn_stencil_gradient_z", TN_ZRaw(variant ? tn_ref : tn_b) == 0, "size=%dx%d trial=%d variant=%d", FW, FH, trial, variant);
		}
		for (i = 0; i < FW * FH; i++) {
			int dz = abs((int)(tn_ref[i] & 0xffffffu) - (int)(tn_b[i] & 0xffffffu));
			badfb += fb[i] != tn_a[i]; badz += dz != 0; if (dz > maxdelta) maxdelta = dz;
		}
		printf("TN STENCILGRAD size=%dx%d noDepth=%d zwrite=%d compared=%d bad_fb_rgba=%d bad_z_low24=%d max_z_low24_delta=%d\n", FW, FH, nodepth, zwrite, FW * FH, badfb, badz, maxdelta);
		CHECK("tn_stencil_gradient_exact", !badfb && !badz, "size=%dx%d noDepth=%d zwrite=%d", FW, FH, nodepth, zwrite);
	}
	for (trial = 0; trial < 5; trial++) {
		static const u32 fogs[5] = {0, 64, 128, 192, 255};
		int variant, badfb = 0, badz = 0;
		for (variant = 0; variant < 2; variant++) {
			TN_StencilContext(1 + (FW == 640)); tn_pattern(5); D.pfnFinishUpdate(0);
			for (i = 0; i < FW * FH; i++) tn_ref[i] = (u32)i & 0xffffffu;
			TN_StencilSeed(tn_ref); TN_StencilMask(0, 0, FW, FH, 0, 0);
			TN_StencilFog(!variant, fogs[trial], paint);
			CHECK("tn_stencil_fog_fb", tn_grab(variant ? fb : tn_a) == 0, "size=%dx%d fog=%u variant=%d", FW, FH, fogs[trial], variant);
			CHECK("tn_stencil_fog_z", TN_ZRaw(variant ? tn_ref : tn_b) == 0, "size=%dx%d fog=%u variant=%d", FW, FH, fogs[trial], variant);
		}
		for (i = 0; i < FW * FH; i++) { badfb += fb[i] != tn_a[i]; badz += (tn_ref[i] & 0xffffffu) != (tn_b[i] & 0xffffffu); }
		printf("TN STENCILFOG size=%dx%d fog=%u compared=%d bad_fb_rgba=%d bad_z_low24=%d\n", FW, FH, fogs[trial], FW * FH, badfb, badz);
		CHECK("tn_stencil_fog_exact", !badfb && !badz, "size=%dx%d fog=%u", FW, FH, fogs[trial]);
	}
	TN_StencilContext(1); frame_start(rgb(7, 11, 13)); CHECK("tn_stencil_restore_fb_read", tn_grab(fb) == 0, "size=%dx%d", FW, FH);
	CHECK("tn_stencil_restore_z_read", TN_ZRaw(tn_ref) == 0, "size=%dx%d", FW, FH);
	{
		int badfb = 0, badz = 0;
		for (i = 0; i < FW * FH; i++) { badfb += fb[i] != 0x800d0b07u; badz += (tn_ref[i] & 0xffffffu) != 0; }
		CHECK("tn_stencil_context_and_clear", !badfb && !badz, "size=%dx%d wrong_FB_RGBA=%d wrong_Zlow24=%d", FW, FH, badfb, badz);
	}
}
#endif

int main(int argc, char **argv)
{
	u32 *refs[4]; int i;
	g_argc = argc; g_argv = argv; make_pals(); dump_on = 1;
	fb = memalign(64, 640u * 512u * 4u); tn_ref = memalign(64, 640u * 512u * 4u);
	tn_a = memalign(64, 640u * 512u * 4u); tn_b = memalign(64, 640u * 512u * 4u);
	tn_rgb = memalign(64, 640u * 512u * 3u);
	CHECK("tn_buffers", fb && tn_ref && tn_a && tn_b && tn_rgb, "native readback working buffers");
	tn_init(320, 200, PS2HWD_VIDEO_NTSC);
#ifdef TN_STENCIL_PROBE
	if (arg("stencil")) tn_stencil_cases();
	else
#endif
#ifdef TN_BORROW_PROBE
	if (arg("borrowwide")) for (int screen = 0; screen < 3; screen++) { tn_borrow_case(0, 0, screen); tn_borrow_case(0, 1, screen); tn_borrow_case(1, 0, screen); tn_borrow_case(1, 1, screen); }
	else
#else
	if (arg("borrowwide")) CHECK("tn_borrow_api", 0, "source has no borrowed-wide cache API");
	else
#endif
#ifdef TN_ONECORE_PROBE
	if (arg("onecore")) {
		for (tn_cell_case = 1; tn_cell_case <= 3; tn_cell_case++) { TN_AttributeCell(tn_cell_case); tn_attribute_case(0, 0); tn_attribute_case(0, 1); tn_attribute_case(1, 0); tn_attribute_case(1, 1); }
		if (arg("widewindow")) { tn_cell_case = 5; TN_AttributeCell(5); tn_attribute_case(0, 0); tn_attribute_case(0, 1); tn_attribute_case(1, 0); tn_attribute_case(1, 1); }
		tn_cell_case = 4; TN_AttributeCell(4); tn_attribute_case(1, 1);
	}
	else
#else
	if (arg("onecore")) CHECK("tn_onecore_api", 0, "source has no one-core counter API");
	else
#endif
	if (arg("attributes")) { tn_attribute_case(0, 0); tn_attribute_case(0, 1); tn_attribute_case(1, 0); tn_attribute_case(1, 1); }
	else if (arg("light-only")) tn_texture_pair(0, 1024, 512, 4, 0);
	else if (arg("coordinates")) { tn_coordinate_case(0); tn_coordinate_case(3); }
	else if (arg("phase-only")) { tn_texture_pair(0, 1024, 512, 9, 0); tn_texture_pair(0, 1024, 512, 9, 1); }
	else if (arg("halo-only")) { tn_texture_pair(2, 512, 256, 6, 1); if (arg("indexed-halo")) tn_texture_pair(0, 1024, 512, 6, 1); }
	else {
		tn_texture_pair(0, 1024, 512, 0, 0);
		tn_texture_pair(0, 1024, 512, 2, 1);
		tn_texture_pair(0, 1000, 500, 1, 0);
		tn_texture_pair(0, 1024, 512, 3, 1);
		tn_texture_pair(0, 1024, 512, 4, 0);
		tn_texture_pair(1, 512, 256, 2, 1);
		tn_texture_pair(2, 512, 256, 2, 1);
		tn_texture_pair(0, 1024, 512, 5, 0);
		tn_texture_pair(0, 1024, 512, 6, 1);
		tn_texture_pair(1, 512, 256, 6, 1);
		tn_texture_pair(2, 512, 256, 6, 1);
		tn_texture_pair(0, 1024, 512, 7, 0);
		tn_texture_pair(0, 1024, 512, 7, 1);
		tn_texture_pair(0, 1024, 512, 8, 0);
		tn_texture_pair(0, 1024, 512, 8, 1);
		tn_texture_pair(0, 1024, 512, 9, 0);
		tn_texture_pair(0, 1024, 512, 9, 1);
		tn_sky_pair();
		if (!arg("source-only")) {
			for (i = 0; i < 4; i++) refs[i] = memalign(64, 320u * 200u * 4u);
			tn_capture_pair(0, refs); tn_capture_pair(1, refs);
			for (i = 0; i < 4; i++) free(refs[i]);
		}
	}
	D.pfnShutdown();
#ifdef TN_STENCIL_PROBE
	if (arg("stencil")) { tn_init(640, 512, PS2HWD_VIDEO_PAL); tn_stencil_cases(); D.pfnShutdown(); }
	else
#endif
	if (!arg("halo-only") && !arg("source-only") && !arg("phase-only") && !arg("coordinates") && !arg("attributes") && !arg("light-only") && !arg("onecore") && !arg("borrowwide")) {
		tn_highres(640, 400, PS2HWD_VIDEO_NTSC);
		tn_highres(640, 480, PS2HWD_VIDEO_480P);
		tn_highres(640, 512, PS2HWD_VIDEO_PAL);
	}
	free(fb); free(tn_ref); free(tn_a); free(tn_b); free(tn_rgb);
	printf("TN COMPLETE checks=%d failures=%d\n", checks, failures);
	return failures ? 1 : 0;
}
