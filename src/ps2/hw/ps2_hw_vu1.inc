// PS2 GS hardware renderer driver: polygons drawn by the VU1 program (included by ps2_hwd.c after ps2_hw_fast.inc/ps2_hw_draw.inc). PS2-HW-45 (OPT9, HG), v2: PS2-HW-100 (OPT11, VU).
//
// The EE does not transform, clip, cut at the steps of the light staircase or pack the polygons of a batch (PS2HWD_DrawFans) any more: it copies the vertices as they
// are (x, y, z, 1 / s, t) into a chunk of VU1 data, the VIF unpacks the chunk into the VU1 data memory, and the program vu1/ps2_hw_vu1.vsm transforms, clips, divides,
// (palette lighting) cuts the polygon where the colormap row changes, loads the CLUT of the row of every piece and kicks the GIF packet (see there for the formats).
// The chunk sits in the ring buffer between the runs of GIF data: UNPACK tag, data, zero length MSCAL tag (the VIF codes live in the tags of the DMA chain,
// ps2_hw_vif.inc). What stays on the EE decides whether a polygon may take the shortcut: one pass, no texture repeat cut of the plan, at most 12 vertices, fewer
// than UV_EXTENT texels in one primitive. Everything else goes the old way (emit_poly); the stream keeps the order of the calls.
//
// Real hardware, not only PCSX2 (what the code below keeps):
//  * the VU1 data memory (16 KB) holds the constants (once per plan, UNPACK behind FLUSHE: the previous program is done with them), the TEX0 of the colormap rows
//    of the plan, the two input buffers (double buffered by the VIF: BASE / OFFSET / TOPS), two output buffers (the VU writes the one the GIF is not reading: the
//    next XGKICK waits for the transfer of the previous one) and the vertex lists. Nothing the VU needs survives in a VU register between two runs: the output
//    buffer index and the row the GS holds live in the data memory.
//  * every DIRECT run (GS state, uploads) is behind FLUSH (the VU1 packets are in the GIF before it), and every MSCAL is behind FLUSH too: the A+D state packets
//    of PATH2 are executed by the GS before the VU1 packets (PATH1 has the higher priority at a packet boundary) start.
//  * the data the VIF reads are written back (SyncDCache) when the buffer is queued (pk_flush), and referenced engine data (textures) by tex_upload_ref.

#ifdef __mips__
extern const u8 VU1Draw_CodeStart[] __attribute__((aligned(16)));
extern const u8 VU1Draw_CodeEnd[];
extern const u8 VU1Draw_EntryInit[];
extern const u8 VU1Draw_EntryDraw[];
extern const u8 VU1Spr_CodeStart[] __attribute__((aligned(16))); // OPT11 round 3 (FX3, PS2-HW-255): the sprite program (vu1/ps2_hw_vu1_spr.vsm), behind the polygon program in the micro memory
extern const u8 VU1Spr_CodeEnd[];
extern const u8 VU1Spr_EntryDraw[];

#define VU_MAXVERT 12 // vertices of a polygon the program takes (20 per list, 6 planes add up to 6)
#define VU_CHUNK_QW 255 // UNPACK count is 8 bits; the VU1 input buffers hold 256
#define VU_MIN_FANS 1 // a plan with fewer polygons than this is not worth the constants and the synchronisation
#define VU_NCONST 18 // constant quad words (vu1/ps2_hw_vu1.vsm, K0..K17)
#define VU_ROWTAB 32 // VU1 memory address of the TEX0 table of the colormap rows (one quad word per row)
#define VU_PLANES 20 // VU1 memory address of the plane table (uploaded at boot)
#define VU_BASE 64 // double buffer of the input: BASE / OFFSET of the VIF
#define VU_OFFSET 256

static struct
{
	u32 chunk_tag; // index of the UNPACK tag of the open chunk in the buffer
	u32 chunk_n; // quadwords of the chunk so far (the header included)
	u32 npoly;
	int open;
	int kind; // OPT11 round 3 (PS2-HW-255): the open chunk is read by the polygon program (0) or by the sprite program (1)
	u32 cgen; // bumped by every send of the plan constants: a plan (the sprite stream's) made earlier is stale when it moved
	u32 spr_base; // micro memory address (instruction) of the sprite program; 0 = it is not there (the sprite stream is off)
	int consts_ok; // the constants of the current plan are in the VU1 memory
	int reuse; // the next vu_plan_setup is a retarget of the plan (another texture): the constants stay (PS2-HW-107)
	int rowmode; // the plan is lit by the colormap rows of a light table (P.pal)
	int rlo, rhi; // the rows the program may meet (their TEX0 are in the table)
	float cn[VU_NCONST][4] __attribute__((aligned(16)));
	u32 rt[32][4] __attribute__((aligned(16))); // TEX2 of every row (lo, hi, register 0x16, 0)
	int tab_tbl, tab_keyed, tab_lo, tab_hi; // the rows [tab_lo, tab_hi] of this table are in the VU1 memory (tab_hi < 0: none) ...
	u32 tab_epoch, tab_ltgen, tab_palgen; // ... as made for this CLUT epoch / table generation / palette
	float mp[16] __attribute__((aligned(16))); // M' = diag(1/gx, 1/gy, 1, 1) * mvp
	u32 mp_gx, mp_gy, mp_ver;
	u32 stat_polys, stat_chunks, stat_fallback, stat_plans, stat_rowfail, stat_sprchunks;
} VU;

#define PR_TBL 64 // light tables the VU path knows (the others take the general path)

static u8 prow_slot[PR_TBL][2][32]; // CLUT slot + 1 the row was found in last time (checked against the key: a replaced slot costs one clut_get)

typedef struct
{
	u32 epoch, ltgen, palgen; // what the rows [lo, hi] were made for (H.clut_epoch, lt_gen[], H.pal_gen)
	int lo, hi; // -1 = nothing
} prowv_t;

static prowv_t prowv[PR_TBL][2];

static inline u32 vu_entry(const u8 *e)
{
	return (u32)(e - VU1Draw_CodeStart) / 8u;
}

// plain DMA of a few quadwords to the VIF (boot only, before the ring is in use)
static int vif1_send_wait(const void *data, u32 qwc)
{
	u32 t0 = cyc();

	SyncDCache((void *)data, (u8 *)data + (size_t)qwc * 16);
	*D1_MADR = phys(data);
	*D1_QWC = qwc;
	__asm__ volatile("sync.l");
	*D1_CHCR = 0x101;
	__asm__ volatile("sync.p");
	while (*D1_CHCR & 0x100)
		if ((u32)(cyc() - t0) > 294912000u)
			return 0;
	while (*VIF1_STAT & 0x07) // VPS (packet status) and VEW (waiting for VU1)
		if ((u32)(cyc() - t0) > 294912000u)
			return 0;
	return 1;
}

static int vu1_boot(void)
{
	const u32 ninstr = (u32)(VU1Draw_CodeEnd - VU1Draw_CodeStart) / 8u;
	const u32 per = 128; // instructions per MPG (even)
	u32 nmpg = (ninstr + per - 1) / per, i, qwc, left;
	qw_t *pk;
	const u8 *src = VU1Draw_CodeStart;
	u32 at = 0;
	// OPT11 round 3 (PS2-HW-255): the sprite program goes behind the polygon program (on a boundary of 8 instructions); both are found by their size, whichever way the polygon program grows
	const u32 sinstr = (u32)(VU1Spr_CodeEnd - VU1Spr_CodeStart) / 8u;
	const u32 sbase = (ninstr + 7u) & ~7u;
	const int spr_ok = sinstr != 0 && !(sinstr & 1) && sbase + sinstr <= 2048;
	const u32 nmpg2 = spr_ok ? (sinstr + per - 1) / per : 0;

	if (ninstr == 0 || ninstr > 2048 || (ninstr & 1))
		return 0;
	memset(prow_slot, 0, sizeof prow_slot); // the CLUT cache starts empty with every Init
	for (i = 0; i < PR_TBL * 2; i++)
		((prowv_t *)(void *)prowv)[i].hi = -1;
	VU.tab_hi = -1;
	*VIF1_FBRST = 1; // reset the VIF
	__asm__ volatile("sync.l");
	qwc = 1 + nmpg + ninstr / 2 + (spr_ok ? nmpg2 + sinstr / 2 : 0) + 13 + 1 + 1;
	pk = memalign(64, (size_t)qwc * sizeof(qw_t));
	if (!pk)
		return 0;
	memset(pk, 0, (size_t)qwc * sizeof(qw_t));
	pk[0].w[0] = VIF_NOP;
	pk[0].w[1] = VIF_STCYCL(1, 1);
	pk[0].w[2] = VIF_BASE(VU_BASE);
	pk[0].w[3] = VIF_OFFSET(VU_OFFSET); // (OFFSET also sets TOPS = BASE)
	at = 1;
	for (i = 0, left = ninstr; i < nmpg; i++)
	{
		const u32 num = left < per ? left : per;

		pk[at].w[3] = VIF_MPG(num, i * per);
		at++;
		memcpy(&pk[at], src, (size_t)num * 8);
		at += num / 2;
		src += (size_t)num * 8;
		left -= num;
	}
	if (spr_ok)
	{
		const u8 *ssrc = VU1Spr_CodeStart;

		for (i = 0, left = sinstr; i < nmpg2; i++)
		{
			const u32 num = left < per ? left : per;

			pk[at].w[3] = VIF_MPG(num, sbase + i * per);
			at++;
			memcpy(&pk[at], ssrc, (size_t)num * 8);
			at += num / 2;
			ssrc += (size_t)num * 8;
			left -= num;
		}
		VU.spr_base = sbase;
	}
	else
	{
		VU.spr_base = 0;
	}
	{
		// the six planes of the view volume as the clipper of the program takes them (inside: dot product >= 0), in the order they are clipped (near, far, x-, x+, y-, y+),
		// then the outcode bit of each plane and the bits of the planes that come after it. They stay in the VU1 memory for the whole run.
		static const float pv[6][4] __attribute__((aligned(16))) = {{0, 0, 1, 1}, {0, 0, -1, 1}, {1, 0, 0, 1}, {-1, 0, 0, 1}, {0, 1, 0, 1}, {0, -1, 0, 1}};
		static const u32 pb[6][2] = {{0x0800, 0x0295}, {0x0010, 0x0285}, {0x0080, 0x0205}, {0x0001, 0x0204}, {0x0200, 0x0004}, {0x0004, 0x0000}};

		pk[at].w[3] = VIF_UNPACK_V4_32(VU_PLANES, 12, 0);
		at++;
		memcpy(&pk[at], pv, sizeof pv);
		at += 6;
		for (i = 0; i < 6; i++)
		{
			pk[at].w[0] = pb[i][0];
			pk[at].w[1] = pb[i][1];
			at++;
		}
	}
	pk[at].w[0] = VIF_MSCAL(vu_entry(VU1Draw_EntryInit));
	at++;
	if (!vif1_send_wait(pk, at))
	{
		free(pk);
		return 0;
	}
	free(pk);
	V.ready = 1;
	CONS_Printf("HWD VU1: program of %u instructions uploaded, entries init %u draw %u; sprite program %u instructions at %u, entry %u\n", (unsigned)ninstr, (unsigned)vu_entry(VU1Draw_EntryInit), (unsigned)vu_entry(VU1Draw_EntryDraw),
		(unsigned)(spr_ok ? sinstr : 0), (unsigned)VU.spr_base, (unsigned)(spr_ok ? VU.spr_base + (u32)(VU1Spr_EntryDraw - VU1Spr_CodeStart) / 8u : 0));
	return 1;
}

static void vu1_shutdown(void)
{
	if (V.ready)
	{
		*VIF1_FBRST = 1;
		V.ready = 0;
	}
	memset(&VU, 0, sizeof VU);
}

// ---- chain elements in the ring buffer ----

// room for `nqw` quadwords of elements (tags and data); starts a new buffer when the open one lacks it
static void vu_room(u32 nqw)
{
	if (H.wr + nqw + 2 > BUF_QW)
		pk_flush();
}

// closes the open GIF run (as pk_ref does) and returns the slot of the next element's tag
static qw_t *vu_tag_slot(void)
{
	qw_t *b = H.ring[H.cur], *t;
	u32 n = H.wr - H.run - 1;

	if (n)
	{
		b[H.run].d[0] = dma_tag(n, DMAT_CNT, 0);
		b[H.run].d[1] = vif_gif_codes(n);
		t = b + H.wr++;
	}
	else
	{
		t = b + H.run;
	}
	return t;
}

// the data of the element have been written behind the tag; a new run of GIF data opens behind them
static void vu_next_run(void)
{
	H.run = H.wr++;
}

// ---- colormap rows (palette lighting): the CLUT images of a light table ----
// The program loads the CLUT of the row of every piece (TEX2: the CLUT fields of TEX0 with CLD, the texture fields stay); the driver makes sure the rows it
// may meet are in VRAM and keeps their TEX2 values in the VU1 memory (a table that does not depend on the texture: it is valid for one CLUT epoch).

static inline u32 prow_param(u32 tbl, u32 row)
{
	return tbl | ((u32)lt_gen[tbl] << 10) | (row << 18);
}

// the CLUT block of row `row` of the table (building it when needed)
static u32 prow_blk(u32 tbl, int keyed, u32 row)
{
	const u32 param = prow_param(tbl, row);
	const u64 key = (u64)CK_LIGHT | ((u64)(keyed ? 1 : 0) << 8) | ((u64)param << 16);
	u8 *sl = &prow_slot[tbl][keyed ? 1 : 0][row];
	u32 blk;

	if (*sl)
	{
		clutent_t *e = &H.clut_ent[*sl - 1];

		if (e->key == key && e->gen == H.pal_gen)
		{
			e->stamp = ++H.clut_tick;
			return H.clut_base + (u32)(*sl - 1) * CLUT_BLOCKS;
		}
	}
	G.vr_blk_slow++;
	blk = clut_get(CK_LIGHT, param, (u32)(keyed ? 1 : 0));
	*sl = (u8)((blk - H.clut_base) / CLUT_BLOCKS + 1);
	return blk;
}

// the CLUTs of the rows [rlo, rhi] of the table are in VRAM (made when they are not); the check is a few compares while nothing in the CLUT cache changed
static void prow_ensure(u32 tbl, int keyed, int rlo, int rhi)
{
	prowv_t *v = &prowv[tbl][keyed ? 1 : 0];
	int lo = rlo, hi = rhi, row;
	const int same = v->hi >= 0 && v->ltgen == lt_gen[tbl] && v->palgen == H.pal_gen;

	G.vr_calls++;
	if (same && v->epoch == H.clut_epoch && rlo >= v->lo && rhi <= v->hi)
	{
		G.vr_same++;
		return;
	}
	G.vr_slow++;
	if (same)
	{
		lo = lo < v->lo ? lo : v->lo; // the union: a plan with another light level meets other rows of the same table
		hi = hi > v->hi ? hi : v->hi;
	}
	for (row = lo; row <= hi; row++)
	{
		G.vr_rows++;
		prow_blk(tbl, keyed, (u32)row);
	}
	v->ltgen = lt_gen[tbl];
	v->palgen = H.pal_gen;
	v->lo = lo;
	v->hi = hi;
	v->epoch = H.clut_epoch;
}

static inline int cm_row(float cm)
{
	return cm < 0.0f ? 0 : cm >= 31.0f ? 31 : (int)cm;
}

// light equation of the plan: row = clamp(floor(max(base - k80 / (lightz + 1), cap)), 0, 31), lightz = clamp((w - near) * kz16, 0, 127), with base and cap made
// by the program for the light level of every polygon (lightnum = clamp(light / 17, 0, 15), base = (15 - lightnum) * 4 * sfudge, cap = (155 - light) * 0.26)
// from the constants here (lf_params and doom_depth_of_level are the EE reference)
typedef struct
{
	float sf4, k80, near_w, kz16, invkz16;
	int rlo, rhi;
} vlight_t;

static void vu_light(vlight_t *vl)
{
	const lightp_t *lp = &P.rs.lp;
	float kz;

	vl->sf4 = 4.0f * lp->sfudge;
	vl->k80 = 80.0f * lp->cfudge;
	vl->near_w = H.near_plane;
	kz = FAR_CLIPPING_PLANE / (FAR_CLIPPING_PLANE - H.near_plane);
	vl->kz16 = kz * (1.0f / 16.0f);
	vl->invkz16 = 16.0f / kz;
	vl->rlo = 0; // the rows of every light level of the table
	vl->rhi = 31;
}

// ---- the constants of the plan ----

static void vu_consts_build(const vlight_t *vl)
{
	const pass_t *ps = &P.pass[0];
	const float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);
	const u32 prim = PRIM_TRIFAN | (pass_prim(ps) & ~(u32)PRIM_FGE);
	u64 t0 = HWGIF_TAG(0, 1, 1, prim, 0, 3), t1 = HWGIF_TAG(0, 1, 1, prim, 0, 1), t2 = HWGIF_TAG(1, 0, 0, 0, 0, 1);
	u32 *w;
	int c;

	if (VU.mp_ver != xf_ver || VU.mp_gx != (u32)H.guard_x || VU.mp_gy != (u32)H.guard_y)
	{
		for (c = 0; c < 4; c++)
		{
			VU.mp[c * 4 + 0] = H.mvp[c * 4 + 0] / gx;
			VU.mp[c * 4 + 1] = H.mvp[c * 4 + 1] / gy;
			VU.mp[c * 4 + 2] = H.mvp[c * 4 + 2];
			VU.mp[c * 4 + 3] = H.mvp[c * 4 + 3];
		}
		VU.mp_ver = xf_ver;
		VU.mp_gx = (u32)H.guard_x;
		VU.mp_gy = (u32)H.guard_y;
	}
	memcpy(VU.cn[0], VU.mp, 16 * sizeof(float));
	VU.cn[4][0] = P.kx * gx;
	VU.cn[4][1] = P.ky * gy;
	VU.cn[4][2] = P.zk;
	VU.cn[4][3] = 1.0f;
	VU.cn[5][0] = P.ox;
	VU.cn[5][1] = P.oy;
	VU.cn[5][2] = P.zo + P.zbias;
	VU.cn[5][3] = 0.0f;
	VU.cn[6][0] = VU.cn[6][1] = VU.cn[6][3] = 0.0f;
	VU.cn[6][2] = P.zmax;
	VU.cn[7][0] = P.tsx;
	VU.cn[7][1] = P.tsy;
	VU.cn[7][2] = 1.0f;
	VU.cn[7][3] = 0.0f;
	w = (u32 *)(void *)VU.cn[8];
	w[0] = ps->col & 255;
	w[1] = (ps->col >> 8) & 255;
	w[2] = (ps->col >> 16) & 255;
	w[3] = ps->col >> 24;
	w = (u32 *)(void *)VU.cn[9];
	w[0] = (u32)t0;
	w[1] = (u32)(t0 >> 32);
	w[2] = (u32)GIF_REGS_STQ_RGBA_XYZF;
	w[3] = 0;
	w = (u32 *)(void *)VU.cn[10];
	w[0] = (u32)t1;
	w[1] = (u32)(t1 >> 32);
	w[2] = (u32)GIF_REGS_XYZF;
	w[3] = 0;
	VU.cn[11][0] = vl ? (float)vl->rlo : 0.0f;
	VU.cn[11][1] = 155.0f;
	VU.cn[11][2] = 0.0f;
	VU.cn[11][3] = 1e-30f;
	if (vl)
	{
		VU.cn[12][0] = vl->near_w;
		VU.cn[12][1] = vl->k80;
		VU.cn[12][2] = VU.cn[12][3] = 0.0f; // (cap and base: the program makes them for every polygon)
		VU.cn[13][0] = vl->kz16;
		VU.cn[13][1] = vl->invkz16;
		VU.cn[13][2] = 127.0f;
		VU.cn[13][3] = (float)vl->rhi;
		VU.cn[14][2] = -vl->k80;
		VU.cn[17][0] = 1.0f / 17.0f;
		VU.cn[17][1] = 15.0f;
		VU.cn[17][2] = vl->sf4;
		VU.cn[17][3] = 0.26f;
	}
	else
	{
		VU.cn[12][0] = VU.cn[12][1] = VU.cn[12][2] = VU.cn[12][3] = 0.0f;
		VU.cn[13][0] = VU.cn[13][1] = 0.0f;
		VU.cn[13][2] = 127.0f;
		VU.cn[13][3] = 0.0f;
		VU.cn[14][2] = 0.0f;
		VU.cn[17][0] = VU.cn[17][1] = VU.cn[17][2] = VU.cn[17][3] = 0.0f;
	}
	VU.cn[14][0] = 1e30f;
	VU.cn[14][1] = -1e30f;
	VU.cn[14][3] = -1e-4f;
	w = (u32 *)(void *)VU.cn[15];
	w[0] = (u32)t2;
	w[1] = (u32)(t2 >> 32);
	w[2] = (u32)GIF_REGS_AD;
	w[3] = 0;
	w = (u32 *)(void *)VU.cn[16];
	w[0] = vl ? 1u : 0u; // palette rows
	w[1] = ps->tex ? 1u : 0u; // textured
	w[2] = vl ? (u32)vl->rlo : 0u; // the row the GS holds (pass_setup wrote the TEX0 of the first row)
	w[3] = 0;
}

static void vu_chunk_end(void);
static inline void vu_sync(void);

// the constants of the plan go behind FLUSHE: the program that may still run is done with them; the TEX2 table of the colormap rows follows when the
// VU1 memory does not hold the rows of this table (rows [rlo, rhi]). what = 1: only the integers of the plan (the row the GS holds: a retargeted plan loaded the
// CLUT of its first row), the rest of the constants did not change.
static void vu_consts_send(int send_rows, int what)
{
	qw_t *b, *t;
	const u32 nrow = send_rows ? (u32)(VU.rhi - VU.rlo + 1) : 0;
	const u32 nc = what ? 1u : (u32)VU_NCONST;

	vu_room(1 + nc + 1 + nrow + 2);
	t = vu_tag_slot();
	b = H.ring[H.cur];
	t->d[0] = dma_tag(nc, DMAT_CNT, 0);
	t->d[1] = 0x10000000ull | ((u64)VIF_UNPACK_V4_32(what ? 16u : 0u, nc, 0) << 32); // FLUSHE
	memcpy(b + H.wr, what ? VU.cn[16] : VU.cn[0], nc * sizeof(qw_t));
	H.wr += nc;
	if (nrow)
	{
		t = b + H.wr++;
		t->d[0] = dma_tag(nrow, DMAT_CNT, 0);
		t->d[1] = (u64)VIF_NOP | ((u64)VIF_UNPACK_V4_32(VU_ROWTAB + (u32)VU.rlo, nrow, 0) << 32);
		memcpy(b + H.wr, &VU.rt[VU.rlo], nrow * sizeof(qw_t));
		H.wr += nrow;
	}
	vu_next_run();
	VU.consts_ok = 1;
	VU.cgen++;
}

// The GS state of a retargeted plan (PS2-HW-107): only what depends on the texture (TEX0 with the CLUT of the first row, TEX1, CLAMP) is written; the other registers are
// the plan's: H.serial has not moved since pass_setup made them (every write of GS state goes through ad_alloc, which counts). Same registers, same values and same cache
// updates as pass_setup / state_flush make for the texture part.
static void vu_retex(void)
{
	const pass_t *ps = &P.pass[0];
	const texrec_t *r = P.r;
	const u64 cld_mask = 7ull << 61;
	u64 t0, t1, cl, vals[3];
	u32 regs[3], cbp = 0;
	int n = 0, i;

	if (!ps->tex || !r)
		return;
	if (r->psm == PSM_T8)
	{
		if (ps->clut_kind)
			cbp = clut_get(ps->clut_kind, ps->clut_param, ps->clut_keyed);
		else
			cbp = clut_get(r->clut == 3 ? CK_ALPHA : (r->clut == 2 ? CK_KEYED : CK_OPAQUE), 0, 0);
	}
	tex_regs(r, 0, 0, P.linear, cbp, ps->decal, &t0, &t1, &cl);
	cl = P.clamp;
	if (!(H.gsr.valid & RV_CLAMP) || H.gsr.clamp != cl)
	{
		vals[n] = cl;
		regs[n++] = GSR_CLAMP_1;
		H.gsr.clamp = cl;
		H.gsr.valid |= RV_CLAMP;
	}
	if (!(H.gsr.valid & RV_TEX1) || H.gsr.tex1 != t1)
	{
		vals[n] = t1;
		regs[n++] = GSR_TEX1_1;
		H.gsr.tex1 = t1;
		H.gsr.valid |= RV_TEX1;
	}
	if (!(H.gsr.valid & RV_TEX0) || (H.gsr.tex0 & ~cld_mask) != (t0 & ~cld_mask) || (t0 & cld_mask))
	{
		vals[n] = t0;
		regs[n++] = GSR_TEX0_1;
		H.gsr.tex0 = t0;
		H.gsr.valid |= RV_TEX0;
	}
	if (n)
	{
		qw_t *p = ad_alloc((u32)n);

		for (i = 0; i < n; i++)
			ad_set(&p[i], vals[i], regs[i]);
		H.st.state_writes += (u32)n;
	}
}

// The plan of the batch for the VU1 program: rows in VRAM, GS state, constants. 0 = the VU1 path cannot take it (the general path draws it).
static __attribute__((noinline)) int vu_plan_setup(void)
{
	vlight_t vl;
	const texrec_t *r = P.r;
	int send_rows = 0;
	const int reuse = VU.reuse && VU.consts_ok;
	u32 c0 = cyc(), c1, c2;

	VU.reuse = 0;
	vu_sync();
	VU.rowmode = P.pal;
	VU.stat_plans++;
	G.vs_n++;
	if (VU.rowmode)
	{
		const u32 tbl = P.pal_tbl;
		int keyed;

		if (tbl >= PR_TBL || !r || r->psm != PSM_T8)
		{
			VU.stat_rowfail++;
			return 0;
		}
		// PS2-HW-105: the CLUT rows of the plain and of the keyed palette differ in the entry 255 only: a map texture or flat that has no texel of index 255 takes the keyed
		// rows in both variants (the same picture, half the CLUT images of the cache: the program may meet any row of the range, the images of both variants would not fit)
		{
			const texrec_t *im = &H.rec[img_of(H.cur_tex)];

			keyed = r->clut == 2 || (r->clut == 1 && im->shareable && !im->has255);
			P.pass[0].clut_keyed = (u8)keyed;
		}
		vu_light(&vl);
		VU.rlo = vl.rlo;
		VU.rhi = vl.rhi;
		c1 = cyc();
		G.vs_light += c1 - c0;
		prow_ensure(tbl, keyed, vl.rlo, vl.rhi);
		c2 = cyc();
		G.vs_rows += c2 - c1;
		if (!(VU.tab_hi >= 0 && VU.tab_tbl == (int)tbl && VU.tab_keyed == keyed && VU.tab_epoch == H.clut_epoch && VU.tab_ltgen == lt_gen[tbl] && VU.tab_palgen == H.pal_gen
			&& vl.rlo >= VU.tab_lo && vl.rhi <= VU.tab_hi))
		{
			// TEX2 of every row: the CLUT fields of TEX0 (PSM, CBP, CPSM = CT32, CSM1, CSA 0, CLD = 1), the texture's own fields stay as the plan's TEX0 wrote them
			u32 row;

			for (row = (u32)vl.rlo; row <= (u32)vl.rhi; row++)
			{
				const u32 blk = H.clut_base + (u32)(prow_slot[tbl][keyed][row] - 1) * CLUT_BLOCKS;
				const u64 t = ((u64)PSM_T8 << 20) | ((u64)blk << 37) | (1ull << 61);

				VU.rt[row][0] = (u32)t;
				VU.rt[row][1] = (u32)(t >> 32);
				VU.rt[row][2] = GSR_TEX2_1;
				VU.rt[row][3] = 0;
			}
			VU.tab_tbl = (int)tbl;
			VU.tab_keyed = keyed;
			VU.tab_lo = vl.rlo;
			VU.tab_hi = vl.rhi;
			VU.tab_epoch = H.clut_epoch;
			VU.tab_ltgen = lt_gen[tbl];
			VU.tab_palgen = H.pal_gen;
			send_rows = 1;
		}
		P.pass[0].clut_param = pal_param((u32)vl.rlo); // the state packet loads the CLUT of the first row
	}
	c1 = cyc();
	if (P.pass[0].tex)
	{
		// the program writes the colour of every vertex (RGBAQ): the colour register of the GS need not hold the plan's (chunk end forgets what the GS has)
		H.gsr.rgbaq = rgbaq_of(P.pass[0].col);
		H.gsr.valid |= RV_RGBAQ;
	}
	if (reuse && (H.gsr.valid & (RV_FRAME | RV_ZBUF | RV_SCISSOR | RV_TEST | RV_ALPHA | RV_RGBAQ)) == (RV_FRAME | RV_ZBUF | RV_SCISSOR | RV_TEST | RV_ALPHA | RV_RGBAQ))
		vu_retex(); // (the other registers are the plan's)
	else
		pass_setup(0, 0);
	P.serial = H.serial;
	c2 = cyc();
	G.vs_pass += c2 - c1;
	if (reuse)
	{
		// the same plan, another texture: the constants are the plan's; the row the GS holds is the first row of the table again (pass_setup loaded it)
		if (VU.rowmode)
		{
			((u32 *)(void *)VU.cn[16])[2] = (u32)VU.rlo;
			vu_consts_send(send_rows, 1);
		}
		c1 = cyc();
		G.vs_consts += 0;
		G.vs_send += c1 - c2;
		G.vs_reuse++;
		G.vs_c += c1 - c0;
		return 1;
	}
	vu_consts_build(VU.rowmode ? &vl : NULL);
	c1 = cyc();
	G.vs_consts += c1 - c2;
	vu_consts_send(send_rows, 0);
	c2 = cyc();
	G.vs_send += c2 - c1;
	G.vs_c += c2 - c0;
	return 1;
}

// ---- single draws (hw_DrawPolygon: HUD, sprites that are not batched, shadows) through the VU1 program ----
// A chunk stays open between two single draws; a draw whose plan is the one of the last single draw (the same key: flags, surface, texture, view) joins it: no
// begin_draw, no constants, no new chunk. A draw of the same plan with another texture of the same kind (and, when the plan lights by rows, another light level)
// only retargets the plan (PS2-HW-107): the new texture's registers, the texture scale (it travels in the polygon header), no begin_draw_inner, no constants.
// Anything else ends the chunk (the ring writers do) and makes a plan.

#define VK_N 32 // words (16 doublewords: the key is compared by doublewords)

typedef struct
{
	u32 w[VK_N];
} __attribute__((aligned(8))) vukey_t;

// the groups of words: core 0..5 (flags, colour, tint, fade, light table, kind of the texture), light 6..9, texture 10..17, view 18..31
#define VKL_BEG 3
#define VKT_BEG 5
#define VKV_BEG 9
#define VKV_END 16

static vukey_t VK; // the key vu_plan_valid belongs to

static inline u32 f2u_(float f)
{
	union
	{
		float f;
		u32 u;
	} c;

	c.f = f;
	return c.u;
}

static void vu_key_make(vukey_t *k, u32 flags, const FSurfaceInfo *surf)
{
	const texrec_t *r = (!(flags & PF_NoTexture) && H.cur_tex != NOREC) ? &H.rec[H.cur_tex] : NULL;
	u32 *w = k->w;

	if (H.mat_dirty)
		xf_update();
	w[0] = flags;
	if (surf && (flags & PF_ColorMapped))
	{
		w[1] = surf->PolyColor.rgba;
		w[2] = surf->TintColor.rgba;
		w[3] = surf->FadeColor.rgba;
		w[4] = surf->LightTableId;
		w[6] = (u32)surf->LightInfo.light_level;
		w[7] = (u32)surf->LightInfo.fade_start;
		w[8] = (u32)surf->LightInfo.fade_end;
	}
	else if (surf)
	{
		// (PS2-HW-235) a polygon the shader does not light: begin_draw_inner reads the colour only; the rest of the surface is what the caller left on its stack, and it made the key of
		// two draws of one plan differ for nothing
		w[1] = surf->PolyColor.rgba;
		w[2] = w[3] = w[4] = w[6] = w[7] = w[8] = 0;
	}
	else
	{
		w[1] = w[2] = w[3] = w[4] = w[6] = w[7] = w[8] = 0;
	}
	w[9] = 0;
	if (r)
	{
		// the kind: what of the texture decides the plan (begin_draw_inner), the same bits as the plan cache's key
		w[5] = (u32)r->psm | ((u32)r->clut << 6) | ((u32)(r->notcc != 0) << 8) | ((u32)(r->screen != 0) << 9) | ((u32)(r->wrapx != 0) << 10) | ((u32)(r->wrapy != 0) << 11)
			| ((u32)(r->npot_x != 0) << 12) | ((u32)(r->npot_y != 0) << 13) | ((u32)(r->region & 3) << 14) | ((u32)(r->nearest_only != 0) << 16) | (1u << 17);
		w[10] = (u32)H.cur_tex + 1u;
		w[11] = r->blk;
		w[12] = (u32)r->psm | ((u32)r->clut << 8) | ((u32)r->wrapx << 12) | ((u32)r->wrapy << 13) | ((u32)r->nearest_only << 14) | ((u32)r->notcc << 15) | ((u32)r->region << 16)
			| ((u32)r->npot_x << 18) | ((u32)r->npot_y << 19) | ((u32)r->screen << 20) | ((u32)r->tw << 24) | ((u32)r->th << 28);
		w[13] = (u32)r->uw | ((u32)r->vh << 16);
		w[14] = f2u_(r->sxs);
		w[15] = f2u_(r->sys);
		w[16] = (u32)r->tbw | ((u32)r->w << 8) | ((u32)r->h << 20);
	}
	else
	{
		// a texture that is missing (begin_draw_inner skips the polygon) never matches a plan
		w[5] = (!(flags & PF_NoTexture) && H.cur_missing) ? (1u << 18) : 0;
		w[10] = w[11] = w[12] = w[13] = w[14] = w[15] = w[16] = 0;
	}
	w[17] = 0;
	w[18] = xf_ver;
	w[19] = f2u_(H.vpx);
	w[20] = f2u_(H.vpy);
	w[21] = f2u_(H.vpw);
	w[22] = f2u_(H.vph);
	w[23] = f2u_(H.near_plane);
	w[24] = (u32)H.guard_x;
	w[25] = (u32)H.guard_y;
	w[26] = (u32)H.shader | ((u32)H.shaders_on << 8) | ((u32)H.tex_filter << 12) | ((u32)(H.imm_tex != NULL) << 20);
	w[27] = H.pal_gen;
	w[28] = H.sciss_x0 | (H.sciss_y0 << 16);
	w[29] = H.sciss_x1 | (H.sciss_y1 << 16);
	w[30] = (u32)H.target | (H.frame_no << 4) | ((u32)palette_mode_fr() << 3);
	w[31] = 0;
}

static inline u64 vk_d(const vukey_t *a, const vukey_t *b, int i)
{
	return ((const u64 *)(const void *)a)[i] ^ ((const u64 *)(const void *)b)[i];
}

// 2 = the same plan, the same texture and light; 1 = the same plan, another texture of the same kind (and another light level when the plan lights by rows); 0 = another plan
static inline int vu_key_cmp(const vukey_t *a, const vukey_t *b)
{
	u64 dc, dl, dt, dv;
	int i;

	dc = vk_d(a, b, 0) | vk_d(a, b, 1) | vk_d(a, b, 2);
	dv = 0;
	for (i = VKV_BEG; i < VKV_END; i++)
		dv |= vk_d(a, b, i);
	if (dc | dv)
		return 0;
	dl = vk_d(a, b, 3) | vk_d(a, b, 4);
	dt = vk_d(a, b, 5) | vk_d(a, b, 6) | vk_d(a, b, 7) | vk_d(a, b, 8);
	if (!(dl | dt))
		return 2;
	if (vu_noretarget)
		return 0;
	if (dl && !P.pal)
		return 0; // the light level is a part of the plan unless the program lights by rows
	return 1;
}

// PS2-HW-111: the texture registers of a retargeted plan as a state record in the open chunk (the program sends the record's GIF packet with XGKICK between the polygons):
// no end of the chunk, no MSCAL, no GS state through PATH2. Plans lit by colormap rows (P.pal) keep the other way: their rows are tracked by the program. Returns 1 when the
// state of the new texture is in the chunk (or was the GS's already); 0 = nothing was changed (the caller makes the plan again).
static int vu_state_rec(void)
{
	const pass_t *ps = &P.pass[0];
	const texrec_t *r = P.r;
	const u64 cld_mask = 7ull << 61;
	u64 t0, t1, cl, vals[3];
	u32 regs[3], cbp = 0;
	int n = 0, i;
	qw_t *o;

	if (!VU.open || VU.kind || P.pal || !ps->tex || !r || vu_norecord)
		return 0;
	if ((H.gsr.valid & (RV_CLAMP | RV_TEX1 | RV_TEX0)) != (RV_CLAMP | RV_TEX1 | RV_TEX0))
		return 0;
	if (VU.chunk_n + 6 > VU_CHUNK_QW || H.wr + 6 + 3 > BUF_QW)
		return 0; // (the worst case: the header, the GIF tag and three registers)
	if (r->psm == PSM_T8)
	{
		if (ps->clut_kind)
			cbp = clut_get(ps->clut_kind, ps->clut_param, ps->clut_keyed);
		else
			cbp = clut_get(r->clut == 3 ? CK_ALPHA : (r->clut == 2 ? CK_KEYED : CK_OPAQUE), 0, 0);
		if (!VU.open || H.wr + 6 + 3 > BUF_QW)
			return 0; // the CLUT had to be made: its upload closed the chunk
	}
	tex_regs(r, 0, 0, P.linear, cbp, ps->decal, &t0, &t1, &cl);
	cl = P.clamp;
	if (H.gsr.clamp != cl)
	{
		vals[n] = cl;
		regs[n++] = GSR_CLAMP_1;
		H.gsr.clamp = cl;
	}
	if (H.gsr.tex1 != t1)
	{
		vals[n] = t1;
		regs[n++] = GSR_TEX1_1;
		H.gsr.tex1 = t1;
	}
	if ((H.gsr.tex0 & ~cld_mask) != (t0 & ~cld_mask) || (t0 & cld_mask))
	{
		vals[n] = t0;
		regs[n++] = GSR_TEX0_1;
		H.gsr.tex0 = t0;
	}
	if (!n)
		return 1;
	o = H.ring[H.cur] + H.wr;
	o[0].w[0] = 0; // n = 0: a state record
	o[0].w[1] = 1u + (u32)n; // its length behind this header
	o[0].w[2] = o[0].w[3] = 0;
	o[1].d[0] = HWGIF_TAG((u32)n, 1, 0, 0, 0, 1);
	o[1].d[1] = GIF_REGS_AD;
	for (i = 0; i < n; i++)
		ad_set(&o[2 + i], vals[i], regs[i]);
	H.wr += 2 + (u32)n;
	VU.chunk_n += 2 + (u32)n;
	VU.npoly++;
	H.st.state_writes += (u32)n;
	G.s_rec++;
	return 1;
}

// The plan of the last single draw (P, GS state, VU1 constants) for another texture of the same kind: what begin_draw_inner derives from the texture record. The kind
// (psm, palette kind, wrap, npot, region, filter inputs) is equal by the key, so the passes, the cut flags and the filter stay.
static void vu_retarget(const FSurfaceInfo *surf)
{
	texrec_t *r = (!(P.flags & PF_NoTexture) && H.cur_tex != NOREC) ? &H.rec[H.cur_tex] : NULL;

	if (r)
	{
		const int wms = P.wrapx ? 0 : 1, wmt = P.wrapy ? 0 : 1;

		P.r = r;
		P.tsx = r->sxs;
		P.tsy = r->sys;
		P.clamp = (u64)((r->region & 1) ? 2 : wms) | ((u64)((r->region & 2) ? 2 : wmt) << 2) | ((u64)(r->uw - 1) << 14) | ((u64)(r->vh - 1) << 34);
		plan_tex_touch(r);
	}
	if (P.pal && surf)
	{
		P.rs.lp.light = (float)surf->LightInfo.light_level; // the polygon header carries it; the general path (a polygon the program does not take) needs it in the plan
		P.rs.lp.fade_start = surf->LightInfo.fade_start;
		P.rs.lp.fade_end = surf->LightInfo.fade_end;
	}
	if (r && vu_state_rec())
		return; // the texture registers are in the open chunk
	VU.reuse = 1; // vu_plan_setup keeps the constants of the plan (the texture scale travels in the polygon header)
	P.serial = H.serial - 1; // the GS state of the pass again (the texture registers)
}

// kind 0: polygons for the polygon program; kind 1 (PS2-HW-255): sprites for the sprite program (its chunk header holds 0.5 in lane y)
static void vu_chunk_open(int kind)
{
	qw_t *b, *t;

	vu_room(1 + 1 + 4 + 2);
	t = vu_tag_slot();
	b = H.ring[H.cur];
	VU.chunk_tag = (u32)(t - b);
	H.wr = (u32)(t - b) + 1;
	VU.chunk_n = 1; // the chunk header: the number of polygons
	b[H.wr].d[0] = b[H.wr].d[1] = 0;
	if (kind)
		b[H.wr].f[1] = 0.5f;
	H.wr++;
	VU.npoly = 0;
	VU.open = 1;
	VU.kind = kind;
	V.chunk_open = 1;
}

static void vu_chunk_begin(void)
{
	vu_chunk_open(0);
}

static void vu_chunk_end(void)
{
	qw_t *b = H.ring[H.cur], *t, *m;

	if (!VU.open)
		return;
	t = b + VU.chunk_tag;
	t->d[0] = dma_tag(VU.chunk_n, DMAT_CNT, 0);
	t->d[1] = (u64)VIF_NOP | ((u64)VIF_UNPACK_V4_32(0, VU.chunk_n, 1) << 32);
	b[VU.chunk_tag + 1].w[0] = VU.npoly;
	m = b + H.wr++;
	m->d[0] = dma_tag(0, DMAT_CNT, 0);
	m->d[1] = (u64)VIF_FLUSH | ((u64)VIF_MSCAL(VU.kind ? VU.spr_base + (u32)(VU1Spr_EntryDraw - VU1Spr_CodeStart) / 8u : vu_entry(VU1Draw_EntryDraw)) << 32);
	vu_next_run();
	VU.open = 0;
	V.chunk_open = 0;
	VU.stat_chunks++;
	G.vu_chunks++;
	H.gsr.valid &= ~(u32)RV_RGBAQ; // the packets of the program change Q (and the colour) behind the A+D state cache
	if (VU.kind)
	{
		// the sprite program writes TEX0 and CLAMP of every sprite and loads the CLUT of the rows (TEX2): what the GS holds of them is unknown now
		H.gsr.valid &= ~(u32)(RV_TEX0 | RV_CLAMP);
		H.clut_loaded = 0;
		P.pal_row = -1;
		VU.kind = 0;
		VU.stat_sprchunks++;
	}
	if (VU.rowmode)
	{
		// the program loaded CLUTs: what the GS holds as TEX0 / CLUT is unknown now
		H.gsr.valid &= ~(u32)RV_TEX0;
		H.clut_loaded = 0;
		P.pal_row = -1;
	}
}

// Everything that goes into the ring in another way ends the chunk first (the order of the stream is the order of the draws): the ring writers (pk_alloc,
// pk_ref, pk_flush) call vu_chunk_flush themselves, so that a chunk can stay open between two draws of the engine (PS2-HW-103).
static inline void vu_sync(void)
{
	if (VU.open)
		vu_chunk_end();
}

static void vu_chunk_flush(void)
{
	if (VU.open)
		vu_chunk_end();
}

// -hwvudump N (OPT11-STAB, diagnosis of a VU1 program that does not end): every polygon the VU1 path takes in the frame number N (H.frame_no) is printed with its light level
// and vertices (x y z s t), in the order of the chunk
static u32 vu_dump_frame;
static u32 vu_max_poly, vu_poly_seen, vu_poly_frame; // -hwvumax K (with -hwvudump N): only the first K polygons of frame N take the VU1 path (bisecting a hang)

static u32 vu_stop_frame; // -hwvustop N: from frame N on the VU1 path takes nothing (bisecting a hang by frame)

static int vu_over_limit(void)
{
	if (vu_stop_frame && H.frame_no >= vu_stop_frame)
		return 1;
	if (!vu_max_poly || !vu_dump_frame || H.frame_no != vu_dump_frame)
		return 0;
	if (vu_poly_frame != H.frame_no)
	{
		vu_poly_frame = H.frame_no;
		vu_poly_seen = 0;
	}
	return vu_poly_seen >= vu_max_poly;
}

static void vu_dump(const FOutVector *base, int n, float light)
{
	int i;

	I_OutputMsg("VUPOLY f%u n=%d light=%g", (unsigned)H.frame_no, n, (double)light);
	for (i = 0; i < n; i++)
		I_OutputMsg(" [%g %g %g %g %g]", (double)base[i].x, (double)base[i].y, (double)base[i].z, (double)base[i].s, (double)base[i].t);
	I_OutputMsg("\n");
}

// Called by emit_poly (before anything else) for a plan that is VU1 capable. Returns 1 when the polygon was taken (drawn or dropped).
// The polygon goes into the chunk in one pass over its vertices (PS2-HW-107: the extent of s, t for the texture repeat is found while the vertices are copied,
// the w lanes and the zero lanes are not written: the program does not read them; the header carries the texture scale of the polygon).
static int vu_poly(const FOutVector *base, int n, float light)
{
	float sb = 0.0f, tb = 0.0f;
	int i;
	u32 need;
	qw_t *o, *v;

	if (n < 3 || n > VU_MAXVERT || P.cutx || P.cuty || vu_over_limit()) // (PS2-HW-236: a polygon of a texture with a side that is not a power of two is cut: vu_poly_blk does that for the blocks of a batch; vu_over_limit: STAB debug limit)
		return 0;
	if (VU.open && VU.kind)
		vu_chunk_end(); // PS2-HW-255: the open chunk is a sprite chunk
	if (!VU.consts_ok || P.serial != H.serial)
	{
		// first polygon of the plan, or the general path wrote GS state in between: the state of pass 0 and the constants again
		if (!vu_plan_setup())
		{
			P.vu = 0;
			P.vuok = 0;
			VU.stat_fallback++;
			return 0;
		}
	}
	need = 2 + 2 * (u32)n;
	if (VU.open && (VU.chunk_n + need > VU_CHUNK_QW || H.wr + need + 3 > BUF_QW))
		vu_chunk_end();
	if (!VU.open)
		vu_chunk_begin();
	if (H.wr + need + 3 > BUF_QW)
	{
		vu_chunk_end();
		pk_flush();
		vu_chunk_begin();
	}
	o = H.ring[H.cur] + H.wr;
	v = o + 2;
	if (P.textured && (P.wrapx || P.wrapy))
	{
		float smin = base[0].s, smax = smin, tmin = base[0].t, tmax = tmin;

		for (i = 0; i < n; i++, v += 2)
		{
			const float s = base[i].s, t = base[i].t;

			v[0].f[0] = base[i].x;
			v[0].f[1] = base[i].y;
			v[0].f[2] = base[i].z;
			v[1].f[0] = s;
			v[1].f[1] = t;
			smin = hw_minf(smin, s);
			smax = hw_maxf(smax, s);
			tmin = hw_minf(tmin, t);
			tmax = hw_maxf(tmax, t);
		}
		if ((P.wrapx && (smax - smin) * (float)P.r->w > UV_EXTENT) || (P.wrapy && (tmax - tmin) * (float)P.r->h > UV_EXTENT))
			return 0; // cut at whole repeats: cut_and_emit (nothing of this polygon was committed)
		sb = (P.wrapx && (fbits(smin) & 0x7FFFFFFF) > 0x40800000) ? floorf(smin) : 0.0f; // |smin| > 4
		tb = (P.wrapy && (fbits(tmin) & 0x7FFFFFFF) > 0x40800000) ? floorf(tmin) : 0.0f;
	}
	else
	{
		for (i = 0; i < n; i++, v += 2)
		{
			v[0].f[0] = base[i].x;
			v[0].f[1] = base[i].y;
			v[0].f[2] = base[i].z;
			v[1].f[0] = base[i].s;
			v[1].f[1] = base[i].t;
		}
	}
	o[0].w[0] = (u32)n;
	o[0].f[1] = light; // the light level of the polygon's sector: the program makes cap and base of the colormap row equation from it
	o[1].f[0] = sb;
	o[1].f[1] = tb;
	o[1].f[2] = P.tsx;
	o[1].f[3] = P.tsy;
	H.st.polys++;
	H.st.verts_in += (u32)n;
	H.wr += need;
	VU.chunk_n += need;
	VU.npoly++;
	if (vu_dump_frame && H.frame_no == vu_dump_frame)
	{
		vu_poly_seen++;
		vu_dump(base, n, light);
	}
	return 1;
}

// The polygons of one batch (PS2HWD_DrawFans) from index i on, in one loop with the values of the plan in locals (PS2-HW-108): the same chunk contents as vu_poly makes.
// What is not the common case (the first polygon of the plan, a full chunk or buffer, a polygon of an unusual size) goes through vu_poly. Returns the index of the first
// polygon that was not taken (nfans: all taken): the caller draws it by the general path (a polygon of too wide a texture extent, a plan the program cannot take).
static u32 vu_fans(const FOutVector *base, const u32 *desc, u32 i, u32 nfans)
{
	const int wrap = P.textured && (P.wrapx || P.wrapy);
	const int wx = P.wrapx, wy = P.wrapy;
	const float tsx = P.tsx, tsy = P.tsy;
	// PS2-HW-228: the extent test (smax - smin) * width > UV_EXTENT as a compare of the bits of (smax - smin) with the limit (the difference is not negative): no multiply, no
	// floating point compare (4 + 5 cycles an axis); a power of two texture side makes the product exact, the others differ from it by a rounding in the last bit of the limit
	const int ilx = wx ? fbits(UV_EXTENT / (float)P.r->w) : 0x7F7FFFFF, ily = wy ? fbits(UV_EXTENT / (float)P.r->h) : 0x7F7FFFFF;
	// PS2-HW-223: the chunk position in locals (the stores into the ring may alias any global as far as the compiler knows: every polygon reloaded and stored six of them), the
	// counters of the frame added once at the end
	qw_t *ring = NULL;
	u32 wr = 0, cn = 0, np = 0, nv = 0, ready = 0;

	if (P.cutx || P.cuty)
		return i; // (PS2-HW-236: the blocks of a batch are cut by vu_poly_blk; the earlier collection leaves such a plan to the EE)
	while (i < nfans)
	{
		const u32 n = desc[3 * i + 1];
		const FOutVector *fv = base + desc[3 * i];
		const u32 need = 2 + 2 * n;
		qw_t *o, *v;
		float sb = 0.0f, tb = 0.0f;
		u32 k;

		if (vu_over_limit())
			break;

		if (!ready || n - 3 > VU_MAXVERT - 3 || cn + need > VU_CHUNK_QW || wr + need + 3 > BUF_QW)
		{
			if (ready)
			{
				H.wr = wr;
				VU.chunk_n = cn;
				VU.npoly += np;
				H.st.polys += np;
				H.st.verts_in += nv;
				np = nv = 0;
				ready = 0;
			}
			if (!vu_poly(fv, (int)n, (float)(int)desc[3 * i + 2]))
				break;
			i++;
			if (VU.open && !VU.kind && VU.consts_ok && P.serial == H.serial)
			{
				ring = H.ring[H.cur];
				wr = H.wr;
				cn = VU.chunk_n;
				ready = 1;
			}
			continue;
		}
		o = ring + wr;
		v = o + 2;
		if (wrap)
		{
			float smin = fv[0].s, smax = smin, tmin = fv[0].t, tmax = tmin;

			for (k = 0; k < n; k++, v += 2)
			{
				const float s = fv[k].s, t = fv[k].t;

				v[0].f[0] = fv[k].x;
				v[0].f[1] = fv[k].y;
				v[0].f[2] = fv[k].z;
				v[1].f[0] = s;
				v[1].f[1] = t;
				smin = hw_minf(smin, s);
				smax = hw_maxf(smax, s);
				tmin = hw_minf(tmin, t);
				tmax = hw_maxf(tmax, t);
			}
			if (fbits(smax - smin) > ilx || fbits(tmax - tmin) > ily)
				break; // cut at whole repeats: cut_and_emit (nothing of this polygon was committed)
			sb = (wx && (fbits(smin) & 0x7FFFFFFF) > 0x40800000) ? floorf(smin) : 0.0f; // |smin| > 4
			tb = (wy && (fbits(tmin) & 0x7FFFFFFF) > 0x40800000) ? floorf(tmin) : 0.0f;
		}
		else
		{
			for (k = 0; k < n; k++, v += 2)
			{
				v[0].f[0] = fv[k].x;
				v[0].f[1] = fv[k].y;
				v[0].f[2] = fv[k].z;
				v[1].f[0] = fv[k].s;
				v[1].f[1] = fv[k].t;
			}
		}
		o[0].w[0] = n;
		o[0].f[1] = (float)(int)desc[3 * i + 2];
		o[1].f[0] = sb;
		o[1].f[1] = tb;
		o[1].f[2] = tsx;
		o[1].f[3] = tsy;
		np++;
		nv += n;
		wr += need;
		cn += need;
		if (vu_dump_frame && H.frame_no == vu_dump_frame)
		{
			vu_poly_seen++;
			vu_dump(fv, (int)n, light);
		}
		i++;
	}
	if (ready)
	{
		H.wr = wr;
		VU.chunk_n = cn;
		VU.npoly += np;
		H.st.polys += np;
		H.st.verts_in += nv;
	}
	return i;
}

// ---- PS2-HW-233: the polygons of a batch as blocks of the pool (hardware/hw_pbatch.inc), already in the form of the chunk ----
// A block is the polygon of the chunk (header, texture scale / repeat quadword, two quadwords a vertex) except for the quadword (sb, tb, tsx, tsy), which holds the extent of s, t
// the collection made (s min, t min, s extent, t extent) and is made here, when the plan (the texture's scale, the repeat limit of its size) is known. The vertices go into the ring
// with quadword moves.
static inline void vu_qcopy(qw_t *dst, const qw_t *src, u32 cnt)
{
#ifdef __mips__
	while (cnt >= 4)
	{
		__asm__ volatile(
			"lq $8, 0(%1)\n lq $9, 16(%1)\n lq $10, 32(%1)\n lq $11, 48(%1)\n"
			"sq $8, 0(%0)\n sq $9, 16(%0)\n sq $10, 32(%0)\n sq $11, 48(%0)\n"
			: : "r"(dst), "r"(src) : "$8", "$9", "$10", "$11", "memory");
		dst += 4;
		src += 4;
		cnt -= 4;
	}
	while (cnt--)
	{
		__asm__ volatile("lq $8, 0(%1)\n sq $8, 0(%0)\n" : : "r"(dst), "r"(src) : "$8", "memory");
		dst++;
		src++;
	}
#else
	memcpy(dst, src, (size_t)cnt * sizeof(qw_t));
#endif
}

// The plan's constants (the state of pass 0 and the constants of the program again when the plan is new, or the general path wrote GS state in between). 0: the plan cannot be drawn
// by the program (PS2-HW-236: the part vu_poly and vu_poly_blk share).
static inline int vu_pready(void)
{
	if (VU.open && VU.kind)
		vu_chunk_end(); // PS2-HW-255: the open chunk is a sprite chunk
	if (!VU.consts_ok || P.serial != H.serial)
	{
		if (!vu_plan_setup())
		{
			P.vu = 0;
			P.vuok = 0;
			VU.stat_fallback++;
			return 0;
		}
	}
	return 1;
}

// The chunk's begin and room for one more polygon of `need` quadwords (the plan is ready); the position to write at.
static qw_t *vu_pbegin(u32 need)
{
	if (VU.open && VU.kind)
		vu_chunk_end(); // PS2-HW-255
	if (VU.open && (VU.chunk_n + need > VU_CHUNK_QW || H.wr + need + 3 > BUF_QW))
		vu_chunk_end();
	if (!VU.open)
		vu_chunk_begin();
	if (H.wr + need + 3 > BUF_QW)
	{
		vu_chunk_end();
		pk_flush();
		vu_chunk_begin();
	}
	return H.ring[H.cur] + H.wr;
}

static inline void vu_pcommit(u32 need, u32 n)
{
	H.st.polys++;
	H.st.verts_in += n;
	H.wr += need;
	VU.chunk_n += need;
	VU.npoly++;
}

// ---- PS2-HW-236: polygons that must be cut at whole repeats of the texture ----
// Before, a texture whose side is not a power of two (the GS can repeat only a power of two) and a polygon that covers more than UV_EXTENT texels of a power of two texture were left to the EE:
// cut_and_emit transformed and clipped the polygon, cut it along the repeats (clip_attr2 on every cut), and transformed, clipped and packed every piece again (1.4 K cycles for a wall of a
// 128x48 texture that needs no cut at all, 25..79 K for a 256x256 floor of 120 x 87 repeats; 0.1 / 0.5 / 0.7 M cycles a frame in DEMO_001 / 003 / 004). Now the EE cuts in object space (no
// transform; the attributes are affine) and the pieces go into the chunk as polygons of their own, with the whole repeats of the piece in sb / tb; the program transforms, clips and lights them.
// The cuts are those of cut_and_emit (stage 0: s, stage 1: t, the repeat limit of the size of the texture level); a polygon with many pieces is first transformed and clipped to the guard band
// like the EE path (vu0_xform / clip_poly): only the strips its visible part covers are cut. A polygon with more vertices than the program takes goes in as a fan. Anything that does not fit
// (too many pieces, too many vertices) is left to the EE path with nothing of it written.
typedef struct
{
	float x, y, z, s, t;
} vc_v_t;

#define VC_MAXV 32 // vertices of a polygon in the cutter (a cell of a polygon of 22 or fewer vertices has 26 or fewer)
#define VC_MAXPIECES 128 // cells of one polygon

static vc_v_t vc_poly[VC_MAXV], vc_s[VC_MAXV], vc_c[VC_MAXV], vc_r1[VC_MAXV], vc_r2[VC_MAXV], vc_t1[VC_MAXV], vc_t2[VC_MAXV];

// the part of in[] with attribute < cut (lo) and >= cut (hi), the point on an edge from the end with the smaller value (both parts get the same vertex): clip_attr2 on object space vertices
static void vc_split(const vc_v_t *in, int n, vc_v_t *lo, int *nlo, vc_v_t *hi, int *nhi, int on_t, float cut)
{
	int i, ml = 0, mh = 0;

	for (i = 0; i < n; i++)
	{
		const vc_v_t *v0 = &in[i], *v1 = &in[i + 1 == n ? 0 : i + 1];
		const float f0 = on_t ? v0->t : v0->s, f1 = on_t ? v1->t : v1->s;
		const int l0 = f0 < cut, l1 = f1 < cut, h0 = f0 >= cut, h1 = f1 >= cut;

		if (l0)
			lo[ml++] = *v0;
		if (h0)
			hi[mh++] = *v0;
		if (l0 != l1 || h0 != h1)
		{
			const vc_v_t *a = f0 < f1 ? v0 : v1, *b = f0 < f1 ? v1 : v0;
			const float fa = f0 < f1 ? f0 : f1, fb = f0 < f1 ? f1 : f0, tt = (cut - fa) / (fb - fa);
			vc_v_t o;

			o.x = a->x + (b->x - a->x) * tt;
			o.y = a->y + (b->y - a->y) * tt;
			o.z = a->z + (b->z - a->z) * tt;
			o.s = a->s + (b->s - a->s) * tt;
			o.t = a->t + (b->t - a->t) * tt;
			if (on_t)
				o.t = cut;
			else
				o.s = cut;
			if (l0 != l1)
				lo[ml++] = o;
			if (h0 != h1)
				hi[mh++] = o;
		}
	}
	*nlo = ml;
	*nhi = mh;
}

// a polygon of the object space vertices v[0..n) into the chunk, as a fan of polygons of VU_MAXVERT vertices or fewer if it has more (the plan is ready)
static void vc_put(const vc_v_t *v, int n, u32 lightbits, float sb, float tb)
{
	int start = 1;

	while (start < n - 1)
	{
		const int m = n - start < VU_MAXVERT - 1 ? n - start : VU_MAXVERT - 1; // the vertex 0 and m more
		const u32 need = 2 + 2 * ((u32)m + 1);
		qw_t *o = vu_pbegin(need), *q = o + 2;
		int k;

		q[0].f[0] = v[0].x;
		q[0].f[1] = v[0].y;
		q[0].f[2] = v[0].z;
		q[1].f[0] = v[0].s;
		q[1].f[1] = v[0].t;
		q += 2;
		for (k = 0; k < m; k++, q += 2)
		{
			q[0].f[0] = v[start + k].x;
			q[0].f[1] = v[start + k].y;
			q[0].f[2] = v[start + k].z;
			q[1].f[0] = v[start + k].s;
			q[1].f[1] = v[start + k].t;
		}
		o[0].w[0] = (u32)m + 1;
		o[0].w[1] = lightbits;
		o[1].f[0] = sb;
		o[1].f[1] = tb;
		o[1].f[2] = P.tsx;
		o[1].f[3] = P.tsy;
		vu_pcommit(need, (u32)m + 1);
		G.vc_pieces++;
		start += m - 1;
	}
}

// The strips of one axis. cut: the axis is cut (a texture side that is not a power of two: at every repeat; a power of two texture: when the polygon covers more than UV_EXTENT texels, every
// `step` repeats); strip j covers [base + step * j, base + step * (j + 1)]; j0..j1 are the strips drawn.
typedef struct
{
	int cut;
	float base, step;
	int j0, j1;
} vc_axis_t;

static void vc_axis(vc_axis_t *ax, float lo, float hi, int npot, int wrap, float texsize)
{
	ax->cut = 0;
	ax->step = 1.0f;
	ax->base = 0.0f;
	ax->j0 = ax->j1 = 0;
	if (!(npot || (wrap && (hi - lo) * texsize > UV_EXTENT)))
		return;
	ax->cut = 1;
	if (!npot)
	{
		ax->step = floorf(UV_EXTENT / texsize);
		if (ax->step < 1.0f)
			ax->step = 1.0f;
	}
	ax->base = floorf(lo);
	while (ax->base + ax->step * (float)(ax->j1 + 1) < hi && ax->j1 < 100000) // (the cuts are at base + step * (j + 1) while below hi, as cut_and_emit makes them)
		ax->j1++;
}

// The polygon of the block cut and written (the plan is ready). Returns 1 when it is drawn (or nothing of it is seen), 0 when the EE path has to do it (nothing was written).
static int vu_cut_blk_run(const qw_t *b);

static int vu_cut_blk(const qw_t *b)
{
	const u32 t0 = cyc();
	const int r = vu_cut_blk_run(b);

	G.vc_n++;
	G.vc_cyc += cyc() - t0;
	if (!r)
		G.vc_ee++;
	return r;
}

static int vu_cut_blk_run(const qw_t *b)
{
	const int n = (int)b[0].w[0];
	const u32 lightbits = b[0].w[1];
	vc_axis_t ax_s, ax_t;
	float smin, smax, tmin, tmax, sb_one, tb_one;
	int i;

	if (n < 3 || n > VC_MAXV - 10 || !P.r)
		return 0;
	for (i = 0; i < n; i++)
	{
		vc_poly[i].x = b[2 + 2 * i].f[0];
		vc_poly[i].y = b[2 + 2 * i].f[1];
		vc_poly[i].z = b[2 + 2 * i].f[2];
		vc_poly[i].s = b[3 + 2 * i].f[0];
		vc_poly[i].t = b[3 + 2 * i].f[1];
	}
	smin = smax = vc_poly[0].s;
	tmin = tmax = vc_poly[0].t;
	for (i = 1; i < n; i++)
	{
		smin = hw_minf(smin, vc_poly[i].s);
		smax = hw_maxf(smax, vc_poly[i].s);
		tmin = hw_minf(tmin, vc_poly[i].t);
		tmax = hw_maxf(tmax, vc_poly[i].t);
	}
	vc_axis(&ax_s, smin, smax, P.cutx, P.wrapx, (float)P.r->w);
	vc_axis(&ax_t, tmin, tmax, P.cuty, P.wrapy, (float)P.r->h);
	if (((ax_s.cut ? ax_s.j1 + 1 : 1) * (ax_t.cut ? ax_t.j1 + 1 : 1)) > 4)
	{
		// many pieces: only the strips that the visible part of the polygon covers (the EE path clipped first and cut the visible part)
		FOutVector tmp[VC_MAXV];
		cv_t *cvv;
		int cnt, and_ = 0x3F, or_ = 0;
		const float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);
		float vs0, vs1, vt0, vt1;

		for (i = 0; i < n; i++)
		{
			int oc;

			tmp[i].x = vc_poly[i].x;
			tmp[i].y = vc_poly[i].y;
			tmp[i].z = vc_poly[i].z;
			tmp[i].s = vc_poly[i].s;
			tmp[i].t = vc_poly[i].t;
			xform_vertex(&tmp[i], NULL, &vbuf_a[i]);
			oc = vbuf_a[i].w <= 0.0f ? 0x3F : outcode(&vbuf_a[i], gx, gy);
			and_ &= oc;
			or_ |= oc;
		}
		if (and_)
			return 1; // wholly outside the view volume: nothing to draw (the EE path rejects it too)
		cvv = vbuf_a;
		cnt = n;
		if (or_)
		{
			cnt = clip_poly(vbuf_a, vbuf_b, n, 2, or_, &cvv);
			if (cnt < 3)
				return 1;
		}
		vs0 = vs1 = cvv[0].a[0];
		vt0 = vt1 = cvv[0].a[1];
		for (i = 1; i < cnt; i++)
		{
			vs0 = hw_minf(vs0, cvv[i].a[0]);
			vs1 = hw_maxf(vs1, cvv[i].a[0]);
			vt0 = hw_minf(vt0, cvv[i].a[1]);
			vt1 = hw_maxf(vt1, cvv[i].a[1]);
		}
		if (ax_s.cut)
		{
			const int last = ax_s.j1, j0 = (int)((vs0 - ax_s.base) / ax_s.step), j1 = (int)((vs1 - ax_s.base) / ax_s.step);

			ax_s.j0 = j0 < 0 ? 0 : j0 > last ? last : j0;
			ax_s.j1 = j1 < ax_s.j0 ? ax_s.j0 : j1 > last ? last : j1;
		}
		if (ax_t.cut)
		{
			const int last = ax_t.j1, j0 = (int)((vt0 - ax_t.base) / ax_t.step), j1 = (int)((vt1 - ax_t.base) / ax_t.step);

			ax_t.j0 = j0 < 0 ? 0 : j0 > last ? last : j0;
			ax_t.j1 = j1 < ax_t.j0 ? ax_t.j0 : j1 > last ? last : j1;
		}
	}
	if (((ax_s.cut ? ax_s.j1 - ax_s.j0 + 1 : 1) * (ax_t.cut ? ax_t.j1 - ax_t.j0 + 1 : 1)) > VC_MAXPIECES)
		return 0; // more cells than the cutter holds: the EE path
	sb_one = (P.wrapx && (fbits(smin) & 0x7FFFFFFF) > 0x40800000) ? floorf(smin) : 0.0f; // an axis that is not cut: the base of vu_poly (|s| > 4)
	tb_one = (P.wrapy && (fbits(tmin) & 0x7FFFFFFF) > 0x40800000) ? floorf(tmin) : 0.0f;
	{
		vc_v_t *const sbuf[2] = {vc_r1, vc_r2};
		vc_v_t *const tbuf[2] = {vc_t1, vc_t2};
		const vc_v_t *srem = vc_poly;
		int nsrem = n, sw = 0, js;

		if (ax_s.cut && ax_s.j0 > 0)
		{
			int nlo, nhi;

			vc_split(srem, nsrem, vc_s, &nlo, sbuf[sw], &nhi, 0, ax_s.base + ax_s.step * (float)ax_s.j0);
			srem = sbuf[sw];
			nsrem = nhi;
			sw ^= 1;
		}
		for (js = ax_s.cut ? ax_s.j0 : 0; nsrem >= 3; js++)
		{
			const int last_s = !ax_s.cut || js >= ax_s.j1;
			const float sb = ax_s.cut ? ax_s.base + ax_s.step * (float)js : sb_one;
			const vc_v_t *piece;
			int npc, nnext = 0;

			if (!last_s)
			{
				vc_split(srem, nsrem, vc_s, &npc, sbuf[sw], &nnext, 0, ax_s.base + ax_s.step * (float)(js + 1));
				piece = vc_s;
			}
			else
			{
				piece = srem;
				npc = nsrem;
			}
			if (npc >= 3)
			{
				const vc_v_t *trem = piece;
				int ntrem = npc, tw = 0, kt;

				if (ax_t.cut && ax_t.j0 > 0)
				{
					int nlo, nhi;

					vc_split(trem, ntrem, vc_c, &nlo, tbuf[tw], &nhi, 1, ax_t.base + ax_t.step * (float)ax_t.j0);
					trem = tbuf[tw];
					ntrem = nhi;
					tw ^= 1;
				}
				for (kt = ax_t.cut ? ax_t.j0 : 0; ntrem >= 3; kt++)
				{
					const int last_t = !ax_t.cut || kt >= ax_t.j1;
					const float tb = ax_t.cut ? ax_t.base + ax_t.step * (float)kt : tb_one;
					const vc_v_t *cell;
					int nc, ntnext = 0;

					if (!last_t)
					{
						vc_split(trem, ntrem, vc_c, &nc, tbuf[tw], &ntnext, 1, ax_t.base + ax_t.step * (float)(kt + 1));
						cell = vc_c;
					}
					else
					{
						cell = trem;
						nc = ntrem;
					}
					if (nc >= 3)
						vc_put(cell, nc, lightbits, sb, tb);
					if (last_t)
						break;
					trem = tbuf[tw];
					ntrem = ntnext;
					tw ^= 1;
				}
			}
			if (last_s)
				break;
			srem = sbuf[sw];
			nsrem = nnext;
			sw ^= 1;
		}
	}
	return 1;
}

// one block through the general entry (the plan's constants, the chunk's begin and end): the same as vu_poly for a polygon of the engine's vertices, and the cuts and the fan splits.
// 1 = taken (drawn or nothing to draw), 0 = the EE path has to draw it (nothing was written).
static int vu_poly_blk(const qw_t *b)
{
	const u32 n = b[0].w[0];
	float sb = 0.0f, tb = 0.0f;
	u32 need;
	qw_t *o;

	if (n < 3 || !vu_pready())
		return 0;
	if (vu_nocut)
	{
		if (n > VU_MAXVERT || P.cutx || P.cuty)
			return 0; // (-hwcut 0: the earlier rule, the EE path cuts and splits)
		if (P.textured && (P.wrapx || P.wrapy) && ((P.wrapx && b[1].f[2] * (float)P.r->w > UV_EXTENT) || (P.wrapy && b[1].f[3] * (float)P.r->h > UV_EXTENT)))
			return 0;
	}
	if (P.cutx || P.cuty)
	{
		// a texture side that is not a power of two: every polygon has its own repeat base; one that lies inside one repeat (with a margin) of every such axis is one piece (a wall of a
		// 128x48 texture: sb = 0, tb = the whole repeats of t), the others are cut
		const float smin = b[1].f[0], tmin = b[1].f[1];
		int one = n <= VU_MAXVERT;

		if (P.cutx)
		{
			sb = floorf(smin);
			one &= ((smin - sb) + b[1].f[2]) < 0.9999f;
		}
		else
		{
			sb = (P.wrapx && (fbits(smin) & 0x7FFFFFFF) > 0x40800000) ? floorf(smin) : 0.0f;
			one &= !(P.wrapx && b[1].f[2] * (float)P.r->w > UV_EXTENT);
		}
		if (P.cuty)
		{
			tb = floorf(tmin);
			one &= ((tmin - tb) + b[1].f[3]) < 0.9999f;
		}
		else
		{
			tb = (P.wrapy && (fbits(tmin) & 0x7FFFFFFF) > 0x40800000) ? floorf(tmin) : 0.0f;
			one &= !(P.wrapy && b[1].f[3] * (float)P.r->h > UV_EXTENT);
		}
		if (!one)
			return vu_cut_blk(b);
	}
	else
	{
		if (P.textured && (P.wrapx || P.wrapy) && ((P.wrapx && b[1].f[2] * (float)P.r->w > UV_EXTENT) || (P.wrapy && b[1].f[3] * (float)P.r->h > UV_EXTENT)))
			return vu_cut_blk(b); // cut at whole repeats
		if (P.textured && (P.wrapx || P.wrapy))
		{
			const float smin = b[1].f[0], tmin = b[1].f[1];

			sb = (P.wrapx && (fbits(smin) & 0x7FFFFFFF) > 0x40800000) ? floorf(smin) : 0.0f; // |smin| > 4
			tb = (P.wrapy && (fbits(tmin) & 0x7FFFFFFF) > 0x40800000) ? floorf(tmin) : 0.0f;
		}
	}
	if (n > VU_MAXVERT)
	{
		// more vertices than the program takes: a fan of polygons of VU_MAXVERT vertices or fewer, all with the vertex 0 (the engine's polygon is convex; the shared diagonals are the same vertices)
		vc_v_t pv[VC_MAXV];
		u32 i;

		if (n > VC_MAXV - 10)
			return 0;
		for (i = 0; i < n; i++)
		{
			pv[i].x = b[2 + 2 * i].f[0];
			pv[i].y = b[2 + 2 * i].f[1];
			pv[i].z = b[2 + 2 * i].f[2];
			pv[i].s = b[3 + 2 * i].f[0];
			pv[i].t = b[3 + 2 * i].f[1];
		}
		vc_put(pv, (int)n, b[0].w[1], sb, tb);
		return 1;
	}
	need = 2 + 2 * n;
	o = vu_pbegin(need);
	vu_qcopy(o + 2, b + 2, 2 * n);
	o[0].w[0] = n;
	o[0].w[1] = b[0].w[1]; // the light level (a float)
	o[1].f[0] = sb;
	o[1].f[1] = tb;
	o[1].f[2] = P.tsx;
	o[1].f[3] = P.tsy;
	vu_pcommit(need, n);
	return 1;
}

// The blocks blks[i..] of one batch, the loop of vu_fans: the values of the plan in locals, the position in the chunk in locals. Returns the index of the first block that was not taken.
static u32 vu_blocks(const qw_t *pool, const u32 *blks, u32 i, u32 nfans)
{
	const int wrap = P.textured && (P.wrapx || P.wrapy);
	const int wx = P.wrapx, wy = P.wrapy;
	const float tsx = P.tsx, tsy = P.tsy;
	const int ilx = wx ? fbits(UV_EXTENT / (float)P.r->w) : 0x7F7FFFFF, ily = wy ? fbits(UV_EXTENT / (float)P.r->h) : 0x7F7FFFFF; // (PS2-HW-228)
	const int cutplan = P.cutx || P.cuty; // (PS2-HW-236: every polygon takes the general entry: its own repeat base, the cuts)
	qw_t *ring = NULL;
	u32 wr = 0, cn = 0, np = 0, nv = 0, ready = 0;

	while (i < nfans)
	{
		const qw_t *b = pool + blks[i];
		const u32 n = b[0].w[0];
		const u32 need = 2 + 2 * n;
		qw_t *o;
		float sb = 0.0f, tb = 0.0f;

		if (cutplan || !ready || n - 3 > VU_MAXVERT - 3 || cn + need > VU_CHUNK_QW || wr + need + 3 > BUF_QW || (wrap && (fbits(b[1].f[2]) > ilx || fbits(b[1].f[3]) > ily)))
		{
			if (ready)
			{
				H.wr = wr;
				VU.chunk_n = cn;
				VU.npoly += np;
				H.st.polys += np;
				H.st.verts_in += nv;
				np = nv = 0;
				ready = 0;
			}
			if (!vu_poly_blk(b))
				break;
			i++;
			if (VU.open && !VU.kind && VU.consts_ok && P.serial == H.serial)
			{
				ring = H.ring[H.cur];
				wr = H.wr;
				cn = VU.chunk_n;
				ready = 1;
			}
			continue;
		}
		if (wrap)
		{
			const float smin = b[1].f[0], tmin = b[1].f[1];

			sb = (wx && (fbits(smin) & 0x7FFFFFFF) > 0x40800000) ? floorf(smin) : 0.0f; // |smin| > 4
			tb = (wy && (fbits(tmin) & 0x7FFFFFFF) > 0x40800000) ? floorf(tmin) : 0.0f;
		}
		o = ring + wr;
		vu_qcopy(o + 2, b + 2, 2 * n);
		o[0].w[0] = n;
		o[0].w[1] = b[0].w[1];
		o[1].f[0] = sb;
		o[1].f[1] = tb;
		o[1].f[2] = tsx;
		o[1].f[3] = tsy;
		np++;
		nv += n;
		wr += need;
		cn += need;
		i++;
	}
	if (ready)
	{
		H.wr = wr;
		VU.chunk_n = cn;
		VU.npoly += np;
		H.st.polys += np;
		H.st.verts_in += nv;
	}
	return i;
}

#else // host test build: no VU1
static u32 vu_dump_frame, vu_max_poly, vu_stop_frame;
static int vu1_boot(void)
{
	return 0;
}

static void vu1_shutdown(void)
{
}

static inline void vu_sync(void)
{
}

static void vu_chunk_flush(void)
{
}

static inline int vu_poly(const FOutVector *base, int n, float light)
{
	(void)base;
	(void)n;
	(void)light;
	return 0;
}

static inline u32 vu_fans(const FOutVector *base, const u32 *desc, u32 i, u32 nfans)
{
	(void)base;
	(void)desc;
	(void)nfans;
	return i;
}

static inline u32 vu_blocks(const qw_t *pool, const u32 *blks, u32 i, u32 nfans)
{
	(void)pool;
	(void)blks;
	(void)nfans;
	return i;
}
#endif
