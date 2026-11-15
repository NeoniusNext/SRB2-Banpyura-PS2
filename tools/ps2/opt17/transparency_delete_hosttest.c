/* Actual DeleteTexture: a child view must leave its sibling intact, but the
 * final canonical object's deletion must not leak old source bytes into a
 * newly created mipmap with the same texture number. */
#define main mixed_sequence_unused_main
#include "fixture_base.c"
#undef main
static void free_source_checked(void *p)
{
	for (int i = 0; i < DC_MAX; i++) if (dc[i].p == p) check(!dc[i].busy, "source freed only after queued references complete");
	free(p);
}
static void plan_forget(const GLMipmap_t *m) { (void)m; }
#include "texture-delete.inc"

int main(void)
{
	GLMipmap_t plain, keyed, replacement;
	u8 *old = malloc(65536), *fresh = malloc(65536), *image = malloc(65536);
	u32 wrong = 0, before = 0, after = 0, pending;
	int ri, vi;
	capture = calloc(CAP_QW, sizeof *capture); check(capture && old && fresh && image, "delete witness allocation");
	reset_driver(); memset(old, 41, 65536); memcpy(fresh, old, 65536);
	for (u32 y = 0; y < 256; y += 8) for (u32 x = 0; x < 256; x += 8) fresh[y * 256 + x] = 255;
	init_mipmap(&plain, 400, 0, 1, malloc(65536)); init_mipmap(&keyed, 400, 1, 1, NULL);
	check(plain.data != NULL, "old engine source allocation"); memcpy(plain.data, old, 65536);
	plain.nextcolormap = &keyed; keyed.ps2_twin = &plain;
	ri = upload(&plain, old, image); vi = tex_view(&keyed, ri); check(vi != NOREC, "old keyed view");
	pending = (u32)rel_total; check(pending == 1, "initial source DMA is pending");
	hw_DeleteTexture(&keyed);
	check(!rel_total && !keyed.downloaded && plain.downloaded && rec_of(&plain), "view deletion fences DMA and preserves sibling image");
	check(dc_find(dc_key(&plain), 256, 256, 0) != NULL, "view deletion preserves valid sibling canonical source");
	tex_drop(ri, 0); wanted_level = 1;
	ri = upload(&plain, old, image); vi = tex_view(&keyed, ri); check(vi != NOREC, "new view of cached mip");
	for (int i = 0; i < DC_MAX; i++) before += dc[i].key == dc_key(&plain);
	check(before == 2, "old full and derived source levels both cached");
	hw_DeleteTexture(&plain);
	check(!plain.downloaded && !keyed.downloaded && !rel_total, "canonical image deletion invalidates every view and fences references");
	for (int i = 0; i < DC_MAX; i++) after += dc[i].key == dc_key(&plain);
	init_mipmap(&replacement, 400, 1, 1, malloc(65536)); check(replacement.data != NULL, "new engine object with reused source key"); memcpy(replacement.data, fresh, 65536);
	wanted_level = 0; capture_n = 0;
	ri = tex_upload(&replacement); check(ri != NOREC, "replacement upload admitted"); decode_t8(&H.rec[ri], image);
	for (u32 i = 0; i < 65536; i++) wrong += image[i] != fresh[i];
	printf("T17D pending_before=%u pending_after=%u cache_before=%u cache_after=%u wrong_indices=%u clut=%u canonical=%u index_crc=%08x expected_index_crc=%08x\n",
		(unsigned)pending, (unsigned)0, (unsigned)before, (unsigned)after, (unsigned)wrong,
		(unsigned)H.rec[ri].clut, (unsigned)replacement.ps2_h255, (unsigned)crc32_bytes(image, 65536), (unsigned)crc32_bytes(fresh, 65536));
	dma_fence(); free(plain.data); free(keyed.data); free(replacement.data); plain.data = keyed.data = replacement.data = NULL;
	reset_driver(); free(H.rec); free(blk_owner_p); free(capture); free(scratch); free(image); free(fresh); free(old);
	printf("T17D COMPLETE checks=%llu\n", checks); return 0;
}
