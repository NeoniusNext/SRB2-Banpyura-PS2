/* Included only by gs_list_eetest.py's generated copy of hw_test.c. */
static void test_gs_list_model_perf(void)
{
	enum { NT = 3000 };
	static short xyz[9] = {-32, -32, 0, 32, -32, 0, 0, 32, 0};
	static char normals[9] = {0, 127, 0, 0, -127, 0, 127, 0, 0};
	static float uv[6] = {0, 1, 1, 1, 0.5f, 0};
	static unsigned short indices[NT * 3];
	tinyframe_t tiny = {0}; mesh_t mesh = {0}; model_t model = {0};
	FTransform pos = {0}; FSurfaceInfo surface = surf_rgba(100, 180, 240, 255);
	GLMipmap_t *texture = tex_new(16, 16, GL_TEXFMT_RGBA, 0);
	int light, i, x, y;

	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			rgba_set(texture, x, y, PAL, (x * 13 + y * 19) % 255, 255);
	for (i = 0; i < NT * 3; i++) indices[i] = (unsigned short)(i % 3);
	tiny.vertices = xyz; tiny.normals = normals;
	mesh.numFrames = 1; mesh.numVertices = 3; mesh.numTriangles = NT;
	mesh.uvs = uv; mesh.indices = indices; mesh.tinyframes = &tiny;
	model.numMeshes = 1; model.meshes = &mesh;
	pos.y = 1;
	surface.PolyFlags = PF_Masked | PF_Modulated | PF_Occlude;
	for (light = 0; light < 2; light++)
	{
		ps2hwd_stats_t before, after;
		u32 t0, t1, t2;
		int r, bad = 0, visible = 0;

		D.pfnSetSpecialState(HWD_SET_MODEL_LIGHTING, light);
		ps2hwd_model_ab = 0;
		frame_start(0); D.pfnSetTexturePalette(PAL); D.pfnSetTexture(texture);
		PS2HWD_Sync();
		PS2HWD_GetStats(&before, 0);
		t0 = cyc();
		D.pfnDrawModel(&model, 0, 0, 0, -1, &pos, 0.25f, 0.25f, 0, 0, &surface);
		t1 = cyc();
		PS2HWD_Sync();
		t2 = cyc();
		PS2HWD_GetStats(&after, 0);
		r = frame_grab(); memcpy(fb2, fb, (size_t)FW * FH * 4);
		dump_ppm(light ? "gs-list-model-light" : "gs-list-model-flat", fb);
		printf("H0 model-perf light=%d triangles=%u vertices=%u EE=%u wait=%u qwords=%u kicks=%u\n",
			light, after.polys - before.polys, after.verts_out - before.verts_out,
			(unsigned)(t1 - t0), (unsigned)(t2 - t1), after.qwords - before.qwords, after.dma_kicks - before.dma_kicks);
		ps2hwd_model_ab = 4; /* exact indexed fallback, including the model's vertex lighting */
		frame_start(0); D.pfnSetTexture(texture);
		D.pfnDrawModel(&model, 0, 0, 0, -1, &pos, 0.25f, 0.25f, 0, 0, &surface);
		r |= frame_grab();
		for (i = 0; i < FW * FH; i++) { bad += fb[i] != fb2[i]; visible += fb2[i] != 0; }
		CHECK(light ? "gs_list_model_light" : "gs_list_model_flat", r == 0 && bad == 0 && visible > 10 && after.polys - before.polys == NT,
			"textured optimized model vs indexed fallback: bad=%d visible=%d readback=%d", bad, visible, r);
	}
	ps2hwd_model_ab = 0;
	D.pfnSetSpecialState(HWD_SET_MODEL_LIGHTING, 0);
	tex_free(texture);
}
