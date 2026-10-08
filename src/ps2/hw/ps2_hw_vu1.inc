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

	xf_update();
	w[0] = flags;
	if (surf)
	{
		w[1] = surf->PolyColor.rgba;
		w[2] = surf->TintColor.rgba;
		w[3] = surf->FadeColor.rgba;
		w[4] = surf->LightTableId;
		w[6] = (u32)surf->LightInfo.light_level;
		w[7] = (u32)surf->LightInfo.fade_start;
		w[8] = (u32)surf->LightInfo.fade_end;
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
	w[30] = (u32)H.target | (H.frame_no << 4) | ((u32)palette_mode() << 3);
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

// Called by emit_poly (before anything else) for a plan that is VU1 capable. Returns 1 when the polygon was taken (drawn or dropped).
// The polygon goes into the chunk in one pass over its vertices (PS2-HW-107: the extent of s, t for the texture repeat is found while the vertices are copied,
// the w lanes and the zero lanes are not written: the program does not read them; the header carries the texture scale of the polygon).
static int vu_poly(const FOutVector *base, int n, float light)
{
	float sb = 0.0f, tb = 0.0f;
	int i;
	u32 need;
	qw_t *o, *v;

	if (n < 3 || n > VU_MAXVERT)
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
		sb = (P.wrapx && fabsf(smin) > 4.0f) ? floorf(smin) : 0.0f;
		tb = (P.wrapy && fabsf(tmin) > 4.0f) ? floorf(tmin) : 0.0f;
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
	const float rw = wrap ? (float)P.r->w : 0.0f, rh = wrap ? (float)P.r->h : 0.0f;

	while (i < nfans)
	{
		const u32 n = desc[3 * i + 1];
		const FOutVector *fv = base + desc[3 * i];
		const float light = (float)(int)desc[3 * i + 2];
		const u32 need = 2 + 2 * n;
		qw_t *o, *v;
		float sb = 0.0f, tb = 0.0f;
		u32 k;

		if (!VU.open || VU.kind || !VU.consts_ok || P.serial != H.serial || n - 3 > VU_MAXVERT - 3 || VU.chunk_n + need > VU_CHUNK_QW || H.wr + need + 3 > BUF_QW)
		{
			if (!vu_poly(fv, (int)n, light))
				break;
			i++;
			continue;
		}
		o = H.ring[H.cur] + H.wr;
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
			if ((wx && (smax - smin) * rw > UV_EXTENT) || (wy && (tmax - tmin) * rh > UV_EXTENT))
				break; // cut at whole repeats: cut_and_emit (nothing of this polygon was committed)
			sb = (wx && fabsf(smin) > 4.0f) ? floorf(smin) : 0.0f;
			tb = (wy && fabsf(tmin) > 4.0f) ? floorf(tmin) : 0.0f;
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
		o[0].f[1] = light;
		o[1].f[0] = sb;
		o[1].f[1] = tb;
		o[1].f[2] = tsx;
		o[1].f[3] = tsy;
		H.st.polys++;
		H.st.verts_in += n;
		H.wr += need;
		VU.chunk_n += need;
		VU.npoly++;
		i++;
	}
	return i;
}

#else // host test build: no VU1
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
#endif
