/* Actual hw_UpdateTexture plus the complete texture manager from the mixed
 * sequence fixture. Only SetTexture's caller boundary is reduced to upload. */
#define main mixed_sequence_unused_main
#include "fixture_base.c"
#undef main

static void free_source_checked(void *p)
{
	for (int i = 0; i < DC_MAX; i++) if (dc[i].p == p) check(!dc[i].busy, "update source freed only after queued references complete");
	free(p);
}

static int update_record;
static void settex_now(GLMipmap_t *m)
{
	update_record = tex_upload(m);
	check(update_record != NOREC, "update reupload admitted");
}
#include "texture-update.inc"

static void update_case(int zero_copy, int eviction)
{
	GLMipmap_t plain, keyed;
	u8 *old = malloc(65536), *fresh = malloc(65536), *image = malloc(65536);
	u32 entries[256], wrong_indices = 0, wrong_rgba = 0;
	int ri, vi;
	check(old && fresh && image, "update backing allocation"); reset_driver();
	if (!zero_copy) ps2hwd_dbg_flags = HWDBG_NOZC;
	memset(old, 41, 65536); memcpy(fresh, old, 65536);
	for (u32 y = 0; y < 256; y += 8) for (u32 x = 0; x < 256; x += 8) fresh[y * 256 + x] = 255;
	init_mipmap(&plain, (u32)(200 + zero_copy), 0, 1, malloc(65536));
	init_mipmap(&keyed, (u32)(200 + zero_copy), 1, 1, NULL);
	check(plain.data != NULL, "old engine source allocation"); memcpy(plain.data, old, 65536);
	plain.nextcolormap = &keyed; keyed.ps2_twin = &plain;
	ri = upload(&plain, old, image);
	if (eviction != 2) {
		vi = tex_view(&keyed, ri); check(vi != NOREC && H.rec[vi].clut == 1, "existing keyed view before update");
	}
	check(plain.ps2_h255 == 1 && !keyed.ps2_h255, "valid original opaque canonical knowledge");
	if (eviction == 1) tex_drop(ri, 0); /* DMA references deliberately remain pending. */
	if (zero_copy) check(rel_total == 1, "old source still has a queued DMA reference");
	keyed.data = malloc(65536); check(keyed.data != NULL, "new engine source allocation"); memcpy(keyed.data, fresh, 65536);
	capture_n = 0; update_record = NOREC;
	hw_UpdateTexture(&keyed);
	check(update_record != NOREC, "actual update called SetTexture boundary");
	decode_t8(&H.rec[update_record], image);
	clut_entries(H.rec[update_record].clut == 2 ? CK_KEYED : CK_OPAQUE, 0, 0, entries);
	for (u32 i = 0; i < 65536; i++) {
		u32 want = H.pal[fresh[i]] | ((fresh[i] == 255 ? 0u : 128u) << 24);
		wrong_indices += image[i] != fresh[i]; wrong_rgba += entries[image[i]] != want;
	}
	printf("T17U zero_copy=%d eviction=%d plain_canonical=%u keyed_canonical=%u clut=%u wrong_indices=%u wrong_rgba=%u index_crc=%08x expected_index_crc=%08x clut_alpha=%u other_handle=%u\n",
		zero_copy, eviction, (unsigned)plain.ps2_h255, (unsigned)keyed.ps2_h255, (unsigned)H.rec[update_record].clut,
		(unsigned)wrong_indices, (unsigned)wrong_rgba, (unsigned)crc32_bytes(image, 65536), (unsigned)crc32_bytes(fresh, 65536),
		(unsigned)(entries[255] >> 24), (unsigned)plain.downloaded);
	dma_fence(); free(plain.data); free(keyed.data); plain.data = keyed.data = NULL;
	reset_driver(); free(image); free(fresh); free(old);
}

int main(void)
{
	capture = calloc(CAP_QW, sizeof *capture); check(capture != NULL, "update capture allocation");
	for (int zero_copy = 0; zero_copy <= 1; zero_copy++) for (int eviction = 0; eviction <= 2; eviction++) update_case(zero_copy, eviction);
	reset_driver(); free(H.rec); free(blk_owner_p); free(capture); free(scratch);
	printf("T17U COMPLETE checks=%llu\n", checks);
	return 0;
}
