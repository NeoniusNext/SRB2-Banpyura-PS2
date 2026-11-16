/* Isolated native fixture hooks. Allocator limits select the real full-size
 * admission path; upload, geometry, DMA, GS rasterization and readback are real. */
_Static_assert(sizeof(texrec_t) == 140, "frozen native EE record ABI");

void TN_Pool(unsigned int blocks)
{
	ov_flush_all();
	dma_fence();
	tex_free_all();
	tile_cache_rec = NOREC;
	H.pool_blocks = blocks ? blocks : VRAM_BLOCKS - H.pool_base;
	H.free_n = 1;
	H.free_range[0].start = H.pool_base;
	H.free_range[0].len = H.pool_blocks;
	H.used_blocks = 0;
	H.imm_tex = NULL;
	plan_reset();
}

int TN_Upload(GLMipmap_t *m, unsigned int *info)
{
	int ri, saved = imm_level;
	imm_level = 0;
	ri = tex_upload(m);
	imm_level = saved;
	if (ri == NOREC) return 0;
	H.cur_tex = ri; H.cur_missing = 0; H.imm_tex = NULL;
	if (info) {
		texrec_t *r = &H.rec[ri];
		info[0] = r->tiled; info[1] = r->psm; info[2] = r->w; info[3] = r->h;
		info[4] = r->nblk; info[5] = r->clut; info[6] = r->dx; info[7] = r->dy;
	}
	return 1;
}

int TN_Raw(unsigned int *dest)
{
	return PS2HWD_ReadVram(dest, H.fb_addr[H.last_drawn] / 256,
		(unsigned int)H.fbw / 64, H.fbpsm, 0, 0, H.vw, H.vh);
}

int TN_Slot(int slot, unsigned int *info)
{
	int ri = H.scr_rec[slot];
	if (ri == NOREC) return 0;
	info[0] = H.rec[ri].tiled; info[1] = H.rec[ri].nblk;
	info[2] = H.scr_data[slot] != NULL;
	info[3] = H.screen_bytes; info[4] = H.used_blocks; info[5] = H.pool_blocks;
	return 1;
}

int TN_Spill(void) { ov_flush_all(); dma_fence(); return scr_spill_victim(); }

void TN_ForceProxy(int slot)
{
	int ri = H.scr_rec[slot];
	if (ri == NOREC || H.rec[ri].tiled) return;
	ov_flush_all(); dma_fence();
	if (scr_backing_read(slot, H.rec[ri].blk, H.rec[ri].tbw)) {
		tex_drop(ri, 1);
		(void)scr_resident(slot);
	}
}

/* Valid pre-classified fog piece at the same boundary used by sky colors and
 * gr_modellighting. Calls the real production emit_piece/format4/format6. */
static int tn_attribute_onecell;
void TN_AttributeCell(int on) { tn_attribute_onecell = on; }
#ifdef TN_DYNAMIC_TILE_PROBE
void TN_PhysicalTileInfo(u32 *out)
{
	out[0] = tile_cache.valid;
	out[1] = tile_cache.region;
	out[2] = tile_cache.region & 2 ? TILE_MAX_SIZE : TILE_SIZE;
	out[3] = tile_cache_rec == NOREC ? 0 : H.rec[tile_cache_rec].nblk;
}
#endif
#ifdef TN_ONECORE_PROBE
void TN_OneCoreInfo(u32 *out)
{
	out[0] = tile_core_fast_count();
	out[1] = tile_stencil_rectangles;
	out[2] = tile_stencil_mask_sprites;
	out[3] = tile_workspace_peak();
}
#endif
#ifdef TN_BORROW_PROBE
static u32 tn_borrow_blocked, tn_borrow_ordinary;
int TN_BorrowCapture(int slot, u32 *out)
{
	const int ri = scr_resident(slot);
	if (ri == NOREC) return 0;
	H.cur_tex = ri; H.cur_missing = 0; H.imm_tex = NULL;
	out[0] = H.rec[ri].tiled;
	return H.rec[ri].screen && H.rec[ri].psm == PSM_CT32;
}
void TN_BorrowInfo(u32 *out)
{
	const int ri = tile_cache_rec;
	out[0] = tile_cache_borrowed_blocks();
	out[1] = ri == NOREC ? 0 : H.rec[ri].blk;
	out[2] = ri == NOREC ? 0 : H.rec[ri].nblk;
	out[3] = H.st.upload_bytes;
	out[4] = H.used_blocks;
	out[5] = H.wr;
	out[6] = !tile_cache.valid ? 0 : tile_cache.region & 2 ? 128 : 64;
	out[7] = !tile_cache.valid ? 0 : tile_cache.region & 8 ? 256 : tile_cache.region & TILE_REGION_WIDE ? 64 : out[6];
}
int TN_BorrowBlockPrefix(void)
{
	const int ri = tile_cache_get();
	if (ri == NOREC) return 0;
	tn_borrow_blocked = vram_alloc_hi(64, PAGE_BLOCKS);
	return tn_borrow_blocked && tn_borrow_blocked + 64 == H.rec[ri].blk;
}
static void tn_borrow_overwrite_row(void *ctx, u32 row, u8 *dst)
{
	(void)ctx;
	for (u32 x = 0; x < 64; x++) ((u32 *)(void *)dst)[x] = 0x7f003100u ^ (row * 0x010101u + x * 0x070503u);
}
int TN_BorrowReclaimOverwrite(u32 *out)
{
	const int ri = tile_cache_rec;
	if (ri == NOREC || tile_cache_borrowed_blocks() != 64) return 0;
	const u32 prefix = H.rec[ri].blk;
	// No fixture flush/fence/read occurs before this real ordinary allocation.
	tn_borrow_ordinary = vram_alloc_evicting(416, PAGE_BLOCKS, 1);
	out[0] = prefix; out[1] = tn_borrow_ordinary;
	out[2] = tile_cache_borrowed_blocks(); out[3] = H.rec[ri].blk;
	if (!tn_borrow_ordinary || out[2] || H.rec[ri].nblk != 64
		|| tn_borrow_ordinary > prefix || tn_borrow_ordinary + 416 < prefix + 64) return 0;
	// Overwrite the complete returned prefix before the first frame readback.
	upload_bands(prefix, 1, PSM_CT32, 64, 64, 4, tn_borrow_overwrite_row, NULL);
	return 1;
}
void TN_BorrowCleanup(void)
{
	if (tn_borrow_blocked) { vram_free(tn_borrow_blocked, 64); tn_borrow_blocked = 0; }
	if (tn_borrow_ordinary) { vram_free(tn_borrow_ordinary, 416); tn_borrow_ordinary = 0; }
}
#endif
int TN_AttributeFog(int model_light, int zwrite)
{
	FSurfaceInfo surf;
	piece_t piece = {0.0f, 0.0f, 12, 0};
	cv_t tri[3];
	const float nx[3] = {-0.93f, 0.94f, -0.15f}, ny[3] = {-0.88f, -0.73f, 0.94f};
	const float w[3] = {1.0f, 2.0f, 1.5f}, nz[3] = {0.3f, 0.15f, 0.7f};
	const float rgba[3][4] = {{220,80,50,255},{70,230,140,255},{100,130,250,255}};
	const float factor[3] = {0.25f, 0.7f, 1.0f};
	int i, a, on;
	u32 fog;
	memset(&surf, 0, sizeof surf); surf.PolyColor.rgba = 0xffffffffu;
	if (!begin_draw(PF_Masked | PF_Modulated | PF_NoDepthTest, &surf) || P.npass != 1) return 0;
	P.vcol = 1; P.vlight = model_light;
	P.fast = P.litfast = P.vuok = 0;
	P.pal = 0; P.oldpath = 1; P.lmode = LM_BANDS;
	light_setup(&P.rs.lp, 144, 0, 0, 31);
	P.pass[0].fog = 1; P.pass[0].zwrite = (u8)zwrite;
	P.fogcol = 0x00675432u;
	memset(tri, 0, sizeof tri);
	for (i = 0; i < 3; i++) {
		tri[i].x = nx[i] * w[i]; tri[i].y = ny[i] * w[i]; tri[i].z = nz[i] * w[i]; tri[i].w = w[i];
		tri[i].a[0] = i == 1 ? 0.95f : i == 2 ? 0.2f : 0.05f;
		tri[i].a[1] = i == 2 ? 0.05f : i == 1 ? 0.9f : 0.95f;
		if (tn_attribute_onecell) {
			float u = i == 1 ? 95.0f : i == 2 ? 90.0f : 80.0f;
			float v = i == 2 ? 70.0f : i == 1 ? 80.0f : 90.0f;
			if (tn_attribute_onecell == 2) { u = i == 1 ? 32.0f : i == 2 ? 16.0f : 0.0f; v = i == 2 ? 32.0f : 0.0f; }
			if (tn_attribute_onecell == 3) { u = i == 1 ? 141.0f : i == 2 ? 92.0f : 80.0f; v = i == 2 ? 161.0f : i == 1 ? 110.0f : 100.0f; }
			if (tn_attribute_onecell == 4) u = i == 1 ? 592.0f : i == 2 ? 90.0f : 80.0f;
			if (tn_attribute_onecell == 5) { u = i == 1 ? 205.0f : i == 2 ? 92.0f : 80.0f; v = i == 2 ? 225.0f : i == 1 ? 110.0f : 100.0f; }
			if (tn_attribute_onecell == 6) { u = i == 1 ? 205.0f : i == 2 ? 92.0f : 80.0f; v = i == 2 ? 325.0f : i == 1 ? 110.0f : 100.0f; }
			if (tn_attribute_onecell == 7 || tn_attribute_onecell == 8) { u = i == 1 ? (tn_attribute_onecell == 8 ? 205.0f : 204.0f) : i == 2 ? 92.0f : 80.0f; v = i == 2 ? (tn_attribute_onecell == 8 ? 131.0f : 130.0f) : i == 1 ? 110.0f : 70.0f; }
			tri[i].a[0] = u / (tn_attribute_onecell >= 7 ? (float)P.r->sw : 512.0f);
			tri[i].a[1] = v / (tn_attribute_onecell >= 7 ? (float)P.r->sh : 512.0f);
		}
		if (model_light) tri[i].a[2] = factor[i];
		else for (a = 0; a < 4; a++) tri[i].a[2 + a] = rgba[i][a];
	}
#ifdef TN_BORROW_PROBE
	if (tn_attribute_onecell >= 7) {
		int ox = 0, oy = 0;
		float raw[4] = {tri[0].a[0] * P.r->sw, tri[1].a[0] * P.r->sw, tri[0].a[1] * P.r->sh, tri[2].a[1] * P.r->sh};
		u32 bits[4]; memcpy(bits, raw, sizeof bits);
		const int eligible = tile_one_wide(tri, &piece, P.r, &ox, &oy);
		printf("TN BORROWELIG cell=%d tiled=%u sw=%u sh=%u raw_slo=%08x raw_shi=%08x raw_tlo=%08x raw_thi=%08x eligible=%d ox=%d oy=%d\n", tn_attribute_onecell, P.r->tiled, P.r->sw, P.r->sh, bits[0], bits[1], bits[2], bits[3], eligible, ox, oy);
	}
#endif
	fog = fog_of_class(&P.rs, piece.cls, &on);
	if (!on) return 0;
	{ qw_t *p = ad_alloc(1); ad_set(p, (u64)fog << 56, GSR_FOG); }
	printf("TN ATTRIBUTEPACK model_light=%d zwrite=%d format=%d fog=%u tiled=%u\n", model_light, zwrite, model_light ? 6 : 4, fog, P.r->tiled);
	emit_piece(tri, 3, &piece);
	P.vcol = P.vlight = 0;
	return 1;
}
