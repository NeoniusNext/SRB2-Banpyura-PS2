/* Real PS2 zone allocator, actual engine lock/unlock/steal callbacks, complete
 * texture manager, and actual caller release condition. DMA is a boundary
 * service; all pointer owners, tags and purge decisions belong to z_zone.c. */
#include <malloc.h>
#include <stdint.h>
static int netgame;
static int PS2Lua_InCall(void) { return 0; }
void PS2Spill_Reset(void) { }
typedef uint8_t UINT8;
#define max(a,b) ((a) > (b) ? (a) : (b))
#define main zone_unused_main
#define checks zone_checks
#define check zone_check
#include "zone_fixture.c"
#undef check
#undef checks
#undef main
#undef I_OutputMsg
#undef memalign
#define main mixed_unused_main
#include "fixture_base.c"
#undef main

static unsigned failures;
static int source_tag(const void *p) { return (int)ZA_TAG(ZA_BLOCK(p)); }
static void release_as_caller(GLMipmap_t *m)
{
#include "caller-release.inc"
}

static void lock_case(int mode)
{
	GLMipmap_t m;
	u8 expected[65536], *image = malloc(65536);
	const u8 *source;
	dcent_t *old_cache = NULL, *new_cache;
	int ri, old_tag_before, old_tag_queued, old_tag_done;
	u32 old_busy_before, old_busy_queued, old_busy_done, held_before, held_after;
	void **source_owner;
	check(image != NULL, "lock witness image allocation");
	reset_driver(); arena_reset(8u << 20, 1, 0, 0); reset_driver();
	Z_SetHWCacheLRU(true, 0, 0);
	memset(expected, 41, sizeof expected);
	init_mipmap(&m, (u32)(500 + mode), 0, 1, NULL);
	m.data = Z_MallocAlign(65536, PU_HWRCACHE, &m.data, 6); check(m.data != NULL, "real-zone engine source allocation");
	memcpy(m.data, expected, 65536);
	if (mode == 0) {
		/* First mip from an engine-owned full source, no source DMA ref. */
		source = m.data; source_owner = &m.data; old_tag_before = source_tag(source); old_busy_before = 0;
		wanted_level = 1;
	} else if (mode == 1 || mode == 2) {
		wanted_level = 0; ri = upload(&m, expected, image);
		old_cache = dc_find(dc_key(&m), 256, 256, 0); check(old_cache && !m.data, "actual full-source transfer to zone-owned DC block");
		if (mode == 1) dma_fence(); /* idle source versus still queued source */
		tex_drop(ri, 0); source = old_cache->p; source_owner = (void **)(void *)&old_cache->p;
		old_tag_before = source_tag(source); old_busy_before = old_cache->busy; wanted_level = 1;
	} else {
		wanted_level = 1; ri = upload(&m, expected, image); release_as_caller(&m); dma_fence();
		old_cache = dc_find(dc_key(&m), 128, 128, 1); check(old_cache != NULL, "actual initial level one cache source");
		tex_drop(ri, 0); source = old_cache->p; source_owner = (void **)(void *)&old_cache->p;
		old_tag_before = source_tag(source); old_busy_before = old_cache->busy; wanted_level = 2;
	}
	ri = upload(&m, expected, image); release_as_caller(&m);
	new_cache = dc_find(dc_key(&m), 256u >> wanted_level, 256u >> wanted_level, wanted_level);
	check(new_cache && new_cache->busy && source != new_cache->p && zc_last, "new mip is a distinct pending zero-copy source");
	old_tag_queued = source_tag(source); old_busy_queued = old_cache ? old_cache->busy : 0;
	if (old_busy_queued) check(old_tag_queued == PU_HWRCACHE, "old pending DMA source cannot be unlocked early");
	dma_fence(); old_tag_done = source_tag(source); old_busy_done = old_cache ? old_cache->busy : 0;
	check(!old_busy_done && !new_cache->busy && !rel_total, "all old and new source DMA references complete");
	held_before = *source_owner != NULL;
	Z_FreeTags(PU_HWRCACHE_LRU, PU_HWRCACHE_LRU);
	held_after = *source_owner != NULL;
	printf("T17L mode=%d source_tag_before=%d source_busy_before=%u source_tag_queued=%d source_busy_queued=%u source_tag_done=%d source_busy_done=%u source_alive_before_purge=%u source_alive_after_purge=%u new_mip_alive_after_purge=%u index_crc=%08x\n",
		mode, old_tag_before, (unsigned)old_busy_before, old_tag_queued, (unsigned)old_busy_queued, old_tag_done, (unsigned)old_busy_done,
		(unsigned)held_before, (unsigned)held_after, (unsigned)(new_cache->p != NULL), (unsigned)crc32_bytes(image, (size_t)H.rec[ri].w * H.rec[ri].h));
	if (held_after || old_tag_done == PU_HWRCACHE) failures++;
	if (m.data) Z_Free(m.data); m.data = NULL;
	reset_driver(); free(image);
}

int main(void)
{
	capture = calloc(CAP_QW, sizeof *capture); check(capture != NULL, "real-zone lock witness capture");
	for (int mode = 0; mode <= 3; mode++) lock_case(mode);
	reset_driver(); free(H.rec); free(blk_owner_p); free(capture); free(scratch);
	if (arena_raw) free(arena_raw);
	printf("T17L COMPLETE stranded_sources=%u checks=%llu\n", failures, checks);
	return 0;
}
