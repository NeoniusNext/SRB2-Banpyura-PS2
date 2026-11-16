// Isolated GS experiment only. Included after the real driver in the native
// fixture; no shipping source/layout change. All rectangles are half-open.
static int tn_stencil_context;

void TN_StencilContext(int context)
{
	tn_stencil_context = context == 2;
}

void TN_StencilDims(int *width, int *height, int *stride)
{
	*width = H.vw; *height = H.vh; *stride = H.fbw;
}

static void tn_stencil_state(u64 frame, u64 test, u64 zbuf, u32 rgba, int rows)
{
	const int c = tn_stencil_context;
	qw_t *p = ad_alloc(8);
	ad_set(&p[0], frame, GSR_FRAME_1 + c);
	ad_set(&p[1], test, GSR_TEST_1 + c);
	ad_set(&p[2], zbuf, GSR_ZBUF_1 + c);
	ad_set(&p[3], scissor_reg(0, 0, H.fbw - 1, rows - 1), GSR_SCISSOR_1 + c);
	ad_set(&p[4], (u64)(2048 * 16) | ((u64)(2048 * 16) << 32), 0x18 + c); // XYOFFSET_1/2
	ad_set(&p[5], rgbaq_of(rgba), GSR_RGBAQ);
	ad_set(&p[6], 0, 0x4a + c); // FBA_1/2: retain the requested alpha byte
	ad_set(&p[7], 1, 0x1a); // PRMODECONT: PRIM chooses the context and drawing flags
	H.gsr.valid = 0;
}

// Host->GS32 depth transfer. Ring owns every byte; source may be released when
// this function returns. This intentionally targets reserved Z, not the pool.
void TN_StencilSeed(const u32 *pixels)
{
	const u32 stride = (u32)H.fbw, rowbytes = stride * 4u;
	u32 row = 0;
	if (OV.n) ov_flush();
	frame_prepare(0);
	while (row < (u32)H.vh)
	{
		u32 nr = (65536u / rowbytes) & ~15u;
		if (!nr) nr = 1;
		if (nr > (u32)H.vh - row) nr = (u32)H.vh - row;
		const u32 nqw = (rowbytes * nr + 15u) / 16u;
		qw_t *p = pk_alloc(6u + nqw);
		p[0].d[0] = HWGIF_TAG(4, 0, 0, 0, 0, 1); p[0].d[1] = GIF_REGS_AD;
		ad_set(&p[1], ((u64)(H.z_addr / 256u) << 32) | ((u64)(stride / 64u) << 48) | ((u64)PSM_Z32 << 56), GSR_BITBLTBUF);
		ad_set(&p[2], (u64)row << 48, GSR_TRXPOS);
		ad_set(&p[3], (u64)stride | ((u64)nr << 32), GSR_TRXREG);
		ad_set(&p[4], 0, GSR_TRXDIR);
		p[5].d[0] = HWGIF_TAG(nqw, 1, 0, 0, 2, 0); p[5].d[1] = 0;
		memcpy(&p[6], pixels + (size_t)row * stride, (size_t)rowbytes * nr);
		if ((rowbytes * nr) & 15u) memset((u8 *)&p[6] + rowbytes * nr, 0, 16u - ((rowbytes * nr) & 15u));
		row += nr;
	}
	qw_t *p = ad_alloc(1); ad_set(p, 0, GSR_TEXFLUSH);
	H.gsr.valid = 0;
}

static void tn_stencil_sprite(int x0, int y0, int x1, int y1)
{
	qw_t *p = pk_alloc(3);
	p[0].d[0] = HWGIF_TAG(2, 1, 1, PRIM_SPRITE | ((u32)tn_stencil_context << 9), 0, 1);
	p[0].d[1] = GIF_REGS_XYZ;
	p[1].w[0] = (u32)((2048 + x0) * 16); p[1].w[1] = (u32)((2048 + y0) * 16); p[1].w[2] = 0; p[1].w[3] = 0;
	p[2].w[0] = (u32)((2048 + x1) * 16); p[2].w[1] = (u32)((2048 + y1) * 16); p[2].w[2] = 0; p[2].w[3] = 0;
}

// PSMZ32 block number = PSMCT32 block number XOR24. In a 64x32
// page, this is x XOR32, y XOR16; word/column addressing is unchanged.
// outside_high>255 leaves outside untouched, permitting ordered per-run masks.
void TN_StencilMask(int x0, int y0, int x1, int y1, u32 outside_high, u32 inside_high)
{
	const int rows = (H.fbh + 31) & ~31;
	const u64 frame = (u64)(H.z_addr / 8192u) | ((u64)(H.fbw / 64) << 16) | ((u64)PSM_CT32 << 24) | (0x00ffffffull << 32);
	const u64 test = (1ull << 16) | (1ull << 17); // ZTST ALWAYS, no alpha test
	const u64 zbuf = (u64)(H.z_addr / 8192u) | (1ull << 32); // PSMZ32, depth writes masked
	int y, x;
	if (OV.n) ov_flush();
	frame_prepare(0);
	if (outside_high <= 255u)
	{
		tn_stencil_state(frame, test, zbuf, outside_high << 24, rows);
		tn_stencil_sprite(0, 0, H.fbw, rows);
	}
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > H.vw) x1 = H.vw;
	if (y1 > H.vh) y1 = H.vh;
	if (x0 >= x1 || y0 >= y1) return;
	tn_stencil_state(frame, test, zbuf, (inside_high & 255u) << 24, rows);
	for (y = y0; y < y1;)
	{
		int ey = (y | 15) + 1;
		if (ey > y1) ey = y1;
		for (x = x0; x < x1;)
		{
			int ex = (x | 31) + 1;
			if (ex > x1) ex = x1;
			const int ax = x ^ 32, ay = y ^ 16;
			tn_stencil_sprite(ax, ay, ax + ex - x, ay + ey - y);
			x = ex;
		}
		y = ey;
	}
	H.gsr.valid = 0;
}

// This is one original full-scissor top-left triangle: (0,0),(vw,0),(0,vh).
// z32 is copied as all32 integer bits; sourcehigh=1 tests NoDepthTest routing.
void TN_StencilDraw(u32 z32, u32 rgba, int zwrite)
{
	const u64 test = (1ull << 16) | (2ull << 17); // GEQUAL
	const u64 zbuf = (u64)(H.z_addr / 8192u) | ((u64)(!zwrite) << 32);
	const u64 frame = frame_reg(H.target, 0);
	qw_t *p;
	frame_prepare(0);
	tn_stencil_state(frame, test, zbuf, rgba, H.vh);
	p = pk_alloc(4);
	p[0].d[0] = HWGIF_TAG(3, 1, 1, PRIM_TRI | ((u32)tn_stencil_context << 9), 0, 1); p[0].d[1] = GIF_REGS_XYZ;
	p[1].w[0] = 2048u * 16u; p[1].w[1] = 2048u * 16u; p[1].w[2] = z32; p[1].w[3] = 0;
	p[2].w[0] = (u32)(2048 + H.vw) * 16u; p[2].w[1] = 2048u * 16u; p[2].w[2] = z32; p[2].w[3] = 0;
	p[3].w[0] = 2048u * 16u; p[3].w[1] = (u32)(2048 + H.vh) * 16u; p[3].w[2] = z32; p[3].w[3] = 0;
	H.gsr.valid = 0;
}

int TN_ZRaw(u32 *dest)
{
	return PS2HWD_ReadVram(dest, H.z_addr / 256u, H.fbw / 64, PSM_Z32, 0, 0, H.fbw, H.vh);
}
