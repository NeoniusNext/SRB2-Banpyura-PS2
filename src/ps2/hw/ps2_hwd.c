// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// Copyright (C) 1999-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_hwd.c
/// \brief GS hardware renderer: implements struct hwdriver_s (src/hardware/hw_drv.h) on the Graphics Synthesizer.
///        Design, VRAM layout and the list of deviations from the OpenGL driver: docs/HW_RENDERER.md.
///        The code is split into .inc files that are part of this translation unit:
///        ps2_hw_priv.inc (state), ps2_hw_gs.inc (bring-up, DMA ring, frames), ps2_hw_xform.inc (matrices, clipping),
///        ps2_hw_light.inc (the Doom light staircase), ps2_hw_tex.inc (VRAM pool, textures, CLUT), ps2_hw_draw.inc (render
///        plans, passes, pieces, vertex packing), ps2_hw_model.inc, ps2_hw_screen.inc (captures, wipes, read-back).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

#include <tamtypes.h>
#include <kernel.h>
#include <delaythread.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <rom0_info.h>
#include <screenshot.h>

#include "../../doomdef.h"
#include "../../console.h"
#include "../../hardware/hw_drv.h"
#include "../../hardware/hw_main.h"
#ifdef PS2_HWDETAIL
extern boolean hwr_sprite_batch; // hw_batching.c
#endif
#include "../../m_argv.h" // -hwnosplit
#include "../../r_main.h" // rendertimefrac (the shader time of the water, PS2-HW-125)
#include "../../z_zone.h" // PS2-171: hwbig_alloc

#include "ps2_hwd.h"
#include "ps2_hwd_dbg.h"
#include "../ps2_models.h" // PS2Models_TryAlloc (the model work areas)

// PS2-HW-67: the driver's diagnostics (limitation warnings, texture cap changes, HWT/HWFX/HWPROF traces) go to the log only: through CONS_Printf/CONS_Alert
// they were drawn over the game picture as console lines ("HWD texture cap 512 -> 1024 blocks", "WARNING: PS2 GS experimental limitation ...")
#include "../../i_system.h"
#define CONS_Printf I_OutputMsg
#define CONS_Alert(level, ...) I_OutputMsg(__VA_ARGS__)

#include "ps2_hw_priv.inc"
#include "ps2_hw_hg.inc" // OPT10 HG: geometry-path counters (HWPROF3)
#include "ps2_hw_vif.inc" // PS2-HW-44: VIF1 as the transport of the GIF stream (-hwdbg 0x4000000)
#include "ps2_hw_regs.inc"
#include "ps2_hw_gs.inc"
#include "ps2_hw_val.inc" // PS2-HW-74: -hwdbg 536870912 validates the GIF stream
#include "ps2_hw_xform.inc"
#include "ps2_hw_vu0.inc"
#include "ps2_hw_light.inc"
#include "ps2_hw_pal.inc" // PS2-HW-71: palette rendering (light tables as CLUT rows)
#include "ps2_hw_fx2.h" // OPT11 round 3 (FX3): -hwfx bits (FX3_NOFILL in the texture upload)
#include "ps2_hw_tex.inc"
#include "ps2_hw_draw.inc"
#include "ps2_hw_plan.inc"
#include "ps2_hw_fx2.inc" // OPT11 round 2 (FX2): the sphere test data of the things, -hwfx
static void settex_now(GLMipmap_t *TexInfo); // (below)
#include "ps2_hw_spr.inc" // OPT11 round 3 (FX3): the sprite stream (VU1 sprite program)
#include "ps2_hw_sky.inc" // PS2-HW-42: the sky dome as strips (OPT9)
#include "ps2_hw_model.inc"
#include "ps2_hw_wire.inc" // OPT11-MODEL (PS2-HW-270): PF_WireFrame and gr_wireframe as GS lines
#include "ps2_hw_tt.inc" // PS2-HW-69: -hwtextest texture conformance self-test
#include "ps2_hw_bench.inc" // OPT11 round 2: -hwbench, what the instructions cost

static FOutVector *sky_vertices;
static float *sky_colors;
static u32 *sky_indices;
static int sky_capacity, sky_index_capacity;

void PS2HWD_SetFatalHandler(void (*handler)(const char *message))
{
	resource_error_handler = handler;
}

static int trace_frame = -1;

void PS2HWD_SetTrace(int frame, int dbg_flags)
{
	trace_frame = frame;
	ps2hwd_dbg_flags = dbg_flags;
}

#define TRACING() (trace_frame >= 0 && H.frame_no == (u32)trace_frame)

// ==========================================================================
// frame buffer read-back (screenshots, tests)
// ==========================================================================

// Wait until the GS has executed everything queued so far (also in the middle of a frame).
int PS2HWD_Sync(void)
{
	qw_t *p;
	int ok;

	if (!H.up)
		return 0;
	if (H.gate && H.pend)
		flip_wait(); // the CSR FINISH bit below must not take the finished frame's
	ov_flush_all();
	pk_flush();
	ring_wait(0);
	*GS_CSR = 2;
	p = ad_alloc(1);
	ad_set(p, 0, GSR_FINISH);
	H.finish_pending = 1;
	pk_flush();
	ok = gs_wait_finished();
	return ok;
}

static int psm_bytes(int psm)
{
	switch (psm)
	{
		case PSM_Z24: // transfers of 24-bit formats are packed: ps2_screenshot does not do that, only use 32-bit formats for Z
		case PSM_CT16:
		case PSM_CT16S:
			return 2;
		default:
			return 4;
	}
}

// Reads w x h pixels of a VRAM buffer into dst. ps2_screenshot takes the buffer width from `w` (TBW = w / 64), so rows
// are always full buffer rows: x must be 0 and w a multiple of 64.
int PS2HWD_ReadVram(void *dst, unsigned int block256, unsigned int tbw, int psm, int x, int y, int w, int h)
{
	int bpp = psm_bytes(psm), rows, done = 0, ok = 1;

	if (!H.up || !dst || ((uintptr_t)dst & 63) || x != 0 || y < 0 || y + h > 2048
		|| w != (int)(tbw * 64) || w <= 0 || h <= 0 || block256 >= VRAM_BLOCKS
		|| (psm != PSM_CT32 && psm != PSM_CT16 && psm != PSM_CT16S && psm != PSM_Z32))
		return -1;
	if (!PS2HWD_Sync())
		return -2;
	rows = 65536 / w; // ps2_screenshot moves at most 64K pixels per call
	if (rows < 1)
		rows = 1;
	while (done < h)
	{
		int n = h - done < rows ? h - done : rows;
		u8 *d = (u8 *)dst + (size_t)done * w * bpp;

		SyncDCache(d, d + (size_t)n * w * bpp);
		ok &= ps2_screenshot(d, block256, 0, (unsigned)(y + done), (unsigned)w, (unsigned)n, (unsigned)psm) == 1;
		SyncDCache(d, d + (size_t)n * w * bpp);
		done += n;
	}
	return ok ? 0 : -3;
}

// frame buffer pixel -> 0x00BBGGRR
static inline u32 expand_pixel(u32 v, int fb32)
{
	if (fb32)
		return v & 0x00FFFFFFu;
	return ((v & 0x1F) << 3 | (v & 0x1F) >> 2) | (((v >> 5) & 0x1F) << 3 | ((v >> 5) & 0x1F) >> 2) << 8 | (((v >> 10) & 0x1F) << 3 | ((v >> 10) & 0x1F) >> 2) << 16;
}

// the frame buffer that holds the newest complete (or, in the middle of a frame, the current) picture
static int readable_buffer(void)
{
	return H.frame_open ? H.target : H.last_drawn;
}

int PS2HWD_ReadFrame(unsigned int *dst)
{
	int buf = readable_buffer(), bpp = H.fb32 ? 4 : 2, x, y, r;
	void *tmp;

	if (!H.up || !dst)
		return -1;
	tmp = memalign(64, (size_t)H.fbw * H.fbh * bpp);
	if (!tmp)
		return -4;
	r = PS2HWD_ReadVram(tmp, H.fb_addr[buf] / 256, (unsigned)(H.fbw / 64), H.fbpsm, 0, 0, H.fbw, H.fbh);
	if (r == 0)
	{
		for (y = 0; y < H.vh; y++)
			for (x = 0; x < H.vw; x++)
			{
				u32 v = H.fb32 ? ((u32 *)tmp)[y * H.fbw + x] : ((u16 *)tmp)[y * H.fbw + x];

				dst[y * H.vw + x] = expand_pixel(v, H.fb32);
			}
	}
	free(tmp);
	return r;
}

int PS2HWD_ReadDepth(unsigned int *dst)
{
	int x, y, r;
	u32 *tmp;

	if (!H.up || !dst)
		return -1;
	tmp = memalign(64, (size_t)H.fbw * H.fbh * 4);
	if (!tmp)
		return -4;
	r = PS2HWD_ReadVram(tmp, H.z_addr / 256, (unsigned)(H.fbw / 64), PSM_Z32, 0, 0, H.fbw, H.fbh); // Z24 shares the layout of Z32; a 24-bit transfer would be packed to 3 bytes
	if (r == 0)
		for (y = 0; y < H.vh; y++)
			for (x = 0; x < H.vw; x++)
				dst[y * H.vw + x] = tmp[y * H.fbw + x] & 0x00FFFFFFu;
	free(tmp);
	return r;
}

#include "ps2_hw_screen.inc"

// ==========================================================================
// bring-up and shutdown
// ==========================================================================

void PS2HWD_Configure(const ps2hwd_config_t *c)
{
	if (!H.up && c)
		cfg = *c;
}

void PS2HWD_GetConfig(ps2hwd_config_t *c)
{
	*c = cfg;
}

static void view_defaults(void)
{
	H.scr_w = cfg.screen_w;
	H.scr_h = cfg.screen_h;
	H.sx = (float)H.vw / (float)H.scr_w;
	H.sy = (float)H.vh / (float)H.scr_h;
	m_identity(H.mv);
	m_scale(H.mv, 1.0f, 1.0f, -1.0f);
	xf_set_2d(NZCLIP_DEFAULT);
	viewport_set(0, 0, H.scr_w, H.scr_h);
}

static void rec_init(void)
{
	int i;

	H.rec_free = NOREC;
	H.lru_head = H.lru_tail = NOREC;
	H.cur_tex = NOREC;
	for (i = 0; i < NSCR; i++)
		H.scr_rec[i] = NOREC;
}

// PS2-171 (OPT11-STAB): the driver's big work arrays (ovq, cutbuf, plan_info, blk_owner: 230 KB) are zone blocks that exist while the driver runs.
// They were .bss: a game that runs the software renderer (MAP11 / CEZ2 has 0.2 MB of the arena left) carried them for nothing.
static unsigned hwbig_nomem_n;

static void hwbig_free(void)
{
	Z_Free(ovq_p);
	Z_Free(cutbuf_p);
	Z_Free(plan_info_p);
	Z_Free(blk_owner_p);
	ovq_p = NULL;
	cutbuf_p = NULL;
	plan_info_p = NULL;
	blk_owner_p = NULL;
}

static boolean hwbig_alloc(void)
{
	if (M_CheckParm("-hwnomem") && !(hwbig_nomem_n++ & 1)) // test: every other start of the driver finds no memory (-hwnomem: the fallback to software and the return)
	{
		CONS_Alert(CONS_ERROR, "PS2 GS hardware renderer: no memory for its work arrays (-hwnomem)\n");
		return false;
	}
	ovq_p = Z_TryMallocAlign(sizeof *ovq_p, PU_STATIC, NULL, 6);
	cutbuf_p = Z_TryMallocAlign(sizeof *cutbuf_p, PU_STATIC, NULL, 6);
	plan_info_p = Z_TryMallocAlign(sizeof *plan_info_p, PU_STATIC, NULL, 6);
	blk_owner_p = Z_TryMallocAlign(sizeof *blk_owner_p, PU_STATIC, NULL, 6);
	if (ovq_p && cutbuf_p && plan_info_p && blk_owner_p)
	{
		memset(ovq_p, 0, sizeof *ovq_p);
		memset(cutbuf_p, 0, sizeof *cutbuf_p);
		memset(plan_info_p, 0, sizeof *plan_info_p);
		memset(blk_owner_p, 0, sizeof *blk_owner_p);
		return true;
	}
	hwbig_free();
	CONS_Alert(CONS_ERROR, "PS2 GS hardware renderer: no memory for its work arrays\n");
	return false;
}

boolean PS2HWD_Init(void)
{
	int rc;

	if (H.up)
		return true;
	if (!hwbig_alloc())
		return false; // VID_StartupOpenGL / VID_CheckRenderer then stay with the software renderer
	memset(&H, 0, sizeof H);
	// PS2-HW-255 (OPT11 round 3, FX3): the sprite stream is opt-in (-hwspr). Measured on the tree with the VU2 collection of polygons in blocks (docs/GATES/g1/opt11-FX.md 7.7): the stream costs the EE
	// what the batch costs for the same sprites (D1 +1.3 % of the sprite set, D2 -2.7 %, D3 -1.5 %, D4 +4.6 %, against the cheap projection alone), and its lists are 75 KB of the zone, which is
	// at its edge on 32 MB (DEMO_003 ends in "Out of memory" on the tip of the main branch already). Without -hwspr the lists are not made and every sprite goes through the batch.
	if (!M_CheckParm("-hwspr"))
		ps2hwd_fx2 |= FX3_NOSTREAM;
	// PS2-HW-251: the parallelogram test (PS2HWD_ParaHidden) for shadows and the exact test of sprites is opt-in (-hwpara) on the tree merged with MODEL / AUDIO: the check mode (-hwfx 2) of the merged
	// ELF reported 38574 of 116736 shadow quads that the test hides and PS2HWD_QuadHidden does not (1..3 before the merge); not explained, not run again (delivered without tests).
	if (!M_CheckParm("-hwpara"))
		ps2hwd_fx2 |= FX3_NOPARA;
	if (!(ps2hwd_fx2 & (FX3_NOSTREAM | FX3_NOSPR)))
		spr_alloc(); // the lists of the sprite stream (zone blocks while the driver runs; no stream when there is no room)
	wd_last_flips = ~0u;
	vu_noretarget = M_CheckParm("-hwnoretarget") != 0; // PS2-HW-107 off (A/B)
	if (M_CheckParm("-hwqh") && M_IsNextParm())
		qh_mode = atoi(M_GetNextParm()); // PS2-HW-220: 1 = the scalar sprite test, 2 = both and the differences counted
	vu_nobretarget = M_CheckParm("-hwnobretarget") != 0;
	if (M_CheckParm("-hwbench"))
		PS2HWD_Bench();
	if (M_CheckParm("-hwplan") && M_IsNextParm())
		plan_mode = atoi(M_GetNextParm()); // PS2-HW-229: 0 = VU0 transform and lean arithmetic of the planner, 1 = scalar as before, 2 = both, compared
	vu_nocut = M_CheckParm("-hwnocut") != 0; // PS2-HW-236 off (-hwnocut 0 = on, for an A/B run with the same form of the command line): the polygons that need cutting at whole repeats (and the fans of more than 12 vertices) are drawn by the EE as before
	if (vu_nocut && M_CheckParm("-hwnocut") && M_IsNextParm())
		vu_nocut = atoi(M_GetNextParm()) != 0;
	pk_oldtail = M_CheckParm("-hwoldtail") != 0;
	if (M_CheckParm("-hwvudump") && M_IsNextParm())
		vu_dump_frame = (u32)atoi(M_GetNextParm());
	if (M_CheckParm("-hwvustop") && M_IsNextParm())
		vu_stop_frame = (u32)atoi(M_GetNextParm());
	if (M_CheckParm("-hwvumax") && M_IsNextParm())
		vu_max_poly = (u32)atoi(M_GetNextParm());
	vu_norecord = M_CheckParm("-hwnorecord") != 0; // PS2-HW-111 off: a retargeted plan sets its GS state up as before
	if (M_CheckParm("-hwbretmask") && M_IsNextParm())
		vu_bretmask = atoi(M_GetNextParm());
	tex_nosplit = M_CheckParm("-hwnosplit") != 0; // PS2-HW-70 off: images over 1024 rows are decimated
	tex_split_rows = 1024;
	if (M_CheckParm("-hwsplitrows") && M_IsNextParm())
	{
		const int rows = atoi(M_GetNextParm());

		if (rows >= 64 && rows <= 1024 && !(rows & 63))
			tex_split_rows = (u32)rows; // test: the split is exercised on ordinary textures
	}
	H.dmac = -1;
	H.sema_vbl = H.sema_dma = -1;
	H.shader = -1;
	ramp_tex = NOREC;
	OV.n = 0;
	col_lut_init();
	rec_init();
	rc = gs_setup();
	if (rc)
	{
		CONS_Alert(CONS_ERROR, "PS2 GS hardware renderer: GS setup failed (%d)\n", rc);
		H.up = 1; // let Shutdown release what was allocated
		PS2HWD_Shutdown();
		return false;
	}
	view_defaults();
	H.tex_filter = cfg.linear ? HWD_SET_TEXTUREFILTER_BILINEAR : HWD_SET_TEXTUREFILTER_POINTSAMPLED;
	H.tex_cap_blocks = cfg.tex_cap_blocks;
	// OPT9 (PS2-HW-33): no adaptive decimation of textures (PS2-HW-24 is withdrawn): the working set is handled by sharing the image of the
	// CLUT variants (PS2-HW-30), by drawing the batches texture by texture in alternating directions (PS2-HW-31) and by streaming
	// uploads with DMA references (PS2-HW-32). cfg.tex_adapt is ignored; a fixed cap (-hwtexcap N) still decimates for experiments.
	H.cap_adapt = 0;
	H.gate = !(ps2hwd_dbg_flags & HWDBG_NOGATE); // -hwdbg 0x800000: the old frame start (wait for the flip of the previous frame)
	H.up = 1;
	if (!H.fb32)
		hw_limit(HW_SCREEN_LOSS, "opt-in CT16S frame buffer loses RGB precision and continuous destination alpha");
	gs_irq_selftest();
	clut_fixed_refresh(1);
	gs_clear_both();
	H.last_drawn = 0;
	H.have_drawn = 1;
	H.disp = 0;
	GS_SET_DISPFB2(H.fb_addr[0] / 8192, H.fbw / 64, H.fbpsm, 0, 0);
	H.intc_vbl = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
	EnableIntc(INTC_VBLANK_S);
	H.vbl_installed = 1;
	CONS_Printf("PS2 GS hardware renderer: internal %dx%d (frame %d x %d %s, CRTC x%d/x%d, output %d), frame buffers at %uK/%uK, Z24 at %uK, texture pool %uK (%u pages), GIF DMA %s\n",
		H.vw, H.vh, H.fbw, H.fbh, H.fb32 ? "CT32" : "CT16S", H.magh, H.magv, H.outid, H.fb_addr[0] >> 10, H.fb_addr[1] >> 10, H.z_addr >> 10,
		H.pool_blocks / 4, H.pool_blocks / PAGE_BLOCKS, H.dma_irq ? "interrupt" : "polled");
	return true;
}

void PS2HWD_Shutdown(void)
{
	int i;

	if (!H.up)
		return;
	if (H.ring[0])
	{
		if (H.frame_open)
			frame_end();
		ov_flush_all();
		pk_flush();
		ring_wait(0);
		gs_wait_finished();
	}
	// PS2-HW-71: the light tables stay (the engine keeps their ids in its colormaps until it clears them: ClearLightTables); the screen palette is set again
	spal_set = 0;
	pal_last_mode = -1;
	if (H.vbl_installed)
	{
		DisableIntc(INTC_VBLANK_S);
		RemoveIntcHandler(INTC_VBLANK_S, H.intc_vbl);
	}
	if (H.dmac >= 0)
	{
		DisableDmac(DMAC_GIF);
		RemoveDmacHandler(DMAC_GIF, H.dmac);
	}
	if (V.dmac_set) // PS2-HW-44
	{
		DisableDmac(DMAC_VIF1);
		RemoveDmacHandler(DMAC_VIF1, V.dmac);
		V.dmac_set = 0;
	}
	val_shutdown();
	spr_free(); // PS2-HW-255
	vu1_shutdown();
	V.on = 0;
	if (H.sema_vbl >= 0)
		DeleteSema(H.sema_vbl);
	if (H.sema_dma >= 0)
		DeleteSema(H.sema_dma);
	dma_fence();
	rel_release_all();
	dc_flush();
	tex_free_all();
	H.imm_tex = NULL;
	plan_reset();
	ramp_tex = NOREC;
	OV.n = 0;
	for (i = 0; i < NBUF; i++)
		free(H.ring[i]);
	free(H.rec);
	free(sky_vertices);
	free(sky_colors);
	free(sky_indices);
	sky_vertices = NULL;
	sky_colors = NULL;
	sky_indices = NULL;
	sky_capacity = sky_index_capacity = 0;
	sky_fast_free(); // PS2-HW-42
	free(scratch);
	scratch = NULL;
	scratch_cap = 0;
	if (H.gs)
		gsKit_deinit_global(H.gs);
	memset(&H, 0, sizeof H);
	hwbig_free();
}

// PS2-170 (OPT11-STAB): the engine jumped out of a frame (out of memory, a resource failure: z_zone.h Z_GUARD_TRY) and is going to shut the driver down.
// The packet the drawing was building (possibly cut off half way) is dropped, the queued buffers are never started, the GIF forgets a packet it was
// in the middle of, and the frame state is closed, so that the shutdown (and the texture owners' deletes before it) find an ordinary idle driver.
void PS2HWD_Abort(void)
{
	u32 irq;

	if (!H.up || !H.ring[0])
		return;
	irq = DIntr();
	*D2_CHCR = 0; // stop channel 2 (the queued buffers are read by nobody now)
	if (V.on)
		*D1_CHCR = 0;
	__asm__ volatile("sync.l");
	*(volatile u32 *)0x10003000 = 1; // GIF_CTRL.RST: a GIF packet that was cut off (an IMAGE upload) must not swallow the next packets
	if (V.on)
		*VIF1_FBRST = 1;
	H.dma_busy = 0;
	H.q_n = 0;
	H.q_head = 0;
	H.pending = 0;
	H.wr = 1;
	H.run = 0;
	H.done_seq = H.q_seq;
	H.gsr.valid = 0;
	H.clut_loaded = 0;
	H.finish_pending = 0;
	H.frame_open = 0;
	H.imm_tex = NULL;
	V.chunk_open = 0;
	OV.n = 0;
	PS2HWD_SprReset(); // PS2-HW-255
	if (H.pend)
		H.pend_done = 1; // show what there is
	*GS_CSR = 2;
	val_reset();
	if (irq)
		EIntr();
	rel_release_all();
}

// The engine's size (vid.width x vid.height): the coordinate space of GClipRect. A size that needs a different internal frame
// (docs/VIDEO_MODES.md: the frame buffer is the size the CRTC can magnify to the screen) restarts the GS; the textures the
// engine holds are dropped (their handles cleared) and the palette is kept.
void PS2HWD_SetScreenSize(INT32 w, INT32 h)
{
	if (w <= 0 || h <= 0)
		return;
	if (!H.up)
	{
		cfg.screen_w = w;
		cfg.screen_h = h;
		return;
	}
	if (w == H.scr_w && h == H.scr_h)
		return;
	{
		int nw, nh, ff;
		u32 pal[256];
		int pal_set = H.pal_set, filter = H.tex_filter, shaders_on = H.shaders_on;

		hw_choose_size(H.mode, w, h, &nw, &nh, &ff);
		if (cfg.fbw)
			nw = cfg.fbw;
		if (cfg.fbh)
			nh = cfg.fbh;
		if (nw == H.vw && nh == H.vh)
		{
			cfg.screen_w = w;
			cfg.screen_h = h;
			H.scr_w = w;
			H.scr_h = h;
			H.sx = (float)H.vw / (float)w;
			H.sy = (float)H.vh / (float)h;
			viewport_set(0, 0, w, h);
			return;
		}
		memcpy(pal, H.pal, sizeof pal);
		cfg.screen_w = w;
		cfg.screen_h = h;
		cfg.video = H.mode;
		PS2HWD_Shutdown();
		if (!PS2HWD_Init())
		{
			hw_failure("GS restart for a new internal size failed");
			return;
		}
		memcpy(H.pal, pal, sizeof pal);
		H.pal_set = pal_set;
		if (pal_set)
		{
			pal_build();
			H.pal_gen++;
		}
		H.tex_filter = filter;
		H.shaders_on = shaders_on;
	}
}

void PS2HWD_GetInfo(ps2hwd_info_t *out)
{
	memset(out, 0, sizeof *out);
	out->fbw = H.fbw;
	out->fbh = H.fbh;
	out->vw = H.vw;
	out->vh = H.vh;
	out->magh = H.magh;
	out->magv = H.magv;
	out->fb32 = H.fb32;
	out->outid = H.outid;
	out->mode = H.mode;
	out->fb_block[0] = H.fb_addr[0] / 256;
	out->fb_block[1] = H.fb_addr[1] / 256;
	out->z_block = H.z_addr / 256;
	out->z_bits = 24;
	out->displayed = H.disp;
	out->target = H.target;
	out->pool_block = H.pool_base;
	out->pool_blocks = H.pool_blocks;
	out->pool_used_blocks = H.used_blocks;
	out->clut_block = H.clut_base;
	out->screen_w = H.scr_w;
	out->screen_h = H.scr_h;
	out->dma_irq = H.dma_irq;
	out->state_bytes = sizeof H;
	out->ring_bytes = NBUF * BUF_QW * sizeof(qw_t);
	out->record_bytes = (unsigned int)((size_t)H.rec_cap * sizeof(texrec_t));
	out->scratch_bytes = (unsigned int)scratch_cap;
	out->sky_bytes = (unsigned int)((size_t)sky_capacity * (sizeof(FOutVector) + 4 * sizeof(float)) + (size_t)sky_index_capacity * sizeof(u32));
	out->screen_bytes = H.screen_bytes;
	out->screen_peak_bytes = H.screen_peak_bytes;
}

void PS2HWD_DumpWorkingSet(void)
{
	enum { TOPN = 48 };
	int i, k, top[TOPN], n = 0;
	u32 total = 0;

	for (i = 0; i < H.rec_n; i++)
	{
		if (!H.rec[i].used || H.rec[i].ws != H.frame_no)
			continue;
		total += H.rec[i].nblk;
		for (k = n < TOPN ? n++ : TOPN - 1; k > 0 && H.rec[top[k - 1]].nblk < H.rec[i].nblk; k--)
			top[k] = top[k - 1];
		top[k] = i;
	}
	for (i = 0; i < n; i++)
	{
		const texrec_t *r = &H.rec[top[i]];

		CONS_Printf("HWWS %s %ux%u psm %d blocks %u clut %d scr %d wrap %d%d fl 0x%x\n", r->owner ? HWR_PS2_TexName(r->owner) : "-", (unsigned)r->w, (unsigned)r->h, (int)r->psm,
			(unsigned)r->nblk, (int)r->clut, (int)r->screen, (int)r->wrapx, (int)r->wrapy, r->owner ? (unsigned)r->owner->flags : 0u);
	}
	CONS_Printf("HWWS total resident-in-frame blocks %u (last frame)\n", (unsigned)total);
}

// scalar reference of vu0_xform: xform_vertex + outcode + the projection of put_vertex
static void scalar_xform(const FOutVector *s, int *oc, int *xi, int *yi, int *zi, float *q)
{
	cv_t d;
	float zf;
	const float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);

	xform_vertex(s, NULL, &d);
	*oc = d.w <= 0.0f ? 0x3F : outcode(&d, gx, gy);
	*q = 1.0f / d.w;
	zf = clampf(P.zo + d.z * *q * P.zk + P.zbias, 0.0f, P.zmax);
	*xi = (int)(P.ox + d.x * *q * P.kx);
	*yi = (int)(P.oy + d.y * *q * P.ky);
	*zi = (int)(u32)zf;
}

void PS2HWD_TestVU0(unsigned int n, unsigned int seed, ps2hwd_vu0test_t *out)
{
	memset(out, 0, sizeof *out);
#ifdef HW_VU0
	{
		FTransform t;
		FSurfaceInfo surf;
		unsigned int i, rng = seed * 2654435761u + 12345u;
		float xs = 0.0f;
		static FOutVector pts[4096];
		u32 c0;

#define TRND() (rng = rng * 1664525u + 1013904223u, (float)((rng >> 8) & 0xFFFF) * (1.0f / 65536.0f))
		if (!H.up)
			return;
		if (n > 4096)
			n = 4096;
		out->have_vu0 = 1;
		memset(&t, 0, sizeof t);
		t.x = (TRND() - 0.5f) * 8192.0f;
		t.y = (TRND() - 0.5f) * 8192.0f;
		t.z = TRND() * 2048.0f;
		t.anglex = (TRND() - 0.5f) * 80.0f;
		t.angley = TRND() * 360.0f;
		t.fovxangle = 70.0f + TRND() * 40.0f;
		t.scalex = t.scaley = t.scalez = 1.0f;
		xf_set_transform(&t);
		memset(&surf, 0, sizeof surf);
		surf.PolyColor.rgba = 0xFFFFFFFFu;
		if (!begin_draw(PF_NoTexture | PF_Modulated | PF_Occlude, &surf))
			return;
		if (ps2hwd_dbg_flags & 256) // negative control of tools/ps2/hw_test.c: a wrong scale in VU0
			vu0_batch_load(H.mvp, (float)H.guard_x * (1.0f / 1024.0f), (float)H.guard_y * (1.0f / 1024.0f), P.kx * 1.01f, P.ky, P.zk, P.ox, P.oy, P.zo + P.zbias, P.zmax);
		for (i = 0; i < n; i++)
		{
			// vertices around the camera: mostly in front of it, some beside and behind
			pts[i].x = t.x + (TRND() - 0.5f) * 6000.0f;
			pts[i].z = t.y + (TRND() - 0.5f) * 6000.0f;
			pts[i].y = t.z + (TRND() - 0.5f) * 3000.0f;
			pts[i].s = pts[i].t = 0.0f;
		}
		{
			volatile int sink = 0;
			cv_t d;
			pv_t pv;
			int oc, xi, yi, zi;
			float q;

			c0 = cyc();
			for (i = 0; i < n; i++)
			{
				scalar_xform(&pts[i], &oc, &xi, &yi, &zi, &q);
				sink += xi;
			}
			out->cyc_scalar = cyc() - c0;
			c0 = cyc();
			for (i = 0; i < n; i++)
				sink += vu0_xform(&pts[i], &d, &pv) + pv.xi;
			out->cyc_vu0 = cyc() - c0;
			c0 = cyc();
			for (i = 0; i + 1 < n; i += 2)
			{
				cv_t d2[2];
				pv_t p2[2];
				int oc2[2];

				vu0_xform2(&pts[i], &pts[i + 1], &d2[0], &d2[1], &p2[0], &p2[1], &oc2[0], &oc2[1]);
				sink += oc2[0] + oc2[1] + p2[0].xi + p2[1].xi;
			}
			out->cyc_vu0p = (cyc() - c0) * n / (n & ~1u);
			for (i = 0; i + 1 < n; i += 2)
			{
				cv_t d1[2], d2[2];
				pv_t p1[2], p2[2];
				int oc1[2], oc2[2];

				oc1[0] = vu0_xform(&pts[i], &d1[0], &p1[0]);
				oc1[1] = vu0_xform(&pts[i + 1], &d1[1], &p1[1]);
				vu0_xform2(&pts[i], &pts[i + 1], &d2[0], &d2[1], &p2[0], &p2[1], &oc2[0], &oc2[1]);
				if (oc1[0] != oc2[0] || oc1[1] != oc2[1] || memcmp(&d1[0], &d2[0], sizeof(float) * 6) || memcmp(&d1[1], &d2[1], sizeof(float) * 6) || memcmp(&p1[0], &p2[0], sizeof p1[0])
					|| memcmp(&p1[1], &p2[1], sizeof p1[1]))
					out->pair_diff++;
			}
			(void)sink;
		}
		for (i = 0; i < n; i++)
		{
			FOutVector v = pts[i];
			cv_t d;
			pv_t pv;
			int oc, xi, yi, zi, oc2, dd;
			float q, qd;

			scalar_xform(&v, &oc, &xi, &yi, &zi, &q);
			oc2 = vu0_xform(&v, &d, &pv);
			xs += d.x;
			out->n++;
			if (oc != oc2)
				out->oc_diff++;
			if (oc == 0 && oc2 == 0)
			{
				dd = pv.xi - xi;
				if (dd) out->dx_nonzero++;
				if (dd < 0) dd = -dd;
				if (dd > out->max_dx) out->max_dx = dd;
				dd = pv.yi - yi;
				if (dd) out->dy_nonzero++;
				if (dd < 0) dd = -dd;
				if (dd > out->max_dy) out->max_dy = dd;
				dd = pv.zi - zi;
				if (dd) out->dz_nonzero++;
				if (dd < 0) dd = -dd;
				if (dd > out->max_dz) out->max_dz = dd;
				qd = (pv.q - q) / q;
				if (qd < 0) qd = -qd;
				if (qd > out->max_q_rel) out->max_q_rel = qd;
			}
		}
		if (xs == 12345.678f)
			out->n++; // keeps the compiler from dropping the clip results
#undef TRND
	}
#endif
}

void PS2HWD_VramStats(ps2hwd_vram_t *out) // OPT11-MEM (PS2-HW-300)
{
	memset(out, 0, sizeof *out);
	if (!H.up)
		return;
	out->up = 1;
	out->total_blocks = VRAM_BLOCKS;
	out->fb_blocks = H.z_addr / 256;
	out->z_blocks = H.clut_base - H.z_addr / 256;
	out->clut_blocks = H.pool_base - H.clut_base;
	out->pool_blocks = H.pool_blocks;
	out->pool_used_blocks = H.used_blocks;
	out->ws_blocks = H.ws_last;
}

void PS2HWD_GetStats(ps2hwd_stats_t *out, int reset)
{
	H.st.vblanks = H.vbl;
	H.st.flips = H.flips;
	H.st.cap_blocks = H.tex_cap_blocks;
	*out = H.st;
	if (reset)
		memset(&H.st, 0, sizeof H.st);
}

// ==========================================================================
// hwdriver_s entry points
// ==========================================================================

static void hw_SetTexturePalette(RGBA_t *ppal)
{
	if (H.up && ppal)
	{
		palette_set(ppal);
	}
}

static void hw_FinishUpdate(INT32 waitvbl)
{
	u32 t0 = cyc(), d;

	if (!H.up)
		return;
	drv_in();
	frame_end();
	drv_out();
	H.st.frames++;
	if (H.st.ws_blocks >= H.ws_prev) // OPT11-MEM (PS2-HW-300): the working set of the frame just ended (else the profiler cleared H.st in this frame: the last value stays)
		H.ws_last = H.st.ws_blocks - H.ws_prev;
	H.ws_prev = H.st.ws_blocks;
	if (waitvbl)
		vblank_wait(1);
	d = cyc() - t0;
	if (d > H.st.cyc_finish)
		H.st.cyc_finish = d;
}

// the 2D line of the automap as a quad of about one pixel width (screen space, identity transform)
static void hw_Draw2DLine(F2DCoord *v1, F2DCoord *v2, RGBA_t Color)
{
	FOutVector q[4];
	FSurfaceInfo surf;
	view_save_t sv;
	float dx, dy, angle;

	if (!H.up)
		return;
	if (fabsf(v2->x - v1->x) > 1.1920929e-7f)
		angle = atanf((v2->y - v1->y) / (v2->x - v1->x));
	else
		angle = 1.57079632679f;
	dx = sinf(angle) / (float)H.scr_w;
	dy = cosf(angle) / (float)H.scr_h;
	q[0].x = v1->x - dx;
	q[0].y = -(v1->y + dy);
	q[1].x = v2->x - dx;
	q[1].y = -(v2->y + dy);
	q[2].x = v2->x + dx;
	q[2].y = -(v2->y - dy);
	q[3].x = v1->x + dx;
	q[3].y = -(v1->y - dy);
	q[0].z = q[1].z = q[2].z = q[3].z = 0.0f;
	q[0].s = q[0].t = q[1].s = q[1].t = q[2].s = q[2].t = q[3].s = q[3].t = 0.0f;
	memset(&surf, 0, sizeof surf);
	surf.PolyColor = Color;
	screen_mode_begin(&sv);
	if (begin_draw(PF_NoTexture | PF_Modulated | PF_Translucent | PF_NoDepthTest, &surf))
		emit_fan(q, NULL, 4, NULL);
	screen_mode_end(&sv);
}

// ---- images of more than 1024 rows (PS2-HW-70) ----
// The GS takes 1024 rows of texture at most. A P_8 map texture of 1025..2048 rows (the pipes of THZ: 64 x 1536) is stored as two images, the rows
// [0, 1024) and the rest (tex_upload), and a polygon that has such a texture is cut along t at the boundary between the pieces (and at the whole
// repeats when the texture repeats); every part is drawn with its piece, t made the piece's own. The cut is made on the engine's polygon, in world
// space where the texture mapping is affine, so the two parts get the very same vertices on the cut (no crack). Without wrap the texture is clamped
// at its edges: the part with t below the boundary belongs to the first piece (clamped at its top), the one above it to the second (clamped at the end).
#define SPLIT_VERT FOutVector
#include "ps2_hw_split.h"

#define SPLIT_MAXV (PS2HWD_MAXPOLY + 8)
static FOutVector split_buf[2][SPLIT_MAXV];

static inline int split_active(u32 flags)
{
	return H.cur_tex != NOREC && H.rec[H.cur_tex].psplit == 1 && !(flags & PF_NoTexture);
}

// the record to draw the second piece with: the variant (CLUT, wrap, filter) of the record the engine selected (cur) on the image of the second piece
static int split_piece_rec(int bi, int cur)
{
	int w;
	u8 clut, wrapx, wrapy, nearest_only, notcc;

	if (cur == img_of(cur))
		return bi; // the first piece's own record: the second piece was made with the same variant
	clut = H.rec[cur].clut;
	wrapx = H.rec[cur].wrapx;
	wrapy = H.rec[cur].wrapy;
	nearest_only = H.rec[cur].nearest_only;
	notcc = H.rec[cur].notcc;
	for (w = H.rec[bi].sec; w != NOREC; w = H.rec[w].sec_next)
		if (H.rec[w].clut == clut && H.rec[w].wrapx == wrapx && H.rec[w].wrapy == wrapy && H.rec[w].nearest_only == nearest_only && H.rec[w].notcc == notcc)
			return w;
	w = rec_new(); // may move the table
	if (w == NOREC)
		return NOREC;
	H.rec[w] = H.rec[bi];
	H.rec[w].prev = H.rec[w].next = NOREC;
	H.rec[w].pnext = H.rec[w].ppar = NOREC;
	H.rec[w].owner = NULL; // a view of the second piece belongs to no engine texture: it goes with its image
	H.rec[w].share = bi;
	H.rec[w].sec = NOREC;
	H.rec[w].sec_next = H.rec[bi].sec;
	H.rec[bi].sec = w;
	H.rec[w].used = 1;
	H.rec[w].pin = 0;
	H.rec[w].clut = clut;
	H.rec[w].wrapx = wrapx;
	H.rec[w].wrapy = wrapy;
	H.rec[w].nearest_only = nearest_only;
	H.rec[w].notcc = notcc;
	H.rec[w].stamp = H.frame_no;
	H.rec[w].done = 0;
	H.rec[w].ws = 0;
	return w;
}

typedef struct
{
	const FSurfaceInfo *surf;
	u32 fl;
	int bi, cur;
} split_ctx_t;

static void split_emit(void *vctx, int piece, int cell, const FOutVector *v, int n)
{
	const split_ctx_t *c = vctx;
	const int rec = piece ? split_piece_rec(c->bi, c->cur) : c->cur;

	(void)cell;
	if (rec == NOREC)
		return;
	TX.split_sub++;
	H.cur_tex = rec;
	if (begin_draw(c->fl, c->surf))
		emit_fan(v, NULL, n, NULL);
	H.cur_tex = c->cur;
}

static void split_draw(const FSurfaceInfo *surf, u32 flags, const FOutVector *v, int n)
{
	split_ctx_t c;
	const int ai = img_of(H.cur_tex), bi = H.rec[ai].pnext;
	float lo_t[2], hi_t[2], tk[2];

	if (n < 3)
		return;
	if (bi == NOREC)
	{
		if (begin_draw(flags, surf))
			emit_fan(v, NULL, n, NULL);
		return;
	}
	TX.split_poly++;
	c.surf = surf;
	c.fl = (flags | PF_RemoveYWrap) & ~(u32)PF_ForceWrapY; // a part is inside one piece: it clamps (the repeats are made by the cuts)
	c.bi = bi;
	c.cur = H.cur_tex;
	lo_t[0] = H.rec[ai].pt0;
	hi_t[0] = H.rec[ai].pt1;
	tk[0] = H.rec[ai].ptk;
	lo_t[1] = H.rec[bi].pt0;
	hi_t[1] = H.rec[bi].pt1;
	tk[1] = H.rec[bi].ptk;
	split_polygon(v, n, (H.rec[c.cur].wrapy || (flags & PF_ForceWrapY)) && !(flags & PF_RemoveYWrap), lo_t, hi_t, tk, split_buf[0], split_buf[1], split_emit, &c);
}

// A single polygon (hw_DrawPolygon: HUD, shadows, sprites that are not batched) through the VU1 program: with the plan of the previous single draw (same key) it joins
// the open chunk (no begin_draw, no constants); with the plan of the previous single draw and another texture of the same kind (a run of HUD glyphs, sprites) the plan
// is retargeted (PS2-HW-107: the texture registers and the texture scale, nothing else); else the plan is made and the constants go to the VU1 memory. What the VU1
// path does not take (a plan with cuts, a polygon of too many texels) is drawn by the general path.
static void single_draw(FSurfaceInfo *surf, u32 flags, FOutVector *v, int n)
{
	vukey_t k;
	int how = 0;

	vu_key_make(&k, flags, surf);
	if (vu_plan_valid && VU.consts_ok && P.serial == H.serial)
		how = vu_key_cmp(&k, &VK);
	if (how)
	{
		if (how == 1)
		{
			vu_retarget(surf);
			VK = k;
		}
		if (vu_poly(v, n, P.rs.lp.light))
		{
			G.s_vu++;
			if (how == 2)
				G.s_same++;
			else
				G.s_retarget++;
			return;
		}
		VU.reuse = 0;
		vu_plan_valid = 0;
		vu_sync();
		P.vu = 0;
		if (how == 1 && P.lmode == LM_BANDS)
			lit_fast_plan(); // the light level of this polygon (the program took it from the header)
		emit_fan(v, NULL, n, NULL);
		return;
	}
	if (!begin_draw(flags, surf))
		return;
	if (P.vuok)
	{
		VU.consts_ok = 0;
		if (vu_poly(v, n, P.rs.lp.light))
		{
			VK = k;
			vu_plan_valid = 1;
			G.s_vu++;
			return;
		}
		vu_sync();
	}
	P.vu = 0;
	emit_fan(v, NULL, n, NULL);
}

static void hw_DrawPolygon(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags)
{
	if (!H.up)
		return;
	if ((PolyFlags & PF_WireFrame) || wire_mode)
	{
		drv_in(); // OPT11-MODEL (PS2-HW-270)
		if (PolyFlags & PF_WireFrame)
			wire_pairs(pSurf, (u32)PolyFlags, pOutVerts, (int)iNumPts);
		else
			wire_poly(pSurf, (u32)PolyFlags, pOutVerts, (int)iNumPts);
		drv_out();
		return;
	}
	if (TRACING())
	{
		const texrec_t *tr = H.cur_tex != NOREC ? &H.rec[H.cur_tex] : NULL;

		CONS_Printf("HWT poly n=%u fl=0x%x tex=%s rec=%d blk=%u %ux%u\n", (unsigned)iNumPts, (unsigned)PolyFlags, tr && tr->owner ? HWR_PS2_TexName(tr->owner) : "-", H.cur_tex,
			tr ? (unsigned)tr->blk : 0u, tr ? (unsigned)tr->w : 0u, tr ? (unsigned)tr->h : 0u);
	}
	{
		const u32 gt0 = cyc();
		const int gk = ((PolyFlags & PF_NoTexture) ? 1 : 0) | ((PolyFlags & PF_NoDepthTest) ? 2 : 0) | ((PolyFlags & PF_Occlude) ? 4 : 0);
		u32 gt1, gt2;

		drv_in();
		G.single++;
		G.sk_single += ps2hwp_skyview;
		G.sing_by[gk]++;
		if (H.imm_tex && !(PolyFlags & PF_NoTexture))
			imm_prepare(pOutVerts, (unsigned int)iNumPts); // PS2-HW-37: the texture is made resident at the level this polygon needs
		gt1 = cyc();
		G.s_imm += gt1 - gt0;
		if (split_active((u32)PolyFlags))
		{
			split_draw(pSurf, (u32)PolyFlags, pOutVerts, (int)iNumPts); // PS2-HW-70: a texture of two images
		}
		else if (V.on && V.ready && !H.imm_tex && iNumPts >= 3 && iNumPts <= VU_MAXVERT)
		{
			single_draw(pSurf, (u32)PolyFlags, pOutVerts, (int)iNumPts); // PS2-HW-103: the VU1 program, the open chunk when the plan is the last one's
			G.s_emit += cyc() - gt1;
		}
		else if (begin_draw((u32)PolyFlags, pSurf))
		{
			gt2 = cyc();
			G.s_begin += gt2 - gt1;
			emit_fan(pOutVerts, NULL, (int)iNumPts, NULL);
			G.s_emit += cyc() - gt2;
		}
		else
		{
			G.s_begin += cyc() - gt1;
		}
		G.sing_cyc[gk] += cyc() - gt0;
		drv_out();
	}
}

// PS2-HW-106: does the polygon take the palette rows of the VU1 program, which light every polygon with its own light level (the header of the polygon carries it)?
// hw_batching.c asks when it makes the key of a batch: such polygons need not be kept apart by the light level of their sector (248 batches of a DEMO_001 frame
// are 93 without it), and must be: a plan that is not the palette rows (fog classes of the GLSL equations, water, a texture of 32 bit colours) keeps the level in its key.
// What it asks is what begin_draw_inner decides at draw time (the shader state is the base shader slot of the polygon), as far as it can be known before the
// texture is uploaded: map textures and flats (P_8) and the patches whose record is a palette image already.
// PS2-HW-227 (OPT11 round 2, VU2): asked for every polygon of the frame (2000 times in DEMO_001, 80 cycles a call: fourteen tests, each a compare with a branch and a
// delay slot that stays empty). What does not depend on the polygon (the driver and VU1 program are up, the shaders are on, the debug switches, the palette mode) is one
// word made once a frame; the flags of the polygon are tested with two masks.
static int pallit_frame_gate(void)
{
	static u32 key = ~0u;
	static int on;
	const u32 k = H.frame_no * 2u + (H.shaders_on ? 1u : 0u);

	if (key != k)
	{
		key = k;
		on = H.up && V.on && V.ready && H.shaders_on && !(ps2hwd_dbg_flags & (2048 | 8192 | HWDBG_NOVU1 | HWDBG_NOLIGHTMERGE)) && palette_mode_fr();
	}
	return on;
}

int PS2HWD_PalLit(const void *vsurf, unsigned int flags, const void *vtex, int shader)
{
	const FSurfaceInfo *surf = (const FSurfaceInfo *)vsurf;
	const GLMipmap_t *m = (const GLMipmap_t *)vtex;
	u32 lt, blend, ok;

	if (!surf || !m)
		return 0;
	// the conditions are made as bits and tested once: every compare with its own branch costs two cycles on the machine of the profile (the branch and its delay slot) on top of the compare
	lt = surf->LightTableId;
	blend = flags & PF_Blending;
	ok = (u32)(H.up != 0) & (u32)((unsigned int)shader <= 5u) & (u32)((flags & (PF_ColorMapped | PF_NoTexture | PF_Invisible | PF_Ripple | PF_Corona | PF_WireFrame)) == PF_ColorMapped)
		& (u32)(blend != PF_Fog) & (u32)(blend != (PF_Multiplicative & PF_Blending)) & (u32)(lt - 1u < (u32)(PR_TBL - 1)) & (u32)(lt_idx[lt & (PR_TBL - 1)] != NULL); // (PR_TBL <= LT_MAX)
	if (!ok || !pallit_frame_gate())
		return 0;
	if (m->format == GL_TEXFMT_P_8)
		return 1;
	if (m->downloaded && (int)m->downloaded <= H.rec_n)
	{
		const texrec_t *r = &H.rec[m->downloaded - 1];

		return r->used && r->psm == PSM_T8 && (r->clut == 1 || r->clut == 2) && !r->screen;
	}
	return 0;
}

void PS2HWD_DrawFans(void *surf, void *base, unsigned int nfans, unsigned int flags, const unsigned int *desc)
{
	unsigned int i;
	vukey_t k;
	int how = 0;

	if (!H.up)
		return;
	drv_in();
	G.c_fans += nfans;
	if (TRACING())
	{
		const texrec_t *tr = H.cur_tex != NOREC ? &H.rec[H.cur_tex] : NULL;

		CONS_Printf("HWT fans n=%u fl=0x%x tex=%s rec=%d blk=%u %ux%u psm=%d clut=%d\n", nfans, flags, tr && tr->owner ? HWR_PS2_TexName(tr->owner) : "-", H.cur_tex,
			tr ? (unsigned)tr->blk : 0u, tr ? (unsigned)tr->w : 0u, tr ? (unsigned)tr->h : 0u, tr ? (int)tr->psm : -1, tr ? (int)tr->clut : -1);
	}
	if (wire_mode)
	{
		wire_fans((const FSurfaceInfo *)surf, flags, (const FOutVector *)base, nfans, desc); // OPT11-MODEL (PS2-HW-270)
		drv_out();
		return;
	}
	G.batches++;
	G.fans += nfans;
	G.sk_batches += ps2hwp_skyview;
	G.sk_fans += ps2hwp_skyview ? nfans : 0;
	if (split_active(flags))
	{
		for (i = 0; i < nfans; i++) // PS2-HW-70: a texture of two images: every polygon is cut along its pieces
			split_draw((const FSurfaceInfo *)surf, flags, (const FOutVector *)base + desc[3 * i], (int)desc[3 * i + 1]);
		drv_out();
		return;
	}
	if (V.on && V.ready)
	{
		// PS2-HW-107: a batch of the plan of the last VU1 draw (single or batch) with another texture of the same kind only retargets the plan
		vu_key_make(&k, (u32)flags, (const FSurfaceInfo *)surf);
		if (vu_plan_valid && VU.consts_ok && P.serial == H.serial)
			how = vu_key_cmp(&k, &VK);
		if (how == 1 && vu_nobretarget)
			how = 0;
		if ((how == 1 && !(vu_bretmask & (P.pal ? 1 : 2))) || (how == 2 && !(vu_bretmask & 4)))
			how = 0;
	}
#ifdef PS2_HWDETAIL
	if (hwr_sprite_batch) // (the flush of a sprite batch: how its draws are made, HWPROF41)
	{
		HWC_ADD(HWC_SF_CALLS);
		HWC_ADD(how == 0 ? HWC_SF_BEGIN : how == 1 ? HWC_SF_RET : HWC_SF_SAME);
		ps2hwp_cnt[HWC_SF_POLYS] += nfans;
	}
#endif
	if (how)
	{
		if (how == 1)
		{
			vu_retarget((const FSurfaceInfo *)surf);
			G.b_retarget++;
		}
		P.vu = 1; // (vu_plan_valid is set only for plans the program takes: P.vuok)
	}
	else
	{
#ifdef PS2_HWDETAIL
		const unsigned int sf_b0 = ps2hwp_now();
		const int sf_ok = begin_draw((u32)flags, (const FSurfaceInfo *)surf);

		if (hwr_sprite_batch)
			ps2hwp_cyc[HWP_SF_BEGIN] += (unsigned int)(ps2hwp_now() - sf_b0);
		if (!sf_ok)
#else
		if (!begin_draw((u32)flags, (const FSurfaceInfo *)surf))
#endif
		{
			drv_out();
			return;
		}
		if (P.vuok && nfans >= VU_MIN_FANS) // PS2-HW-45
		{
			P.vu = 1;
			VU.consts_ok = 0;
		}
	}
	for (i = 0; i < nfans; i++)
	{
		const FOutVector *fv;
		int fn;
		float fl; // the light level of the sector of the polygon (the batch's key leaves it out when the VU1 program lights by rows)

		if (P.vu) // PS2-HW-100: the VU1 program takes the polygons (PS2-HW-108: all it can in one loop); the one it leaves goes the general way below, in order
		{
			const unsigned int i0 = i;

#ifdef PS2_HWDETAIL
			{
				const unsigned int sf_v0 = ps2hwp_now();

				i = vu_fans((const FOutVector *)base, desc, i, nfans);
				if (hwr_sprite_batch)
					ps2hwp_cyc[HWP_SF_VUFANS] += (unsigned int)(ps2hwp_now() - sf_v0);
			}
#else
			i = vu_fans((const FOutVector *)base, desc, i, nfans);
#endif
			G.p_vu += i - i0;
			if (i >= nfans)
				break;
			vu_sync();
		}
		fv = (const FOutVector *)base + desc[3 * i];
		fn = (int)desc[3 * i + 1];
		fl = (float)(int)desc[3 * i + 2];
		if (fl != P.rs.lp.light)
		{
			P.rs.lp.light = fl; // the plan took the level of the first polygon of the batch: the general path lights this one with its own
			lit_fast_plan();
		}
		if (P.vu)
		{
			P.vu = 0; // (emit_poly would try the VU1 path again)
			emit_fan(fv, NULL, fn, NULL);
			P.vu = P.vuok;
			P.serial = H.serial - 1; // the general path may have loaded another CLUT row: the plan's GS state and the program's row again
			continue;
		}
		emit_fan(fv, NULL, fn, NULL);
	}
	if (P.vu)
	{
		// the chunk stays open (PS2-HW-103): the next draw of this plan joins it, anything else that writes to the ring closes it
		if (VU.consts_ok && P.serial == H.serial)
		{
			VK = k;
			vu_plan_valid = 1;
		}
		P.vu = 0;
	}
	drv_out();
}

// PS2-HW-233: the polygon of a block of the batch pool as the engine's vertices (the general path, the splitter and the reference planner take those). One conversion is alive at a time.
static FOutVector *blk_tmp;
static unsigned int blk_tmp_cap;

static const FOutVector *blk_fov(const qw_t *b)
{
	const unsigned int n = b[0].w[0];
	unsigned int i;

	if (n > blk_tmp_cap)
	{
		FOutVector *nv = realloc(blk_tmp, (size_t)(n + 16) * sizeof *nv);

		if (!nv)
		{
			hw_failure("polygon staging allocation failed; polygon rejected");
			return NULL;
		}
		blk_tmp = nv;
		blk_tmp_cap = n + 16;
	}
	for (i = 0; i < n; i++)
	{
		blk_tmp[i].x = b[2 + 2 * i].f[0];
		blk_tmp[i].y = b[2 + 2 * i].f[1];
		blk_tmp[i].z = b[2 + 2 * i].f[2];
		blk_tmp[i].s = b[3 + 2 * i].f[0];
		blk_tmp[i].t = b[3 + 2 * i].f[1];
	}
	return blk_tmp;
}

// PS2HWD_DrawFans for the blocks of the pool (hardware/hw_pbatch.inc): the polygon is a block (vertex count and light level in its header), the same plan for all of them, the VU1 program takes
// the blocks it can in one loop (vu_blocks), the others go the general way in order.
void PS2HWD_DrawBlocks(void *surf, const void *pool, unsigned int nfans, unsigned int flags, const unsigned int *blks)
{
	const qw_t *pl = (const qw_t *)pool;
	unsigned int i;
	vukey_t k;
	int how = 0;

	if (!H.up)
		return;
	drv_in();
	G.c_fans += nfans;
	if (TRACING())
	{
		const texrec_t *tr = H.cur_tex != NOREC ? &H.rec[H.cur_tex] : NULL;

		CONS_Printf("HWT fans n=%u fl=0x%x tex=%s rec=%d blk=%u %ux%u psm=%d clut=%d\n", nfans, flags, tr && tr->owner ? HWR_PS2_TexName(tr->owner) : "-", H.cur_tex,
			tr ? (unsigned)tr->blk : 0u, tr ? (unsigned)tr->w : 0u, tr ? (unsigned)tr->h : 0u, tr ? (int)tr->psm : -1, tr ? (int)tr->clut : -1);
	}
	G.batches++;
	G.fans += nfans;
	G.sk_batches += ps2hwp_skyview;
	G.sk_fans += ps2hwp_skyview ? nfans : 0;
	if (split_active(flags))
	{
		for (i = 0; i < nfans; i++) // PS2-HW-70: a texture of two images: every polygon is cut along its pieces
		{
			const qw_t *b = pl + blks[i];
			const FOutVector *fv = blk_fov(b);

			if (fv)
				split_draw((const FSurfaceInfo *)surf, flags, fv, (int)b[0].w[0]);
		}
		drv_out();
		return;
	}
	if (V.on && V.ready)
	{
		vu_key_make(&k, (u32)flags, (const FSurfaceInfo *)surf);
		if (vu_plan_valid && VU.consts_ok && P.serial == H.serial)
			how = vu_key_cmp(&k, &VK);
		if (how == 1 && vu_nobretarget)
			how = 0;
		if ((how == 1 && !(vu_bretmask & (P.pal ? 1 : 2))) || (how == 2 && !(vu_bretmask & 4)))
			how = 0;
	}
	if (how)
	{
		if (how == 1)
		{
			vu_retarget((const FSurfaceInfo *)surf);
			G.b_retarget++;
		}
		P.vu = 1;
	}
	else
	{
		const u32 bt0 = cyc();
		int bok;

		if (V.on && V.ready) // (HWPROF51: why the plan of the last draw could not be used)
		{
			const u32 *a = k.w, *c = VK.w;

			G.bb_diff[a[0] != c[0] ? 0 : a[1] != c[1] ? 1 : a[2] != c[2] ? 2 : a[3] != c[3] ? 3 : a[4] != c[4] ? 4 : a[5] != c[5] ? 5 : 6]++;
			if (!vu_plan_valid || !VU.consts_ok || P.serial != H.serial)
				G.bb_diff[7]++;
		}
		bok = begin_draw((u32)flags, (const FSurfaceInfo *)surf);
		G.bb_n++;
		G.bb_cyc += cyc() - bt0;
		if (!bok)
		{
			drv_out();
			return;
		}
		if (P.vuok && nfans >= VU_MIN_FANS)
		{
			P.vu = 1;
			VU.consts_ok = 0;
		}
	}
	for (i = 0; i < nfans; i++)
	{
		const qw_t *b;
		const FOutVector *fv;
		int fn;
		float fl; // the light level of the sector of the polygon (the batch's state leaves it out when the VU1 program lights by rows)

		if (P.vu)
		{
			const unsigned int i0 = i;

			i = vu_blocks(pl, blks, i, nfans);
			G.p_vu += i - i0;
			if (i >= nfans)
				break;
			vu_sync();
		}
		b = pl + blks[i];
		fn = (int)b[0].w[0];
		fl = b[0].f[1];
		fv = blk_fov(b);
		if (!fv)
			continue;
		if (fl != P.rs.lp.light)
		{
			P.rs.lp.light = fl;
			lit_fast_plan();
		}
		if (P.vu)
		{
			P.vu = 0;
			emit_fan(fv, NULL, fn, NULL);
			P.vu = P.vuok;
			P.serial = H.serial - 1;
			continue;
		}
		emit_fan(fv, NULL, fn, NULL);
	}
	if (P.vu)
	{
		if (VU.consts_ok && P.serial == H.serial)
		{
			VK = k;
			vu_plan_valid = 1;
		}
		P.vu = 0;
	}
	drv_out();
}

static void hw_DrawIndexedTriangles(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags, UINT32 *IndexArray)
{
	if (!H.up)
		return;
	drv_in();
	if (split_active((u32)PolyFlags))
	{
		u32 i;

		for (i = 0; i + 2 < (u32)iNumPts; i += 3) // PS2-HW-70
		{
			FOutVector tri[3];

			tri[0] = pOutVerts[IndexArray[i]];
			tri[1] = pOutVerts[IndexArray[i + 1]];
			tri[2] = pOutVerts[IndexArray[i + 2]];
			split_draw(pSurf, (u32)PolyFlags, tri, 3);
		}
		drv_out();
		return;
	}
	if (wire_mode)
	{
		wire_tris(pSurf, (u32)PolyFlags, pOutVerts, IndexArray, (u32)iNumPts); // OPT11-MODEL (PS2-HW-270)
		drv_out();
		return;
	}
	if (begin_draw((u32)PolyFlags, pSurf))
		emit_tris(pOutVerts, IndexArray, (u32)iNumPts, NULL);
	drv_out();
}

// The dome is built from vertices with colours (gl_skyvertex_t has the layout of FOutVector plus r, g, b, a).
static void hw_RenderSkyDome(gl_sky_t *sky)
{
	FOutVector *sv;
	float *sc, saved_mv[16];
	u32 *si;
	int i, j, n;

	if (!H.up || !sky || !sky->data || !sky->loops || sky->vertex_count <= 0)
		return;
	n = sky->vertex_count;
	if (sky_dome_fast(sky))
		return; // PS2-HW-42
	if (n > sky_capacity)
	{
		FOutVector *nv = malloc((size_t)n * sizeof *sv);
		float *nc = malloc((size_t)n * 4 * sizeof *sc);

		if (!nv || !nc)
		{
			free(nv);
			free(nc);
			hw_failure("sky staging allocation failed; dome rejected");
			return;
		}
		free(sky_vertices);
		free(sky_colors);
		sky_vertices = nv;
		sky_colors = nc;
		sky_capacity = n;
	}
	for (i = 0; i < sky->loopcount; i++)
	{
		int count = sky->loops[i].vertexcount;
		if (count >= 3 && count <= n && (count - 2) * 3 > sky_index_capacity)
		{
			u32 *ni = realloc(sky_indices, (size_t)(count - 2) * 3 * sizeof *ni);
			if (!ni)
			{
				hw_failure("sky index allocation failed; dome rejected");
				return;
			}
			sky_indices = ni;
			sky_index_capacity = (count - 2) * 3;
		}
	}
	sv = sky_vertices; sc = sky_colors; si = sky_indices;
	for (i = 0; i < n; i++)
	{
		const gl_skyvertex_t *d = &sky->data[i];

		sv[i].x = d->x;
		sv[i].y = d->y;
		sv[i].z = d->z;
		sv[i].s = d->u;
		sv[i].t = d->v;
		sc[i * 4 + 0] = d->r;
		sc[i * 4 + 1] = d->g;
		sc[i * 4 + 2] = d->b;
		sc[i * 4 + 3] = d->a;
	}
	// as r_opengl.c: the dome is scaled to the sky texture's height and turned, on top of the current model view
	memcpy(saved_mv, H.mv, sizeof saved_mv);
	m_scale(H.mv, 1.0f, (float)sky->height / 200.0f, 1.0f);
	m_rotate(H.mv, 270.0f, 0.0f, 1.0f, 0.0f);
	H.mat_dirty = 1;
	if (!begin_draw(H.cur_flags, NULL))
		goto restore_sky;
	P.vcol = 1;
	for (j = 0; j < 2; j++)
	{
		for (i = 0; i < sky->loopcount; i++)
		{
			const gl_skyloopdef_t *loop = &sky->loops[i];
			int k, cnt = loop->vertexcount, first = loop->vertexindex, ni = 0;

			if (j == 0 ? loop->use_texture : !loop->use_texture)
				continue; // untextured caps first, then the textured bands
			if (first < 0 || cnt < 3 || first > n - cnt || (cnt - 2) * 3 > sky_index_capacity)
			{
				hw_limit(HW_INVALID, "invalid sky loop; draw rejected");
				continue;
			}
			if (!loop->use_texture) // the caps have no texture coordinates (the OpenGL driver samples whatever is there)
				for (k = 0; k < cnt; k++)
					sv[first + k].s = sv[first + k].t = 0.0f;
			if (loop->mode == HWD_SKYLOOP_FAN)
			{
				for (k = 1; k + 1 < cnt; k++)
				{
					si[ni++] = (u32)first;
					si[ni++] = (u32)(first + k);
					si[ni++] = (u32)(first + k + 1);
				}
			}
			else if (loop->mode == HWD_SKYLOOP_STRIP)
			{
				for (k = 0; k + 2 < cnt; k++)
				{
					si[ni++] = (u32)(first + k);
					si[ni++] = (u32)(first + k + 1 + (k & 1));
					si[ni++] = (u32)(first + k + 2 - (k & 1));
				}
			}
			else
			{
				hw_limit(HW_INVALID, "unknown sky loop primitive; draw rejected");
				continue;
			}
			emit_tris(sv, si, (u32)ni, sc);
		}
	}
	P.vcol = 0;
restore_sky:
	memcpy(H.mv, saved_mv, sizeof H.mv);
	H.mat_dirty = 1;
}

static void hw_SetBlend(FBITFIELD PolyFlags)
{
	H.cur_flags = (u32)PolyFlags;
}

static void hw_ClearBuffer(FBOOLEAN ColorMask, FBOOLEAN DepthMask, FRGBAFloat *ClearColor)
{
	if (!H.up)
		return;
	do_clear(ColorMask ? 1 : 0, DepthMask ? 1 : 0, ClearColor);
	H.cur_flags = DepthMask ? (H.cur_flags | PF_Occlude) : (H.cur_flags & ~(u32)PF_Occlude);
}

// OPT9 (PS2-HW-30): the record of the other CLUT variant of a map texture / flat that is resident, or NULL
static texrec_t *twin_rec(GLMipmap_t *m)
{
	GLMipmap_t *t;
	texrec_t *o;

	if (!m->regen_kind || m->format != GL_TEXFMT_P_8)
		return NULL;
	t = m->ps2_twin ? m->ps2_twin : m->nextcolormap;
	if (!t || t->regen_kind != m->regen_kind || t->regen_id != m->regen_id || t->width != m->width || t->height != m->height)
		return NULL;
	o = rec_of(t);
	if (!o || !H.rec[img_of((int)(o - H.rec))].shareable)
		return NULL;
	return o;
}

// Select a texture. While the engine collects batched polygons nothing is uploaded: the texture is made resident when its batch is
// drawn (the GS pool cannot hold all the textures of a frame), asking the engine for the data again when the zone dropped it.
static void settex_now(GLMipmap_t *TexInfo)
{
	texrec_t *r;
	int ri;
	u32 want = 0;

	if (!H.up)
		return;
	if (!TexInfo)
	{
		H.cur_tex = NOREC;
		H.cur_missing = 0;
		return;
	}
	if (ps2hwd_dbg_flags & HWDBG_NOUP) // OPT10 HG measurement: one dummy image for every texture
	{
		static GLMipmap_t noup_mip;
		static u8 *noup_data; // PS2-171: measurement mode only: taken from the C heap when used (was 64 KB of .bss)

		texrec_t *d = rec_of(&noup_mip);

		if (!d && !noup_data)
			noup_data = memalign(64, 256 * 256);
		if (!d && noup_data)
		{
			int x, y;

			for (y = 0; y < 256; y++)
				for (x = 0; x < 256; x++)
					noup_data[y * 256 + x] = (u8)((x ^ y) & 127);
			memset(&noup_mip, 0, sizeof noup_mip);
			noup_mip.format = GL_TEXFMT_P_8;
			noup_mip.width = noup_mip.height = 256;
			noup_mip.flags = TF_WRAPXY;
			noup_mip.data = noup_data;
			ri = tex_upload(&noup_mip);
			if (ri != NOREC)
				H.rec[ri].pin = 1;
			d = ri != NOREC ? &H.rec[ri] : NULL;
		}
		H.cur_tex = d ? (int)(d - H.rec) : NOREC;
		H.cur_missing = d == NULL;
		return;
	}
	r = rec_of(TexInfo);
	if (!r && batch_phase != 1)
	{
		// the other variant of this texture is in VRAM already: a view of its image, nothing to upload (and no need for the data)
		texrec_t *o = twin_rec(TexInfo);

		if (o && (ri = tex_view(TexInfo, (int)(o - H.rec))) != NOREC)
		{
			r = &H.rec[ri];
			TX.shared++;
		}
	}
	if (TexInfo->format == GL_TEXFMT_P_8 && (TexInfo->regen_kind == 1 || TexInfo->regen_kind == 2) && (u32)TexInfo->width * TexInfo->height >= PLAN_MIN_TEXELS && batch_phase != 1)
	{
		// PS2-HW-34: the mip level the frame plan asks for (0 = full size); a texture drawn without a plan needs the full size
		GLMipmap_t *pk = TexInfo->ps2_twin ? TexInfo->ps2_twin : TexInfo;
		int vis = 1;

		want = tex_want(TexInfo, &vis);
		if ((batch_phase == 0 && imm_level < 0) || pk->ps2_planfr != H.frame_no + 1)
			pk->ps2_full_fr = H.frame_no + 1;
		if (!vis && !r && !TT.on) // PS2-HW-69: the conformance test draws textures no frame plan sees
		{
			// no polygon of the frame can see it: nothing is uploaded, the (clipped away) draws are skipped
			H.cur_tex = NOREC;
			H.cur_missing = 1;
			TX.invisible++;
			return;
		}
		if (r && !r->screen && plan_too_coarse(TexInfo, (u32)r->dx, want))
		{
			// stored at a coarser level than this draw needs (beyond the tolerance of the plan): the image is made again (every variant of it)
			if (ps2hwd_dbg_flags & HWDBG_IMMDBG)
				CONS_Printf("HWIMM f=%u %s %ux%u want=%u have=%d UPGRADE imm=%d\n", (unsigned)H.frame_no, HWR_PS2_TexName(TexInfo), (unsigned)TexInfo->width, (unsigned)TexInfo->height, (unsigned)want, (int)r->dx, imm_level);
			ov_flush_all();
			tex_drop(img_of((int)(r - H.rec)), 0);
			r = NULL;
			TX.upgrades++;
		}
		else if (r && (batch_phase == 2 || imm_level >= 0) && plan_too_fine(TexInfo, r, want))
		{
			// stored finer than the plan needs and the difference is worth the blocks: made again at the planned level (PS2-HW-37)
			if (ps2hwd_dbg_flags & HWDBG_IMMDBG)
				CONS_Printf("HWIMM f=%u %s %ux%u want=%u have=%d DOWNGRADE imm=%d\n", (unsigned)H.frame_no, HWR_PS2_TexName(TexInfo), (unsigned)TexInfo->width, (unsigned)TexInfo->height, (unsigned)want, (int)r->dx, imm_level);
			ov_flush_all();
			tex_drop(img_of((int)(r - H.rec)), 0);
			r = NULL;
			TX.downgrades++;
		}
	}
	if (r && H.cap_adapt && r->capi != H.cap_idx && !r->screen && !r->pin && batch_phase != 1 && r->sw)
	{
		// stored under another cap: re-make it when its size would differ (smaller at once; larger within the per-frame budget)
		u32 dx, dy;

		if (tex_plan(r->psm, r->sw, r->sh, H.tex_cap_blocks, r->shareable, &dx, &dy) && (dx != r->dx || dy != r->dy))
		{
			u32 nb = tex_plan_blocks(r->psm, r->sw, r->sh, dx, dy);

			if (nb <= r->nblk || H.upg_blocks + nb <= CAP_UPGRADE_BLOCKS)
			{
				if (nb > r->nblk)
					H.upg_blocks += nb;
				ov_flush_all();
				tex_drop((int)(r - H.rec), 0);
				H.st.tex_restamped++;
				r = NULL;
			}
		}
		else
		{
			r->capi = (u8)H.cap_idx;
		}
	}
	if (TRACING())
		CONS_Printf("HWT settex %s %ux%u fmt=%d fl=0x%x data=%p %s phase=%d\n", HWR_PS2_TexName(TexInfo), (unsigned)TexInfo->width, (unsigned)TexInfo->height,
			(int)TexInfo->format, (unsigned)TexInfo->flags, (void *)TexInfo->data, r ? "resident" : "upload", batch_phase);
	if ((ps2hwd_dbg_flags & HWDBG_BATCHORDER) && batch_phase == 2 && H.frame_no % 105 == 60)
		CONS_Printf("HWBO f=%u %s %ux%u %s fmt=%d blk=%u\n", (unsigned)H.frame_no, HWR_PS2_TexName(TexInfo), (unsigned)TexInfo->width, (unsigned)TexInfo->height, r ? "hit" : "UPLOAD", (int)TexInfo->format, r ? (unsigned)r->nblk : 0u);
	if (r)
	{
		H.cur_tex = (int)(r - H.rec);
		H.cur_missing = 0;
		if (batch_phase == 2)
			H.rec[img_of(H.cur_tex)].done = H.frame_no + 1;
		return;
	}
	if (batch_phase == 1)
	{
		H.cur_tex = NOREC;
		H.cur_missing = 1;
		return;
	}
	if (!TexInfo->data && !(TexInfo->format == GL_TEXFMT_P_8 && (TexInfo->regen_kind == 1 || TexInfo->regen_kind == 2) && (u32)TexInfo->width * TexInfo->height >= 2048
		&& (dc_find(dc_key(TexInfo), TexInfo->width, TexInfo->height, 0) || (want && dc_find(dc_key(TexInfo), TexInfo->width >> want, TexInfo->height >> want, want))
			|| (want && TexInfo->regen_kind == 2) || (want > 1 && dc_find_finer(dc_key(TexInfo), TexInfo->width, TexInfo->height, want, &(u32){0}))))) // PS2-HW-38/39: a level of a flat needs no copy of the flat (tex_upload pins the engine's)
	{
		u32 c0 = cyc();

		HWR_PS2_RegenerateMipmap(TexInfo);
		TX.regen_cyc += cyc() - c0;
		TX.regen_n++;
		H.st.tex_regen++;
	}
	if (TT.on)
		tt_capture(TexInfo); // PS2-HW-69: the texels as the engine hands them over
	if ((ps2hwd_dbg_flags & HWDBG_IMMDBG) && (u32)TexInfo->width * TexInfo->height >= PLAN_MIN_TEXELS)
		CONS_Printf("HWIMM f=%u %s %ux%u want=%u UPLOAD imm=%d phase=%d\n", (unsigned)H.frame_no, HWR_PS2_TexName(TexInfo), (unsigned)TexInfo->width, (unsigned)TexInfo->height, (unsigned)want, imm_level, batch_phase);
	ri = tex_upload(TexInfo);
	if (tex_flatpin)
	{
		HWR_PS2_FlatUnpin(tex_flatpin, (size_t)TexInfo->width * TexInfo->height);
		tex_flatpin = NULL;
	}
	if (!zc_last) // a zero-copy upload keeps the block locked until the DMA has read it (rel_add)
		HWR_PS2_ReleaseMipmapData(TexInfo);
	if (ri == NOREC)
	{
		// Recoverable: the draws that need the texture are skipped (the engine asks again next frame). PC's GL driver never
		// runs out; here a frame whose textures do not fit the pool, or a texture whose data the zone purged, loses polygons.
		H.st.tex_missing++;
		if (H.st.tex_missing <= 4)
			CONS_Printf("HWD texture not resident [frame %u phase %d spr_flush %d tex %p dl %u]: %s %s fmt=%d %ux%u flags=0x%x data=%p pool used %u/%u blocks, free ranges %d (draws skipped)\n", (unsigned)H.frame_no, batch_phase, SPR.in_flush, (void *)TexInfo, (unsigned)TexInfo->downloaded, tex_fail_why, HWR_PS2_TexName(TexInfo),
				(int)TexInfo->format, (unsigned)TexInfo->width, (unsigned)TexInfo->height, (unsigned)TexInfo->flags, (void *)TexInfo->data,
				(unsigned)H.used_blocks, (unsigned)H.pool_blocks, H.free_n);
		hw_limit(HW_MISSING, "a texture could not be made resident in the GS pool (or its data was purged); its draws are skipped");
		H.cur_tex = NOREC;
		H.cur_missing = 1;
		return;
	}
	H.cur_tex = ri;
	H.cur_missing = 0;
	if (batch_phase == 2)
		H.rec[ri].done = H.frame_no + 1;
}

// hwdriver SetTexture: a big map texture selected outside the batches waits for its polygon (imm_prepare, PS2-HW-37), the rest is made resident now
static void hw_SetTexture(GLMipmap_t *TexInfo)
{
	u32 t0;

	if (!H.up)
		return;
	t0 = cyc();
	drv_in();
	H.imm_tex = NULL;
	if (TexInfo && batch_phase == 0 && plan_wants(TexInfo) && !(ps2hwd_dbg_flags & HWDBG_NOPLAN))
	{
		H.imm_tex = TexInfo;
		H.cur_tex = NOREC;
		H.cur_missing = 0;
	}
	else
	{
		settex_now(TexInfo);
	}
	G.s_set_n++;
	G.s_set_cyc += cyc() - t0;
	drv_out();
}

// The batched polygon of this texture is drawn later in the frame: the texture (or the image of its other variant) must stay in VRAM until then.
// Only the frame stamp is set: the order of the LRU list is the order of DRAWING (PS2-HW-31).
void PS2HWD_TouchTexture(GLMipmap_t *TexInfo)
{
	static GLMipmap_t *last; // PS2-HW-227: a texture is stamped once a frame (a run of polygons of one texture, 60 cycles each, was stamped again and again)
	static u32 last_frame = ~0u;
	texrec_t *r;

	if (!H.up || !TexInfo)
		return;
	if (TexInfo == last && last_frame == H.frame_no)
		return;
	r = rec_of(TexInfo);
	if (!r)
		r = twin_rec(TexInfo);
	if (r)
	{
		H.rec[img_of((int)(r - H.rec))].stamp = H.frame_no;
		last = TexInfo;
		last_frame = H.frame_no;
	}
}

static void hw_UpdateTexture(GLMipmap_t *TexInfo)
{
	texrec_t *r;

	if (!H.up || !TexInfo)
		return;
	r = rec_of(TexInfo);
	if (r)
	{
		ov_flush_all();
		dma_fence(); // the engine has changed the texels: no queued reference may still read the old ones
		tex_drop((int)(r - H.rec), 0);
	}
	settex_now(TexInfo);
}

static void hw_DeleteTexture(GLMipmap_t *TexInfo)
{
	texrec_t *r;

	if (!TexInfo)
		return;
	if (H.up)
		dma_fence(); // the engine frees the texels after this call: no queued DMA may read them (also when the image was evicted meanwhile)
	if (H.imm_tex == TexInfo)
		H.imm_tex = NULL;
	if (H.up)
		plan_forget(TexInfo);
	if (H.up && (r = rec_of(TexInfo)) != NULL)
	{
		ov_flush_all();
		tex_drop((int)(r - H.rec), 0);
	}
	TexInfo->downloaded = 0;
}

static void hw_GClipRect(INT32 minx, INT32 miny, INT32 maxx, INT32 maxy, float nearclip)
{
	if (!H.up)
		return;
	viewport_set(minx, miny, maxx, maxy);
	xf_set_2d(nearclip);
}

static void hw_ClearMipMapCache(void)
{
	int i;

	if (!H.up)
		return;
	ov_flush_all();
	dma_fence();
	H.imm_tex = NULL;
	plan_reset();
	dc_flush(); // the engine may have another set of textures under the same numbers after this call (a new level, an add-on)
	// ordinary textures only: screen textures have their own life cycle (FlushScreenTextures)
	for (i = 0; i < H.rec_n; i++)
		if (H.rec[i].used && !H.rec[i].screen)
			tex_drop(i, 0);
	H.cur_tex = NOREC;
	H.cur_missing = 0;
}

static void hw_SetSpecialState(hwdspecialstate_t IdState, INT32 Value)
{
	if (IdState == HWD_SET_TEXTUREFILTERMODE)
	{
		H.tex_filter = Value;
		if (Value != HWD_SET_TEXTUREFILTER_POINTSAMPLED && Value != HWD_SET_TEXTUREFILTER_BILINEAR)
			hw_limit(HW_FILTER, "mipmapped/mixed texture filter approximated; no GS mip chain uploaded");
	}
	else if (IdState == HWD_SET_MODEL_LIGHTING)
	{
		H.model_light = Value; // OPT11-MODEL (PS2-HW-264): the vertex light factor of the OpenGL model shader is implemented (ps2_hw_model.inc)
	}
	else if (IdState == HWD_SET_SHADERS)
	{
		H.shaders_on = Value;
	}
	else if (Value > 1 && IdState == HWD_SET_TEXTUREANISOTROPICMODE)
		hw_limit(HW_ANISO, "anisotropic filtering unavailable");
	else if (IdState == HWD_SET_WIREFRAME)
	{
		wire_mode = Value ? 1 : 0; // OPT11-MODEL (PS2-HW-270): the walls and planes of the frame are drawn as the edges of their triangles (ps2_hw_wire.inc)
	}
}

static void hw_DrawModel(model_t *model, INT32 frameIndex, float duration, float tics, INT32 nextFrameIndex, FTransform *pos,
	float hscale, float vscale, UINT8 flipped, UINT8 hflipped, FSurfaceInfo *Surface)
{
	draw_model(model, frameIndex, duration, tics, nextFrameIndex, pos, hscale, vscale, flipped, hflipped, Surface);
}

static void hw_CreateModelVBOs(model_t *model)
{
	// The CPU path reads the current mesh UVs directly, including sprite UV adjustments.
	if (model)
	{
		model->vbo_max_s = model->max_s;
		model->vbo_max_t = model->max_t;
	}
}

static void hw_SetTransform(FTransform *stransform)
{
	if (H.up)
		xf_set_transform(stransform);
}

static INT32 hw_GetTextureUsed(void)
{
	return (INT32)(H.used_blocks * 256u);
}

// ---- shaders: an experimental subset has fixed GS passes; this is not built-in/custom GLSL capability ----

// PS2-HW-61 (OPT9): the base shaders (floor/wall/sprite/water/fog/sky) exist as GS passes: sector light as depth bands (GS fog) or a depth
// ramp overlay, tint through CLUT entries, fog blocks, wipes. The engine then lights with LightInfo instead of its flat per-polygon
// fallback colour. -hwdbg 2048 = the old behaviour (engine fallback lighting), for A/B pictures.
static boolean hw_InitShaders(void)
{
	CONS_Printf("HWD shaders %s (-hwdbg %d)\n", (ps2hwd_dbg_flags & 2048) ? "off" : "on", ps2hwd_dbg_flags);
	return !(ps2hwd_dbg_flags & 2048);
}

static void hw_LoadShader(int slot, char *code, hwdshaderstage_t stage)
{
	(void)code; (void)stage;
	if (slot >= NUMSHADERTARGETS) // the built-in GLSL is stood for by the GS passes; only a custom shader is a loss
		hw_limit(HW_SHADER, "custom GLSL cannot run on the GS; the base shader of its target is used");
}

static boolean hw_CompileShader(int slot)
{
	if (slot >= 0 && slot < NUMSHADERTARGETS)
		return true; // built-in: the fixed GS passes stand for the GLSL
	hw_limit(HW_SHADER, "custom GLSL shaders cannot run on the GS (the base shader of the target is used)");
	return false;
}

static void hw_SetShader(int slot)
{
	H.shader = slot;
	if (slot >= NUMSHADERTARGETS)
		hw_limit(HW_SHADER, "custom shader selected; the fixed built-in passes are used");
}

static void hw_UnSetShader(void)
{
	H.shader = -1;
}

static void hw_SetShaderInfo(hwdshaderinfo_t info, INT32 value)
{
	if (info == HWD_SHADERINFO_LEVELTIME)
	{
		H.leveltime = ps2hwd_force_lt >= 0 ? ps2hwd_force_lt : value;
		H.lt_frac = ps2hwd_force_lt >= 0 ? 1.0f : FIXED_TO_FLOAT(rendertimefrac); // as r_opengl.c SetShaderInfo: (value - 1 + rendertimefrac) / TICRATE (PS2-HW-125)
	}
}

// PS2-HW-71: palette rendering (gr_paletterendering): the colour of a texel is lighttable[index][row]. The GS does it with CLUTs (ps2_hw_pal.inc): the
// RGB-to-index lookup is not needed (the textures are indexed), the light tables are kept as indices, the screen palette is applied in the CLUTs.
static void hw_SetPaletteLookup(UINT8 *lut)
{
	(void)lut;
}

static UINT32 hw_CreateLightTable(RGBA_t *hw_lighttable)
{
	if (!H.up)
		return 0;
	return lt_store(0, hw_lighttable);
}

static void hw_UpdateLightTable(UINT32 id, RGBA_t *hw_lighttable)
{
	if (H.up && id && id < LT_MAX && lt_idx[id])
	{
		ov_flush_all(); // queued draws use the old rows
		lt_store(id, hw_lighttable);
	}
}

static void hw_ClearLightTables(void)
{
	if (H.up)
		ov_flush_all();
	lt_clear();
}

static void hw_SetScreenPalette(RGBA_t *palette)
{
	u32 np[256];
	int i;

	if (!H.up || !palette)
		return;
	for (i = 0; i < 256; i++)
		np[i] = (u32)palette[i].s.red | ((u32)palette[i].s.green << 8) | ((u32)palette[i].s.blue << 16);
	if (spal_set && !memcmp(np, spal, sizeof np))
		return;
	ov_flush_all(); // pending draws must consume the previous screen palette before the CLUTs change
	memcpy(spal, np, sizeof np);
	spal_set = 1;
	H.pal_gen++; // every CLUT made from the palette is rebuilt when it is next used
}

void PS2HWD_FillDriver(struct hwdriver_s *drv)
{
	drv->pfnInit = PS2HWD_Init;
	drv->pfnSetTexturePalette = hw_SetTexturePalette;
	drv->pfnFinishUpdate = hw_FinishUpdate;
	drv->pfnDraw2DLine = hw_Draw2DLine;
	drv->pfnDrawPolygon = hw_DrawPolygon;
	drv->pfnDrawIndexedTriangles = hw_DrawIndexedTriangles;
	drv->pfnRenderSkyDome = hw_RenderSkyDome;
	drv->pfnSetBlend = hw_SetBlend;
	drv->pfnClearBuffer = hw_ClearBuffer;
	drv->pfnSetTexture = hw_SetTexture;
	drv->pfnUpdateTexture = hw_UpdateTexture;
	drv->pfnDeleteTexture = hw_DeleteTexture;
	drv->pfnReadScreenTexture = hw_ReadScreenTexture;
	drv->pfnGClipRect = hw_GClipRect;
	drv->pfnClearMipMapCache = hw_ClearMipMapCache;
	drv->pfnSetSpecialState = hw_SetSpecialState;
	drv->pfnDrawModel = hw_DrawModel;
	drv->pfnCreateModelVBOs = hw_CreateModelVBOs;
	drv->pfnSetTransform = hw_SetTransform;
	drv->pfnGetTextureUsed = hw_GetTextureUsed;
	drv->pfnShutdown = PS2HWD_Shutdown;
	drv->pfnPostImgRedraw = hw_PostImgRedraw;
	drv->pfnFlushScreenTextures = hw_FlushScreenTextures;
	drv->pfnDoScreenWipe = hw_DoScreenWipe;
	drv->pfnDrawScreenTexture = hw_DrawScreenTexture;
	drv->pfnMakeScreenTexture = hw_MakeScreenTexture;
	drv->pfnDrawScreenFinalTexture = hw_DrawScreenFinalTexture;
	drv->pfnInitShaders = hw_InitShaders;
	drv->pfnLoadShader = hw_LoadShader;
	drv->pfnCompileShader = hw_CompileShader;
	drv->pfnSetShader = hw_SetShader;
	drv->pfnUnSetShader = hw_UnSetShader;
	drv->pfnSetShaderInfo = hw_SetShaderInfo;
	drv->pfnSetPaletteLookup = hw_SetPaletteLookup;
	drv->pfnCreateLightTable = hw_CreateLightTable;
	drv->pfnUpdateLightTable = hw_UpdateLightTable;
	drv->pfnClearLightTables = hw_ClearLightTables;
	drv->pfnSetScreenPalette = hw_SetScreenPalette;
}
