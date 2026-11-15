/* Build a map mip through an actual keyed full-source upload. The original
 * variant remains canonically unknown; no fixture-written DC entry is used. */
#define main mixed_sequence_unused_main
#include "fixture_base.c"
#undef main

int main(void)
{
	GLMipmap_t plain, keyed;
	u8 *full = malloc(65536), *level1 = malloc(16384), *level2 = malloc(4096), *image = malloc(65536);
	int ri, vi;
	capture = calloc(CAP_QW, sizeof *capture); check(capture && full && level1 && level2 && image, "map twin witness allocation");
	reset_driver();
	for (u32 y = 0; y < 256; y++) for (u32 x = 0; x < 256; x++) full[y * 256 + x] = x % 8 == 0 && y % 8 == 0 ? 255 : (u8)(40 + ((x + y) % 40));
	stream_source = full; stream_width = stream_height = 256;
	dec_level_ref(full, 256, 256, 1, level1); dec_level_ref(level1, 128, 128, 1, level2);
	init_mipmap(&plain, 300, 0, 1, NULL); init_mipmap(&keyed, 300, 1, 1, malloc(65536));
	check(keyed.data != NULL, "initial keyed engine full source"); memcpy(keyed.data, full, 65536);
	plain.nextcolormap = &keyed; keyed.ps2_twin = &plain;
	wanted_level = 1;
	ri = upload(&keyed, level1, image); inspect("map-real-twin", "keyed-full-to-L1", &keyed, ri, image, level1);
	check(keyed.ps2_h255 == 2 && plain.ps2_h255 == 0, "only uploaded keyed variant knows the full source");
	check(dc_find(dc_key(&plain), 128, 128, 1) != NULL, "actual upload generated the shared level one");
	tex_drop(ri, 0);
	wanted_level = 2;
	ri = upload(&plain, level2, image); inspect("map-real-twin", "plain-derived-L2", &plain, ri, image, level2);
	tex_drop(ri, 0);
	wanted_level = 0; plain.data = malloc(65536); check(plain.data != NULL, "regenerated plain engine source"); memcpy(plain.data, full, 65536);
	ri = upload(&plain, full, image); inspect("map-real-twin", "full-plain", &plain, ri, image, full);
	vi = tex_view(&keyed, ri); check(vi != NOREC, "known-positive keyed variant takes full plain image view");
	inspect("map-real-twin", "full-keyed-view", &keyed, vi, image, full);
	reset_driver(); stream_source = full; stream_width = stream_height = 256;
	check(!plain.downloaded && !keyed.downloaded && keyed.ps2_h255 == 2, "actual driver teardown retains engine canonical knowledge");
	plain.data = malloc(65536); check(plain.data != NULL, "post-restart engine source"); memcpy(plain.data, full, 65536);
	ri = upload(&plain, full, image); inspect("map-real-twin", "restart-full-plain", &plain, ri, image, full);
	vi = tex_view(&keyed, ri); check(vi != NOREC, "post-restart keyed view");
	inspect("map-real-twin", "restart-keyed-view", &keyed, vi, image, full);
	free(keyed.data); keyed.data = NULL; reset_driver(); stream_source = NULL; stream_width = stream_height = 0;
	free(H.rec); free(blk_owner_p); free(capture); free(scratch); free(image); free(level2); free(level1); free(full);
	printf("T17 COMPLETE observations=%u rgba_bad=%u poisoned=%u checks=%llu\n", observations, rgba_bad, poisoned, checks);
	return 0;
}
