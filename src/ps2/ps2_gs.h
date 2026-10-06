// PS2 GS output for the software renderer (indexed frame of any size from ps2_vmodes.h; 320x200 by default).
// The index frame is uploaded as PSMT8 (+ CLUT, CT32, CSM1) and drawn as one textured sprite
// into the back frame buffer; the flip happens in the vblank interrupt (no busy-wait on vsync).
// The output format (NTSC/PAL, field/frame, 480p, 576p, 720p, 1080i, VESA) is a table in ps2_vmodes.h.
// Independent of the engine headers so that the hardware test can link it alone.

#ifndef __PS2_GS_H__
#define __PS2_GS_H__

#include "ps2_vmodes.h"

// the default source size and the size of the buffers of the first mode
#define PS2GS_FRAME_W 320
#define PS2GS_FRAME_H 200
#define PS2GS_FRAME_BYTES (PS2GS_FRAME_W * PS2GS_FRAME_H) // multiple of 64: every screens[] slice is DMA aligned

enum
{
	PS2GS_MODE_AUTO = PS2VM_AUTO, // NTSC unless rom0:ROMVER says Europe
	PS2GS_MODE_NTSC = PS2VM_NTSC, // 640x448 interlaced (field mode)
	PS2GS_MODE_PAL = PS2VM_PAL, // 640x512 interlaced (field mode)
	PS2GS_MODE_480P = PS2VM_480P, // 640x480 progressive (component cable)
};

// ps2gs_init() / ps2gs_check() results
enum
{
	PS2GS_OK = 0,
	PS2GS_E_GS = -1, // gsKit/DMA failed
	PS2GS_E_VRAM = -2, // frame buffers + source texture do not fit into the 4 MB
	PS2GS_E_SEMA = -3,
	PS2GS_E_BIOS = -4, // the BIOS does not know this mode (576p needs romver 2.20)
	PS2GS_E_ID = -5, // no such output format / source size
};

typedef struct
{
	unsigned int frames; // frames submitted
	unsigned int dropped; // frames replaced before they were ever shown
	unsigned int flips; // flips done by the vblank handler
	unsigned int vblanks; // vblank interrupts seen
	unsigned int waited; // present() calls that slept on a vblank
	unsigned int timeouts; // bounded spins that ran out (DMA or GS FINISH)
	unsigned int dma_wait_max; // longest wait for the GIF DMA to drain, EE cycles
	unsigned int flush_max; // longest FlushCache + chain build/patch, EE cycles
	unsigned int present_max; // longest ps2gs_present call, EE cycles
	unsigned int packet_builds; // complete GIF packet rebuilds (mode, destination, filter or clear change)
	unsigned int clut_uploads; // palette DMA uploads and internal CLUT reloads
} ps2gs_stats_t;

// Can this output format be used with a source of w x h (BIOS, VRAM)? Callable before ps2gs_init. 0 or PS2GS_E_*.
int ps2gs_check(int outid, int w, int h);
// Initialise the GS and the vblank handler. outid: PS2GS_MODE_AUTO or an index of ps2vm_output(). Returns 0 or PS2GS_E_*.
int ps2gs_init(int outid);
void ps2gs_shutdown(void);
int ps2gs_is_up(void);

// The size of the index frame passed to ps2gs_present (default 320x200; kept across ps2gs_shutdown/init).
// Fails (PS2GS_E_*) when the size is not a table size or the texture does not fit into VRAM; then nothing changes.
int ps2gs_set_source(int w, int h);
// Where the frame goes inside the frame buffer (PS2VM_FIT_*), default PS2VM_FIT_43; takes effect on the next present.
void ps2gs_set_fit(int fit);
void ps2gs_get_dest(int *x, int *y, int *w, int *h);

// 256 entries 0x00BBGGRR (alpha is forced to 0x80). The index bits 3/4 swap of the CT32 CLUT layout is done here.
void ps2gs_set_palette(const unsigned int *rgb);

// Upload and draw one index frame (source size, 64-byte aligned). Never waits for a vblank, except
// when wait_flip is set and an older frame is still waiting for its flip: then it sleeps at most one vblank.
// The frame buffer may be reused by the caller as soon as this returns.
void ps2gs_present(const unsigned char *frame, int wait_flip);

// Sleep (semaphore, no spinning) until count more vblanks passed.
void ps2gs_wait_vblank(int count);

// Destination rectangle on the frame buffer for the frame (default: from the fit); takes effect on the next present.
// An explicit rectangle stays until ps2gs_set_fit() or the next ps2gs_set_source() is called.
void ps2gs_set_dest(int x, int y, int w, int h);
// 0 = nearest (default), 1 = bilinear
void ps2gs_set_filter(int linear);

int ps2gs_fb_width(void);
int ps2gs_fb_height(void);
int ps2gs_mode(void); // outid in use
int ps2gs_source_width(void);
int ps2gs_source_height(void);
void ps2gs_get_crtc(ps2vm_crtc_t *c); // what was written to DISPLAY2
void ps2gs_get_layout(unsigned int *fb0, unsigned int *fb1, unsigned int *tex, unsigned int *clut); // VRAM byte addresses
unsigned int ps2gs_fb_block(int index); // VRAM address of frame buffer index in 256-byte blocks (for GS readback)
int ps2gs_displayed(void); // index of the frame buffer the CRTC reads right now
int ps2gs_flip_pending(void);
void ps2gs_get_stats(ps2gs_stats_t *out);
int ps2gs_bios_version(void); // romver number, e.g. 200 for 2.00
int ps2gs_region_output(void); // PS2GS_MODE_PAL on a European console, PS2GS_MODE_NTSC otherwise

// Test-only: skip the bit 3/4 swap (negative control).
extern int ps2gs_dbg_noswap;

#endif
