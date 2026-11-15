/* Compile the complete production texture manager. Engine/packet boundaries
 * are deterministic host services; upload bytes, CLUT image entries and TEX0
 * are inspected separately from the expected logical RGBA oracle. */
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"
#include "ps2_hwd_dbg.h"
#define CONS_WARNING 1
#define CONS_ERROR 2
#define CONS_Alert(level, ...) ((void)(level))
#define CONS_Printf(...) ((void)0)
#define I_OutputMsg(...) ((void)0)
#include "ps2_hw_priv.inc"
#include "ps2_hw_regs.inc"
#include "ps2_hw_fx2.h"
#include "ps2_hw_xform.inc"

#define CAP_QW (1u << 19)
static qw_t *capture;
static u32 capture_n, locks, unlocks;
static unsigned long long checks;
static unsigned rgba_bad, poisoned, observations;
static struct { int chunk_open; } V;
UINT32 hwr_texsig;
int ps2hwd_fx2;
static u32 wanted_level;
static const u8 *stream_source;
static u32 stream_width, stream_height, stream_reads;
static int stream_on;

static void check(int yes, const char *what)
{
	checks++;
	if (!yes) { fprintf(stderr, "Transparency proof failed: %s (check %llu)\n", what, checks); exit(1); }
}
void Z_SetRebuildCost(void **user, unsigned int cost) { (void)user; (void)cost; }
static int HWR_ShouldUsePaletteRendering(void) { return 1; }
static void pk_flush(void)
{
	if (!H.wr) return;
	H.buf_seq[H.cur] = ++H.q_seq;
	H.cur = (H.cur + 1) % NBUF;
	H.wr = 0;
}
static qw_t *pk_alloc(u32 n)
{
	qw_t *p;
	if (H.wr + n > BUF_QW) pk_flush();
	check(capture_n + n <= CAP_QW, "bounded packet capture");
	p = capture + capture_n; capture_n += n; H.wr += n;
	return p;
}
static qw_t *ad_alloc(u32 n)
{
	qw_t *p = pk_alloc(n + 1);
	p[0].d[0] = HWGIF_TAG(n, 1, 0, 0, 0, 1); p[0].d[1] = GIF_REGS_AD;
	H.serial++; return p + 1;
}
static void pk_ref(const void *p, u32 n) { memcpy(pk_alloc(n), p, (size_t)n * 16); }
static void pk_ref_tail(const void *p, u32 bytes) { qw_t *q = pk_alloc(1); memset(q, 0, 16); memcpy(q, p, bytes); }
static void ring_wait(int limit) { (void)limit; H.done_seq = H.q_seq; }
static void vu_chunk_flush(void) { V.chunk_open = 0; }
#define SyncDCache(a,b) ((void)0)
void HWR_PS2_LockData(void *p) { check(p != NULL, "lock live source"); locks++; }
void HWR_PS2_UnlockData(void *p) { check(p != NULL, "unlock live source"); unlocks++; }
void HWR_PS2_FreeData(void *p) { free(p); }
void *HWR_PS2_StealData(GLMipmap_t *m, void **user) { void *p = m->data; m->data = NULL; *user = p; return p; }
void *HWR_PS2_AllocData(size_t n, void **user) { void *p = malloc(n); *user = p; return p; }
const u8 *HWR_PS2_FlatPin(const GLMipmap_t *m) { (void)m; return stream_source; }
boolean HWR_PS2_FlatStreamable(const GLMipmap_t *m)
{
	return stream_on && m->regen_kind == 2 && m->width == stream_width && m->height == stream_height;
}
boolean HWR_PS2_FlatRows(const GLMipmap_t *m, UINT32 row, UINT32 n, UINT8 *p)
{
	check(m->width == stream_width && row + n <= stream_height, "raw flat row range");
	memcpy(p, stream_source + (size_t)row * stream_width, (size_t)n * stream_width);
	stream_reads++; return 1;
}
void HWR_PS2_FlatUnpin(const u8 *p, size_t n) { (void)p; (void)n; }
static void ov_flush_all(void) { }
static u32 fb_pages(int stride, int rows, int psm32) { return (u32)(stride / 64) * (u32)((rows + (psm32 ? 31 : 63)) / (psm32 ? 32 : 64)); }
int PS2HWD_ReadVram(void *dst, unsigned int blk, unsigned int tbw, int psm, int x, int y, int w, int h)
{ (void)blk; (void)tbw; (void)x; (void)y; memset(dst, 0x5a, (size_t)w * h * (psm == PSM_CT32 ? 4 : 2)); return 0; }
static u32 tex_want(const GLMipmap_t *m, int *visible) { (void)m; *visible = 1; return wanted_level; }
#include "ps2_hw_pal.inc"
#include "ps2_hw_tex.inc"
#include "texture-registers.inc"

static u32 crc32_bytes(const void *p, size_t n)
{
	const u8 *b = p;
	u32 crc = ~0u;
	while (n--) {
		crc ^= *b++;
		for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

/* This resets only driver-owned state. The real engine objects deliberately
 * survive, as they do across SetScreenSize -> Shutdown/Init. */
static void reset_driver(void)
{
	int i;
	dma_fence(); rel_release_all(); dc_flush();
	if (H.up) tex_free_all();
	free(H.rec);
	for (i = 0; i < NSCR; i++) free(H.scr_data[i]);
	memset(&H, 0, sizeof H);
	if (!blk_owner_p) blk_owner_p = calloc(1, sizeof *blk_owner_p);
	check(blk_owner_p != NULL, "owner bitmap allocation"); memset(blk_owner, 0, sizeof blk_owner);
	H.up = 1; H.pool_base = 3872; H.pool_blocks = VRAM_BLOCKS - H.pool_base; H.clut_base = 3360;
	H.free_n = 1; H.free_range[0].start = H.pool_base; H.free_range[0].len = H.pool_blocks;
	H.rec_free = H.lru_head = H.lru_tail = H.cur_tex = NOREC;
	for (i = 0; i < NSCR; i++) H.scr_rec[i] = NOREC;
	H.fbw = H.vw = 320; H.fbh = H.vh = 224; H.fb32 = 1; H.fbpsm = PSM_CT32;
	H.pal_set = 1; H.pal_gen = 7; H.frame_no = 37;
	for (i = 0; i < 256; i++) H.pal[i] = (u32)i | ((u32)(255 - i) << 8) | ((u32)(i ^ 0x5a) << 16);
	pal_build(); batch_phase = 0; ps2hwd_dbg_flags = 0; cfg.tex_max_bytes = 0; wanted_level = 0;
#ifdef T17_NOZC
	ps2hwd_dbg_flags = HWDBG_NOZC;
#endif
	capture_n = 0; hwr_texsig = 0; stream_reads = 0; stream_on = 0;
}

static void init_mipmap(GLMipmap_t *m, u32 id, int keyed, int kind, u8 *source)
{
	memset(m, 0, sizeof *m);
	m->width = m->height = 256;
	m->format = GL_TEXFMT_P_8; m->flags = TF_WRAPXY | (keyed ? TF_CHROMAKEYED : 0);
	m->regen_kind = (u8)kind; m->regen_id = (INT32)id; m->data = source;
}

static void decode_t8(const texrec_t *r, u8 *dst)
{
	u32 pos = 0, rows = 0;
	check(r->psm == PSM_T8 && r->pnext == NOREC, "fixture is an unsplit T8 image");
	while (pos < capture_n) {
		const qw_t *q = capture + pos;
		u32 n = (u32)q[0].d[0] & 0x7fffu;
		if (n == 4 && q[0].d[1] == GIF_REGS_AD && q[1].d[1] == GSR_BITBLTBUF) {
			u32 blk = (u32)(q[1].d[0] >> 32) & 0x3fffu, psm = (u32)(q[1].d[0] >> 56) & 63u;
			u32 w = (u32)q[3].d[0], h = (u32)(q[3].d[0] >> 32), row = (u32)(q[2].d[0] >> 48);
			u32 nq = (u32)q[5].d[0] & 0x7fffu;
			check(blk == r->blk && psm == PSM_T8 && w == r->w && row + h <= r->h, "upload destination matches admitted image");
			check(nq == (w * h + 15u) / 16u && pos + 6u + nq <= capture_n, "upload source payload bounded");
			memcpy(dst + (size_t)row * w, q + 6, (size_t)w * h);
			rows += h; pos += 6 + nq;
		} else {
			check(n == 1 && q[0].d[1] == GIF_REGS_AD && q[1].d[1] == GSR_TEXFLUSH, "only transfer flushes accompany image");
			pos += 2;
		}
	}
	check(rows == r->h, "every stored image row transferred");
}

static void inspect(const char *case_name, const char *step, GLMipmap_t *m, int ri, const u8 *image, const u8 *expected)
{
	const texrec_t *r = &H.rec[ri];
	const u32 count = (u32)r->w * r->h;
	u32 entries[256], *actual = malloc((size_t)count * 4), *want = malloc((size_t)count * 4);
	u32 ff = 0, bad = 0, cbp, cc, expected_cc, start = capture_n;
	u64 t0, t1, clamp;
	check(actual && want, "logical RGBA oracle allocation");
	check(!memcmp(image, expected, count), "complete uploaded index bytes equal independent level oracle");
	cbp = clut_get(r->clut == 2 ? CK_KEYED : CK_OPAQUE, 0, 0);
	if (H.st.clut_uploads) {
		/* Inspect the actual CSM1 upload image, not the helper's return value. */
		int found = 0;
		for (u32 p = start; p + 70 <= capture_n; p++) {
			const qw_t *q = capture + p;
			if ((q[0].d[0] & 0x7fffu) == 4 && q[0].d[1] == GIF_REGS_AD && q[1].d[1] == GSR_BITBLTBUF
				&& ((u32)(q[1].d[0] >> 32) & 0x3fffu) == cbp && q[3].d[0] == (16ull | (16ull << 32))) {
				const u32 *swizzled = q[6].w;
				for (u32 i = 0; i < 256; i++) {
					u32 slot = (i & ~0x18u) | ((i & 8u) << 1) | ((i & 16u) >> 1);
					entries[i] = swizzled[slot];
				}
				found = 1; break;
			}
		}
		/* A cache hit has no transfer. Its contents are separately checked on
		 * every fresh generation and covered by the CLUT lifetime cases. */
		if (!found) clut_entries(r->clut == 2 ? CK_KEYED : CK_OPAQUE, 0, 0, entries);
	} else check(0, "CLUT image exists");
	for (u32 i = 0; i < count; i++) {
		actual[i] = entries[image[i]];
		want[i] = H.pal[expected[i]] | (((m->flags & TF_CHROMAKEYED) && expected[i] == 255 ? 0u : 128u) << 24);
		ff += expected[i] == 255;
		bad += actual[i] != want[i];
	}
	tex_regs(r, 0, 0, 1, cbp, 0, &t0, &t1, &clamp);
	check(((t0 >> 37) & 0x3fffu) == cbp && ((t0 >> 20) & 63u) == PSM_T8, "TEX0 uses selected CLUT and T8 image");
	check(H.clut_loaded == cbp && (t1 & 0x60u) == 0x60u, "CLUT load tracking and requested bilinear mode preserved");
	cc = crc32_bytes(actual, (size_t)count * 4); expected_cc = crc32_bytes(want, (size_t)count * 4);
	printf("T17 case=%s step=%s w=%u h=%u canonical=%u has255=%u clut=%u ff=%u bad=%u index_crc=%08x rgba_crc=%08x expected_rgba_crc=%08x cbp=%u tex0=%016llx\n",
		case_name, step, (unsigned)r->w, (unsigned)r->h, (unsigned)m->ps2_h255, (unsigned)r->has255, (unsigned)r->clut,
		(unsigned)ff, (unsigned)bad, (unsigned)crc32_bytes(image, count), (unsigned)cc, (unsigned)expected_cc,
		(unsigned)cbp, (unsigned long long)t0);
	if (bad) rgba_bad++;
	if (m->ps2_h255 == 1 && scan_has_ff(stream_source, stream_width * stream_height)) poisoned++;
	check(m->ps2_h255 <= 2, "classification remains in original two-bit domain");
	observations++; free(actual); free(want);
}

static int upload(GLMipmap_t *m, const u8 *expected, u8 *image)
{
	int ri;
	capture_n = 0;
	ri = tex_upload(m); check(ri != NOREC, "actual texture upload succeeds");
	decode_t8(&H.rec[ri], image);
	check(!memcmp(image, expected, (size_t)H.rec[ri].w * H.rec[ri].h), "all upload bytes exactly match reference");
	return ri;
}

static void mixed_sequence(int initial_keyed, int kind, int restart_first, u32 id)
{
	GLMipmap_t original, keyed, *first;
	u8 *full = malloc(65536), *level1 = malloc(16384), *level2 = malloc(4096), *image = malloc(65536);
	char name[64]; int ri, vi;
	check(full && level1 && level2 && image, "sequence backing allocation");
	reset_driver();
	for (u32 y = 0; y < 256; y++) for (u32 x = 0; x < 256; x++) full[y * 256 + x] = (x % 8 == 0 && y % 8 == 0) ? 255 : (u8)(40 + ((x + y) % 40));
	stream_source = full; stream_width = stream_height = 256; stream_on = kind == 2;
	dec_level_ref(full, 256, 256, 1, level1); dec_level_ref(level1, 128, 128, 1, level2);
	check(scan_has_ff(full, 65536) && !scan_has_ff(level1, 16384) && !scan_has_ff(level2, 4096), "real decimator loses thin holes at coarser levels");
	init_mipmap(&original, id, 0, kind, NULL); init_mipmap(&keyed, id, 1, kind, NULL);
	keyed.ps2_twin = &original; original.nextcolormap = &keyed;
	first = initial_keyed ? &keyed : &original;
	snprintf(name, sizeof name, "%s-%s-%s", kind == 2 ? "flat" : "map", initial_keyed ? "keyed" : "plain", restart_first ? "restart" : "direct");
	/* A level made by the real raw-flat streaming path leaves canonical
	 * classification unknown. The map case starts with the same valid cache
	 * state shared from a streamed flat only as a general dc contract probe. */
	wanted_level = 1;
	if (kind == 1) {
		dcent_t *e = dc_slot(16384); check(e != NULL, "cached mip fixture slot");
		check(HWR_PS2_AllocData(16384, (void **)(void *)&e->p) != NULL, "cached mip fixture storage");
		memcpy(e->p, level1, 16384); e->key = dc_key(first); e->bytes = 16384; e->w = e->h = 128; e->lvl = 1; e->stamp = ++dc_seq; dc_bytes += e->bytes;
	}
	ri = upload(first, level1, image); inspect(name, "cached-L1", first, ri, image, level1);
	check(first->ps2_h255 == 0, "direct cached level does not classify canonical source");
	if (kind == 2) check(stream_reads == 4, "real raw flat streaming made level one in four bands");
	tex_drop(ri, 0);
	wanted_level = 2;
	ri = upload(first, level2, image); inspect(name, "derived-L2", first, ri, image, level2);
	check(dc_find(dc_key(first), 64, 64, 2) != NULL, "actual finer-source conversion creates requested coarser cache");
	tex_drop(ri, 0);
	if (restart_first) {
		reset_driver();
		check(!original.downloaded && !keyed.downloaded, "driver restart clears both engine handles");
		stream_source = full; stream_width = stream_height = 256;
	}
	wanted_level = 0; original.data = malloc(65536); check(original.data != NULL, "regenerated engine full source");
	memcpy(original.data, full, 65536);
	ri = upload(&original, full, image); inspect(name, "full-plain", &original, ri, image, full);
	vi = tex_view(&keyed, ri); check(vi != NOREC && H.rec[vi].share == ri, "actual keyed twin shares existing full image");
	inspect(name, "full-keyed-view", &keyed, vi, image, full);
	tex_drop(ri, 0);
	if (!dc_find(dc_key(&keyed), 256, 256, 0)) {
		keyed.data = malloc(65536); check(keyed.data != NULL, "regenerated keyed engine source");
		memcpy(keyed.data, full, 65536);
	}
	ri = upload(&keyed, full, image); inspect(name, "full-keyed-upload", &keyed, ri, image, full);
	tex_drop(ri, 0); free(original.data); free(keyed.data); original.data = keyed.data = NULL;
	reset_driver();
	stream_source = NULL; stream_width = stream_height = 0;
	free(image); free(level2); free(level1); free(full);
}

static void canonical_and_clut_lifetimes(void)
{
	GLMipmap_t m;
	u8 *full = malloc(65536), *image = malloc(65536);
	int ri; u64 t0, t1, clamp; u32 cbp, before;
	check(full && image, "canonical lifetime allocation"); reset_driver();
	memset(full, 41, 65536); full[0] = 255;
	stream_source = full; stream_width = stream_height = 256;
	init_mipmap(&m, 99, 1, 1, malloc(65536)); check(m.data != NULL, "canonical engine source allocation");
	memcpy(m.data, full, 65536);
	ri = upload(&m, full, image); inspect("canonical", "full", &m, ri, image, full);
	check(m.ps2_h255 == 2 && H.rec[ri].has255 && H.rec[ri].clut == 2, "full source caches correct positive classification");
	cbp = clut_get(CK_KEYED, 0, 0); H.clut_loaded = cbp; before = H.clut_epoch;
	clut_build_into(cbp, CK_KEYED, 0, 0);
	check(H.clut_loaded == 0 && H.clut_epoch == before + 1, "rebuilding loaded CLUT invalidates load and row epoch");
	tex_regs(&H.rec[ri], 0, 0, 1, cbp, 0, &t0, &t1, &clamp);
	check(((t0 >> 61) & 7u) == 1, "rewritten CLUT requests hardware reload");
	before = H.clut_epoch; H.pal_gen++; clut_get(CK_KEYED, 0, 0);
	check(H.clut_epoch == before + 2 && H.clut_loaded == 0, "palette change rebuilds both fixed CLUTs and invalidates load");
	tex_drop(ri, 0); free(m.data); m.data = NULL; reset_driver();
	check(m.ps2_h255 == 2, "canonical positive survives driver restart without reclassification");
	stream_source = NULL; stream_width = stream_height = 0; free(image); free(full);
}

int main(void)
{
	capture = calloc(CAP_QW, sizeof *capture); check(capture != NULL, "capture allocated");
	for (int kind = 1; kind <= 2; kind++) for (int keyed = 0; keyed <= 1; keyed++) for (int restart = 0; restart <= 1; restart++)
		mixed_sequence(keyed, kind, restart, (u32)(kind * 10 + keyed * 2 + restart));
	canonical_and_clut_lifetimes(); reset_driver(); free(H.rec); free(blk_owner_p); free(capture); free(scratch);
	printf("T17 COMPLETE observations=%u rgba_bad=%u poisoned=%u checks=%llu\n", observations, rgba_bad, poisoned, checks);
	return 0;
}
