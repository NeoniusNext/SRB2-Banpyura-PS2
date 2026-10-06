// PS2 GS output for the software renderer (see ps2_gs.h, docs/VIDEO_MODES.md).
//
// Frame path: the index frame (PSMT8, TBW from the width) --GIF path 3 IMAGE, own DMA chain with a REF tag straight
// from the 64-byte aligned buffer--> VRAM texture; CLUT (CT32, CSM1, bit 3/4 swapped) only when the palette changed;
// one textured sprite into the back frame buffer; FINISH. Unchanged packets are reused, and the internal CLUT is
// retained between palette uploads. The CRTC flip (DISPFB2) is done by the vblank
// interrupt handler once the GS reports FINISH, so the main thread never spins on vsync.
// The output format is programmed here with the arithmetic of gsKit's gsKit_set_buffer_attributes (ps2_vmodes.h);
// gsKit itself is used only for its register macros, none of its queues, VRAM allocator or blocking transfers.

#include <tamtypes.h>
#include <kernel.h>
#include <syscallnr.h>
#include <osd_config.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <rom0_info.h>

#include "ps2_gs.h"

#define CLUT_BYTES 8192 // 1 KB used, a whole page so nothing else shares it

// GS registers (A+D addresses)
#define R_PRIM 0x00
#define R_RGBAQ 0x01
#define R_UV 0x03
#define R_XYZ2 0x05
#define R_TEX0_1 0x06
#define R_CLAMP_1 0x08
#define R_XYOFFSET_1 0x18
#define R_PRMODECONT 0x1a
#define R_TEX1_1 0x14
#define R_TEXFLUSH 0x3f
#define R_SCISSOR_1 0x40
#define R_DTHE 0x45
#define R_COLCLAMP 0x46
#define R_TEST_1 0x47
#define R_PABE 0x49
#define R_FBA_1 0x4a
#define R_FRAME_1 0x4c
#define R_ZBUF_1 0x4e
#define R_BITBLTBUF 0x50
#define R_TRXPOS 0x51
#define R_TRXREG 0x52
#define R_TRXDIR 0x53
#define R_FINISH 0x61

// DMA channel 2 (GIF)
#define D2_CHCR ((volatile u32 *)0x1000A000)
#define D2_MADR ((volatile u32 *)0x1000A010)
#define D2_QWC ((volatile u32 *)0x1000A020)
#define D2_TADR ((volatile u32 *)0x1000A030)
#define CHCR_START_CHAIN 0x105 // DIR=1 (from memory), MOD=chain, STR
#define CHCR_STR 0x100

#define TAG_CNT 1
#define TAG_REF 3
#define TAG_END 7

#define SPIN_LIMIT (294912000u / 20u) // 50 ms of EE cycles; bounded spins never hang the game

#define CHAIN_U64 256

static u64 chain[CHAIN_U64] __attribute__((aligned(64)));
static u32 clut[256] __attribute__((aligned(64)));
static u8 *staging; // only used when the caller's buffer is not 64-byte aligned
static size_t staging_bytes;
static struct
{
	u64 *frame_ref, *fb_reg, *tex0_reg;
	int valid, clear;
} packet;

// The optional CLUT upload is the first CNT + REF (eight quadwords).
#define FRAME_CHAIN_OFFSET 16

// what survives ps2gs_shutdown / ps2gs_init (output switches, renderer switches)
static struct
{
	int sw, sh; // source size
	int fit, linear;
} cfg = {PS2GS_FRAME_W, PS2GS_FRAME_H, PS2VM_FIT_43, 0};

static struct
{
	int up;
	int mode; // outid
	const ps2vm_output_t *o;
	int fbw, fbh, stride; // frame buffer, stride in pixels (multiple of 64)
	int dar_w, dar_h;
	int sw, sh; // source size in use
	ps2vm_crtc_t crtc;
	u32 fb[2];
	u32 tex, clut_vram;
	int sema, intc;
	int dx, dy, dw, dh;
	int dest_manual;
	int clear[2]; // per frame buffer: black it before the next frame (the destination rectangle changed)
	int clut_dirty;
	volatile int disp, pend, pend_buf;
	volatile u32 vbl, flips;
	ps2gs_stats_t st;
} g;

int ps2gs_dbg_noswap;

static inline u32 count(void)
{
	u32 v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

static inline u32 phys(const void *p)
{
	return (u32)(uintptr_t)p & 0x0FFFFFFFu;
}

static u64 *ad(u64 *p, u64 data, u32 reg)
{
	*p++ = data;
	*p++ = reg;
	return p;
}

static u64 *giftag(u64 *p, u32 nloop, u32 eop, u32 flg, u32 nreg, u64 regs)
{
	*p++ = (u64)nloop | ((u64)eop << 15) | ((u64)flg << 58) | ((u64)nreg << 60);
	*p++ = regs;
	return p;
}

static void set_dmatag(u64 *t, u32 id, u32 qwc, u32 addr)
{
	t[0] = (u64)qwc | ((u64)id << 28) | ((u64)(addr & 0x7FFFFFFFu) << 32);
	t[1] = 0;
}

static u64 *dmatag(u64 *p, u32 id, u32 qwc, u32 addr)
{
	set_dmatag(p, id, qwc, addr);
	return p + 2;
}

// NLOOP of a PACKED A+D giftag = the A+D entries written after it
static void fix_nloop(u64 *gt, const u64 *end)
{
	gt[0] |= (u64)((end - gt - 2) / 2);
}

static u64 vertex(int x, int y)
{
	return (u64)((2048 + x) * 16) | ((u64)((2048 + y) * 16) << 16);
}

// state that must be valid for every draw (cheap, so it is resent with each frame)
static u64 *draw_env(u64 *p, u32 fb)
{
	p = ad(p, 1, R_PRMODECONT); // the primitive attributes come from PRIM (gsKit sets this once; there is no gsKit init here)
	p = ad(p, (u64)(2048 * 16) | ((u64)(2048 * 16) << 32), R_XYOFFSET_1);
	p = ad(p, 0 | ((u64)(g.fbw - 1) << 16) | ((u64)0 << 32) | ((u64)(g.fbh - 1) << 48), R_SCISSOR_1);
	p = ad(p, (u64)1 << 32, R_ZBUF_1); // ZMSK=1: the Z buffer is never written
	p = ad(p, (1u << 16) | (1u << 17), R_TEST_1); // ZTE=1, ZTST=ALWAYS, no alpha test
	p = ad(p, 1, R_COLCLAMP);
	p = ad(p, 0, R_DTHE);
	p = ad(p, 0, R_PABE);
	p = ad(p, 0, R_FBA_1);
	p = ad(p, (u64)(fb / 8192) | ((u64)(g.stride / 64) << 16) | ((u64)GS_PSM_CT32 << 24), R_FRAME_1);
	return p;
}

#define BITBLTBUF(dbp, dbw, dpsm) (((u64)((dbp) / 256) << 32) | ((u64)(dbw) << 48) | ((u64)(dpsm) << 56))

// host -> local upload header: PACKED A+D block + IMAGE giftag, as one CNT segment
static u64 *upload_header(u64 *p, u32 vram, u32 tbw, u32 psm, u32 w, u32 h, u32 qwords)
{
	u64 *t = p;
	p += 2;
	p = giftag(p, 4, 0, 0, 1, 0xE);
	p = ad(p, BITBLTBUF(vram, tbw, psm), R_BITBLTBUF);
	p = ad(p, 0, R_TRXPOS);
	p = ad(p, (u64)w | ((u64)h << 32), R_TRXREG);
	p = ad(p, 0, R_TRXDIR);
	p = giftag(p, qwords, 0, 2, 0, 0);
	set_dmatag(t, TAG_CNT, (u32)(p - t - 2) / 2, 0);
	return p;
}

static void dma_start(const u64 *start)
{
	*D2_QWC = 0;
	*D2_TADR = phys(start);
	__asm__ volatile("sync.l");
	*D2_CHCR = CHCR_START_CHAIN;
	__asm__ volatile("sync.p");
}

// true when the GIF DMA drained (bounded)
static int dma_wait(void)
{
	u32 t0 = count();
	while (*D2_CHCR & CHCR_STR)
	{
		if ((u32)(count() - t0) > SPIN_LIMIT)
		{
			g.st.timeouts++;
			return 0;
		}
	}
	return 1;
}

static int finish_wait(void)
{
	u32 t0 = count();
	while (!(*GS_CSR & 2))
	{
		if ((u32)(count() - t0) > SPIN_LIMIT)
		{
			g.st.timeouts++;
			return 0;
		}
	}
	return 1;
}

// black sprite over the whole frame buffer (state: PRIM untextured)
static u64 *black_sprite(u64 *p)
{
	p = ad(p, 6, R_PRIM); // sprite, no texture
	p = ad(p, 0x80000000u | ((u64)0x3f800000 << 32), R_RGBAQ);
	p = ad(p, vertex(0, 0), R_XYZ2);
	p = ad(p, vertex(g.fbw, g.fbh), R_XYZ2);
	return p;
}

// Used only while bringing the GS up: fill a frame buffer with black and wait for the GS.
static void clear_fb(u32 fb)
{
	u64 *p = chain + 2;
	u64 *t = chain;
	u64 *gt = p;
	p = giftag(p, 0, 1, 0, 1, 0xE);
	p = draw_env(p, fb);
	p = black_sprite(p);
	p = ad(p, 0, R_FINISH);
	fix_nloop(gt, p);
	set_dmatag(t, TAG_END, (u32)(p - t - 2) / 2, 0);
	FlushCache(0);
	dma_wait();
	*GS_CSR = 2;
	dma_start(chain);
	dma_wait();
	finish_wait();
}

static int vblank_handler(int cause)
{
	(void)cause;
	g.vbl++;
	if (g.pend && (*GS_CSR & 2)) // the frame is completely drawn: show it
	{
		int b = g.pend_buf;
		GS_SET_DISPFB2(g.fb[b] / 8192, g.stride / 64, GS_PSM_CT32, 0, 0);
		g.disp = b;
		g.pend = 0;
		g.flips++;
	}
	iSignalSema(g.sema);
	ExitHandler();
	return 0;
}

int ps2gs_bios_version(void)
{
	char rom[16];

	memset(rom, 0, sizeof rom);
	GetRomName(rom); // "0200EC20040614": VVVVRTYYYYMMDD
	return (int)strtol(rom, NULL, 10); // stops at the region letter: 200
}

static int detect_pal(void)
{
	char rom[16];

	memset(rom, 0, sizeof rom);
	GetRomName(rom); // index 4 is the region letter, 'E' = Europe
	return rom[4] == 'E';
}

int ps2gs_region_output(void)
{
	return detect_pal() ? PS2GS_MODE_PAL : PS2GS_MODE_NTSC;
}

static int source_ok(int w, int h)
{
	return w >= 8 && h >= 1 && w <= PS2VM_MAXW && h <= PS2VM_MAXH && !(w & 7) && !((w * h) & 63);
}

int ps2gs_check(int outid, int w, int h)
{
	const ps2vm_output_t *o;

	if (outid == PS2GS_MODE_AUTO)
		outid = detect_pal() ? PS2GS_MODE_PAL : PS2GS_MODE_NTSC;
	o = ps2vm_output(outid);
	if (!o || !source_ok(w, h))
		return PS2GS_E_ID;
	if ((o->flags & PS2VM_NEEDS_BIOS220) && ps2gs_bios_version() < 220)
		return PS2GS_E_BIOS;
	if (ps2vm_vram_pages(o, w, h) > PS2VM_VRAM_PAGES)
		return PS2GS_E_VRAM;
	return PS2GS_OK;
}

// where in VRAM the source texture and the CLUT are (depends on the source size)
static void layout_vram(void)
{
	u32 fbbytes = (u32)ps2vm_fb_pages(g.fbw, g.fbh) * 8192u;
	g.fb[0] = 0;
	g.fb[1] = fbbytes;
	g.tex = 2 * fbbytes;
	g.clut_vram = g.tex + (u32)ps2vm_tex_pages(g.sw, g.sh) * 8192u;
}

static void update_dest(void)
{
	int x, y, w, h, full;

	if (g.dest_manual)
		return;
	ps2vm_dest(cfg.fit, g.sw, g.sh, g.fbw, g.fbh, g.dar_w, g.dar_h, &x, &y, &w, &h);
	full = (x == 0 && y == 0 && w == g.fbw && h == g.fbh);
	if (x != g.dx || y != g.dy || w != g.dw || h != g.dh)
	{
		if (!full || g.dw != g.fbw || g.dh != g.fbh) // the borders may hold the old picture
			g.clear[0] = g.clear[1] = 1;
		g.dx = x; g.dy = y; g.dw = w; g.dh = h;
		packet.valid = 0;
	}
}

// Program SMODE/PMODE/DISPFB/DISPLAY like gsKit_init_screen does for the same mode.
static void crtc_start(const ps2vm_output_t *o)
{
	int ddx = 0, ddy = 0, ddw, ddh;

	if (o->gsmode != 0x02 && o->gsmode != 0x03 && GetSyscallHandler(__NR__GetGsDxDyOffset)) // board specific offsets
		_GetGsDxDyOffset(o->gsmode, &ddx, &ddy, &ddw, &ddh);
	ps2vm_crtc(o, ddx, ddy, &g.crtc);

	GS_RESET();
	__asm__ volatile("sync.p; nop;");
	*GS_CSR = 0x00000000; // clean CSR registers
	GsPutIMR(0x00007F00); // masks all interrupts

	SetGsCrt(o->interlace, o->gsmode, o->ffmd);
	if (o->gsmode == 0x51 && o->ffmd) // gsKit: fix 1080i frame mode
		GS_SET_SMODE2(1, 1, 0);

	DIntr();
	GS_SET_PMODE(0, 1, 0, 1, 0, 0x80); // read circuit 2 only, alpha 1.0
	GS_SET_DISPFB1(0, g.stride / 64, GS_PSM_CT32, 0, 0);
	GS_SET_DISPFB2(0, g.stride / 64, GS_PSM_CT32, 0, 0);
	GS_SET_DISPLAY1(g.crtc.dx, g.crtc.dy, g.crtc.magh, g.crtc.magv, g.crtc.dw - 1, g.crtc.dh - 1);
	GS_SET_DISPLAY2(g.crtc.dx, g.crtc.dy, g.crtc.magh, g.crtc.magv, g.crtc.dw - 1, g.crtc.dh - 1);
	GS_SET_BGCOLOR(0, 0, 0);
	EIntr();
}

int ps2gs_init(int outid)
{
	ee_sema_t sema;
	const ps2vm_output_t *o;
	int i, r;

	if (g.up)
		return 0;
	if (outid == PS2GS_MODE_AUTO)
		outid = detect_pal() ? PS2GS_MODE_PAL : PS2GS_MODE_NTSC;
	r = ps2gs_check(outid, cfg.sw, cfg.sh);
	if (r)
		return r;
	o = ps2vm_output(outid);

	memset(&g, 0, sizeof g);
	packet.valid = 0;
	g.mode = outid;
	g.o = o;
	g.fbw = o->fbw;
	g.fbh = o->fbh;
	g.stride = ps2vm_fb_stride(o->fbw);
	g.dar_w = o->dar_w;
	g.dar_h = o->dar_h;
	if ((o->flags & PS2VM_SD) && configGetTvScreenType() == TV_SCREEN_169) // anamorphic SD output on a wide TV
	{
		g.dar_w = 16;
		g.dar_h = 9;
	}
	g.sw = cfg.sw;
	g.sh = cfg.sh;
	layout_vram();

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	crtc_start(o);

	g.dx = 0;
	g.dy = 0;
	g.dw = g.fbw;
	g.dh = g.fbh;

	*GS_IMR = 0xFF00; // no GS interrupts, only vblank is used
	clear_fb(g.fb[1]);
	clear_fb(g.fb[0]);
	GS_SET_DISPFB2(g.fb[0] / 8192, g.stride / 64, GS_PSM_CT32, 0, 0);
	g.disp = 0;
	update_dest();

	sema.init_count = 0;
	sema.max_count = 1;
	sema.option = 0;
	g.sema = CreateSema(&sema);
	if (g.sema < 0)
		return PS2GS_E_SEMA;

	for (i = 0; i < 256; i++)
		clut[i] = 0x80000000u;
	g.clut_dirty = 1;

	g.intc = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
	EnableIntc(INTC_VBLANK_S);
	g.up = 1;
	return PS2GS_OK;
}

void ps2gs_shutdown(void)
{
	int drained;
	if (!g.up)
		return;
	g.up = 0;
	drained = dma_wait();
	DisableIntc(INTC_VBLANK_S);
	RemoveIntcHandler(INTC_VBLANK_S, g.intc);
	DeleteSema(g.sema);
	if (drained) // a timed-out DMA may still hold a REF to this buffer
	{
		free(staging);
		staging = NULL;
		staging_bytes = 0;
	}
	packet.valid = 0;
}

int ps2gs_is_up(void)
{
	return g.up;
}

int ps2gs_set_source(int w, int h)
{
	if (!source_ok(w, h))
		return PS2GS_E_ID;
	if (!g.up)
	{
		cfg.sw = w;
		cfg.sh = h;
		return PS2GS_OK;
	}
	if (w == g.sw && h == g.sh)
		return PS2GS_OK;
	if (ps2vm_vram_pages(g.o, w, h) > PS2VM_VRAM_PAGES)
		return PS2GS_E_VRAM;

	// the GS may still be reading the old texture: let the last chain finish before the layout moves
	if (!dma_wait() || (g.pend && !finish_wait()))
		return PS2GS_E_GS;
	cfg.sw = g.sw = w;
	cfg.sh = g.sh = h;
	layout_vram();
	g.clut_dirty = 1; // the CLUT moves with the texture end
	g.dest_manual = 0;
	g.clear[0] = g.clear[1] = 1;
	packet.valid = 0;
	update_dest();
	return PS2GS_OK;
}

void ps2gs_set_fit(int fit)
{
	cfg.fit = (fit >= 0 && fit < PS2VM_FIT_COUNT) ? fit : PS2VM_FIT_43;
	g.dest_manual = 0;
	if (g.up)
		update_dest();
}

void ps2gs_get_dest(int *x, int *y, int *w, int *h)
{
	*x = g.dx;
	*y = g.dy;
	*w = g.dw;
	*h = g.dh;
}

void ps2gs_set_palette(const unsigned int *rgb)
{
	int i;

	for (i = 0; i < 256; i++)
	{
		// CT32 CLUT in CSM1 swaps bits 3 and 4 of the index (a PSMT8 index addresses blocks of 8 entries)
		unsigned idx = ps2gs_dbg_noswap ? (unsigned)i : (((unsigned)i & ~0x18u) | (((unsigned)i & 0x08u) << 1) | (((unsigned)i & 0x10u) >> 1));
		u32 colour = (rgb[i] & 0x00FFFFFFu) | 0x80000000u;
		if (clut[idx] != colour)
		{
			clut[idx] = colour;
			g.clut_dirty = 1;
		}
	}
}

void ps2gs_set_dest(int x, int y, int w, int h)
{
	if (x != g.dx || y != g.dy || w != g.dw || h != g.dh)
	{
		g.clear[0] = g.clear[1] = 1;
		packet.valid = 0;
	}
	g.dx = x;
	g.dy = y;
	g.dw = w;
	g.dh = h;
	g.dest_manual = 1;
}

void ps2gs_set_filter(int linear)
{
	linear = linear ? 1 : 0;
	if (cfg.linear != linear)
	{
		cfg.linear = linear;
		packet.valid = 0;
	}
}

void ps2gs_wait_vblank(int n)
{
	if (!g.up)
		return;
	while (n-- > 0)
	{
		while (PollSema(g.sema) == g.sema)
			; // drop vblanks that passed already
		WaitSema(g.sema);
	}
}

// Everything the GS needs for one frame, in one chain:
//   [CLUT image] [index image] state, TEXFLUSH, [black fill], sprite, FINISH
static void build_frame(const u8 *frame, u32 fb, int clear)
{
	u64 *p = chain;
	u64 *t, *gt;
	u32 clut_tex = g.clut_vram / 256;
	const u32 tbw = (u32)ps2vm_tex_tbw(g.sw);
	const u32 qwords = (u32)(g.sw * g.sh) / 16;

	p = upload_header(p, g.clut_vram, 1, GS_PSM_CT32, 16, 16, 64);
	p = dmatag(p, TAG_REF, 64, phys(clut));
	p = upload_header(p, g.tex, tbw, GS_PSM_T8, (u32)g.sw, (u32)g.sh, qwords);
	packet.frame_ref = p;
	p = dmatag(p, TAG_REF, qwords, phys(frame));

	t = p;
	p += 2;
	gt = p;
	p = giftag(p, 0, 1, 0, 1, 0xE);
	p = ad(p, 0, R_TEXFLUSH);
	p = draw_env(p, fb);
	packet.fb_reg = p - 2; // FRAME_1 is the last register of draw_env.
	if (clear)
	{
		p = black_sprite(p); // the picture moved: the bars of the old one go
	}
	packet.tex0_reg = p;
	p = ad(p, (u64)(g.tex / 256) | ((u64)tbw << 14) | ((u64)GS_PSM_T8 << 20) | ((u64)ps2vm_log2ceil(g.sw) << 26) | ((u64)ps2vm_log2ceil(g.sh) << 30)
		| ((u64)1 << 34) | ((u64)1 << 35) | ((u64)clut_tex << 37) | ((u64)GS_PSM_CT32 << 51) | ((u64)1 << 61), R_TEX0_1); // DECAL, load CLUT
	p = ad(p, cfg.linear ? ((1u << 5) | (1u << 6)) : 0, R_TEX1_1);
	p = ad(p, 2 | (2 << 2) | ((u64)0 << 4) | ((u64)(g.sw - 1) << 14) | ((u64)0 << 24) | ((u64)(g.sh - 1) << 34), R_CLAMP_1); // region clamp
	p = ad(p, 6 | (1 << 4) | (1 << 8), R_PRIM); // sprite, TME, FST
	p = ad(p, 0x80808080u | ((u64)0x3f800000 << 32), R_RGBAQ);
	p = ad(p, 0, R_UV);
	p = ad(p, vertex(g.dx, g.dy), R_XYZ2);
	p = ad(p, (u64)(g.sw * 16) | ((u64)(g.sh * 16) << 16), R_UV);
	p = ad(p, vertex(g.dx + g.dw, g.dy + g.dh), R_XYZ2);
	p = ad(p, 0, R_FINISH);
	fix_nloop(gt, p);
	set_dmatag(t, TAG_END, (u32)(p - t - 2) / 2, 0);
	packet.valid = 1;
	packet.clear = clear;
	g.st.packet_builds++;
}

void ps2gs_present(const unsigned char *frame, int wait_flip)
{
	u32 t0, t1, tb;
	int b, with_clut;
	const size_t bytes = (size_t)g.sw * g.sh;

	if (!g.up)
		return;
	t0 = count();

	if (!frame)
		return;
	if (!dma_wait()) // includes an older staging REF before copying or resizing its buffer
		return;
	if ((uintptr_t)frame & 63) // DMA wants 16, we promise 64
	{
		if (staging_bytes < bytes)
		{
			free(staging);
			staging = memalign(64, bytes);
			staging_bytes = staging ? bytes : 0;
		}
		if (!staging)
			return;
		memcpy(staging, frame, bytes);
		frame = staging;
	}

	if (wait_flip && g.pend)
	{
		g.st.waited++;
		ps2gs_wait_vblank(1); // at most one vblank, then the frame is replaced if it still waits
	}
	if (g.pend && !finish_wait()) // a timed-out draw must not be overwritten
		return;

	with_clut = g.clut_dirty;
	tb = count();
	for (;;)
	{
		u32 irq;

		b = g.disp ^ 1; // the buffer the CRTC is not reading
		if (!packet.valid || packet.clear != g.clear[b])
			build_frame(frame, g.fb[b], g.clear[b]);
		else
		{
			packet.frame_ref[0] = (packet.frame_ref[0] & 0xFFFFFFFFu) | ((u64)phys(frame) << 32);
			packet.fb_reg[0] = (packet.fb_reg[0] & ~0x1FFull) | (g.fb[b] / 8192);
		}
		// CLD=0 keeps the internal CLUT; reload only after its VRAM upload.
		packet.tex0_reg[0] = (packet.tex0_reg[0] & ~(7ull << 61)) | ((u64)with_clut << 61);
		FlushCache(0); // chain, CLUT and frame to RAM: dirty D-cache lines are what the DMA must see
		t1 = count();
		irq = DIntr();
		if ((g.disp ^ 1) == b)
		{
			if (g.pend)
				g.st.dropped++;
			*GS_CSR = 2; // clear FINISH; the handler flips only after this frame's FINISH
			g.pend_buf = b;
			g.pend = 1;
			g.clear[b] = 0; // retire the clear only when this packet is actually submitted
			g.clut_dirty = 0;
			if (with_clut)
				g.st.clut_uploads++;
			dma_start(with_clut ? chain : chain + FRAME_CHAIN_OFFSET);
			if (irq)
				EIntr();
			break;
		}
		if (irq)
			EIntr(); // the flip happened while the chain was built: rebuild for the other buffer
	}
	if ((u32)(t1 - tb) > g.st.flush_max)
		g.st.flush_max = t1 - tb;
	g.st.frames++;

	t1 = count();
	dma_wait(); // the caller will draw into the frame again: wait until the GIF has read it
	if ((u32)(count() - t1) > g.st.dma_wait_max)
		g.st.dma_wait_max = count() - t1;
	if ((u32)(count() - t0) > g.st.present_max)
		g.st.present_max = count() - t0;
}

int ps2gs_fb_width(void)
{
	return g.fbw;
}

int ps2gs_fb_height(void)
{
	return g.fbh;
}

int ps2gs_mode(void)
{
	return g.mode;
}

int ps2gs_source_width(void)
{
	return g.sw;
}

int ps2gs_source_height(void)
{
	return g.sh;
}

void ps2gs_get_crtc(ps2vm_crtc_t *c)
{
	*c = g.crtc;
}

void ps2gs_get_layout(unsigned int *fb0, unsigned int *fb1, unsigned int *tex, unsigned int *clut_addr)
{
	*fb0 = g.fb[0];
	*fb1 = g.fb[1];
	*tex = g.tex;
	*clut_addr = g.clut_vram;
}

unsigned int ps2gs_fb_block(int index)
{
	return g.fb[index & 1] / 256;
}

int ps2gs_displayed(void)
{
	return g.disp;
}

int ps2gs_flip_pending(void)
{
	return g.pend;
}

void ps2gs_get_stats(ps2gs_stats_t *out)
{
	*out = g.st;
	out->flips = g.flips;
	out->vblanks = g.vbl;
}
