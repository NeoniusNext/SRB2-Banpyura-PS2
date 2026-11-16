/* Actual production cache/residency routines with a host CLUT transfer sink. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef union { u64 d[2]; u32 w[4]; } qw_t;

/* PRODUCTION_DEFINITIONS */

#define PR_TBL 64
static struct
{
	u32 serial, clut_epoch, clut_loaded, clut_base, clut_tick, pal_gen;
	clutent_t clut_ent[CLUT_SLOTS];
	struct { u32 clut_uploads; } st;
} H;
static struct { u32 vr_blk_slow, vr_calls, vr_same, vr_slow, vr_rows; } G;
static struct { u32 n; } OV;
static u8 lt_gen[PR_TBL];
static u8 prow_slot[PR_TBL][2][32];
typedef struct { u32 epoch, ltgen, palgen; int lo, hi; } prowv_t;
static prowv_t prowv[PR_TBL][2];
typedef struct { u32 hash; u8 variant; } rampspec_t;
static int ps2hwd_dbg_flags;
static qw_t ring[32768];
static u32 ring_used, ring_applied;
static u32 image[CLUT_SLOTS][256];
static u32 checks, cases, wrong_certifications, restored_rows, palette_bytes;
static u32 wrong_certified_entries;

static void check(int yes, const char *message)
{
	checks++;
	if (!yes)
	{
		fprintf(stderr, "CLUT epoch FAIL case %u: %s\n", cases, message);
		exit(1);
	}
}

static qw_t *pk_alloc(u32 count)
{
	check(count <= sizeof ring / sizeof ring[0] - ring_used, "bounded ring capacity");
	qw_t *out = ring + ring_used;
	ring_used += count;
	return out;
}

static qw_t *ad_alloc(u32 count)
{
	/* Only an unused fixed-palette refresh uses this boundary in this test. */
	(void)count;
	fprintf(stderr, "Unexpected fixed-palette refresh\n");
	exit(1);
}

static void ov_flush_all(void) { OV.n = 0; }
#define ov_flush_pending ov_flush_all

/* Distinct images make stale residency observable in emitted GS payloads. */
static u32 light_pixel(u32 kind, u32 param, u32 keyed, u32 at)
{
	return 0x80000000u ^ kind * 0x10001u ^ param * 0x45d9f3bu ^ keyed * 0x202020u ^ at;
}

static void clut_entries(u32 kind, u32 param, u32 keyed, u32 *out)
{
	for (u32 i = 0; i < 256; i++) out[i] = light_pixel(kind, param, keyed, i);
}

static u32 spec_hash(const rampspec_t *spec) { return spec->hash; }

static void ramp_entries(const rampspec_t *spec, u32 *out)
{
	for (u32 i = 0; i < 256; i++) out[i] = 0xa5000000u ^ spec->hash ^ (u32)spec->variant << 16 ^ i;
}

/* PRODUCTION_FUNCTIONS */

/* Apply the actual CT32 IMAGE bytes, with independent transfer validation. */
static void apply_packets(void)
{
	while (ring_applied < ring_used)
	{
		const qw_t *p = ring + ring_applied;
		check(ring_used - ring_applied >= 72, "complete CLUT transfer");
		check(p[0].d[0] == (4ull | 1ull << 60) && p[0].d[1] == 0xe,
			"four-register CLUT transfer header");
		check(p[1].d[1] == GSR_BITBLTBUF && p[2].d[1] == GSR_TRXPOS
			&& p[3].d[1] == GSR_TRXREG && p[4].d[1] == GSR_TRXDIR,
			"CLUT transfer register order");
		check(p[2].d[0] == 0 && p[3].d[0] == (16ull | 16ull << 32)
			&& p[4].d[0] == 0, "complete 16x16 source image");
		check(p[5].d[0] == (64ull | 1ull << 15 | 2ull << 58), "256-entry IMAGE payload");
		check(p[71].d[1] == GSR_TEXFLUSH, "TEXFLUSH follows CLUT upload");
		const u32 blk = (u32)(p[1].d[0] >> 32) & 0x3fffu;
		check(blk >= H.clut_base && (blk - H.clut_base) % CLUT_BLOCKS == 0,
			"destination is an aligned CLUT slot");
		const u32 slot = (blk - H.clut_base) / CLUT_BLOCKS;
		check(slot < CLUT_SLOTS, "destination CLUT slot is in bounds");
		memcpy(image[slot], &p[6], sizeof image[slot]);
		palette_bytes += sizeof image[slot];
		ring_applied += 72;
	}
}

static u32 permute(u32 i)
{
	return (i & ~0x18u) | ((i & 8u) << 1) | ((i & 16u) >> 1);
}

static u32 wrong_row_entries(u32 tbl, int keyed, u32 row)
{
	const u32 param = tbl | (u32)lt_gen[tbl] << 10 | row << 18;
	const u32 slot = prow_slot[tbl][keyed][row] - 1u;
	u32 wrong = 0;
	check(slot < CLUT_SLOTS, "light row still names a physical CLUT slot");
	for (u32 i = 0; i < 256; i++)
		wrong += image[slot][ps2hwd_dbg_flags & 16 ? i : permute(i)]
			!= light_pixel(CK_LIGHT, param, (u32)keyed, i);
	return wrong;
}

static int correct_row(u32 tbl, int keyed, u32 row)
{
	const u32 param = tbl | (u32)lt_gen[tbl] << 10 | row << 18;
	const u64 expected_key = CK_LIGHT | (u64)keyed << 8 | (u64)param << 16;
	const u32 slot = prow_slot[tbl][keyed][row] - 1u;
	if (slot >= CLUT_SLOTS || H.clut_ent[slot].key != expected_key) return 0;
	for (u32 i = 0; i < 256; i++)
		if (image[slot][ps2hwd_dbg_flags & 16 ? i : permute(i)]
			!= light_pixel(CK_LIGHT, param, (u32)keyed, i)) return 0;
	return 1;
}

static void reset(u32 seed)
{
	memset(&H, 0, sizeof H);
	memset(&G, 0, sizeof G);
	memset(&OV, 0, sizeof OV);
	memset(ring, 0, sizeof ring);
	memset(image, 0, sizeof image);
	memset(prow_slot, 0, sizeof prow_slot);
	memset(prowv, 0, sizeof prowv);
	memset(clut_hint, 0, sizeof clut_hint);
	for (u32 i = 0; i < PR_TBL; i++)
	{
		lt_gen[i] = (u8)(seed * 7u + i * 3u);
		for (u32 k = 0; k < 2; k++) prowv[i][k].hi = -1;
	}
	H.clut_base = 1024;
	H.pal_gen = seed + 1;
	H.clut_epoch = seed & 1u ? UINT32_MAX - 121u : 37u;
	ring_used = ring_applied = 0;
	ps2hwd_dbg_flags = seed & 2u ? 16 : 0;
}

static void scenario(u32 seed, int pressured)
{
	const u32 tbl = 1u + seed % (PR_TBL - 1u), row = seed % 32u;
	const int keyed = (int)((seed >> 5) & 1u);
	const rampspec_t spec = {0x12345678u + seed, (u8)(1u + seed % 5u)};
	reset(seed);
	if (pressured)
	{
		/* The light row is initially the first dynamic slot. */
		prow_blk(tbl, keyed, row);
		for (u32 n = 0; n < CLUT_SLOTS - CLUT_FIXED - 1u; n++)
			clut_get(CK_TINT, 1000u + n, 0);
	}
	prow_ensure(tbl, keyed, (int)row, (int)row);
	apply_packets();
	check(correct_row(tbl, keyed, row), "initial certified light row is present");
	const u32 old_slot = prow_slot[tbl][keyed][row] - 1u;
	if (pressured)
	{
		/* Retouch other images without changing the epoch; the row becomes LRU. */
		for (u32 n = 0; n < CLUT_SLOTS - CLUT_FIXED - 1u; n++)
			clut_get(CK_TINT, 1000u + n, 0);
	}
	const u32 epoch = H.clut_epoch, uploads = H.st.clut_uploads;
	const u32 ramp_blk = ramp_clut(&spec);
	apply_packets();
	check(H.st.clut_uploads == uploads + 1u, "new ramp emits exactly one image");
	check(H.clut_epoch == epoch + (u32)EXPECT_REPAIR, "ramp epoch change matches source variant");
	if (pressured)
	{
		check(ramp_blk == H.clut_base + old_slot * CLUT_BLOCKS, "ramp replaces the certified row");
		check(!correct_row(tbl, keyed, row), "replaced row has the distinct ramp image");
		check(wrong_row_entries(tbl, keyed, row) > 0, "emitted ramp payload differs from light row");
		for (u32 i = 0; i < 256; i++)
			check(image[old_slot][ps2hwd_dbg_flags & 16 ? i : permute(i)]
				== (0xa5000000u ^ spec.hash ^ (u32)spec.variant << 16 ^ i),
				"slot contains the complete emitted ramp payload");
	}
	const u32 same_before = G.vr_same;
	prow_ensure(tbl, keyed, (int)row, (int)row);
	apply_packets();
	if (pressured && !EXPECT_REPAIR)
	{
		check(G.vr_same == same_before + 1u, "old source falsely takes residency fast path");
		check(!correct_row(tbl, keyed, row), "old source certifies wrong slot contents");
		wrong_certifications++;
		wrong_certified_entries += wrong_row_entries(tbl, keyed, row);
	}
	else
	{
		check(correct_row(tbl, keyed, row), "certified row has the original light image");
		if (pressured) restored_rows++;
	}
	/* Cache hits must keep the epoch and image bytes stable. */
	const u32 hit_epoch = H.clut_epoch, hit_uploads = H.st.clut_uploads;
	ramp_clut(&spec);
	check(H.clut_epoch == hit_epoch && H.st.clut_uploads == hit_uploads,
		"cached ramp does not rebuild or invalidate");
	const u32 hit_same = G.vr_same;
	prow_ensure(tbl, keyed, (int)row, (int)row);
	check(G.vr_same == hit_same + 1u && H.clut_epoch == hit_epoch
		&& H.st.clut_uploads == hit_uploads, "unchanged row promise retains fast path");
	if (EXPECT_REPAIR || !pressured)
		check(correct_row(tbl, keyed, row), "subsequent certified row remains exact");
	cases++;
}

int main(void)
{
	for (u32 seed = 0; seed < 256; seed++)
	{
		scenario(seed, 0);
		scenario(seed, 1);
	}
	printf("{\"cases\":%u,\"checks\":%u,\"wrong_certifications\":%u,"
		"\"wrong_certified_entries\":%u,\"restored_rows\":%u,\"uploaded_palette_bytes\":%u}\n",
		cases, checks, wrong_certifications, wrong_certified_entries, restored_rows, palette_bytes);
	return 0;
}
