// PS2 GS hardware renderer driver: polygons drawn by the VU1 program (included by ps2_hwd.c after ps2_hw_fast.inc/ps2_hw_draw.inc). PS2-HW-45 (OPT9, HG).
//
// The EE does not transform, clip or pack the polygons of a batch (PS2HWD_DrawFans) any more: it copies the vertices as they are (x, y, z, 1 / s, t)
// into a chunk of VU1 data, the VIF unpacks the chunk into the VU1 data memory, and the program vu1/ps2_hw_vu1.vsm transforms, clips, divides and
// kicks the GIF packet of every polygon (see there for the formats). The chunk sits in the ring buffer between the runs of GIF data: UNPACK tag,
// data, zero length MSCAL tag (the VIF codes live in the tags of the DMA chain, ps2_hw_vif.inc).
// What stays on the EE decides whether a polygon may take the shortcut, with the checks of the general path (emit_poly / cut_and_emit): one
// pass, no texture repeat cut, one darkness class (lit_fast_poly's class function). Everything else, and every polygon whose vertices the
// plan cannot hold (more than 12), goes the old way; the stream keeps the order of the calls.

#ifdef __mips__
extern const u8 VU1Draw_CodeStart[] __attribute__((aligned(16)));
extern const u8 VU1Draw_CodeEnd[];
extern const u8 VU1Draw_EntryInit[];
extern const u8 VU1Draw_EntryDraw[];

#define VU_MAXVERT 12 // vertices of a polygon the program takes (20 per list, 6 planes add up to 6)
#define VU_CHUNK_QW 255 // UNPACK count is 8 bits; the VU1 input buffers hold 256
#define VU_MIN_FANS 3 // a plan with fewer polygons than this is not worth the constants and the synchronisation

static struct
{
	u32 chunk_tag; // index of the UNPACK tag of the open chunk in the buffer
	u32 chunk_n; // quadwords of the chunk so far (the header included)
	u32 npoly;
	int open;
	int consts_ok; // the constants of the current plan are in the VU1 memory
	float cn[12][4] __attribute__((aligned(16)));
	float m[16] __attribute__((aligned(16))); // the matrix the constants were made for
	float mp[16] __attribute__((aligned(16))); // M' = diag(1/gx, 1/gy, 1, 1) * mvp
	u32 mp_gx, mp_gy, mp_ver;
	u32 stat_polys, stat_chunks, stat_fallback;
} VU;

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
	*VIF1_FBRST = 1; // reset the VIF
	__asm__ volatile("sync.l");
	qwc = 1 + nmpg + ninstr / 2 + 1;
	pk = memalign(64, (size_t)qwc * sizeof(qw_t));
	if (!pk)
		return 0;
	memset(pk, 0, (size_t)qwc * sizeof(qw_t));
	pk[0].w[0] = VIF_NOP;
	pk[0].w[1] = VIF_STCYCL(1, 1);
	pk[0].w[2] = VIF_BASE(128);
	pk[0].w[3] = VIF_OFFSET(256);
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

// the constants of the plan: M', screen scale and offset, limits, texture scale, colour, the two GIF tags, the epsilon
static void vu_consts_build(void)
{
	const pass_t *ps = &P.pass[0];
	const float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);
	u64 t0 = HWGIF_TAG(0, 1, 1, PRIM_TRIFAN | (pass_prim(ps) & ~(u32)PRIM_FGE), 0, 3);
	u64 t1 = HWGIF_TAG(0, 1, 1, PRIM_TRIFAN | pass_prim(ps), 0, 3);
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
	w[2] = (u32)GIF_REGS_STQ_RGBA_XYZF;
	w[3] = 0;
	VU.cn[11][0] = VU.cn[11][1] = VU.cn[11][2] = 0.0f;
	VU.cn[11][3] = 1e-30f;
}

static void vu_chunk_end(void);

static void vu_consts_send(void)
{
	qw_t *b, *t;

	vu_room(1 + 12 + 2);
	vu_consts_build();
	t = vu_tag_slot();
	b = H.ring[H.cur];
	t->d[0] = dma_tag(12, DMAT_CNT, 0);
	t->d[1] = 0x10000000ull | ((u64)VIF_UNPACK_V4_32(0, 12, 0) << 32); // FLUSHE: the previous program is done with the constants
	memcpy(b + H.wr, VU.cn, 12 * sizeof(qw_t));
	H.wr += 12;
	vu_next_run();
	VU.consts_ok = 1;
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
	m->d[1] = (u64)VIF_MSCAL(vu_entry(VU1Draw_EntryDraw)) | ((u64)VIF_NOP << 32);
	vu_next_run();
	VU.open = 0;
	VU.stat_chunks++;
	G.vu_chunks++;
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
	const float *m = H.mvp;
	float wlo = 1e30f, whi = -1e30f, smin = 1e30f, smax = -1e30f, tmin = 1e30f, tmax = -1e30f;
	float sb = 0.0f, tb = 0.0f;
	int i, usefog = 0;
	u32 fogF = 255, need;
	qw_t *o, *b;

	if (n < 3 || n > VU_MAXVERT)
		return 0;
	for (i = 0; i < n; i++)
	{
		const float w = m[3] * base[i].x + m[7] * base[i].y + m[11] * base[i].z + m[15];

		wlo = hw_minf(wlo, w);
		whi = hw_maxf(whi, w);
		smin = hw_minf(smin, base[i].s);
		smax = hw_maxf(smax, base[i].s);
		tmin = hw_minf(tmin, base[i].t);
		tmax = hw_maxf(tmax, base[i].t);
	}
	if (P.textured && (P.wrapx || P.wrapy))
	{
		if ((P.wrapx && (smax - smin) * (float)P.r->w > UV_EXTENT) || (P.wrapy && (tmax - tmin) * (float)P.r->h > UV_EXTENT))
			return 0; // cut at whole repeats: cut_and_emit
		sb = (P.wrapx && fabsf(smin) > 4.0f) ? floorf(smin) : 0.0f;
		tb = (P.wrapy && fabsf(tmin) > 4.0f) ? floorf(tmin) : 0.0f;
	}
	if (P.litfast)
	{
		float lo = wlo;
		int c0;

		if (lo < H.near_plane)
			lo = H.near_plane; // what the near plane leaves is not nearer
		c0 = lf_class_w(lo);
		if (c0 != lf_class_w(whi))
			return 0; // two darkness classes: cut_and_emit makes the pieces
		if (!((LF.have >> c0) & 1))
		{
			int on;

			LF.fog[c0] = (u8)fog_of_class(&P.rs, c0, &on);
			LF.fogon[c0] = (u8)on;
			LF.have |= 1ull << c0;
		}
		fogF = LF.fog[c0];
		usefog = P.pass[0].fog && LF.fogon[c0];
	}
	H.st.polys++;
	H.st.verts_in += (u32)n;
	if (P.serial != H.serial)
	{
		vu_sync();
		pass_setup(0, 0); // the GS state of the plan, written once (and again after the general path wrote state in between)
		P.serial = H.serial;
	}
	if (!VU.consts_ok)
	{
		vu_sync();
		vu_consts_send();
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
	o[0].w[1] = (u32)usefog;
	o[0].w[2] = fogF << 4;
	o[0].w[3] = 0;
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
	H.gsr.valid &= ~(u32)RV_RGBAQ; // the packets of the program change Q behind the A+D state cache
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
