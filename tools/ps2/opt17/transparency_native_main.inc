static const u8 *t17_source;
static unsigned int t17_stream_reads;
int PS2HWD_T17Upload(GLMipmap_t *m, unsigned int level);
int PS2HWD_T17View(GLMipmap_t *m, GLMipmap_t *owner);
int PS2HWD_T17Info(GLMipmap_t *m, unsigned int *out);
void PS2HWD_T17Select(GLMipmap_t *m);

static void t17_info(const char *phase, GLMipmap_t *m)
{
	u32 out[8] = {0};
	int ok = PS2HWD_T17Info(m, out);
	CHECK("t17_info", ok, "phase=%s", phase);
	printf("T17N phase=%s canonical=%u has255=%u clut=%u clut255=%08x cbp=%u tex0=%08x%08x level=%u\n",
		phase, out[0], out[1], out[2], out[3], out[4], out[6], out[5], out[7]);
}

static void t17_case(int restart)
{
	GLMipmap_t plain = {0}, keyed = {0};
	u8 *full = memalign(64, 65536);
	FSurfaceInfo s = surf_rgba(255, 255, 255, 255);
	u32 out[8] = {0}; int result, wrong = 0, compared = 0;
	const u32 background = 0x00332211u;
	CHECK("t17_source", full != NULL, "restart=%d", restart);
	for (u32 y = 0; y < 256; y++) for (u32 x = 0; x < 256; x++)
		full[y * 256 + x] = x % 8 == 0 && y % 8 == 0 ? 255 : (u8)(40 + ((x + y) % 40));
	t17_source = full; t17_stream_reads = 0;
	plain.width = plain.height = keyed.width = keyed.height = 256;
	plain.format = keyed.format = GL_TEXFMT_P_8;
	plain.flags = TF_WRAPXY; keyed.flags = TF_WRAPXY | TF_CHROMAKEYED;
	plain.regen_kind = keyed.regen_kind = 2;
	plain.regen_id = keyed.regen_id = restart + 37;
	plain.nextcolormap = &keyed; keyed.ps2_twin = &plain;
	CHECK("t17_streamed_l1", PS2HWD_T17Upload(&plain, 1), "restart=%d reads=%u", restart, t17_stream_reads);
	t17_info("streamed-L1", &plain); D.pfnDeleteTexture(&plain);
	CHECK("t17_derived_l2", PS2HWD_T17Upload(&plain, 2), "restart=%d", restart);
	t17_info("derived-L2", &plain); D.pfnDeleteTexture(&plain);
	if (restart) {
		/* The real SetScreenSize teardown frees driver caches while these
		 * engine mipmaps and their canonical classification survive. */
		PS2HWD_SetScreenSize(640, 400);
		PS2HWD_GetInfo(&INFO);
		FW = INFO.vw; FH = INFO.vh; FB32 = INFO.fb32; SX = (float)FW / SW; SY = (float)FH / SH;
		printf("T17N restart screen=%dx%d internal=%dx%d canonical=%u handles=%u/%u\n", INFO.screen_w, INFO.screen_h, FW, FH,
			(unsigned)plain.ps2_h255, (unsigned)plain.downloaded, (unsigned)keyed.downloaded);
	}
	plain.data = memalign(64, 65536); CHECK("t17_regenerated_source", plain.data != NULL, "restart=%d", restart);
	memcpy(plain.data, full, 65536);
	CHECK("t17_full_plain", PS2HWD_T17Upload(&plain, 0), "restart=%d", restart);
	t17_info("full-plain", &plain);
	CHECK("t17_keyed_view", PS2HWD_T17View(&keyed, &plain), "restart=%d", restart);
	t17_info("full-keyed", &keyed);
	CHECK("t17_clut_read", PS2HWD_T17Info(&keyed, out), "restart=%d", restart);
	frame_start(background);
	PS2HWD_T17Select(&keyed);
	quad2d(&s, 40, 50, 140, 150, 0.5f / 256, 0.5f / 256, 0.5f / 256, 0.5f / 256, PF_Masked | PF_Modulated | PF_NoDepthTest);
	quad2d(&s, 180, 50, 280, 150, 1.5f / 256, 0.5f / 256, 1.5f / 256, 0.5f / 256, PF_Masked | PF_Modulated | PF_NoDepthTest);
	result = frame_grab();
	for (int y = (int)(60 * SY); y < (int)(140 * SY); y++) for (int x = (int)(50 * SX); x < (int)(130 * SX); x++) {
		wrong += fb[y * FW + x] != background; compared++;
	}
	printf("T17N PIXEL restart=%d read=%d wrong=%d compared=%d hole=%06x expected_hole=%06x opaque=%06x expected_opaque=%06x clut_alpha=%u canonical=%u\n",
		restart, result, wrong, compared, (unsigned)fb[(int)(100 * SY) * FW + (int)(90 * SX)], (unsigned)background,
		(unsigned)fb[(int)(100 * SY) * FW + (int)(220 * SX)], (unsigned)palc(PAL, 41), (unsigned)(out[3] >> 24), (unsigned)plain.ps2_h255);
	CHECK("t17_opaque_control", result == 0 && fb[(int)(100 * SY) * FW + (int)(220 * SX)] == palc(PAL, 41), "restart=%d", restart);
	/* This is an observation witness: the baseline deliberately prints the
	 * wrong-hole count, and the coordinator compares both frozen ELFs. */
	D.pfnClearMipMapCache(); plain.data = keyed.data = NULL; t17_source = NULL; free(full);
}

int main(int argc, char **argv)
{
	ps2hwd_config_t c;
	(void)argc; (void)argv;
	_Static_assert(sizeof(GLMipmap_t) == 68, "real packed EE mipmap ABI");
	make_pals(); PS2HWD_GetConfig(&c);
	c.video = PS2HWD_VIDEO_NTSC; c.fbw = c.fbh = 0; c.fb32 = 1; c.linear = 0; c.dither = 0;
	c.screen_w = 320; c.screen_h = 200; c.tex_adapt = 0; c.tex_cap_blocks = 0;
	PS2HWD_Configure(&c); PS2HWD_FillDriver(&D);
	CHECK("t17_init", D.pfnInit() == true, "native GS transparency sequence");
	D.pfnSetTexturePalette(PAL); PS2HWD_GetInfo(&INFO);
	FW = INFO.vw; FH = INFO.vh; FB32 = INFO.fb32; SX = (float)FW / SW; SY = (float)FH / SH;
	fb = memalign(64, 640u * 512u * 4u); CHECK("t17_frame_buffer", fb != NULL, "max native frame storage");
	t17_case(0); t17_case(1);
	D.pfnShutdown(); free(fb);
	printf("T17N COMPLETE checks=%d failures=%d\n", checks, failures);
	return failures ? 1 : 0;
}
