/* Included only by p4_native_eetest.py's generated HW_TEST fixture.
 * This tests an explicit full-size/no-planner quality mode in both paths.
 * Complete native P8/P4 readbacks must be exact; no tolerance or crop is used.
 */
extern int P4Test_Query(const GLMipmap_t *, unsigned int *);
extern int P4Test_SetVU(int);
extern unsigned int P4Test_VUPolys(void);
extern int P4Test_RowMode(void);
extern int P4Test_RebootVU(void);
extern void P4Test_ReadPlanes(unsigned int *);
extern void P4Test_Guard(float *);
extern int P4Test_Crosses(const FOutVector *, int, int);
static unsigned int p4test_id = 1000;
#define P4TEST_NOPLAN 0x2000000

static GLMipmap_t *p4test_tex(int band, int packed, int keyed, int w, int h, int uniform)
{
	GLMipmap_t *m = tex_new(w, h, GL_TEXFMT_P_8, TF_WRAPXY | (keyed ? TF_CHROMAKEYED : 0));
	int x, y;

	/* regen_kind=0 is the ordinary P8 fallback, even in the full-size mode.
	 * The packed counterpart uses the same byte source with a distinct map id. */
	m->regen_kind = packed ? 1 : 0;
	m->regen_id = (INT32)p4test_id++;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			int low = uniform ? band % 15 : ((x / 4) + (y / 4) * 3) % (band == 15 ? 15 : 16);
			((UINT8 *)m->data)[y * w + x] = (UINT8)((band << 4) | low);
		}
	return m;
}

static unsigned int p4test_record(GLMipmap_t *m, int packed, int band, const char *context)
{
	unsigned int q[8] = {0};
	int ok = P4Test_Query(m, q);
	CHECK("p4_record", ok && q[0] == (unsigned int)(packed ? 20 : 19) && q[1] == (unsigned int)(packed ? band : 0)
		&& q[2] == m->width && q[3] == m->height,
		"%s expected_psm=%d band=%d actual=%u CSA=%u stored=%ux%u blocks=%u TBW=%u view=%u",
		context, packed ? 20 : 19, band, q[0], q[1], q[2], q[3], q[4], q[5], q[6]);
	return q[4];
}

static void p4test_compare(const char *name, int readback, int band, int mode)
{
	int i, bad = 0, drawn = 0, maxrgb = 0;
	unsigned long sum = 0;
	for (i = 0; i < FW * FH; i++)
	{
		int c;
		bad += fb[i] != fb2[i];
		drawn += fb2[i] != quant(rgb(17, 23, 31));
		for (c = 0; c < 3; c++)
		{
			int d = abs(chan(fb[i], c) - chan(fb2[i], c));
			if (d > maxrgb) maxrgb = d;
			sum += (unsigned int)d;
		}
	}
	CHECK(name, readback == 0 && bad == 0 && drawn > 1000,
		"band=%d mode=%d full=%d exact=%d changed=%d maxRGB=%d sumRGB=%lu visible=%d readback=%d",
		band, mode, FW * FH, FW * FH - bad, bad, maxrgb, sum, drawn, readback);
}

static void p4test_draw_mixed(GLMipmap_t *m, GLMipmap_t *keyed, GLMipmap_t *t8)
{
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	const u32 fl = PF_Masked | PF_Modulated | PF_NoDepthTest;
	frame_start(rgb(17, 23, 31));
	D.pfnSetTexture(m);
	quad2d(&s, 8, 8, 155, 96, -0.125f, -0.25f, 2.625f, 1.75f, fl);
	D.pfnSetTexture(t8);
	quad2d(&s, 163, 8, 312, 96, 0, 0, 1.25f, 1.5f, fl);
	D.pfnSetTexture(keyed);
	quad2d(&s, 8, 104, 155, 192, -0.375f, 0.125f, 1.875f, 2.25f, fl);
	D.pfnSetTexture(m); /* T4 -> T8 -> T4/keyed -> T4/plain CLUT identity */
	quad2d(&s, 163, 104, 312, 192, 0.125f, -0.375f, 1.875f, 1.625f, fl);
}

static void p4test_scalar(void)
{
	int band, palette, filter;
	CHECK("p4_scalar_transport", P4Test_SetVU(0) == 0, "test-only V.on=0; debug flags remain 0x%x in both paths", ps2hwd_dbg_flags);
	p4test_palette_mode = 0;
	for (band = 0; band < 16; band++)
	{
		GLMipmap_t *reference = p4test_tex(band, 0, 0, 128, 128, 0);
		GLMipmap_t *reference_keyed = p4test_tex(band, 0, 1, 128, 128, 0);
		GLMipmap_t *packed = p4test_tex(band, 1, 0, 128, 128, 0);
		GLMipmap_t *packed_keyed = p4test_tex(band, 1, 1, 128, 128, 0);
		GLMipmap_t *t8 = p4test_tex((band + 1) & 15, 0, 0, 128, 128, 0);
		unsigned int p8blocks = 0, p4blocks = 0;
		/* Real image-sharing pair: same immutable bytes and map key. */
		packed_keyed->regen_id = packed->regen_id;
		for (palette = 0; palette < 2; palette++)
			for (filter = 0; filter < 2; filter++)
			{
				int r;
				D.pfnSetTexturePalette(palette ? PAL2 : PAL);
				D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE,
					filter ? HWD_SET_TEXTUREFILTER_BILINEAR : HWD_SET_TEXTUREFILTER_POINTSAMPLED);
				p4test_draw_mixed(reference, reference_keyed, t8);
				p8blocks = p4test_record(reference, 0, band, "scalar/reference");
				r = frame_grab();
				memcpy(fb2, fb, (size_t)FW * FH * 4);
				p4test_draw_mixed(packed, packed_keyed, t8);
				p4blocks = p4test_record(packed, 1, band, "scalar/packed");
				p4test_record(packed_keyed, 1, band, "scalar/keyed-view");
				r |= frame_grab();
				p4test_compare("p4_scalar_full_rgb", r, band, palette * 2 + filter);
				if (band == 8 && palette == 1 && filter == 1)
				{
					dump_ppm("p4-scalar-reference", fb2);
					dump_ppm("p4-scalar-packed", fb);
				}
			}
		CHECK("p4_scalar_allocated_saving", p8blocks == 64 && p4blocks == 32,
			"band=%d full-size 128x128 native allocated P8=%u P4=%u blocks saving=%u bytes",
			band, p8blocks, p4blocks, (p8blocks - p4blocks) * 256u);
		tex_free(reference); tex_free(reference_keyed); tex_free(packed); tex_free(packed_keyed); tex_free(t8);
	}
}

static void p4test_big_allocations(void)
{
	static const int sizes[4][2] = {{128, 128}, {512, 512}, {1024, 512}, {1024, 1024}};
	int j;
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	for (j = 0; j < 4; j++)
	{
		GLMipmap_t *m[2];
		unsigned int blocks[2] = {0};
		ps2hwd_info_t before, after;
		int k, r = 0;
		for (k = 0; k < 2; k++)
		{
			m[k] = p4test_tex(8, k, 0, sizes[j][0], sizes[j][1], 0);
			PS2HWD_GetInfo(&before);
			frame_start(rgb(17, 23, 31)); D.pfnSetTexture(m[k]);
			quad2d(&s, 8, 8, 312, 192, -0.25f, -0.25f, 1.25f, 1.25f, PF_Masked | PF_Modulated | PF_NoDepthTest);
			blocks[k] = p4test_record(m[k], k, 8, "large/native-pool");
			r |= frame_grab();
			PS2HWD_GetInfo(&after);
			CHECK("p4_native_pool_charge", after.pool_used_blocks - before.pool_used_blocks == blocks[k],
				"packed=%d %dx%d pool before=%u after=%u image=%u blocks",
				k, sizes[j][0], sizes[j][1], before.pool_used_blocks, after.pool_used_blocks, blocks[k]);
			if (!k) memcpy(fb2, fb, (size_t)FW * FH * 4);
		}
		p4test_compare("p4_large_full_rgb", r, 8, j);
		CHECK("p4_large_allocated_saving", blocks[1] * 2 == blocks[0],
			"same-mode full-size %dx%d native P8=%u P4=%u saving=%u bytes",
			sizes[j][0], sizes[j][1], blocks[0], blocks[1], (blocks[0] - blocks[1]) * 256u);
		tex_free(m[0]); tex_free(m[1]);
	}
}

static int p4test_row_light(int row)
{
	return row == 0 ? 255 : (int)floor(155.0 - ((double)row + 0.5) / 0.26);
}

static void p4test_clip_polygon(FOutVector *v, const float *guard, int plane, int single)
{
	float nx[4] = {-0.7f, 0.7f, 0.7f, -0.7f};
	float ny[4] = {-0.7f, -0.7f, 0.7f, 0.7f};
	float depth[4] = {2, 2, 2, 2};
	int i, count = single ? 1 : 2;
	/* With SetTransform(NULL), the 90-degree projection has w=world z.
	 * Go beyond the actual GS guard volume, which is much wider than NDC1. */
	for (i = 0; i < count; i++)
	{
		int corner;
		if (plane == 0) { corner = i ? 3 : 0; nx[corner] = -guard[0] * 1.5f; }
		else if (plane == 1) { corner = i ? 2 : 1; nx[corner] = guard[0] * 1.5f; }
		else if (plane == 2) { corner = i ? 1 : 0; ny[corner] = -guard[1] * 1.5f; }
		else if (plane == 3) { corner = i ? 3 : 2; ny[corner] = guard[1] * 1.5f; }
		else
		{
			corner = i ? 3 : 0;
			if (plane == 4) depth[corner] = guard[2] * 0.4f;
			else if (plane == 5) depth[corner] = guard[3] * 1.5f;
			else depth[corner] = -guard[2]; /* eye/negative w, crosses near too */
		}
	}
	if (plane == 5)
		for (i = 0; i < 4; i++) if (depth[i] == 2) depth[i] = guard[3] * 0.4f;
	for (i = 0; i < 4; i++)
	{
		v[i].x = nx[i] * depth[i]; v[i].y = ny[i] * depth[i]; v[i].z = depth[i];
		v[i].s = (i == 1 || i == 2) ? 1.0f : 0.0f;
		v[i].t = i < 2 ? 1.0f : 0.0f;
	}
}

static void p4test_clip_boundaries(void)
{
	static UINT8 indices[32 * 256];
	GLMipmap_t *plain = p4test_tex(8, 0, 0, 128, 128, 0);
	GLMipmap_t *packed = p4test_tex(8, 1, 0, 128, 128, 0);
	GLMipmap_t *warm_plain = p4test_tex(8, 0, 0, 128, 128, 1);
	GLMipmap_t *warm_packed = p4test_tex(8, 1, 0, 128, 128, 1);
	FSurfaceInfo warm = surf_rgba(255, 255, 255, 255), surface;
	UINT32 id;
	u32 flags = PF_Masked | PF_Modulated | PF_ColorMapped | PF_Occlude;
	int plane, single, sequence, row, i;
	p4test_palette_mode = 1; D.pfnSetTexturePalette(PAL);
	for (row = 0; row < 32; row++) for (i = 0; i < 256; i++) indices[row * 256 + i] = (UINT8)((i + row * 7) % 255);
	id = PS2HWD_CreateIndexedLightTable(indices, PAL);
	warm.LightTableId = id; warm.LightInfo.light_level = 128; warm.LightInfo.fade_end = 31;
	surface = warm; surface.LightInfo.light_level = 255;
	CHECK("p4_clip_vu_transport", P4Test_SetVU(1) == 1 && id, "fresh-reference native clipping id=%u", (unsigned)id);
	D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, HWD_SET_TEXTUREFILTER_POINTSAMPLED);
	D.pfnSetSpecialState(HWD_SET_SHADERS, 1); D.pfnSetShader(0);
	for (plane = 0; plane < 7; plane++)
		for (single = 0; single < 2; single++)
			for (sequence = 0; sequence < 3; sequence++)
			{
				FOutVector v[4]; float guard[4];
				unsigned int original[48], observed[48], vu0;
				int r, badplanes = 0, crossing;
				/* Oracle: restore the original plane/mask table and capture
				 * clipped P8 BEFORE ANY P4 plan since this fresh VU boot. */
				PS2HWD_SetTrace(-1, P4TEST_NOPLAN);
				CHECK("p4_clip_clean_oracle_boot", P4Test_RebootVU(), "plane=%d single=%d sequence=%d", plane, single, sequence);
				P4Test_ReadPlanes(original);
				frame_start(rgb(17, 23, 31)); D.pfnSetTexture(warm_plain);
				quad2d(&warm, 12, 12, 44, 44, 0, 0, 1, 1, flags);
				PS2HWD_Sync();
				if (sequence == 2) PS2HWD_SetTrace(-1, 0);
				frame_start(rgb(17, 23, 31)); P4Test_Guard(guard);
				p4test_clip_polygon(v, guard, plane, single);
				crossing = P4Test_Crosses(v, 4, plane == 6 ? 4 : plane);
				vu0 = P4Test_VUPolys(); D.pfnSetTexture(plain);
				D.pfnDrawPolygon(&surface, v, 4, flags);
				r = frame_grab(); memcpy(fb2, fb, (size_t)FW * FH * 4);
				CHECK("p4_clip_oracle_path", crossing && P4Test_VUPolys() - vu0 == 1,
					"plane=%d single=%d sequence=%d crosses=%d real_VU_polys=%u guard=%g,%g near=%g far=%g",
					plane, single, sequence, crossing, P4Test_VUPolys() - vu0, guard[0], guard[1], guard[2], guard[3]);
				/* Target: fresh boot, complete P4 row plan, then no reboot or
				 * plane-table rewrite until clipped T8/T4/mode-exit T8. */
				PS2HWD_SetTrace(-1, P4TEST_NOPLAN);
				CHECK("p4_clip_target_boot", P4Test_RebootVU(), "plane=%d single=%d sequence=%d", plane, single, sequence);
				frame_start(rgb(17, 23, 31)); D.pfnSetTexture(warm_packed);
				vu0 = P4Test_VUPolys();
				quad2d(&warm, 12, 12, 44, 44, 0, 0, 1, 1, flags);
				P4Test_ReadPlanes(observed); /* Sync completes the real P4 send */
				for (i = 0; i < 48; i++) badplanes += observed[i] != original[i];
				CHECK("p4_clip_persistent_planes", !badplanes && P4Test_VUPolys() - vu0 == 1 && P4Test_RowMode(),
					"plane=%d single=%d sequence=%d all48 native VU1 plane/mask words changed=%d real_P4_row_polys=%u",
					plane, single, sequence, badplanes, P4Test_VUPolys() - vu0);
				if (sequence == 2) PS2HWD_SetTrace(-1, 0);
				frame_start(rgb(17, 23, 31));
				D.pfnSetTexture(sequence == 1 ? packed : plain);
				vu0 = P4Test_VUPolys(); D.pfnDrawPolygon(&surface, v, 4, flags);
				r |= frame_grab();
				CHECK("p4_clip_target_path", P4Test_VUPolys() - vu0 == 1,
					"plane=%d single=%d sequence=%d real_VU_polys=%u flags=0x%x",
					plane, single, sequence, P4Test_VUPolys() - vu0, ps2hwd_dbg_flags);
				p4test_compare("p4_clip_full_rgb", r, plane, sequence * 2 + single);
				if (plane == 0 && !single && sequence == 0)
				{
					dump_ppm("p4-clip-reference", fb2); dump_ppm("p4-clip-target", fb);
				}
			}
	PS2HWD_SetTrace(-1, P4TEST_NOPLAN);
	D.pfnSetShader(-1); D.pfnSetSpecialState(HWD_SET_SHADERS, 0);
	D.pfnClearLightTables(); p4test_palette_mode = 0;
	tex_free(plain); tex_free(packed); tex_free(warm_plain); tex_free(warm_packed);
}

static void p4test_rows(void)
{
	static UINT8 table[32 * 256];
	GLMipmap_t *m[2][16];
	UINT32 id;
	int k, band, row, palette, generation;
	u32 flags = PF_Masked | PF_Modulated | PF_ColorMapped | PF_Occlude;
	CHECK("p4_vu_transport", P4Test_SetVU(1) == 1, "test-only V.on restored; native VU1 ready, flags=0x%x", ps2hwd_dbg_flags);
	p4test_palette_mode = 1;
	D.pfnSetTexturePalette(PAL);
	for (row = 0; row < 32; row++)
		for (k = 0; k < 256; k++) table[row * 256 + k] = (UINT8)((k + row * 7) % 255);
	id = PS2HWD_CreateIndexedLightTable(table, PAL);
	CHECK("p4_lighttable_created", id != 0, "32 independent palette rows id=%u", (unsigned)id);
	for (k = 0; k < 2; k++)
		for (band = 0; band < 16; band++) m[k][band] = p4test_tex(band, k, 0, 128, 128, 1);
	for (generation = 0; generation < 2; generation++)
	{
		if (generation)
		{
			D.pfnSetTexturePalette(PAL);
			for (row = 0; row < 32; row++)
				for (k = 0; k < 256; k++) table[row * 256 + k] = (UINT8)((k * 3 + row * 11 + 1) % 255);
			PS2HWD_UpdateIndexedLightTable(id, table, PAL);
		}
		for (palette = 0; palette < 2; palette++)
		{
			int r = 0;
			RGBA_t *pal = palette ? PAL2 : PAL;
			D.pfnSetTexturePalette(pal);
			D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, HWD_SET_TEXTUREFILTER_POINTSAMPLED);
			D.pfnSetSpecialState(HWD_SET_SHADERS, 1); D.pfnSetShader(0);
			for (k = 0; k < 2; k++)
			{
				unsigned int start = P4Test_VUPolys();
				int oracle_bad = 0, rowmode_bad = 0;
				frame_start(rgb(17, 23, 31));
				for (band = 0; band < 16; band++)
				{
					D.pfnSetTexture(m[k][band]);
					for (row = 0; row < 32; row++)
					{
						FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
						float x = band * 20.0f, y = row * 6.0f;
						s.LightTableId = id;
						s.LightInfo.light_level = (FUINT)p4test_row_light(row);
						s.LightInfo.fade_start = 0; s.LightInfo.fade_end = 31;
						quad2d(&s, x + 1, y + 1, x + 19, y + 5, 0, 0, 1, 1, flags);
						rowmode_bad += !P4Test_RowMode();
					}
					p4test_record(m[k][band], k, band, "VU1/rows");
				}
				r |= frame_grab();
				for (band = 0; band < 16; band++)
					for (row = 0; row < 32; row++)
					{
						int x = (int)((band * 20 + 10) * SX), y = (int)((row * 6 + 3) * SY);
						int idx = (band << 4) | (band % 15);
						u32 expected = quant(palc(pal, table[row * 256 + idx]));
						oracle_bad += fb[y * FW + x] != expected;
					}
				CHECK("p4_vu_all32_rows", P4Test_VUPolys() - start == 512 && !rowmode_bad && !oracle_bad,
					"packed=%d palette=%d generation=%d native_VU_polys=%u rowmode_bad=%d independent_row_samples_bad=%d of 512",
					k, palette, generation, P4Test_VUPolys() - start, rowmode_bad, oracle_bad);
				if (!k) memcpy(fb2, fb, (size_t)FW * FH * 4);
			}
			p4test_compare("p4_vu_rows_full_rgb", r, -1, generation * 2 + palette);
			if (generation == 1 && palette == 1)
			{
				dump_ppm("p4-vu-rows-reference", fb2);
				dump_ppm("p4-vu-rows-packed", fb);
			}
		}
	}
	D.pfnSetShader(-1); D.pfnSetSpecialState(HWD_SET_SHADERS, 0);
	for (k = 0; k < 2; k++) for (band = 0; band < 16; band++) tex_free(m[k][band]);
	D.pfnClearLightTables(); p4test_palette_mode = 0;
}

static void p4test_transition(void)
{
	GLMipmap_t *m = p4test_tex(8, 1, 0, 128, 128, 0);
	GLMipmap_t *view = p4test_tex(8, 1, 1, 128, 128, 0);
	GLMipmap_t *reference = p4test_tex(8, 0, 0, 128, 128, 0);
	GLMipmap_t *reference_view = p4test_tex(8, 0, 1, 128, 128, 0);
	GLMipmap_t *t8 = p4test_tex(1, 0, 0, 128, 128, 0);
	unsigned int q[8], handle, viewhandle;
	int r;
	view->regen_id = m->regen_id;
	D.pfnSetTexturePalette(PAL);
	p4test_draw_mixed(reference, reference_view, t8);
	r = frame_grab(); memcpy(fb2, fb, (size_t)FW * FH * 4);
	p4test_draw_mixed(m, view, t8); /* deliberately pending VIF/GS draws */
	p4test_record(m, 1, 8, "mode-transition/owner");
	p4test_record(view, 1, 8, "mode-transition/view");
	handle = m->downloaded; viewhandle = view->downloaded;
	PS2HWD_SetTrace(-1, 0); /* documented runtime transition to ordinary planning */
	CHECK("p4_mode_transition_drops_views", handle && viewhandle && !m->downloaded && !view->downloaded
		&& !P4Test_Query(m, q) && !P4Test_Query(view, q), "old owner=%u view=%u; both handles cleared after pending draws", handle, viewhandle);
	r |= frame_grab(); p4test_compare("p4_transition_pending_rgb", r, 8, 0);
	p4test_draw_mixed(m, view, t8);
	p4test_record(m, 0, 8, "mode-transition/new-P8");
	r = frame_grab(); p4test_compare("p4_transition_reupload_rgb", r, 8, 1);
	tex_free(m); tex_free(view); tex_free(reference); tex_free(reference_view); tex_free(t8);
	PS2HWD_SetTrace(-1, P4TEST_NOPLAN);
}

static int p4test_main(int argc, char **argv)
{
	ps2hwd_config_t c;
	ps2hwd_stats_t st;
	g_argc = argc; g_argv = argv;
	dump_on = arg("dump"); make_pals();
	PS2HWD_GetConfig(&c);
	c.video = PS2HWD_VIDEO_NTSC; c.fb32 = !arg("fb16"); c.dither = 0;
	c.tex_adapt = 0; c.tex_cap_blocks = 0; c.tex_max_bytes = 0;
	ps2hwd_dbg_flags = P4TEST_NOPLAN;
	PS2HWD_Configure(&c); PS2HWD_FillDriver(&D);
	CHECK("p4_init", D.pfnInit(), "explicit full-size flags=0x%x cap=%u adapt=%d budget=%d", ps2hwd_dbg_flags, c.tex_cap_blocks, c.tex_adapt, c.tex_max_bytes);
	PS2HWD_SetScreenSize(SW, SH); PS2HWD_GetInfo(&INFO);
	FW = INFO.vw; FH = INFO.vh; FB32 = INFO.fb32; SX = (float)FW / SW; SY = (float)FH / SH;
	fb = memalign(64, (size_t)FW * FH * 4); fb2 = memalign(64, (size_t)FW * FH * 4); zb = memalign(64, (size_t)FW * FH * 4);
	printf("H0 p4 scope explicit NOPLAN, no quality controller; scalar transport test hook is symmetric; complete raw readbacks; no FPS or physical PS2 claim\n");
	p4test_scalar(); p4test_big_allocations(); p4test_clip_boundaries(); p4test_rows(); p4test_transition();
	PS2HWD_GetStats(&st, 0);
	CHECK("p4_no_runtime_errors", !st.timeouts && !st.tex_missing && !st.tex_skipped,
		"timeouts=%u missing=%u skipped=%u", st.timeouts, st.tex_missing, st.tex_skipped);
	D.pfnShutdown();
	printf("H0 COMPLETE checks=%d failures=%d\n", checks, failures);
	for (;;) SleepThread();
	return failures;
}
