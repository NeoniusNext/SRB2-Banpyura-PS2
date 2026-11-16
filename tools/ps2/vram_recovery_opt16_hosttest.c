#include <stdlib.h>
static void alloc_snapshot(void);
#define main retained_p4_main
#include "production-fixture.inc"
#undef main
static u32 prefix_hash;
static void hash_word(u32 word) { prefix_hash = (prefix_hash ^ word) * 16777619u; }
static void alloc_snapshot(void)
{
	u32 i;
	prefix_hash = 2166136261u;
	hash_word(H.used_blocks); hash_word((u32)H.free_n); hash_word((u32)H.lru_head); hash_word((u32)H.lru_tail);
	for (i = 0; i < VRAM_BLOCKS; i++) hash_word(blk_owner[i]);
	for (i = 0; i < (u32)H.free_n; i++) { hash_word(H.free_range[i].start); hash_word(H.free_range[i].len); }
	for (i = 0; i < (u32)H.rec_n; i++) { hash_word(H.rec[i].used); hash_word(H.rec[i].blk); hash_word(H.rec[i].nblk); hash_word(H.rec[i].seq); }
}
static void ensure(int ok, const char *what)
{
	if (!ok) { fprintf(stderr, "VRAM recovery proof failed: %s\n", what); exit(1); }
}
static void pool(u32 size, int full, int alternating)
{
	u32 i;
	reset(); H.pool_blocks = size; H.free_range[0].len = size; H.frame_no = 5; batch_phase = 0;
	for (i = 0; full && i < size; i++)
	{
		int id = rec_new(); texrec_t *r = &H.rec[id];
		r->blk = (u16)vram_alloc(1, 1); r->nblk = 1; r->psm = PSM_CT32;
		r->w = r->h = r->uw = r->vh = r->sw = r->sh = 1; r->tbw = 1;
		own_set(id); lru_push_head(id);
	}
	if (alternating)
	{
		for (i = 0; i < size; i += 2) lru_touch((int)i);
		for (i = 1; i < size; i += 2) lru_touch((int)i);
	}
	for (i = 0; i < (u32)H.rec_n; i++) H.rec[i].stamp = 0;
	prefix_hash = 0;
}
static void validate(u32 block, u32 size)
{
	u32 i, b, occupied = 0, freeblocks = 0;
	if (block)
	{
		int id = rec_new(); H.rec[id].blk = (u16)block; H.rec[id].nblk = (u16)size; own_set(id);
	}
	for (i = 0; i < (u32)H.free_n; i++)
	{
		u32 start = H.free_range[i].start, end = start + H.free_range[i].len;
		ensure(start >= H.pool_base && end <= H.pool_base + H.pool_blocks, "free range stays in pool");
		if (i) ensure(H.free_range[i-1].start + H.free_range[i-1].len < start, "free ranges sorted and fully coalesced");
		for (b = start; b < end; b++) ensure(!blk_owner[b], "free block has no owner");
		freeblocks += end - start;
	}
	for (b = H.pool_base; b < H.pool_base + H.pool_blocks; b++)
	{
		u16 id = blk_owner[b];
		if (id) { const texrec_t *r = &H.rec[id-1]; ensure(r->used && r->share == NOREC && b >= r->blk && b < r->blk + r->nblk, "every owner-map entry names its live physical span"); occupied++; }
	}
	ensure(occupied == H.used_blocks && occupied + freeblocks == H.pool_blocks, "no owned/free bytes lost or duplicated");
}
static void emit(const char *name, u32 block, u32 evictions, int comma)
{
	printf("%s{\"name\":\"%s\",\"allocated\":%u,\"evictions\":%u,\"prefix_hash\":%u,\"used_blocks\":%u}", comma ? "," : "", name, block, evictions, prefix_hash, H.used_blocks);
}
int main(void)
{
	u32 block, oldused, i; GLMipmap_t view; int viewid;
	printf("{\"cases\":[");
	pool(256, 0, 0); block = vram_alloc_evicting(64,32,0); ensure(block && !H.st.evictions, "ordinary first-attempt success unchanged"); validate(block,64); emit("free-fast",block,H.st.evictions,0);
	pool(64, 1, 0); block = vram_alloc_evicting(64,1,0);
	ensure((block != 0) == RECOVERY_EXPECTED, "retry after the 64th victim must allocate the now-free pool");
	if (!RECOVERY_EXPECTED) ensure(!H.used_blocks && best_window(64,1) != NOWIN, "old failure leaves an entirely feasible free window");
	validate(block,64); emit("last-victim-retry",block,H.st.evictions,1);
	pool(256, 1, 1); block = vram_alloc_evicting(128,32,1);
	ensure((block != 0) == RECOVERY_EXPECTED, "fragmented oldest victims need one exact window after 64 attempts");
	if (!RECOVERY_EXPECTED) ensure(H.st.evictions == 64 && best_window(128,32) != NOWIN, "old fragmented failure has a legal evictable window");
	validate(block,128); emit("scattered-oldest",block,H.st.evictions,1);
	pool(256, 1, 1); H.rec[0].pin = 1; H.rec[2].screen = 3; H.rec[2].stamp = H.frame_no; H.scr_rec[2] = 2;
	memset(&view,0,sizeof view); view.width=view.height=1; viewid=tex_view(&view,1); ensure(viewid != NOREC,"real shared view created"); H.cur_tex=viewid;
	block = vram_alloc_evicting(128,32,0);
	ensure((block != 0) == RECOVERY_EXPECTED, "protected-record fragmented request recovers");
	ensure(H.rec[0].used && H.rec[0].pin && H.rec[1].used && H.rec[viewid].used && view.downloaded && H.rec[2].used && H.scr_rec[2] == 2, "recovery preserves pin, current shared image, view and capture");
	validate(block,128); emit("protected-window",block,H.st.evictions,1);
	pool(128, 1, 0); for (i=0;i<128;i++) H.rec[i].pin=1; oldused=H.used_blocks;
	block=vram_alloc_evicting(64,32,0); ensure(!block && H.used_blocks==oldused && !H.st.evictions,"no feasible protected window remains a failure without eviction"); validate(block,64);emit("all-protected",block,H.st.evictions,1);
	pool(64,1,0); H.pool_base++; for(i=0;i<64;i++) H.rec[i].blk++; memset(blk_owner,0,sizeof blk_owner);for(i=0;i<64;i++)own_set((int)i);
	block=vram_alloc_evicting(64,64,0);ensure(!block,"impossible alignment remains a failure");validate(block,64);emit("impossible-alignment",block,H.st.evictions,1);
	printf("],\"recovery_expected\":%d,\"independent_owner_free_bitmap_passed\":true}\n",RECOVERY_EXPECTED);
	reset();free(H.rec);free(blk_owner_p);return 0;
}
