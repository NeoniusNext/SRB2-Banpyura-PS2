// PS2 GS hardware renderer driver: configuration, statistics and test hooks (see docs/HW_RENDERER.md).
// Optional for the engine integration (PS2HWD_Configure/PS2HWD_SetScreenSize/PS2HWD_GetStats); used by tools/ps2/hw_test*.c.

#ifndef __PS2_HWD_DBG_H__
#define __PS2_HWD_DBG_H__

#include "../../doomtype.h"

enum
{
	PS2HWD_VIDEO_AUTO = -1, // NTSC unless rom0:ROMVER says Europe
	PS2HWD_VIDEO_NTSC = 0, // 640x448 interlaced field mode
	PS2HWD_VIDEO_PAL = 1, // 640x512 interlaced field mode
	PS2HWD_VIDEO_480P = 2, // 640x480 progressive
};

typedef struct
{
	int video; // PS2HWD_VIDEO_*
	int fbw, fbh; // internal frame size in GS pixels; 0 = derived from the engine size (screen_w x screen_h) and the video mode
	int fb32; // 1 = CT32 frame buffers (default, no colour loss); 0 = CT16S dithered (opt-in, loses colour precision); the Z buffer is always Z24
	int linear; // initial texture filter: 0 nearest, 1 bilinear (the engine overrides it through SetSpecialState)
	int dither; // 16-bit frame buffer: ordered dither (default 1)
	int tex_max_bytes; // hard limit on GS allocated texture bytes (including layout padding); 0 = pool limit. Never resamples.
	int decal_bias; // PF_Decal depth bias in Z LSBs (default 3)
	int screen_w, screen_h; // engine coordinate space of GClipRect/DrawPolygon (default 320x200)
	int no_keep; // 1 = a new frame does not start as a copy of the previous one (the engine then has to redraw everything)
	unsigned int screen_max_bytes; // EE capture backing budget; 0 = 8 MiB. Exhaustion preserves existing captures and reports failure.
	unsigned int tex_cap_blocks; // footprint cap of one texture in 256-byte blocks; 0 = none. Bigger textures are stored decimated by powers of two (PS2-HW-20).
	int tex_adapt; // OPT8 (PS2-HW-24): 1 = the cap follows the working set (tex_cap_blocks is then the starting cap, 0 = 512); 0 = fixed cap
} ps2hwd_config_t;

typedef struct
{
	unsigned int frames; // FinishUpdate calls
	unsigned int polys; // DrawPolygon + triangle fans of DrawIndexedTriangles
	unsigned int verts_in; // vertices transformed
	unsigned int verts_out; // vertices written into GIF packets (after clipping)
	unsigned int clipped; // polygons that needed clipping
	unsigned int rejected; // polygons outside the view volume
	unsigned int qwords; // 16-byte words sent to the GIF (all packets)
	unsigned int state_writes; // A+D register writes for state changes
	unsigned int uploads; // texture uploads
	unsigned int upload_bytes;
	unsigned int evictions; // textures dropped from VRAM to make room
	unsigned int tex_missing; // draws skipped: texture not in VRAM and its data was purged
	unsigned int dma_kicks; // GIF DMA transfers started
	unsigned int dma_waits; // times the EE had to sleep for a free DMA buffer
	unsigned int frame_waits; // times a new frame had to wait for the GS to finish the previous one
	unsigned int dropped; // frames replaced before they were shown
	unsigned int flips;
	unsigned int vblanks;
	unsigned int timeouts; // bounded waits that ran out
	unsigned int cyc_draw; // EE cycles spent inside DrawPolygon/DrawIndexedTriangles (transform+clip+pack)
	unsigned int cyc_tex; // EE cycles spent converting/uploading textures
	unsigned int cyc_wait; // EE cycles spent waiting (DMA ring, GS)
	unsigned int cyc_finish; // longest FinishUpdate
	unsigned int passes; // extra draws made for one polygon (multi-pass blends, light bands, texture repeats)
	unsigned int bands; // polygon pieces made by depth banding (Doom light / water)
	unsigned int clut_uploads; // CLUT variant images written to VRAM
	unsigned int unsupported_calls; // calls taking an unsupported / approximate path
	unsigned int unsupported_mask; // HW_* bits in ps2_hw_priv.inc; warnings emitted once per Init
	unsigned int screen_spills, screen_restores;
	unsigned int tex_skipped; // draws not made because their texture could not be made resident (see tex_missing)
	unsigned int tex_regen; // textures whose data the engine had to make again at draw time
	unsigned int tex_decimated; // textures stored decimated (over 1024 texels per axis, or over the footprint cap)
	unsigned int ws_blocks, ws_tex; // working set: GS blocks / distinct textures drawn (summed over the frames of the window)
	unsigned int flip_waits; // OPT5: new frames that had to wait for the flip of the finished one (PS2-HW-21)
	unsigned int flip_forced; // finished frames the EE had to flip itself because the vblank handler did not
	unsigned int wd_recoveries; // watchdog resets of the GIF DMA channel (a wait ran out)
	unsigned int cyc_flipwait; // EE cycles spent waiting for flips
	unsigned int cap_changes; // OPT8: changes of the texture footprint cap (adaptive controller)
	unsigned int pred_ws; // OPT8: working set of the last frame under the cap in force, in blocks
	unsigned int cap_blocks; // OPT8: footprint cap in force (0 = none)
	unsigned int tex_restamped; // OPT8: textures re-made after a cap change
} ps2hwd_stats_t;

typedef struct
{
	int fbw, fbh; // frame buffer stride (pixels) and rows
	int vw, vh; // visible internal size
	int magh, magv; // CRTC magnification actually programmed
	int fb32;
	int outid; // docs/VIDEO_MODES.md table B format
	int mode; // PS2HWD_VIDEO_NTSC/PAL/480P actually used
	unsigned int fb_block[2]; // frame buffers, VRAM address in 256-byte blocks
	unsigned int z_block;
	int z_bits; // 24
	int displayed; // index of the frame buffer the CRTC reads right now
	int target; // index of the frame buffer being drawn (or the last one drawn)
	unsigned int pool_block, pool_blocks; // texture pool, 256-byte blocks
	unsigned int pool_used_blocks;
	unsigned int clut_block;
	int screen_w, screen_h;
	int dma_irq; // 1 when GIF DMA completion interrupts are in use, 0 = polling
	unsigned int state_bytes, ring_bytes, record_bytes, scratch_bytes, sky_bytes;
	unsigned int screen_bytes, screen_peak_bytes; // EE capture backing, current and high-water (not GS pool)
} ps2hwd_info_t;

// Must be called before PS2HWD_Init to take effect (the frame buffer is allocated there).
void PS2HWD_Configure(const ps2hwd_config_t *cfg);
void PS2HWD_GetConfig(ps2hwd_config_t *cfg);

void PS2HWD_GetInfo(ps2hwd_info_t *out);
void PS2HWD_GetStats(ps2hwd_stats_t *out, int reset);
// diagnostics: prints the biggest textures drawn in the current frame
void PS2HWD_DumpWorkingSet(void);

// VU0 transform against the scalar one (tools/ps2/hw_test.c, group vu0_transform): n random vertices through a random camera
typedef struct
{
	unsigned int n; // vertices compared
	unsigned int oc_diff; // different outcodes
	unsigned int dx_nonzero, dy_nonzero, dz_nonzero; // vertices whose integer X / Y / Z differ
	int max_dx, max_dy, max_dz; // largest integer difference (X, Y in 1/16 pixel, Z in Z24 LSB)
	float max_q_rel; // largest relative difference of 1/w
	int have_vu0; // 0 when the driver was built without the VU0 path
	unsigned int cyc_scalar, cyc_vu0; // EE cycles of the two paths over the same n vertices (n <= 4096)
} ps2hwd_vu0test_t;
void PS2HWD_TestVU0(unsigned int n, unsigned int seed, ps2hwd_vu0test_t *out);

// Test hooks. Wait until everything queued so far has been executed by the GS (FINISH seen).
int PS2HWD_Sync(void);
// Make the frame buffer index the displayed one without waiting for a vblank (tests read VRAM back).
// Reads a rectangle of VRAM of the given PSM (GS_PSM_*) into dst (32-bit pixels for CT32/Z24, 16-bit for CT16*); 0 = ok.
int PS2HWD_ReadVram(void *dst, unsigned int block256, unsigned int tbw, int psm, int x, int y, int w, int h);
// Read the last completely drawn frame as 0x00BBGGRR (CT16 pixels are expanded, replicating the high bits).
int PS2HWD_ReadFrame(unsigned int *dst);
// Z buffer of the last completely drawn frame, normalised to 0..0xFFFFFF.
int PS2HWD_ReadDepth(unsigned int *dst);
// PS2-HW-69 (-hwtextest): every map texture and level flat is drawn 1:1 and repeated, read back and compared with the engine texels (TTFAIL lines).
void PS2HWD_TextureTest(void);
// Negative controls: bit 0 = ignore Z test, bit 1 = ignore alpha test, bit 2 = ignore blending, bit 3 = never write Z.
extern int ps2hwd_dbg_flags;

#endif
