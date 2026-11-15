/* Literal isolated production allocation, register builders, and two clear functions.
 * The host substitutes ring retirement/VU closure and interprets the emitted GIF.
 * The coverage oracle is the documented sprite top/left-inclusive rule, not a GS.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"
#include "ps2_hwd_dbg.h"
#define CONS_WARNING 1
#define CONS_ERROR 2
#define CONS_Alert(level, ...) ((void)(level))
#include "ps2_hw_priv.inc"
#include "ps2_hw_regs.inc"

enum { MAX_STREAM = 512 };
typedef struct { qw_t guard0, words[BUF_QW], guard1; } guarded_ring_t;
static guarded_ring_t rings[NBUF];
static qw_t stream[MAX_STREAM], reference[MAX_STREAM];
static __typeof__(H) reference_state;
static unsigned stream_n, copied_from, flushes, vu_closes, prefix, allocations;
static unsigned long long checks, cases, widths, rollovers, pixels;
static struct { int chunk_open; } V;
static void pk_flush(void);
static void vu_chunk_flush(void);
#include "literal-allocation.inc"
#include "baseline-clear.inc"
#include "candidate-clear.inc"

static void check(int yes, const char *message)
{
	checks++;
	if (!yes) { fprintf(stderr, "clear proof failed: %s (check %llu, case %llu)\n", message, checks, cases); exit(1); }
}

static void append(void)
{
	u32 n = H.wr - copied_from;
	check(H.wr <= BUF_QW && copied_from <= H.wr, "ring read bounds");
	check(stream_n + n <= MAX_STREAM, "stream capture bounds");
	memcpy(stream + stream_n, H.ring[H.cur] + copied_from, (size_t)n * sizeof(qw_t));
	stream_n += n;
	copied_from = H.wr;
}

static void pk_flush(void)
{
	append();
	flushes++;
	H.cur = (H.cur + 1) % NBUF;
	H.wr = 1;
	copied_from = 1;
	/* Retired buffers are immediately unavailable for further writes. The host
	 * copies them before reuse, which exposes stale-packet writes after rollover. */
}

static void vu_chunk_flush(void)
{
	qw_t *p;
	V.chunk_open = 0;
	vu_closes++;
	/* Model an ordered 7-qword VU closure consuming ring capacity before clear.
	 * This is an ownership boundary fixture, not VIF or VU emulation. */
	p = pk_alloc(7);
	memset(p, 0x3c, 7 * sizeof *p);
	prefix = 7;
}

static void seed(int width, int height, int fb32, int start, int vu)
{
	int i;
	memset(&H, 0, sizeof H);
	H.vw = width; H.vh = height; H.fbw = (width + 63) & ~63; H.fbh = height;
	H.fb32 = fb32; H.fbpsm = fb32 ? PSM_CT32 : PSM_CT16S;
	H.fb_addr[0] = 0; H.fb_addr[1] = 320 * 256 * 4; H.z_addr = 2 * 320 * 256 * 4;
	H.gsr.valid = 0x3ff; H.serial = 43; H.frame_no = 117;
	H.pool_base = 4352; H.pool_blocks = VRAM_BLOCKS - H.pool_base; H.used_blocks = 768;
	H.plan_pct = 69; H.plan_reserve = 3980; H.unplanned_cur = 3483; H.thrash_blk = 512;
	H.cur = 0; H.wr = (u32)start;
	for (i = 0; i < NBUF; i++)
	{
		memset(&rings[i], 0xa5, sizeof rings[i]);
		H.ring[i] = rings[i].words;
	}
	stream_n = flushes = vu_closes = prefix = allocations = 0; copied_from = H.wr;
	V.chunk_open = vu;
}

static void guards(void)
{
	unsigned i, j;
	for (i = 0; i < NBUF; i++) for (j = 0; j < 4; j++)
		check(rings[i].guard0.w[j] == 0xa5a5a5a5u && rings[i].guard1.w[j] == 0xa5a5a5a5u, "ring red zones");
}

static void canonical_state(void)
{
	/* Only ring position changes with packet length. The two paths must leave
	 * every other production state byte identical, including all policy inputs. */
	H.cur = 0; H.wr = 0;
}

static u32 unpack_rgba(const qw_t *p)
{
	check((p->d[0] & 0xffffff00ffffff00ull) == 0 && (p->d[1] & 0xffffff00ffffff00ull) == 0, "packed RGBA reserved bits");
	return (u32)p->d[0] | ((u32)(p->d[0] >> 32) << 8) | ((u32)p->d[1] << 16) | ((u32)(p->d[1] >> 32) << 24);
}

static unsigned sprite_words(unsigned width)
{
	unsigned n = (width + 31u) / 32u;
	return COMPACT ? 3u + n : 1u + 4u * n;
}

static void decode(unsigned n, unsigned pre, int width, int height, int colour, int depth, int buf, u32 rgba, int raster)
{
	unsigned i, strip_n = ((unsigned)width + 31u) / 32u, count[1026] = {0};
	const qw_t *ad = stream + pre, *tag = ad + 9;
	u64 expected_frame = (u64)(H.fb_addr[buf] / 8192) | ((u64)(H.fbw / 64) << 16) | ((u64)H.fbpsm << 24) | ((u64)(colour ? 0 : 0xffffffffu) << 32);
	u64 expected_z = (u64)(H.z_addr / 8192) | (1ull << 24) | ((u64)!depth << 32);
	u64 expected_scissor = ((u64)(width - 1) << 16) | ((u64)(height - 1) << 48);
	static const u32 regs[8] = {0x4c, 0x4e, 0x40, 0x47, 0x42, 0x14, 0x08, 0x3f};
	u64 values[8] = {expected_frame, expected_z, expected_scissor, 3ull << 16, 0, 0, 0, 0};
	check(n == pre + 9 + sprite_words((unsigned)width), "complete exact packet length");
	check(ad[0].d[0] == (8ull | (1ull << 15) | (1ull << 60)) && ad[0].d[1] == 0xe, "AD GIF tag");
	for (i = 0; i < 8; i++) check(ad[i + 1].d[1] == regs[i] && ad[i + 1].d[0] == values[i], "literal clear state writes");
	if (COMPACT)
	{
		check(tag->d[0] == (1ull | (1ull << 46) | (6ull << 47) | (1ull << 60)) && tag->d[1] == 1, "packed PRIM/RGBAQ tag and continuation");
		check(unpack_rgba(tag + 1) == rgba, "constant packed RGBAQ value");
		check(tag[2].d[0] == ((u64)(2 * strip_n) | (1ull << 15) | (1ull << 58) | (1ull << 60)), "REGLIST XYZ2 GIF tag/count/EOP");
		check(tag[2].d[1] == 5, "REGLIST XYZ2 register descriptor");
	}
	else
	{
		check(tag->d[0] == ((u64)(2 * strip_n) | (1ull << 15) | (1ull << 46) | (6ull << 47) | (2ull << 60)), "sprite GIF tag flags/primitive/count");
		check(tag->d[1] == 0x51, "packed RGBAQ/XYZ2 register order");
	}
	for (i = 0; i < strip_n; i++)
	{
		qw_t decoded[4], *p;
		if (COMPACT)
		{
			const qw_t *src = tag + 3 + i;
			check((src->d[0] >> 32) == 0 && (src->d[1] >> 32) == 0, "REGLIST Z32 exactly zero");
			decoded[0] = decoded[2] = tag[1];
			decoded[1].d[0] = (u64)(src->d[0] & 0xffffu) | ((u64)((src->d[0] >> 16) & 0xffffu) << 32);
			decoded[3].d[0] = (u64)(src->d[1] & 0xffffu) | ((u64)((src->d[1] >> 16) & 0xffffu) << 32);
			decoded[1].d[1] = decoded[3].d[1] = 0;
			p = decoded;
		}
		else p = stream + pre + 10 + 4 * i;
		int x0 = (int)((u32)p[1].d[0] / 16u) - 2048;
		int y0 = (int)((u32)(p[1].d[0] >> 32) / 16u) - 2048;
		int x1 = (int)((u32)p[3].d[0] / 16u) - 2048;
		int y1 = (int)((u32)(p[3].d[0] >> 32) / 16u) - 2048;
		unsigned x;
		check(unpack_rgba(&p[0]) == rgba && memcmp(&p[0], &p[2], sizeof *p) == 0, "identical RGBA on both vertices");
		check((p[1].d[0] & 0xfull) == 0 && ((p[1].d[0] >> 32) & 0xfull) == 0 && (p[3].d[0] & 0xfull) == 0 && ((p[3].d[0] >> 32) & 0xfull) == 0, "exact integer XY coordinates");
		check(p[1].d[1] == 0 && p[3].d[1] == 0, "Z zero and no ADC kick suppression");
		check(x0 == (int)(32 * i) && x1 == (int)(i + 1) * 32 - ((i + 1) * 32 > (unsigned)width ? (int)((i + 1) * 32 - (unsigned)width) : 0), "32-pixel strips with clamped final edge");
		check(y0 == 0 && y1 == height && x0 < x1 && x0 >= 0 && x1 <= width, "full-height nonempty exact rectangle");
		for (x = (unsigned)x0; x < (unsigned)x1; x++) count[x + 1]++;
		if (i + 1 == strip_n)
		{
			check(memcmp(&p[3], reference + pre + 13, sizeof(qw_t)) == 0, "final XYZ2 equals baseline");
			check(memcmp(&p[2], reference + pre + 12, sizeof(qw_t)) == 0, "final packed RGBAQ equals baseline");
		}
	}
	check(count[0] == 0 && count[width + 1] == 0, "outside rectangle untouched");
	for (i = 1; i <= (unsigned)width; i++) check(count[i] == 1, "each column covered exactly once");
	/* Final vertex and colour/Q update semantics equal the old single sprite.
	 * PACKED RGBAQ uses the same GIF Q source in both paths; no ST/Q is added. */
	if (raster)
	{
		unsigned x, y, stride = (unsigned)H.fbw;
		for (y = 0; y <= (unsigned)height; y++) for (x = 0; x <= stride; x++)
		{
			u32 old_c = (x * 2654435761u) ^ (y * 2246822519u) ^ 0x86947ba3u;
			u32 old_z = old_c ^ 0xf1d53289u;
			int inside = x < (unsigned)width && y < (unsigned)height;
			u32 converted = H.fb32 ? rgba : ((rgba & 0xf8u) >> 3) | ((rgba & 0xf800u) >> 6) | ((rgba & 0xf80000u) >> 9) | ((rgba & 0x80000000u) >> 16);
			u32 want_c = inside && colour ? converted : old_c;
			u32 want_z = inside && depth ? old_z & 0xff000000u : old_z;
			int hits = x < (unsigned)width && y < (unsigned)height ? (int)count[x + 1] : 0;
			u32 got_c = hits && colour ? converted : old_c;
			u32 got_z = hits && depth ? old_z & 0xff000000u : old_z;
			check(got_c == want_c && got_z == want_z, "per-pixel colour/depth mask and padded sentinel preservation");
			pixels++;
		}
	}
}

static void run_case(int width, int height, int fb32, int colour, int depth, int buf, u32 rgba, int start, int vu, int raster)
{
	unsigned expected_flushes;
	cases++;
	seed(width, height, fb32, start, vu);
	baseline_clear(buf, colour, depth, rgba);
	append(); guards();
	check(stream_n == prefix + 14, "baseline packet size");
	memcpy(reference, stream, stream_n * sizeof *stream);
	canonical_state(); reference_state = H;
	seed(width, height, fb32, start, vu);
	candidate_clear(buf, colour, depth, rgba);
	append(); guards();
	check(vu_closes == (unsigned)vu && !V.chunk_open, "VU closure before scalar stream");
	check(H.gsr.valid == 0 && H.serial == 44, "GS state invalidation and serial preserved");
	check(memcmp(stream + prefix, reference + prefix, 9 * sizeof *stream) == 0, "8 AD register writes and tag exactly equal baseline");
	check(stream_n - prefix <= 9u + sprite_words(1024u), "max clear payload bounded");
	expected_flushes = 0;
	{
		unsigned wr = (unsigned)start, amount[] = {7, 9, sprite_words((unsigned)width)}, k;
		for (k = vu ? 0u : 1u; k < 3; k++) { if (wr + amount[k] > BUF_QW) { expected_flushes++; wr = 1; } wr += amount[k]; }
		check(H.wr == wr && flushes == expected_flushes, "literal pk_alloc rollover/capacity behavior");
	}
	rollovers += flushes;
#if MUTATION == 1
	stream[prefix + (COMPACT ? 12u : 11u)].d[0] += 16u; /* missing first column */
#elif MUTATION == 2
	if (COMPACT) stream[prefix + 11 + (((unsigned)width + 31u) / 32u)].d[1] += 16u;
	else stream[prefix + 10 + 4 * (((unsigned)width + 31u) / 32u) - 1].d[0] += 16u; /* final edge exceeds viewport */
#elif MUTATION == 3
	stream[prefix + 2].d[0] ^= 1ull << 32; /* wrong depth mask */
#elif MUTATION == 4
	stream[prefix + (COMPACT ? 10u : 12u)].d[0] ^= 1u; /* wrong constant/vertex colour */
#elif MUTATION == 5
	stream[prefix + (COMPACT ? 11u : 9u)].d[0] ^= 1u; /* incomplete NLOOP */
#elif MUTATION == 6
	H.plan_reserve++; /* hidden policy-state mutation */
#endif
	decode(stream_n, prefix, width, height, colour, depth, buf, rgba, raster);
	canonical_state();
	check(memcmp(&H, &reference_state, sizeof H) == 0, "all non-ring production state bytes equal baseline");
}

int main(void)
{
	static const int heights[] = {1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 200, 224, 256, 448, 480, 512, 1023, 1024};
	static const int modes[][2] = {{320,224},{320,256},{640,448},{640,480},{640,512},{319,223},{641,479}};
	unsigned w, h, format, flags, buf, i, j;
	for (w = 1; w <= 1024; w++)
	{
		widths++;
		for (h = 0; h < sizeof heights / sizeof heights[0]; h++) for (format = 0; format < 2; format++) for (flags = 0; flags < 4; flags++) for (buf = 0; buf < 2; buf++)
			run_case((int)w, heights[h], (int)format, flags & 1u, (flags >> 1) & 1u, (int)buf, 0x806480bfu ^ (w * 1610612741u), 1, (w + h + flags + buf) & 1u, 0);
	}
	/* Every legal ring starting position at widths surrounding strip/page edges.
	 * State and sprite packets may cross ring boundaries independently. */
	for (i = 0; i < 7; i++) for (j = 1; j <= BUF_QW; j++)
		run_case(modes[i][0], modes[i][1], i & 1u, 1, 1, i & 1u, 0x80000000u, (int)j, j & 1u, 0);
	for (i = 0; i < sizeof modes / sizeof modes[0]; i++) for (format = 0; format < 2; format++) for (flags = 0; flags < 4; flags++)
		run_case(modes[i][0], modes[i][1], (int)format, flags & 1u, (flags >> 1) & 1u, 0, 0x804080bfu, BUF_QW - 10, 1, 1);
	for (i = 0; i < 256; i++) for (j = 0; j < 4; j++)
		run_case(65, 17, i & 1u, 1, 1, 0, i << (j * 8), BUF_QW - 18, 0, 0);
	printf("{\"cases\":%llu,\"checks\":%llu,\"widths_exhausted\":%llu,\"ring_rollovers\":%llu,\"raster_pixels\":%llu,\"strip_width\":32,\"max_sprite_qwords\":%u,\"baseline_sprite_qwords\":5,\"compact\":%d}\n", cases, checks, widths, rollovers, pixels, sprite_words(1024u), COMPACT);
	return 0;
}
