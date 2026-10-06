/* Extended tests of the GS renderer driver (included by hw_test.c): the exact PC blend equations, repeats of textures whose size
 * is not a power of two, the Doom light staircase on floors and walls, fog blocks, full-size captures and wipes.
 * Every expected value comes from code written here from the OpenGL driver's definitions (src/hardware/r_opengl/r_opengl.c and
 * the GLSL in src/hardware/hw_shaders.h), in double precision; the driver is never asked what it would draw.
 */

static unsigned ext_rng = 0x2545F491u;
static unsigned ext_rand(void)
{
	ext_rng ^= ext_rng << 13;
	ext_rng ^= ext_rng >> 17;
	ext_rng ^= ext_rng << 5;
	return ext_rng;
}

static int clamp255(double v)
{
	return v < 0.0 ? 0 : v > 255.0 ? 255 : (int)(v + 0.5);
}

/* ---- blend equations ---- */
enum { BM_TRANS, BM_ADD, BM_RSUB, BM_SUB, BM_ENV, BM_MUL, BM_MASKED, BM_FOG };

/* cs: source colour after modulation by the polygon colour, as = total alpha (0..1), cd: destination */
static void oracle_blend(int mode, const double *cs, double as, const double *cd, int *o)
{
	int c;

	for (c = 0; c < 3; c++)
	{
		double v;

		switch (mode)
		{
			case BM_TRANS: v = cs[c] * as + cd[c] * (1.0 - as); break;
			case BM_ADD: v = cs[c] * as + cd[c]; break;
			case BM_RSUB: v = cd[c] - cs[c] * as; break;
			case BM_SUB: v = cs[c] * as - cd[c]; break;
			case BM_ENV: v = cs[c] + cd[c] * (1.0 - as); break;
			case BM_MUL: v = cs[c] * cd[c] / 255.0; break;
			case BM_MASKED: v = cs[c] * as; break;
			default: v = cs[c] * as + cd[c] * cs[c] / 255.0; break; /* fog block */
		}
		o[c] = clamp255(v);
	}
}

static u32 ext_flags_of(int mode)
{
	switch (mode)
	{
		case BM_TRANS: return PF_Translucent;
		case BM_ADD: return PF_Additive;
		case BM_RSUB: return PF_ReverseSubtract;
		case BM_SUB: return PF_Subtractive;
		case BM_ENV: return PF_Environment;
		case BM_MUL: return PF_Multiplicative;
		case BM_MASKED: return PF_Masked;
		default: return PF_Fog;
	}
}

/* 32 random cases of one blend mode in a grid of cells over random backgrounds, flat or with a palette texture */
static void test_blend_mode(const char *name, int mode, int textured, int tol)
{
	enum { NC = 32, COLS = 8 };
	GLMipmap_t *tex[NC];
	int idx[NC], cp[NC][3], al[NC], bg[NC][3], i, r, bad = 0, n = 0, shown = 0;
	static const int alphas[] = {255, 230, 200, 160, 128, 100, 64, 25};
	u32 flags = ext_flags_of(mode);
	FSurfaceInfo s;

	if (mode == BM_FOG)
		textured = 0;
	D.pfnSetTexturePalette(PAL);
	frame_start(rgb(0, 0, 0));
	for (i = 0; i < NC; i++)
	{
		int cx = (i % COLS) * 40, cy = (i / COLS) * 48, c;

		idx[i] = (int)(ext_rand() % 255);
		al[i] = alphas[ext_rand() % 8];
		for (c = 0; c < 3; c++)
		{
			cp[i][c] = 60 + (int)(ext_rand() % 196);
			bg[i][c] = (int)(ext_rand() % 256);
		}
		if (mode == BM_MASKED && (i % 8) == 7)
			al[i] = 100; /* alpha test: below 0.5 nothing is drawn */
		s = surf_rgba(bg[i][0], bg[i][1], bg[i][2], 255);
		quad2d(&s, (float)cx, (float)cy, (float)cx + 38.0f, (float)cy + 46.0f, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
		tex[i] = NULL;
	}
	for (i = 0; i < NC; i++)
	{
		int cx = (i % COLS) * 40, cy = (i / COLS) * 48;

		s = surf_rgba(cp[i][0], cp[i][1], cp[i][2], al[i]);
		if (textured)
		{
			int x, y;

			tex[i] = tex_new(16, 16, GL_TEXFMT_RGBA, 0);
			for (y = 0; y < 16; y++)
				for (x = 0; x < 16; x++)
					rgba_set(tex[i], x, y, PAL, idx[i], 255);
			D.pfnSetTexture(tex[i]);
			quad2d(&s, (float)cx + 4.0f, (float)cy + 4.0f, (float)cx + 34.0f, (float)cy + 42.0f, 0, 0, 1, 1, flags | PF_Modulated | PF_NoDepthTest);
		}
		else
		{
			quad2d(&s, (float)cx + 4.0f, (float)cy + 4.0f, (float)cx + 34.0f, (float)cy + 42.0f, 0, 0, 0, 0, flags | PF_NoTexture | PF_Modulated | PF_NoDepthTest);
		}
	}
	r = frame_grab();
	for (i = 0; i < NC; i++)
	{
		int cx = (i % COLS) * 40, cy = (i / COLS) * 48, c, want[3], k;
		double cs[3], cd[3], as = al[i] / 255.0;
		int px[5] = {(int)((cx + 12) * SX), (int)((cx + 19) * SX), (int)((cx + 27) * SX), (int)((cx + 16) * SX), (int)((cx + 24) * SX)};
		int py[5] = {(int)((cy + 12) * SY), (int)((cy + 20) * SY), (int)((cy + 30) * SY), (int)((cy + 36) * SY), (int)((cy + 23) * SY)};

		for (c = 0; c < 3; c++)
		{
			double ct = textured ? (double)chan(palc(PAL, idx[i]), c) : 255.0;

			cs[c] = ct * cp[i][c] / 255.0;
			cd[c] = bg[i][c];
		}
		if (mode == BM_MASKED && al[i] < 128)
			for (c = 0; c < 3; c++)
				want[c] = bg[i][c];
		else
			oracle_blend(mode, cs, as, cd, want);
		if (!FB32)
		{
			int cc;

			for (cc = 0; cc < 3; cc++)
				want[cc] = (want[cc] >> 3) * 255 / 31;
		}
		for (k = 0; k < 5; k++)
		{
			u32 g = fb[py[k] * FW + px[k]];
			int ok = abs(chan(g, 0) - want[0]) <= tol && abs(chan(g, 1) - want[1]) <= tol && abs(chan(g, 2) - want[2]) <= tol;

			n++;
			bad += !ok;
			if (!ok && shown < 6)
			{
				shown++;
				printf("H0 info %s case %d: cp=%d,%d,%d alpha=%d bg=%d,%d,%d tex=%d: got %d,%d,%d want %d,%d,%d\n", name, i, cp[i][0], cp[i][1], cp[i][2], al[i], bg[i][0], bg[i][1], bg[i][2],
					textured ? idx[i] : -1, chan(g, 0), chan(g, 1), chan(g, 2), want[0], want[1], want[2]);
			}
		}
	}
	CHECK(name, r == 0 && bad == 0, "readback=%d samples off the PC equation by more than %d: %d of %d (%d cases, %s)", r, tol, bad, n, NC, textured ? "palette texture" : "flat colour");
	dump_ppm(name, fb);
	for (i = 0; i < NC; i++)
		if (tex[i])
			tex_free(tex[i]);
}

/* ---- repeats of textures whose size is not a power of two ---- */
static void test_npot_repeat(const char *name, int w, int h)
{
	GLMipmap_t *m = tex_new(w, h, GL_TEXFMT_RGBA, TF_WRAPXY);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	int x, y, r, bad = 0, n = 0, skipped = 0;
	const float x0 = 20.0f, x1 = 300.0f, y0 = 10.0f, y1 = 190.0f, s0 = -0.37f, s1 = 3.9f, t0 = 0.21f, t1 = 2.83f;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			rgba_set(m, x, y, PAL, (x * 5 + y * 11) % 255, 255);
	D.pfnSetTexturePalette(PAL);
	frame_start(rgb(0, 0, 0));
	D.pfnSetTexture(m);
	quad2d(&s, x0, y0, x1, y1, s0, t0, s1, t1, PF_Masked | PF_Modulated | PF_NoDepthTest);
	r = frame_grab();
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++)
		{
			double ex = (x + 0.5) / SX, ey = (y + 0.5) / SY, u, v, fu, fv;
			int tu, tv;

			if (ex < x0 + 0.5 || ex > x1 - 0.5 || ey < y0 + 0.5 || ey > y1 - 0.5)
				continue;
			u = (s0 + (ex - x0) / (x1 - x0) * (s1 - s0));
			v = (t0 + (ey - y0) / (y1 - y0) * (t1 - t0));
			u -= floor(u);
			v -= floor(v);
			fu = u * w - floor(u * w);
			fv = v * h - floor(v * h);
			if (fu < 0.1 || fu > 0.9 || fv < 0.1 || fv > 0.9 || u * w < 0.6 || u * w > w - 0.6 || v * h < 0.6 || v * h > h - 0.6)
			{
				skipped++;
				continue;
			}
			tu = (int)(u * w);
			tv = (int)(v * h);
			n++;
			bad += fb[y * FW + x] != quant(palc(PAL, (tu * 5 + tv * 11) % 255));
		}
	CHECK(name, r == 0 && bad == 0 && n > scale_n(40000), "%dx%d texture repeated %.1f x %.1f: texels differing from the exact repeat %d of %d (%d near texel/repeat edges skipped)", w, h, s1 - s0, t1 - t0, bad, n, skipped);
	dump_ppm(name, fb);
	tex_free(m);
}

/* ---- the Doom light staircase ---- */
static double ref_darkness(double light, int wall, int fs, int fe, double z)
{
	double sf = wall ? 1.05 : 1.06, cf = wall ? 2.2 : 1.15;
	double lightnum = light / 17.0, lightz = z / 16.0, startmap, scale, cap, cm, d;

	lightnum = lightnum < 0 ? 0 : lightnum > 15 ? 15 : lightnum;
	lightz = lightz < 0 ? 0 : lightz > 127 ? 127 : lightz;
	startmap = (15.0 - lightnum) * 4.0;
	scale = 160.0 / (lightz + 1.0);
	cap = (155.0 - light) * 0.26;
	cm = startmap * sf - scale * 0.5 * cf;
	if (cap > cm)
		cm = cap;
	d = floor(cm) + 0.5;
	d = d < 0 ? 0 : d > 31 ? 31 : d;
	d /= 32.0;
	if (fs != 0 || fe != 31)
	{
		double a = fs / 31.0, b = fe / 31.0;

		d = (d - a) * (1.0 / (b - a));
		d = d < 0 ? 0 : d > 1 ? 1 : d;
	}
	return d;
}

/* eye depth (clip w) and the fragment z of the OpenGL shader for a pixel looking at the plane y = h */
static int ref_plane_depth(const refcam_t *c, double px, double py, double h, double *w)
{
	const double *m = c->m;
	double nx = (px - c->vx) / c->vw * 2.0 - 1.0, ny = 1.0 - (py - c->vy) / c->vh * 2.0;
	double a11 = m[0] - nx * m[3], a12 = m[8] - nx * m[11], b1 = -(m[12] - nx * m[15]) - (m[4] - nx * m[7]) * h;
	double a21 = m[1] - ny * m[3], a22 = m[9] - ny * m[11], b2 = -(m[13] - ny * m[15]) - (m[5] - ny * m[7]) * h;
	double det = a11 * a22 - a12 * a21, X, Z;

	if (fabs(det) < 1e-12)
		return 0;
	X = (b1 * a22 - a12 * b2) / det;
	Z = (a11 * b2 - b1 * a21) / det;
	*w = m[3] * X + m[7] * h + m[11] * Z + m[15];
	return *w > 0.0;
}

static double frag_z(double w, double n)
{
	return (w - n) * (32768.0 / (32768.0 - n));
}

typedef struct
{
	int light, fs, fe, wall;
	u32 fade; /* 0x00BBGGRR */
	int blend; /* 0 = solid (ramp), BM_TRANS = bands */
	int alpha;
} lightcase_t;

static void test_light_floor(const char *name, const lightcase_t *lc)
{
	GLMipmap_t *m = tex_new(64, 64, GL_TEXFMT_RGBA, TF_WRAPXY);
	FSurfaceInfo s;
	FTransform t = cam_default();
	refcam_t c;
	FOutVector v[4];
	double world[4][3], window[16][3];
	refedge_t edges[16];
	int ne;
	int x, y, r, bad = 0, n = 0, skipped = 0, texidx = 77, i, maxerr = 0;
	u32 texc = palc(PAL, texidx), bgc = rgb(30, 60, 90);
	double znear = 0.9;

	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++)
			rgba_set(m, x, y, PAL, texidx, 255);
	D.pfnSetTexturePalette(PAL);
	D.pfnSetSpecialState(HWD_SET_SHADERS, 1);
	D.pfnSetShader(lc->wall ? 1 : 0);
	frame_start(bgc);
	D.pfnSetTransform(&t);
	D.pfnSetTexture(m);
	memset(&s, 0, sizeof s);
	s.PolyColor.rgba = 0xFFFFFFFFu;
	s.PolyColor.s.alpha = (UINT8)(lc->blend ? lc->alpha : 255);
	s.FadeColor.s.red = (UINT8)chan(lc->fade, 0);
	s.FadeColor.s.green = (UINT8)chan(lc->fade, 1);
	s.FadeColor.s.blue = (UINT8)chan(lc->fade, 2);
	s.FadeColor.s.alpha = 255;
	s.LightInfo.light_level = (FUINT)lc->light;
	s.LightInfo.fade_start = (FUINT)lc->fs;
	s.LightInfo.fade_end = (FUINT)lc->fe;
	/* a floor at height 0 (GL y) from 60 to 4000 ahead, 3000 to each side */
	v[0].x = 60.0f; v[0].y = 0.0f; v[0].z = -3000.0f; v[0].s = 0.0f; v[0].t = 0.0f;
	v[1].x = 60.0f; v[1].y = 0.0f; v[1].z = 3000.0f; v[1].s = 0.0f; v[1].t = 1.0f;
	v[2].x = 4000.0f; v[2].y = 0.0f; v[2].z = 3000.0f; v[2].s = 1.0f; v[2].t = 1.0f;
	v[3].x = 4000.0f; v[3].y = 0.0f; v[3].z = -3000.0f; v[3].s = 1.0f; v[3].t = 0.0f;
	for (i = 0; i < 4; i++)
	{
		v[i].s *= 40.0f;
		v[i].t *= 40.0f;
	}
	D.pfnDrawPolygon(&s, v, 4, (lc->blend ? PF_Translucent : PF_Masked | PF_Occlude) | PF_Modulated | PF_ColorMapped);
	r = frame_grab();
	D.pfnSetShader(-1);
	D.pfnSetSpecialState(HWD_SET_SHADERS, 0);
	D.pfnSetTransform(NULL);
	ref_setup(&c, &t, znear, 0, 0, SW, SH);
	for (i = 0; i < 4; i++) { world[i][0] = v[i].x; world[i][1] = v[i].y; world[i][2] = v[i].z; }
	ne = ref_clip(&c, (const double (*)[3])world, 4, window);
	ne = ref_edges((const double (*)[3])window, ne, edges);
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++)
		{
			double w, z, d, d0, d1, cs[3], out[3], fade[3];
			int cc, ok = 1;
			float edge_distance;
			u32 g = fb[y * FW + x];

			// The submitted floor is finite: pixels on the infinite plane beyond its near/far/side edges are background.
			if (!edges_inside(edges, ne, (float)x + 0.5f, (float)y + 0.5f, &edge_distance) || edge_distance < 0.2f)
				continue;
			if (!ref_plane_depth(&c, x + 0.5, y + 0.5, 0.0, &w) || w < znear + 0.5)
				continue;
			z = frag_z(w, znear);
			/* Avoid geometric subpixel uncertainty at a staircase boundary, not sampled-ramp bins. */
			d = ref_darkness(lc->light, lc->wall, lc->fs, lc->fe, z);
			d0 = ref_darkness(lc->light, lc->wall, lc->fs, lc->fe, z - 0.5);
			d1 = ref_darkness(lc->light, lc->wall, lc->fs, lc->fe, z + 0.5);
			if (d0 != d || d1 != d)
			{
				skipped++;
				continue;
			}
			for (cc = 0; cc < 3; cc++)
			{
				cs[cc] = chan(texc, cc);
				fade[cc] = chan(lc->fade, cc);
				out[cc] = cs[cc] + (fade[cc] - cs[cc]) * d;
				if (lc->blend)
					out[cc] = out[cc] * (lc->alpha / 255.0) + chan(bgc, cc) * (1.0 - lc->alpha / 255.0);
				if (abs(chan(g, cc) - clamp255(out[cc])) > 2)
					ok = 0;
				if (abs(chan(g, cc) - clamp255(out[cc])) > maxerr)
					maxerr = abs(chan(g, cc) - clamp255(out[cc]));
			}
			n++;
			bad += !ok;
		}
	CHECK(name, r == 0 && bad == 0 && n > scale_n(10000), "light %d fudge %s fade %06x range %d..%d %s: pixels off the shader equation by more than 2: %d of %d (max error %d, %d skipped near staircase steps)", lc->light,
		lc->wall ? "wall" : "floor", (unsigned)lc->fade, lc->fs, lc->fe, lc->blend ? "banded translucent" : "banded solid", bad, n, maxerr, skipped);
	dump_ppm(name, fb);
	tex_free(m);
}

static void test_ext_all(void)
{
	static const lightcase_t lights[] = {
		{100, 0, 31, 0, 0x000000, 0, 255}, {160, 0, 31, 0, 0x000000, 0, 255}, {40, 0, 31, 1, 0x000000, 0, 255}, {200, 0, 31, 0, 0x2030C0, 0, 255},
		{128, 4, 22, 0, 0x000000, 0, 255}, {110, 0, 31, 0, 0x301080, 1, 200}, {70, 0, 31, 1, 0x000000, 1, 160},
	};
	char name[48];
	int i, tol = FB32 ? 2 : 9;

	test_blend_mode("pc_blend_translucent_tex", BM_TRANS, 1, tol);
	test_blend_mode("pc_blend_additive_tex", BM_ADD, 1, tol);
	test_blend_mode("pc_blend_reverse_subtract_tex", BM_RSUB, 1, tol);
	test_blend_mode("pc_blend_subtractive_flat", BM_SUB, 0, tol);
	test_blend_mode("pc_blend_subtractive_tex", BM_SUB, 1, tol);
	test_blend_mode("pc_blend_environment_flat", BM_ENV, 0, tol);
	test_blend_mode("pc_blend_environment_tex", BM_ENV, 1, tol);
	test_blend_mode("pc_blend_multiplicative_flat", BM_MUL, 0, tol);
	test_blend_mode("pc_blend_multiplicative_tex", BM_MUL, 1, tol);
	test_blend_mode("pc_blend_masked_alpha_tex", BM_MASKED, 1, tol);
	test_blend_mode("pc_blend_fog_block", BM_FOG, 0, tol);
	test_npot_repeat("npot_repeat_96x80", 96, 80);
	test_npot_repeat("npot_repeat_96x64", 96, 64);
	test_npot_repeat("npot_repeat_64x80", 64, 80);
	test_npot_repeat("npot_repeat_21x37", 21, 37);
	for (i = 0; i < (int)(sizeof lights / sizeof lights[0]); i++)
	{
		snprintf(name, sizeof name, "light_floor_%d", i);
		test_light_floor(name, &lights[i]);
	}
}

static void test_capture_spill(void)
{
	GLMipmap_t *pressure;
	UINT8 *pixels = malloc(SW * SH * 3);
	ps2hwd_stats_t before, after;
	int slot, i, ok = 1, bad = 0, r = 0;
	u32 colours[NUMSCREENTEXTURES];
	D.pfnFlushScreenTextures();
	D.pfnClearMipMapCache();
	PS2HWD_GetStats(&before, 0);
	for (slot = 0; slot < NUMSCREENTEXTURES; slot++)
	{
		colours[slot] = rgb(31 + slot * 37, 225 - slot * 31, 17 + slot * 43);
		frame_start(colours[slot]);
		ok &= PS2HWD_CaptureScreen(slot);
		r |= frame_grab();
	}
	/* This exact CT32 allocation fits by itself but forces live captures to EE storage. */
	pressure = tex_new(1024, 768, GL_TEXFMT_ALPHA_INTENSITY_88, 0);
	memset(pressure->data, 255, 1024 * 768 * 2);
	D.pfnSetTexture(pressure);
	ok &= pressure->downloaded != 0;
	D.pfnSetTexture(NULL);
	PS2HWD_GetStats(&after, 0);
	CHECK("five_capture_spill", ok && r == 0 && after.screen_spills > before.screen_spills,
		"all slots retained under exact 3 MiB texture pressure: spills=%u", after.screen_spills - before.screen_spills);
	for (slot = 0; slot < NUMSCREENTEXTURES; slot++)
	{
		u32 want = quant(colours[slot]);
		ok &= PS2HWD_ReadScreenRGB(slot, pixels);
		for (i = 0; i < SW * SH; i++)
			bad += pixels[i * 3] != (UINT8)want || pixels[i * 3 + 1] != (UINT8)(want >> 8) || pixels[i * 3 + 2] != (UINT8)(want >> 16);
		D.pfnDrawScreenTexture(slot, NULL, 0);
		r |= frame_grab();
		for (i = 0; i < FW * FH; i++) bad += fb[i] != want;
	}
	PS2HWD_GetStats(&after, 0);
	CHECK("five_capture_restore", ok && r == 0 && bad == 0 && after.screen_restores > before.screen_restores,
		"selected saved-slot RGB and full-screen restores: wrong pixels=%d restores=%u", bad, after.screen_restores - before.screen_restores);
	tex_free(pressure);
	free(pixels);
	D.pfnFlushScreenTextures();
	PS2HWD_GetInfo(&INFO);
	CHECK("capture_backing_released", INFO.screen_bytes == 0, "EE backing current=%u peak=%u", INFO.screen_bytes, INFO.screen_peak_bytes);
}

static void test_final_screen(void)
{
	FSurfaceInfo white = surf_rgba(255, 255, 255, 255);
	int x, y, r, bad = 0, sharp = 0;
	u32 colour = rgb(73, 149, 217);
	D.pfnFlushScreenTextures();
	frame_start(colour);
	D.pfnDrawScreenFinalTexture(HWD_SCREENTEXTURE_GENERIC2, SW, SH);
	r = frame_grab();
	for (x = 0; x < FW * FH; x++) bad += fb[x] != quant(colour);
	CHECK("final_implicit_keeps_picture", r == 0 && bad == 0, "zero-copy final path: changed pixels=%d", bad);
	frame_start(colour);
	D.pfnDrawScreenFinalTexture(HWD_SCREENTEXTURE_GENERIC2, SW * 2, SH);
	r = frame_grab(); bad = 0;
	for (y = 1; y < FH - 1; y++) for (x = 1; x < FW - 1; x++)
	{
		u32 want;
		if (abs(x - FW / 4) < 2 || abs(x - FW * 3 / 4) < 2) continue;
		want = x < FW / 4 || x >= FW * 3 / 4 ? 0 : quant(colour);
		bad += fb[y * FW + x] != want;
	}
	CHECK("final_implicit_letterbox", r == 0 && bad == 0, "save before clearing, centred aspect 1:2 with black side bars: wrong pixels=%d", bad);
	D.pfnFlushScreenTextures();
	frame_start(0);
	for (x = 0; x < FW; x += 2)
		quad2d(&white, (float)x / SX, 0, (float)(x + 1) / SX, (float)SH, 0, 0, 0, 0, PF_NoTexture | PF_Modulated | PF_NoDepthTest);
	D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1);
	r = frame_grab();
	memcpy(fb2, fb, (size_t)FW * FH * 4);
	for (x = 0; x < FW * FH; x++) sharp += fb[x] == 0xFFFFFFu;
	D.pfnDrawScreenFinalTexture(HWD_SCREENTEXTURE_GENERIC1, SW, SH);
	r |= frame_grab(); bad = 0;
	for (y = 1; y < FH - 1; y++) for (x = 1; x < FW - 1; x++) bad += fb[y * FW + x] != fb2[y * FW + x];
	CHECK("full_resolution_screen_detail", r == 0 && bad == 0 && sharp > FW * FH / 3,
		"one-GS-pixel stripes: capture/final draw changed pixels=%d sharp original pixels=%d sample=%06x,%06x original=%06x,%06x", bad, sharp,
		(unsigned)fb[10 * FW + 10], (unsigned)fb[10 * FW + 11], (unsigned)fb2[10 * FW + 10], (unsigned)fb2[10 * FW + 11]);
	D.pfnFlushScreenTextures();
}

static void test_perspective_texture(void)
{
	GLMipmap_t *m = tex_new(64, 64, GL_TEXFMT_RGBA, 0);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	FTransform t = cam_default();
	refcam_t cam;
	FOutVector v[4] = {{100,-80,-100,0,0}, {100,80,-100,0,1}, {300,80,100,1,1}, {300,-80,100,1,0}};
	double px[4], py[4], iw[4];
	int x, y, k, r, bad = 0, n = 0;
	for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) ((u32 *)m->data)[y * 64 + x] = rgb(x * 4, y * 4, 53) | 0xFF000000u;
	frame_start(0);
	D.pfnSetTransform(&t);
	D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, HWD_SET_TEXTUREFILTER_POINTSAMPLED);
	D.pfnSetTexture(m);
	D.pfnDrawPolygon(&s, v, 4, PF_Masked | PF_Modulated | PF_Occlude);
	r = frame_grab();
	ref_setup(&cam, &t, 0.9, 0, 0, SW, SH);
	for (k = 0; k < 4; k++)
	{
		const double *a = cam.m;
		double w = a[3] * v[k].x + a[7] * v[k].y + a[11] * v[k].z + a[15];
		iw[k] = 1.0 / w;
		px[k] = ((a[0] * v[k].x + a[4] * v[k].y + a[8] * v[k].z + a[12]) / w * 0.5 + 0.5) * FW;
		py[k] = (0.5 - (a[1] * v[k].x + a[5] * v[k].y + a[9] * v[k].z + a[13]) / w * 0.5) * FH;
	}
	for (y = 1; y < FH - 1; y++) for (x = 1; x < FW - 1; x++)
	{
		for (k = 0; k < 2; k++)
		{
			int b = k + 1, c = k + 2;
			double det = (py[b] - py[c]) * (px[0] - px[c]) + (px[c] - px[b]) * (py[0] - py[c]);
			double a0 = ((py[b] - py[c]) * (x + 0.5 - px[c]) + (px[c] - px[b]) * (y + 0.5 - py[c])) / det;
			double a1 = ((py[c] - py[0]) * (x + 0.5 - px[c]) + (px[0] - px[c]) * (y + 0.5 - py[c])) / det;
			double a2 = 1.0 - a0 - a1, q, u, vv, fu, fv;
			if (a0 < 0.02 || a1 < 0.02 || a2 < 0.02) continue;
			q = a0 * iw[0] + a1 * iw[b] + a2 * iw[c];
			u = (a0 * v[0].s * iw[0] + a1 * v[b].s * iw[b] + a2 * v[c].s * iw[c]) / q * 64;
			vv = (a0 * v[0].t * iw[0] + a1 * v[b].t * iw[b] + a2 * v[c].t * iw[c]) / q * 64;
			fu = u - floor(u); fv = vv - floor(vv);
			if (fu < 0.15 || fu > 0.85 || fv < 0.15 || fv > 0.85) continue;
			n++;
			bad += fb[y * FW + x] != quant(rgb((int)u * 4, (int)vv * 4, 53));
			break;
		}
	}
	CHECK("perspective_texture_q", r == 0 && bad == 0 && n > scale_n(5000), "tilted CT32 quad vs independent double perspective interpolation: wrong texels=%d of %d", bad, n);
	D.pfnSetTransform(NULL);
	tex_free(m);
}

static void test_continuous_wipes(void)
{
	GLMipmap_t *mask = tex_new(64, 1, GL_TEXFMT_ALPHA_8, 0);
	u32 start = rgb(80, 90, 110), end = rgb(140, 70, 20);
	int x, mode, r = 0, bad, ch;
	FSurfaceInfo surf;
	for (x = 0; x < 64; x++) ((UINT8 *)mask->data)[x] = (UINT8)(x * 255 / 63);
	frame_start(start); D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_START); r |= frame_grab();
	frame_start(end); D.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_END); r |= frame_grab();
	for (mode = 0; mode < 5; mode++)
	{
		char name[48];
		int tinted = mode != 0, additive = mode == 1 || mode == 3, fadein = mode >= 3;
		frame_start(0); D.pfnSetTexture(mask);
		surf = surf_rgba(2, 4, 6, fadein ? 255 : 0);
		D.pfnDoScreenWipe(HWD_SCREENTEXTURE_WIPE_START, HWD_SCREENTEXTURE_WIPE_END, tinted ? &surf : NULL, additive ? PF_Additive : PF_ReverseSubtract);
		r |= frame_grab(); bad = 0;
		for (x = 0; x < 64; x++)
		{
			int px = (int)((x + 0.5) * FW / 64.0), py = FH / 2;
			double a = ((UINT8 *)mask->data)[x] / 255.0;
			for (ch = 0; ch < 3; ch++)
			{
				double want;
				if (!tinted) want = chan(start, ch) * (1.0 - a) + chan(end, ch) * a;
				else
				{
					double effect = clamp255((2 + ch * 2) * 24.875 * (fadein ? 1.0 - a : a));
					want = chan(fadein ? end : start, ch) + (additive ? effect : -effect);
				}
				bad += abs(chan(fb[py * FW + px], ch) - clamp255(want)) > 2;
			}
		}
		snprintf(name, sizeof name, "wipe_%s_%s", tinted ? (additive ? "tinted_add" : "tinted_subtract") : "continuous", fadein ? "in" : "out");
		CHECK(name, r == 0 && bad == 0, "64 mask levels, PC continuous/tinted equation within 2: wrong channels=%d", bad);
	}
	tex_free(mask);
	D.pfnFlushScreenTextures();
}
