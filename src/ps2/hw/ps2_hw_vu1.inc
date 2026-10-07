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

#define VU_MAXVERT 12 // vertices of a polygon the program takes (20 per list, 6 planes add up to 6)
#define VU_CHUNK_QW 255 // UNPACK count is 8 bits; the VU1 input buffers hold 256
#define VU_MIN_FANS 1 // a plan with fewer polygons than this is not worth the constants and the synchronisation
#define VU_NCONST 17 // constant quad words (vu1/ps2_hw_vu1.vsm, K0..K16)
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
	int consts_ok; // the constants of the current plan are in the VU1 memory
	int rowmode; // the plan is lit by the colormap rows of a light table (P.pal)
	int rlo, rhi; // the rows the program may meet (their TEX0 are in the table)
	float cn[VU_NCONST][4] __attribute__((aligned(16)));
	u32 rt[32][4] __attribute__((aligned(16))); // TEX2 of every row (lo, hi, register 0x16, 0)
	int tab_tbl, tab_keyed, tab_lo, tab_hi; // the rows [tab_lo, tab_hi] of this table are in the VU1 memory (tab_hi < 0: none) ...
	u32 tab_epoch, tab_ltgen, tab_palgen; // ... as made for this CLUT epoch / table generation / palette
	float mp[16] __attribute__((aligned(16))); // M' = diag(1/gx, 1/gy, 1, 1) * mvp
	u32 mp_gx, mp_gy, mp_ver;
	u32 stat_polys, stat_chunks, stat_fallback, stat_plans, stat_rowfail;
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

	if (ninstr == 0 || ninstr > 2048 || (ninstr & 1))
		return 0;
	memset(prow_slot, 0, sizeof prow_slot); // the CLUT cache starts empty with every Init
	for (i = 0; i < PR_TBL * 2; i++)
		((prowv_t *)(void *)prowv)[i].hi = -1;
	VU.tab_hi = -1;
	*VIF1_FBRST = 1; // reset the VIF
	__asm__ volatile("sync.l");
	qwc = 1 + nmpg + ninstr / 2 + 13 + 1 + 1;
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
	CONS_Printf("HWD VU1: program of %u instructions uploaded, entries init %u draw %u\n", (unsigned)ninstr, (unsigned)vu_entry(VU1Draw_EntryInit), (unsigned)vu_entry(VU1Draw_EntryDraw));
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

	if (same && v->epoch == H.clut_epoch && rlo >= v->lo && rhi <= v->hi)
		return;
	if (same)
	{
		lo = lo < v->lo ? lo : v->lo; // the union: a plan with another light level meets other rows of the same table
		hi = hi > v->hi ? hi : v->hi;
	}
	for (row = lo; row <= hi; row++)
		prow_blk(tbl, keyed, (u32)row);
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

// light equation of the plan (the constants of lf_params and doom_depth_of_level): row = clamp(floor(max(base - k80 / (lightz + 1), cap)), 0, 31)
typedef struct
{
	float base, cap, k80, near_w, kz16, invkz16;
	int rlo, rhi;
} vlight_t;

static void vu_light(vlight_t *vl)
{
	const lightp_t *lp = &P.rs.lp;
	float lightnum = lp->light / 17.0f, startmap, kz, c0, c1;

	lightnum = lightnum < 0.0f ? 0.0f : lightnum > 15.0f ? 15.0f : lightnum;
	startmap = (15.0f - lightnum) * 4.0f;
	vl->base = startmap * lp->sfudge;
	vl->cap = (155.0f - lp->light) * 0.26f;
	vl->k80 = 80.0f * lp->cfudge;
	vl->near_w = H.near_plane;
	kz = FAR_CLIPPING_PLANE / (FAR_CLIPPING_PLANE - H.near_plane);
	vl->kz16 = kz * (1.0f / 16.0f);
	vl->invkz16 = 16.0f / kz;
	c0 = lmaxf(vl->base - vl->k80, vl->cap); // lightz = 0 (the nearest the equation goes)
	c1 = lmaxf(vl->base - vl->k80 * (1.0f / 128.0f), vl->cap); // lightz = 127 (everything farther is the same row)
	vl->rlo = cm_row(c0);
	vl->rhi = cm_row(c1);
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
	VU.cn[11][1] = VU.cn[11][2] = 0.0f;
	VU.cn[11][3] = 1e-30f;
	if (vl)
	{
		VU.cn[12][0] = vl->near_w;
		VU.cn[12][1] = vl->k80;
		VU.cn[12][2] = vl->cap;
		VU.cn[12][3] = vl->base;
		VU.cn[13][0] = vl->kz16;
		VU.cn[13][1] = vl->invkz16;
		VU.cn[13][2] = 127.0f;
		VU.cn[13][3] = (float)vl->rhi;
		VU.cn[14][2] = -vl->k80;
	}
	else
	{
		VU.cn[12][0] = VU.cn[12][1] = VU.cn[12][2] = VU.cn[12][3] = 0.0f;
		VU.cn[13][0] = VU.cn[13][1] = 0.0f;
		VU.cn[13][2] = 127.0f;
		VU.cn[13][3] = 0.0f;
		VU.cn[14][2] = 0.0f;
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
// VU1 memory does not hold the rows of this table (rows [rlo, rhi])
static void vu_consts_send(int send_rows)
{
	qw_t *b, *t;
	const u32 nrow = send_rows ? (u32)(VU.rhi - VU.rlo + 1) : 0;

	vu_room(1 + VU_NCONST + 1 + nrow + 2);
	t = vu_tag_slot();
	b = H.ring[H.cur];
	t->d[0] = dma_tag(VU_NCONST, DMAT_CNT, 0);
	t->d[1] = 0x10000000ull | ((u64)VIF_UNPACK_V4_32(0, VU_NCONST, 0) << 32); // FLUSHE
	memcpy(b + H.wr, VU.cn, VU_NCONST * sizeof(qw_t));
	H.wr += VU_NCONST;
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
}

// The plan of the batch for the VU1 program: rows in VRAM, GS state, constants. 0 = the VU1 path cannot take it (the general path draws it).
static int vu_plan_setup(void)
{
	vlight_t vl;
	const texrec_t *r = P.r;
	int send_rows = 0;

	vu_sync();
	VU.rowmode = P.pal;
	VU.stat_plans++;
	if (VU.rowmode)
	{
		const u32 tbl = P.pal_tbl;
		const int keyed = r && r->clut == 2;

		if (tbl >= PR_TBL || !r || r->psm != PSM_T8)
		{
			VU.stat_rowfail++;
			return 0;
		}
		vu_light(&vl);
		VU.rlo = vl.rlo;
		VU.rhi = vl.rhi;
		prow_ensure(tbl, keyed, vl.rlo, vl.rhi);
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
	pass_setup(0, 0);
	P.serial = H.serial;
	vu_consts_build(VU.rowmode ? &vl : NULL);
	vu_consts_send(send_rows);
	return 1;
}

static void vu_chunk_begin(void)
{
	qw_t *b, *t;

	vu_room(1 + 1 + 4 + 2);
	t = vu_tag_slot();
	b = H.ring[H.cur];
	VU.chunk_tag = (u32)(t - b);
	H.wr = (u32)(t - b) + 1;
	VU.chunk_n = 1; // the chunk header: the number of polygons
	b[H.wr].d[0] = b[H.wr].d[1] = 0;
	H.wr++;
	VU.npoly = 0;
	VU.open = 1;
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
	m->d[1] = (u64)VIF_FLUSH | ((u64)VIF_MSCAL(vu_entry(VU1Draw_EntryDraw)) << 32);
	vu_next_run();
	VU.open = 0;
	VU.stat_chunks++;
	G.vu_chunks++;
	H.gsr.valid &= ~(u32)RV_RGBAQ; // the packets of the program change Q (and the colour) behind the A+D state cache
	if (VU.rowmode)
	{
		// the program loaded CLUTs: what the GS holds as TEX0 / CLUT is unknown now
		H.gsr.valid &= ~(u32)RV_TEX0;
		H.clut_loaded = 0;
		P.pal_row = -1;
	}
}

// Everything that goes into the ring in another way ends the chunk first (the order of the stream is the order of the draws).
static inline void vu_sync(void)
{
	if (VU.open)
		vu_chunk_end();
}

// Called by emit_poly (before anything else) for a plan that is VU1 capable. Returns 1 when the polygon was taken (drawn or dropped).
static int vu_poly(const FOutVector *base, int n)
{
	float smin = 0.0f, smax = 0.0f, tmin = 0.0f, tmax = 0.0f;
	float sb = 0.0f, tb = 0.0f;
	int i;
	u32 need;
	qw_t *o, *b;

	if (n < 3 || n > VU_MAXVERT)
		return 0;
	if (P.textured && (P.wrapx || P.wrapy))
	{
		smin = smax = base[0].s;
		tmin = tmax = base[0].t;
		for (i = 1; i < n; i++)
		{
			smin = hw_minf(smin, base[i].s);
			smax = hw_maxf(smax, base[i].s);
			tmin = hw_minf(tmin, base[i].t);
			tmax = hw_maxf(tmax, base[i].t);
		}
		if ((P.wrapx && (smax - smin) * (float)P.r->w > UV_EXTENT) || (P.wrapy && (tmax - tmin) * (float)P.r->h > UV_EXTENT))
			return 0; // cut at whole repeats: cut_and_emit
		sb = (P.wrapx && fabsf(smin) > 4.0f) ? floorf(smin) : 0.0f;
		tb = (P.wrapy && fabsf(tmin) > 4.0f) ? floorf(tmin) : 0.0f;
	}
	H.st.polys++;
	H.st.verts_in += (u32)n;
	if (!VU.consts_ok || P.serial != H.serial)
	{
		// first polygon of the plan, or the general path wrote GS state in between: the state of pass 0 and the constants again
		if (!vu_plan_setup())
		{
			P.vu = 0;
			P.vuok = 0;
			H.st.polys--;
			H.st.verts_in -= (u32)n;
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
	b = H.ring[H.cur];
	o = b + H.wr;
	o[0].w[0] = (u32)n;
	o[0].w[1] = o[0].w[2] = o[0].w[3] = 0;
	o[1].f[0] = sb;
	o[1].f[1] = tb;
	o[1].w[2] = o[1].w[3] = 0;
	for (i = 0; i < n; i++)
	{
		qw_t *v = o + 2 + 2 * i;

		v[0].f[0] = base[i].x;
		v[0].f[1] = base[i].y;
		v[0].f[2] = base[i].z;
		v[0].f[3] = 1.0f;
		v[1].f[0] = base[i].s;
		v[1].f[1] = base[i].t;
		v[1].w[2] = v[1].w[3] = 0;
	}
	H.wr += need;
	VU.chunk_n += need;
	VU.npoly++;
	VU.stat_polys++;
	return 1;
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

static inline int vu_poly(const FOutVector *base, int n)
{
	(void)base;
	(void)n;
	return 0;
}
#endif
