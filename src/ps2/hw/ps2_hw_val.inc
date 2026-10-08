// PS2 GS hardware renderer driver: GIF stream validator (included by ps2_hwd.c after ps2_hw_gs.inc). PS2-HW-74 (OPT10, HF).
//
// -hwdbg 536870912 (HWDBG_VALIDATE): every buffer of the GIF ring is parsed as the GIF and the GS parse it, right before it is queued, and checked
// against the rules a real GS keeps and PCSX2 forgives or models differently: register fields keep to their bit widths, image data is exactly what
// TRXREG announces, the texture cache is flushed (TEXFLUSH) after local memory changed under a texture, vertices stay in the 12.4 fixed point
// window, Z24 stays below 2^24, texture coordinates stay in the range the driver cuts for (UV_EXTENT), DMA tags and references are well formed.
// Results: the first examples of every class as HWVAL lines when they happen, and a summary (HWVAL SUMMARY) every 300 frames and at shutdown.
// The VU1 path (-hwdbg 0x4000000) is not checked: its GIF packets are made by the VU1 program and never pass the ring.

#define HWDBG_VALIDATE 0x20000000 // bit 27 is HWDBG_LODDBG of the planner (HT), bit 28 HWDBG_IMMDBG

#include <stdarg.h>
#include <stdio.h>

enum
{
	VE_DMATAG, VE_GIFTAG, VE_PRIM, VE_PRIMCOUNT, VE_VTXRANGE, VE_VTXZ, VE_RGBA, VE_XYFIELD, VE_STRANGE, VE_STQ, VE_UVFIELD,
	VE_TEX0, VE_TEX0TBW, VE_TEX0VRAM, VE_CLAMP, VE_SCISSOR, VE_FRAME, VE_ALPHA, VE_TESTZTE, VE_BITBLT, VE_TRX, VE_IMGSIZE,
	VE_IMGORDER, VE_TEXFLUSH, VE_CTX2, VE_REGUNK, VE_RESERVED, VE_NERR
};

static const char *const ve_name[VE_NERR] = {
	"DMATAG", "GIFTAG", "PRIM", "PRIMCOUNT", "VTXRANGE", "VTXZ", "RGBA", "XYFIELD", "STRANGE", "STQ", "UVFIELD",
	"TEX0", "TEX0TBW", "TEX0VRAM", "CLAMP", "SCISSOR", "FRAME", "ALPHA", "TESTZTE", "BITBLT", "TRX", "IMGSIZE",
	"IMGORDER", "TEXFLUSH", "CTX2", "REGUNK", "RESERVED"
};

#define VAL_DIRTY 8
#define VAL_STRANGE_LIMIT 4100.0f // UV_EXTENT: the driver cuts polygons whose texel coordinates would pass 4096

static struct
{
	int on;
	u32 err[VE_NERR];
	u32 maxu_seen; // largest |u| or |v| (texels) of any textured vertex
	u32 maxspan; // largest span of u or v (texels) inside one primitive
	float hu[2], hv[2], fu, fv; // texel coordinates of the last two vertices and of the first one since PRIM
	u32 packets, qwords, verts, prims, images, image_qw, tflush, tex0s, ad;
	u32 summary_frame;
	// GIF parser, carried from buffer to buffer
	u32 left; // data qwords of the open packet still to come
	u32 k; // data qwords of the open packet already seen
	u32 nloop, nreg, flg;
	u64 regs;
	// GS shadow
	u64 tex0[2], clamp[2], prim, xyoff, scis, frame, zbuf, bitbltbuf, trxpos, trxreg;
	u32 rgbaq_q, st_s, st_t;
	int have_xyoff, have_frame, have_zbuf;
	u32 vcount; // kicks since the last PRIM
	u32 ptype;
	// image transfer
	int img_open;
	s32 img_left;
	u32 img_blo, img_bhi; // destination blocks of the open transfer
	// texture cache model: local memory regions written since the last TEXFLUSH
	u32 dirty_lo[VAL_DIRTY], dirty_hi[VAL_DIRTY];
	int ndirty;
	int fb_dirty;
	int tf_reported;
	int use_dirty[2]; // TEX0 or CLAMP written since the pair was checked
} VAL;

// the chain lint of the VIF1 transport (lint_buffer below; -hwdbg 536870912 with VU1 on)
static struct
{
	int on;
	u32 left; // data qwords of the open GIF packet still to come
	int open; // the last GIF tag had EOP = 0: the packet continues with the next tag
	u32 errs, runs, flushes;
} LINT;

static void val_err(int cls, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void val_err(int cls, const char *fmt, ...)
{
	if (VAL.err[cls]++ < 3)
	{
		char buf[240];
		va_list ap;

		va_start(ap, fmt);
		vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		I_OutputMsg("HWVAL %s f=%u: %s\n", ve_name[cls], H.frame_no, buf);
	}
}

static void val_summary(const char *why)
{
	char buf[700];
	int i, n;
	u32 tot = 0;

	n = snprintf(buf, sizeof buf, "HWVAL SUMMARY %s f=%u: packets=%u qwords=%u verts=%u prims=%u images=%u image_qw=%u ad=%u texflush=%u tex0=%u max_texel=%u max_span=%u |", why, H.frame_no,
		VAL.packets, VAL.qwords, VAL.verts, VAL.prims, VAL.images, VAL.image_qw, VAL.ad, VAL.tflush, VAL.tex0s, VAL.maxu_seen, VAL.maxspan);
	for (i = 0; i < VE_NERR && n < (int)sizeof buf - 40; i++)
		if (VAL.err[i])
		{
			n += snprintf(buf + n, sizeof buf - (size_t)n, " %s=%u", ve_name[i], VAL.err[i]);
			tot += VAL.err[i];
		}
	snprintf(buf + n, sizeof buf - (size_t)n, " | errors=%u", tot);
	I_OutputMsg("%s\n", buf);
}

static void val_init(void)
{
	memset(&VAL, 0, sizeof VAL);
	VAL.on = (ps2hwd_dbg_flags & HWDBG_VALIDATE) != 0 && !V.on;
	if (VAL.on)
		I_OutputMsg("HWVAL on: the GIF stream is validated (PS2-HW-74)\n");
	memset(&LINT, 0, sizeof LINT);
	LINT.on = (ps2hwd_dbg_flags & HWDBG_VALIDATE) != 0 && V.on;
	if (LINT.on)
		I_OutputMsg("HWVAL LINT on: the DIRECT runs of the VIF1 chain are checked (FLUSH inside a GIF packet)\n");
}

static void val_shutdown(void)
{
	if (VAL.on)
		val_summary("final");
	if (LINT.on)
		I_OutputMsg("HWVAL LINT SUMMARY: direct runs=%u flushes=%u errors=%u\n", LINT.runs, LINT.flushes, LINT.errs);
	VAL.on = 0;
	LINT.on = 0;
}

static void val_reset(void)
{
	VAL.left = 0;
	VAL.k = 0;
	VAL.img_open = 0;
	VAL.img_left = 0;
	VAL.vcount = 0;
	VAL.ndirty = 0;
	VAL.fb_dirty = 0;
}

static u32 val_bpp(u32 psm)
{
	switch (psm)
	{
		case 0x00: case 0x30: return 32;
		case 0x01: case 0x31: return 24;
		case 0x02: case 0x0A: case 0x32: case 0x3A: return 16;
		case 0x13: case 0x1B: return 8;
		case 0x14: case 0x24: case 0x2C: return 4;
		default: return 0;
	}
}

// page and block geometry of a format: page size in pixels, block size in pixels, block order inside the page (the GS block tables)
static const u8 vb_t32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const u8 vb_t16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const u8 vb_t16s[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};

// address (in 256-byte blocks) of the block that holds pixel (x, y) of a buffer at `bp` with width `bw` (64-pixel units)
static u32 val_block_of(u32 psm, u32 bp, u32 bw, u32 x, u32 y)
{
	u32 pw, ph, bkw, bkh, page, idx;

	switch (val_bpp(psm))
	{
		case 32: case 24: pw = 64; ph = 32; bkw = 8; bkh = 8; break;
		case 16: pw = 64; ph = 64; bkw = 16; bkh = 8; break;
		case 8: pw = 128; ph = 64; bkw = 16; bkh = 16; break;
		default: pw = 128; ph = 128; bkw = 32; bkh = 16; break;
	}
	page = (y / ph) * (bw * 64 / pw ? bw * 64 / pw : 1) + x / pw;
	x %= pw;
	y %= ph;
	if (val_bpp(psm) == 32 || val_bpp(psm) == 24 || val_bpp(psm) == 8)
		idx = vb_t32[y / bkh][x / bkw];
	else if (psm == 0x0A || psm == 0x3A)
		idx = vb_t16s[y / bkh][x / bkw];
	else
		idx = vb_t16[y / bkh][x / bkw];
	return bp + page * PAGE_BLOCKS + idx;
}

// end (exclusive, in 256-byte blocks) of the blocks a w x h rectangle at (x0, y0) of a buffer takes. The page based bound answers first; only when it
// passes the end of VRAM the exact block table walk decides (it is a few thousand steps for a 1024 x 1024 texture).
static u32 val_end(u32 psm, u32 bp, u32 bw, u32 x0, u32 y0, u32 w, u32 h)
{
	u32 pw, ph, stride, pages_w, pages_h, x, y, end = bp, bkw, bkh;

	switch (val_bpp(psm))
	{
		case 32: case 24: pw = 64; ph = 32; bkw = 8; bkh = 8; break;
		case 16: pw = 64; ph = 64; bkw = 16; bkh = 8; break;
		case 8: pw = 128; ph = 64; bkw = 16; bkh = 16; break;
		default: pw = 128; ph = 128; bkw = 32; bkh = 16; break;
	}
	stride = bw * 64;
	if (stride < x0 + w)
		stride = x0 + w;
	pages_w = (stride + pw - 1) / pw;
	pages_h = (y0 + h + ph - 1) / ph;
	if (bp + pages_w * pages_h * PAGE_BLOCKS <= VRAM_BLOCKS)
		return bp + pages_w * pages_h * PAGE_BLOCKS;
	for (y = y0 / bkh * bkh; y < y0 + h; y += bkh)
		for (x = x0 / bkw * bkw; x < x0 + w; x += bkw)
		{
			const u32 b = val_block_of(psm, bp, bw, x, y) + 1;

			if (b > end)
				end = b;
		}
	return end;
}

static void val_dirty_add(u32 lo, u32 hi)
{
	if (hi <= lo)
		return;
	if (VAL.ndirty < VAL_DIRTY)
	{
		VAL.dirty_lo[VAL.ndirty] = lo;
		VAL.dirty_hi[VAL.ndirty++] = hi;
	}
	else
	{
		// no room: merge into the last entry (a larger region is the conservative answer)
		if (lo < VAL.dirty_lo[VAL_DIRTY - 1])
			VAL.dirty_lo[VAL_DIRTY - 1] = lo;
		if (hi > VAL.dirty_hi[VAL_DIRTY - 1])
			VAL.dirty_hi[VAL_DIRTY - 1] = hi;
	}
}

// the texels a draw with the TEX0 / CLAMP of `ctx` can read: the clamp region when it is a region clamp, the whole texture otherwise
static void val_tex_extent(int ctx, u32 *w, u32 *h)
{
	const u64 t = VAL.tex0[ctx], c = VAL.clamp[ctx];
	const u32 tw = (u32)((t >> 26) & 15), th = (u32)((t >> 30) & 15), wms = (u32)(c & 3), wmt = (u32)((c >> 2) & 3);

	*w = 1u << (tw > 10 ? 10 : tw);
	*h = 1u << (th > 10 ? 10 : th);
	if (wms == 2 && (u32)((c >> 14) & 0x3FF) + 1 < *w)
		*w = (u32)((c >> 14) & 0x3FF) + 1;
	if (wmt == 2 && (u32)((c >> 34) & 0x3FF) + 1 < *h)
		*h = (u32)((c >> 34) & 0x3FF) + 1;
}

static void val_tex_region(int ctx, u32 *lo, u32 *hi)
{
	const u64 t = VAL.tex0[ctx];
	const u32 tbp = (u32)(t & 0x3FFF), tbw = (u32)((t >> 14) & 63), psm = (u32)((t >> 20) & 63);
	u32 w, h;

	val_tex_extent(ctx, &w, &h);
	*lo = tbp;
	*hi = val_bpp(psm) ? val_end(psm, tbp, tbw, 0, 0, w, h) : tbp;
}

// what is checked when the registers are written: field values alone
static void val_check_tex0(int ctx, u64 t)
{
	const u32 tbp = (u32)(t & 0x3FFF), tbw = (u32)((t >> 14) & 63), psm = (u32)((t >> 20) & 63), tw = (u32)((t >> 26) & 15), th = (u32)((t >> 30) & 15);
	const u32 cbp = (u32)((t >> 37) & 0x3FFF), cpsm = (u32)((t >> 51) & 15), csm = (u32)((t >> 55) & 1), csa = (u32)((t >> 56) & 31), cld = (u32)((t >> 61) & 7);

	VAL.tex0[ctx] = t;
	VAL.tex0s++;
	VAL.tf_reported = 0;
	VAL.use_dirty[ctx] = 1;
	if (!val_bpp(psm))
		val_err(VE_TEX0, "TEX0 PSM %02x is no format (TEX0 %016llx)", psm, (unsigned long long)t);
	if (tw > 10 || th > 10)
		val_err(VE_TEX0, "TEX0 TW=%u TH=%u above 10 (the GS holds 1024 texels per axis; TEX0 %016llx)", tw, th, (unsigned long long)t);
	if (val_bpp(psm) && val_bpp(psm) <= 8 && (tbw & 1))
		val_err(VE_TEX0TBW, "TEX0 TBW=%u is odd for the %u-bit format %02x (pages are 128 pixels wide; TBP0=%u TW=%u)", tbw, val_bpp(psm), psm, tbp, tw);
	if (!tbw)
		val_err(VE_TEX0TBW, "TEX0 TBW=0 (TEX0 %016llx)", (unsigned long long)t);
	if (cld > 5)
		val_err(VE_TEX0, "TEX0 CLD=%u (CLUT load mode)", cld);
	if (cld)
	{
		if (cpsm != 0 && cpsm != 2 && cpsm != 10)
			val_err(VE_TEX0, "TEX0 CPSM=%u is no CLUT format", cpsm);
		if (val_bpp(psm) == 8 && csm == 0 && csa != 0)
			val_err(VE_TEX0, "TEX0 CSA=%u with an 8-bit texture (CSM1 uses all 256 entries; CSA must be 0)", csa);
		if (cbp + 4 > VRAM_BLOCKS)
			val_err(VE_TEX0VRAM, "CLUT CBP=%u reaches the end of VRAM", cbp);
	}
}

static void val_check_clamp(int ctx, u64 c)
{
	VAL.clamp[ctx] = c;
	VAL.use_dirty[ctx] = 1;
	if (c >> 44)
		val_err(VE_CLAMP, "CLAMP %016llx has bits above MAXV (a field overflowed)", (unsigned long long)c);
}

// what depends on the pair TEX0 + CLAMP is checked when a vertex of a textured primitive uses it (the two registers are written in either order)
static void val_check_use(int ctx)
{
	const u64 t = VAL.tex0[ctx], c = VAL.clamp[ctx];
	const u32 tbp = (u32)(t & 0x3FFF), tbw = (u32)((t >> 14) & 63), psm = (u32)((t >> 20) & 63), tw = (u32)((t >> 26) & 15), th = (u32)((t >> 30) & 15);
	const u32 wms = (u32)(c & 3), wmt = (u32)((c >> 2) & 3);
	const u32 minu = (u32)((c >> 4) & 0x3FF), maxu = (u32)((c >> 14) & 0x3FF), minv = (u32)((c >> 24) & 0x3FF), maxv = (u32)((c >> 34) & 0x3FF);
	u32 w, h, lo, hi;

	VAL.use_dirty[ctx] = 0;
	if (tw > 10 || th > 10 || !val_bpp(psm))
		return;
	val_tex_extent(ctx, &w, &h);
	if (wms == 2 && (minu > maxu || maxu >= (1u << tw)))
		val_err(VE_CLAMP, "region clamp U %u..%u outside the texture (TW=%u)", minu, maxu, tw);
	if (wmt == 2 && (minv > maxv || maxv >= (1u << th)))
		val_err(VE_CLAMP, "region clamp V %u..%u outside the texture (TH=%u)", minv, maxv, th);
	if (wms == 3 && ((maxu >> tw) != 0))
		val_err(VE_CLAMP, "region repeat U fix bits %u lie beyond the texture width (TW=%u, mask %u)", maxu, tw, minu);
	if (wmt == 3 && ((maxv >> th) != 0))
		val_err(VE_CLAMP, "region repeat V fix bits %u lie beyond the texture height (TH=%u, mask %u)", maxv, th, minv);
	if (w > tbw * 64u)
		val_err(VE_TEX0TBW, "a draw reads %u texels per row but TBW=%u holds %u (TW=%u, clamp mode %u): the rows overlap", w, tbw, tbw * 64, tw, wms);
	val_tex_region(ctx, &lo, &hi);
	if (hi > VRAM_BLOCKS)
		val_err(VE_TEX0VRAM, "texture TBP0=%u psm=%02x %ux%u (read area) tbw=%u reaches block %u of %u (VRAM wraps on the GS)", tbp, psm, w, h, tbw, hi, VRAM_BLOCKS);
}

static void val_prim(u32 p)
{
	const u32 ty = p & 7;
	const u32 prev = VAL.ptype;

	// the vertices queued by the previous PRIM that do not make a primitive are dropped by the GS
	if (VAL.vcount)
	{
		if ((prev == PRIM_TRI && (VAL.vcount % 3)) || ((prev == PRIM_SPRITE || prev == PRIM_LINE) && (VAL.vcount & 1)) ||
			((prev == PRIM_TRISTRIP || prev == PRIM_TRIFAN) && VAL.vcount < 3) || (prev == PRIM_LINESTRIP && VAL.vcount < 2))
			val_err(VE_PRIMCOUNT, "%u vertices after PRIM type %u do not make a primitive", VAL.vcount, prev);
	}
	if (ty == 7)
		val_err(VE_PRIM, "PRIM type 7 is reserved (PRIM %03x)", p);
	if (p >> 11)
		val_err(VE_PRIM, "PRIM %x has bits above 10", p);
	VAL.prim = p & 0x7FF;
	VAL.ptype = ty;
	VAL.vcount = 0;
	VAL.prims++;
}

static void val_vertex(u32 x, u32 y, u32 z, int kick)
{
	const int ofx = VAL.have_xyoff ? (int)(VAL.xyoff & 0xFFFF) : 0x8000, ofy = VAL.have_xyoff ? (int)((VAL.xyoff >> 32) & 0xFFFF) : 0x8000;
	const int px = ((int)x - ofx), py = ((int)y - ofy); // 1/16 pixel relative to the offset

	if (px < -2000 * 16 || px > 2000 * 16 || py < -2000 * 16 || py > 2000 * 16)
		val_err(VE_VTXRANGE, "vertex (%d.%02d, %d.%02d) px relative to XYOFFSET is outside the guard band (raw %04x,%04x offset %04x,%04x)", px / 16, (px < 0 ? -px : px) % 16 * 100 / 16,
			py / 16, (py < 0 ? -py : py) % 16 * 100 / 16, x, y, ofx, ofy);
	if (!kick)
		return;
	if (VAL.have_zbuf)
	{
		const u32 zpsm = (u32)((VAL.zbuf >> 24) & 15), zmax = zpsm == 1 ? 0xFFFFFFu : zpsm == 10 ? 0xFFFFu : 0xFFFFFFFFu;

		if (z > zmax)
			val_err(VE_VTXZ, "Z %08x above the Z buffer format (max %x; vertex %04x,%04x)", z, zmax, x, y);
	}
	VAL.vcount++;
	VAL.verts++;
	VAL.fb_dirty = 1;
	if (VAL.prim & (1u << 4)) // TME
	{
		const int ctx = (VAL.prim >> 9) & 1;
		const u64 t = VAL.tex0[ctx];
		const u32 tw = (u32)((t >> 26) & 15), th = (u32)((t >> 30) & 15);

		if (VAL.use_dirty[ctx])
			val_check_use(ctx);
		if (!(VAL.prim & (1u << 8))) // ST / Q
		{
			union { u32 w; float f; } s, tt, q;
			float u, v, au, av;

			s.w = VAL.st_s;
			tt.w = VAL.st_t;
			q.w = VAL.rgbaq_q;
			if ((s.w & 0x7F800000u) == 0x7F800000u || (tt.w & 0x7F800000u) == 0x7F800000u || (q.w & 0x7F800000u) == 0x7F800000u || q.f == 0.0f || q.f < 0.0f)
				val_err(VE_STQ, "S=%g T=%g Q=%g at vertex %04x,%04x", s.f, tt.f, q.f, x, y);
			else
			{
				u = s.f / q.f * (float)(1u << (tw > 10 ? 10 : tw));
				v = tt.f / q.f * (float)(1u << (th > 10 ? 10 : th));
				au = u < 0 ? -u : u;
				av = v < 0 ? -v : v;
				if (au > (float)VAL.maxu_seen)
					VAL.maxu_seen = (u32)au;
				if (av > (float)VAL.maxu_seen)
					VAL.maxu_seen = (u32)av;
				// the span of one primitive (the driver cuts at whole repeats so that it stays below UV_EXTENT); the absolute value is only counted:
				// REPEAT of a power of two texture takes the low bits, a 14-bit wrap of U / V is the same picture
				{
					const u32 k = VAL.vcount;
					float mnu = u, mxu = u, mnv = v, mxv = v, a, b, c, d;
					int have = 0;

					if (k == 1)
					{
						VAL.fu = u;
						VAL.fv = v;
					}
					switch (VAL.ptype)
					{
						case PRIM_TRI: case PRIM_TRISTRIP:
							have = (VAL.ptype == PRIM_TRI) ? (k % 3 == 0) : (k >= 3);
							a = VAL.hu[0]; b = VAL.hv[0]; c = VAL.hu[1]; d = VAL.hv[1];
							break;
						case PRIM_TRIFAN:
							have = k >= 3;
							a = VAL.fu; b = VAL.fv; c = VAL.hu[1]; d = VAL.hv[1];
							break;
						default: // sprites and lines: two vertices
							have = (k % 2 == 0);
							a = c = VAL.hu[1];
							b = d = VAL.hv[1];
							break;
					}
					if (have)
					{
						if (a < mnu) mnu = a;
						if (a > mxu) mxu = a;
						if (c < mnu) mnu = c;
						if (c > mxu) mxu = c;
						if (b < mnv) mnv = b;
						if (b > mxv) mxv = b;
						if (d < mnv) mnv = d;
						if (d > mxv) mxv = d;
						if (mxu - mnu > (float)VAL.maxspan)
							VAL.maxspan = (u32)(mxu - mnu);
						if (mxv - mnv > (float)VAL.maxspan)
							VAL.maxspan = (u32)(mxv - mnv);
						if (mxu - mnu > VAL_STRANGE_LIMIT || mxv - mnv > VAL_STRANGE_LIMIT)
							val_err(VE_STRANGE, "one primitive spans %g x %g texels (limit %d; TW=%u TH=%u at vertex %04x,%04x)", mxu - mnu, mxv - mnv, 4096, tw, th, x, y);
					}
					VAL.hu[0] = VAL.hu[1];
					VAL.hv[0] = VAL.hv[1];
					VAL.hu[1] = u;
					VAL.hv[1] = v;
				}
			}
		}
		if (!VAL.tf_reported)
		{
			u32 lo, hi;
			int i;

			val_tex_region(ctx, &lo, &hi);
			for (i = 0; i < VAL.ndirty; i++)
				if (lo < VAL.dirty_hi[i] && VAL.dirty_lo[i] < hi)
				{
					val_err(VE_TEXFLUSH, "draw samples blocks %u..%u, written (%u..%u) since the last TEXFLUSH: the real GS texture cache may hold the old texels", lo, hi, VAL.dirty_lo[i], VAL.dirty_hi[i]);
					VAL.tf_reported = 1;
					break;
				}
			if (!VAL.tf_reported && VAL.fb_dirty && VAL.have_frame)
			{
				const u32 flo = (u32)(VAL.frame & 0x1FF) * PAGE_BLOCKS, fhi = flo + (u32)H.fbw * (u32)H.fbh * 4 / 256;

				if (lo < fhi && flo < hi)
				{
					val_err(VE_TEXFLUSH, "draw samples blocks %u..%u inside the frame buffer (%u..%u) drawn since the last TEXFLUSH", lo, hi, flo, fhi);
					VAL.tf_reported = 1;
				}
			}
		}
	}
}

static void val_native(u32 desc, u64 d);

// the addresses of the registers a GS has
static int val_known(u32 a)
{
	switch (a)
	{
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A:
		case 0x0C: case 0x0D: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x22:
		case 0x34: case 0x35: case 0x36: case 0x37: case 0x3B: case 0x3C: case 0x3D: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43:
		case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
		case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B:
		case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62:
			return 1;
		default:
			return 0;
	}
}

static void val_gsreg(u32 addr, u64 d)
{
	if (!val_known(addr))
	{
		val_err(VE_REGUNK, "A+D register %02x (data %016llx) is not a GS register", addr, (unsigned long long)d);
		return;
	}
	switch (addr)
	{
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x0C: case 0x0D:
			val_native(addr, d);
			break;
		case 0x06: case 0x07:
			if (addr == 0x07)
				val_err(VE_CTX2, "TEX0_2 written (the driver draws in context 1)");
			val_check_tex0(addr & 1, d);
			break;
		case 0x08: case 0x09:
			if (addr == 0x09)
				val_err(VE_CTX2, "CLAMP_2 written");
			val_check_clamp(addr & 1, d);
			break;
		case 0x0A: // FOG
			if (d & 0x00FFFFFFFFFFFFFFull)
				val_err(VE_RESERVED, "FOG %016llx has bits below F", (unsigned long long)d);
			break;
		case 0x14: // TEX1_1
		{
			const u32 mxl = (u32)((d >> 2) & 7), mmin = (u32)((d >> 6) & 7);

			if (mxl > 6 || mmin > 5)
				val_err(VE_RESERVED, "TEX1 MXL=%u MMIN=%u out of range", mxl, mmin);
			break;
		}
		case 0x15: case 0x17: case 0x19: case 0x35: case 0x37: case 0x41: case 0x43: case 0x48: case 0x4B: case 0x4D: case 0x4F:
			val_err(VE_CTX2, "context 2 register %02x written (the driver draws in context 1)", addr);
			break;
		case 0x18: // XYOFFSET_1
			VAL.xyoff = d;
			VAL.have_xyoff = 1;
			if (((d >> 16) & 0xFFFF) || (d >> 48))
				val_err(VE_RESERVED, "XYOFFSET %016llx has bits in the reserved ranges", (unsigned long long)d);
			break;
		case 0x3B: // TEXA
			if (d & ~(0xFFull | (1ull << 15) | (0xFFull << 32)))
				val_err(VE_RESERVED, "TEXA %016llx has bits in the reserved ranges", (unsigned long long)d);
			break;
		case 0x3D: // FOGCOL
			if (d >> 24)
				val_err(VE_RESERVED, "FOGCOL %016llx has bits above B", (unsigned long long)d);
			break;
		case 0x3F: // TEXFLUSH
			VAL.ndirty = 0;
			VAL.fb_dirty = 0;
			VAL.tflush++;
			VAL.tf_reported = 0;
			break;
		case 0x40: // SCISSOR_1
		{
			const u32 x0 = (u32)(d & 0x7FF), x1 = (u32)((d >> 16) & 0x7FF), y0 = (u32)((d >> 32) & 0x7FF), y1 = (u32)((d >> 48) & 0x7FF);

			VAL.scis = d;
			if (d & 0xF800F800F800F800ull)
				val_err(VE_SCISSOR, "SCISSOR %016llx has bits in the reserved ranges (a coordinate above 2047?)", (unsigned long long)d);
			if (x0 > x1 || y0 > y1)
				val_err(VE_SCISSOR, "SCISSOR %u,%u - %u,%u is empty or inverted", x0, y0, x1, y1);
			if (VAL.have_frame)
			{
				const u32 fbw = (u32)((VAL.frame >> 16) & 63) * 64;

				if (x1 >= fbw || y1 >= (u32)H.fbh)
					val_err(VE_SCISSOR, "SCISSOR %u,%u - %u,%u reaches outside the frame buffer (%u x %d)", x0, y0, x1, y1, fbw, H.fbh);
			}
			break;
		}
		case 0x42: // ALPHA_1
		{
			const u32 a = (u32)(d & 3), b = (u32)((d >> 2) & 3), c = (u32)((d >> 4) & 3), dd = (u32)((d >> 6) & 3);

			if (a == 3 || b == 3 || c == 3 || dd == 3 || ((d >> 8) & 0xFFFFFF) || (d >> 40))
				val_err(VE_ALPHA, "ALPHA %016llx uses a reserved selector or reserved bits", (unsigned long long)d);
			break;
		}
		case 0x47: // TEST_1
			if (!(d & (1u << 16)))
				val_err(VE_TESTZTE, "TEST %016llx has ZTE=0 (the real GS then does not use the Z buffer as intended: ZTE=1 with ZTST=ALWAYS is the way to switch the test off)", (unsigned long long)d);
			if (d >> 19)
				val_err(VE_RESERVED, "TEST %016llx has bits above ZTST", (unsigned long long)d);
			break;
		case 0x4C: // FRAME_1
		{
			const u32 fbp = (u32)(d & 0x1FF), fbw = (u32)((d >> 16) & 63), psm = (u32)((d >> 24) & 63);

			VAL.frame = d;
			VAL.have_frame = 1;
			if (((d >> 9) & 0x7F) || ((d >> 22) & 3) || ((d >> 30) & 3))
				val_err(VE_FRAME, "FRAME %016llx has bits in the reserved ranges", (unsigned long long)d);
			if (!fbw)
				val_err(VE_FRAME, "FRAME FBW=0");
			if (psm != 0 && psm != 1 && psm != 2 && psm != 10)
				val_err(VE_FRAME, "FRAME PSM %02x is no colour format", psm);
			if (fbp * PAGE_BLOCKS >= VRAM_BLOCKS)
				val_err(VE_FRAME, "FRAME FBP=%u beyond VRAM", fbp);
			break;
		}
		case 0x4E: // ZBUF_1
		{
			const u32 zbp = (u32)(d & 0x1FF), psm = (u32)((d >> 24) & 15);

			VAL.zbuf = d;
			VAL.have_zbuf = 1;
			if (psm != 0 && psm != 1 && psm != 10)
				val_err(VE_FRAME, "ZBUF PSM %u is no Z format", psm);
			if (zbp * PAGE_BLOCKS >= VRAM_BLOCKS || ((d >> 9) & 0x7FFF) || ((d >> 28) & 15) || (d >> 33))
				val_err(VE_FRAME, "ZBUF %016llx is beyond VRAM or has bits in the reserved ranges", (unsigned long long)d);
			break;
		}
		case 0x50: // BITBLTBUF
		{
			const u32 dbp = (u32)((d >> 32) & 0x3FFF), dbw = (u32)((d >> 48) & 63), dpsm = (u32)((d >> 56) & 63);

			VAL.bitbltbuf = d;
			if (!val_bpp(dpsm))
				val_err(VE_BITBLT, "BITBLTBUF DPSM %02x is no format", dpsm);
			else if (val_bpp(dpsm) <= 8 && (dbw & 1))
				val_err(VE_BITBLT, "BITBLTBUF DBW=%u is odd for a %u-bit destination (DBP=%u)", dbw, val_bpp(dpsm), dbp);
			if (!dbw)
				val_err(VE_BITBLT, "BITBLTBUF DBW=0");
			if (((d >> 46) & 3) || ((d >> 54) & 3) || (d >> 62) || ((d >> 14) & 3) || ((d >> 22) & 3) || ((d >> 30) & 3))
				val_err(VE_BITBLT, "BITBLTBUF %016llx has bits in the reserved ranges", (unsigned long long)d);
			break;
		}
		case 0x51: // TRXPOS
			VAL.trxpos = d;
			if ((d & 0xF800F800F800ull) || (d >> 61))
				val_err(VE_TRX, "TRXPOS %016llx has bits in the reserved ranges", (unsigned long long)d);
			break;
		case 0x52: // TRXREG
			VAL.trxreg = d;
			if ((d & 0xFFFFF000ull) || (d >> 44) || !(d & 0xFFF) || !((d >> 32) & 0xFFF))
				val_err(VE_TRX, "TRXREG %016llx is empty or has bits above 4095", (unsigned long long)d);
			break;
		case 0x53: // TRXDIR
		{
			const u32 dir = (u32)(d & 3);
			const u32 dpsm = (u32)((VAL.bitbltbuf >> 56) & 63), dbp = (u32)((VAL.bitbltbuf >> 32) & 0x3FFF), dbw = (u32)((VAL.bitbltbuf >> 48) & 63);
			const u32 rrw = (u32)(VAL.trxreg & 0xFFF), rrh = (u32)((VAL.trxreg >> 32) & 0xFFF);
			const u32 dsax = (u32)((VAL.trxpos >> 32) & 0x7FF), dsay = (u32)((VAL.trxpos >> 48) & 0x7FF);
			const u32 bpp = val_bpp(dpsm);

			if (VAL.img_open && VAL.img_left > 0)
				val_err(VE_IMGORDER, "TRXDIR written while %d qwords of the previous transfer are missing", VAL.img_left);
			VAL.img_open = 0;
			VAL.img_left = 0;
			if ((dir == 0 || dir == 2) && bpp && rrw && rrh)
			{
				const u32 hi = val_end(dpsm, dbp, dbw, dsax, dsay, rrw, rrh);

				if (hi > VRAM_BLOCKS)
					val_err(VE_BITBLT, "transfer to DBP=%u psm=%02x %ux%u at %u,%u reaches block %u of %u (VRAM wraps on the GS)", dbp, dpsm, rrw, rrh, dsax, dsay, hi, VRAM_BLOCKS);
				if (dbw && dsax + rrw > dbw * 64u)
					val_err(VE_BITBLT, "transfer %u + %u texels wide leaves the buffer width %u", dsax, rrw, dbw * 64);
				if (dir == 0)
				{
					VAL.img_left = (s32)(((u64)rrw * rrh * bpp + 127) / 128);
					VAL.img_open = 1;
					VAL.img_blo = dbp;
					VAL.img_bhi = hi;
				}
				else
					val_dirty_add(dbp, hi); // local -> local copy
			}
			break;
		}
		default:
			break;
	}
}

// one 64-bit native register value out of a PACKED qword
static void val_packed(u32 desc, u64 d0, u64 d1)
{
	switch (desc)
	{
		case 0: // PRIM
			val_prim((u32)d0);
			break;
		case 1: // RGBAQ: R, G, B, A in the low byte of four 32-bit words
		{
			u32 c[4] = {(u32)d0, (u32)(d0 >> 32), (u32)d1, (u32)(d1 >> 32)};
			int i;

			for (i = 0; i < 4; i++)
				if (c[i] > 255)
				{
					val_err(VE_RGBA, "RGBAQ component %d = %u above 255 (R,G,B,A = %u,%u,%u,%u)", i, c[i], c[0], c[1], c[2], c[3]);
					break;
				}
			break;
		}
		case 2: // ST + Q
			VAL.st_s = (u32)d0;
			VAL.st_t = (u32)(d0 >> 32);
			VAL.rgbaq_q = (u32)d1;
			break;
		case 3: // UV
			if ((d0 & 0xFFFFC000ull) || (d0 >> 46))
				val_err(VE_UVFIELD, "packed UV %016llx overflows 14 bits (U=%llu V=%llu raw)", (unsigned long long)d0, (unsigned long long)(d0 & 0xFFFFFFFFull), (unsigned long long)(d0 >> 32));
			break;
		case 4: case 12: // XYZF2 / XYZF3
			if ((d0 & 0xFFFF0000ull) || (d0 >> 48))
				val_err(VE_XYFIELD, "packed XYZF X/Y slots %016llx exceed 16 bits (a coordinate overflowed)", (unsigned long long)d0);
			if ((d1 >> 28) & 0xFF)
				val_err(VE_VTXZ, "packed XYZF Z %016llx above 24 bits", (unsigned long long)d1);
			val_vertex((u32)d0 & 0xFFFFu, (u32)(d0 >> 32) & 0xFFFFu, (u32)(d1 >> 4) & 0xFFFFFFu, desc == 4 && !((d1 >> 47) & 1));
			break;
		case 5: case 13: // XYZ2 / XYZ3
			if ((d0 & 0xFFFF0000ull) || (d0 >> 48))
				val_err(VE_XYFIELD, "packed XYZ X/Y slots %016llx exceed 16 bits (a coordinate overflowed)", (unsigned long long)d0);
			val_vertex((u32)d0 & 0xFFFFu, (u32)(d0 >> 32) & 0xFFFFu, (u32)d1, desc == 5 && !((d1 >> 47) & 1));
			break;
		case 6: case 7: case 8: case 9: // TEX0_1/2, CLAMP_1/2
			val_gsreg(desc, d0);
			break;
		case 10: // FOG
			break;
		case 14: // A+D
			VAL.ad++;
			val_gsreg((u32)(d1 & 0xFF), d0);
			break;
		case 15: // NOP
			break;
		default:
			val_err(VE_GIFTAG, "PACKED register descriptor %u is reserved", desc);
			break;
	}
}

// one 64-bit item of a REGLIST packet
static void val_native(u32 desc, u64 d)
{
	switch (desc)
	{
		case 0:
			val_prim((u32)d);
			break;
		case 1: // RGBAQ + Q
			VAL.rgbaq_q = (u32)(d >> 32);
			break;
		case 2: // ST
			VAL.st_s = (u32)d;
			VAL.st_t = (u32)(d >> 32);
			break;
		case 3:
			if ((d & 0xC000C000ull) || (d >> 30))
				val_err(VE_UVFIELD, "UV %016llx overflows 14 bits", (unsigned long long)d);
			break;
		case 4: // XYZF2
			val_vertex((u32)d & 0xFFFFu, (u32)(d >> 16) & 0xFFFFu, (u32)(d >> 32) & 0xFFFFFFu, 1);
			break;
		case 5: // XYZ2
			val_vertex((u32)d & 0xFFFFu, (u32)(d >> 16) & 0xFFFFu, (u32)(d >> 32), 1);
			break;
		case 12: case 13:
			val_vertex((u32)d & 0xFFFFu, (u32)(d >> 16) & 0xFFFFu, (u32)(d >> 32), 0);
			break;
		case 14: // A+D is not defined in this format
			val_err(VE_GIFTAG, "REGLIST descriptor 14 (A+D)");
			break;
		case 15:
			break;
		default:
			val_gsreg(desc, d);
			break;
	}
}

static void val_qword(u64 d0, u64 d1)
{
	if (!VAL.left)
	{
		// a GIF tag
		const u32 nloop = (u32)(d0 & 0x7FFF), pre = (u32)((d0 >> 46) & 1), prim = (u32)((d0 >> 47) & 0x7FF), flg = (u32)((d0 >> 58) & 3);
		u32 nreg = (u32)(d0 >> 60);

		if (!nreg)
			nreg = 16;
		VAL.packets++;
		VAL.nloop = nloop;
		VAL.flg = flg;
		VAL.nreg = nreg;
		VAL.regs = d1;
		VAL.k = 0;
		if (flg == 3)
			val_err(VE_GIFTAG, "GIF tag FLG=3 (disabled)");
		if (VAL.img_open && VAL.img_left > 0 && flg != 2)
			val_err(VE_IMGORDER, "a %s packet arrives while %d qwords of an image transfer are missing: the GS takes it as image data", flg ? "REGLIST" : "PACKED", VAL.img_left),
			VAL.img_open = 0, VAL.img_left = 0;
		if (flg == 0)
		{
			if (pre)
				val_prim(prim);
			VAL.left = nloop * nreg;
		}
		else if (flg == 1)
			VAL.left = (nloop * nreg + 1) / 2;
		else
		{
			VAL.left = nloop;
			if (!VAL.img_open)
			{
				if (nloop)
					val_err(VE_IMGSIZE, "%u qwords of image data without a transfer in progress", nloop);
			}
			else
			{
				VAL.img_left -= (s32)nloop;
				VAL.image_qw += nloop;
				if (VAL.img_left < 0)
				{
					val_err(VE_IMGSIZE, "image data is %d qwords longer than the TRXREG size", -VAL.img_left);
					VAL.img_left = 0;
				}
				if (VAL.img_left == 0)
				{
					val_dirty_add(VAL.img_blo, VAL.img_bhi);
					VAL.img_open = 0;
					VAL.images++;
				}
			}
		}
		return;
	}
	VAL.qwords++;
	if (VAL.flg == 0)
	{
		val_packed((u32)((VAL.regs >> (4 * (VAL.k % VAL.nreg))) & 15), d0, d1);
	}
	else if (VAL.flg == 1)
	{
		const u32 i0 = VAL.k * 2, total = VAL.nloop * VAL.nreg;

		val_native((u32)((VAL.regs >> (4 * (i0 % VAL.nreg))) & 15), d0);
		if (i0 + 1 < total)
			val_native((u32)((VAL.regs >> (4 * ((i0 + 1) % VAL.nreg))) & 15), d1);
	}
	VAL.k++;
	VAL.left--;
}

static void val_run(const qw_t *p, u32 n)
{
	u32 j = 0;

	while (j < n)
	{
		if (VAL.left && VAL.flg == 2) // image data: nothing to look at
		{
			const u32 m = n - j < VAL.left ? n - j : VAL.left;

			VAL.left -= m;
			VAL.k += m;
			VAL.qwords += m;
			j += m;
			continue;
		}
		val_qword(p[j].d[0], p[j].d[1]);
		j++;
	}
}

// OPT11-STAB (PS2-HW-144): the chain lint of the VIF1 transport (V.on, where the validator above is off: the VU1 program makes the polygon packets). The DIRECT runs of the ring
// are parsed for GIF tags only, and a run whose VIF codes start with FLUSH must not begin while a GIF packet is open (data still to come, or the last tag had EOP = 0): FLUSH waits
// for the end of the packet and the rest of it is behind the FLUSH, the VIF1 and the GIF wait for each other for good (MAPMD: the partial last quadword of a referenced upload).
static void lint_run(const qw_t *p, u32 n)
{
	u32 j = 0;

	while (j < n)
	{
		if (LINT.left)
		{
			const u32 m = n - j < LINT.left ? n - j : LINT.left;

			LINT.left -= m;
			j += m;
			continue;
		}
		{
			const u64 d0 = p[j].d[0];
			const u32 nloop = (u32)(d0 & 0x7FFF), flg = (u32)((d0 >> 58) & 3);
			u32 nreg = (u32)(d0 >> 60);

			if (!nreg)
				nreg = 16;
			LINT.open = !((d0 >> 15) & 1);
			LINT.left = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
			j++;
		}
	}
}

static void lint_buffer(const qw_t *b, u32 wr)
{
	u32 i = 0;

	while (i < wr)
	{
		const u64 t = b[i].d[0], c = b[i].d[1];
		const u32 qwc = (u32)(t & 0xFFFF), id = (u32)((t >> 28) & 7);
		const int direct = ((u32)(c >> 32) & 0x7F000000u) == 0x50000000u;

		if ((u32)c == VIF_FLUSH)
		{
			LINT.flushes++;
			if (LINT.left || LINT.open)
			{
				if (LINT.errs++ < 8)
					I_OutputMsg("HWVAL LINT: VIF1 FLUSH at %u of the buffer while a GIF packet is open (%u qwords still to come, EOP=%d): the FLUSH waits for the end of the packet, stuck for good\n", i, LINT.left, !LINT.open);
				LINT.left = 0;
				LINT.open = 0;
			}
		}
		if (id == 1 || id == 7)
		{
			if (i + 1 + qwc > wr)
				return;
			if (direct && qwc)
				lint_run(b + i + 1, qwc), LINT.runs++;
			i += 1 + qwc;
		}
		else if (id == 3 || id == 0)
		{
			const u32 addr = (u32)(t >> 32);

			if (direct && qwc && !(addr & 15) && addr < 0x02000000u)
				lint_run((const qw_t *)(uintptr_t)addr, qwc), LINT.runs++;
			i++;
		}
		else
			return;
		if (id == 7 || id == 0)
			break;
	}
}

// one finished ring buffer: tags and the data they send, in the order the DMAC sends it
static void val_buffer(const qw_t *b, u32 wr)
{
	u32 i = 0;

	if (LINT.on)
		lint_buffer(b, wr);
	if (!VAL.on)
		return;
	if (H.frame_no >= VAL.summary_frame + 300)
	{
		VAL.summary_frame = H.frame_no;
		val_summary("window");
	}
	while (i < wr)
	{
		const u64 t = b[i].d[0];
		const u32 qwc = (u32)(t & 0xFFFF), id = (u32)((t >> 28) & 7), addr = (u32)(t >> 32);
		u32 j;

		if (id == 1 || id == 7) // CNT / END: the data follows the tag
		{
			if (i + 1 + qwc > wr)
			{
				val_err(VE_DMATAG, "tag %u at %u: %u qwords run past the buffer (%u)", id, i, qwc, wr);
				return;
			}
			(void)j;
			val_run(b + i + 1, qwc);
			i += 1 + qwc;
		}
		else if (id == 3 || id == 0) // REF / REFE
		{
			const qw_t *src = (const qw_t *)(uintptr_t)addr; // KUSEG: the engine data is read through its cached address

			if (addr & 15)
				val_err(VE_DMATAG, "REF address %08x is not 16-byte aligned (the DMAC drops the low bits: the data is read from the wrong place)", addr);
			if (!qwc)
				val_err(VE_DMATAG, "REF with QWC=0");
			if (addr >= 0x02000000u)
				val_err(VE_DMATAG, "REF address %08x is outside the 32 MB of RAM", addr);
			else if (!(addr & 15))
				val_run(src, qwc);
			i += 1;
		}
		else
		{
			val_err(VE_DMATAG, "tag id %u at %u is not CNT/REF/REFE/END", id, i);
			return;
		}
		if (id == 7 || id == 0)
			break;
	}
}
