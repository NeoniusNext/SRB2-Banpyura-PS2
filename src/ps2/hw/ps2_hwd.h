// PS2 GS hardware renderer driver (implements struct hwdriver_s from src/hardware/hw_drv.h).
// Design and VRAM layout: docs/HW_RENDERER.md. Public contract for the engine integration.

#ifndef __PS2_HWD_H__
#define __PS2_HWD_H__

#include "../../doomtype.h"

struct hwdriver_s;

// Bring the GS up for 3D (frame buffers, Z buffer, texture/CLUT areas, vblank flip handler). FALSE on failure.
// Takes over the GS from the software path (ps2gs_*); call ps2gs_shutdown() first if it was running.
boolean PS2HWD_Init(void);

// Fill every pointer of *drv; unsupported shader features report their limitations, never NULL.
// pfnInit is PS2HWD_Init itself, pfnShutdown is PS2HWD_Shutdown.
void PS2HWD_FillDriver(struct hwdriver_s *drv);

// Wait for the GS, drop textures, remove the vblank handler. Safe to call twice.
void PS2HWD_Shutdown(void);

// PS2-170: drop the half-built frame (the engine left the drawing through a recoverable out-of-memory jump); the driver is then shut down. Safe when idle.
void PS2HWD_Abort(void);

// The engine's screen size (vid.width x vid.height): the coordinate space of GClipRect. Default 320x200. May be called at any time.
void PS2HWD_SetScreenSize(INT32 w, INT32 h);

// Engine resource failures must abort the frame, not leave screenshots/draws partly unwritten.
// NULL keeps standalone tests nonfatal. The handler must not return in the engine.
void PS2HWD_SetFatalHandler(void (*handler)(const char *message));
boolean PS2HWD_CaptureScreen(INT32 slot);

// Batched polygons (hardware/hw_batching.c) are drawn after the BSP walk, not when their texture is selected: between BatchBegin
// and BatchEnd a texture touched by TouchTexture is not evicted from the GS pool (the engine drops its data after the upload).
struct GLMipmap_s;
// Fan batches of hw_batching.c: desc holds nfans pairs (index of the first vertex in base, vertex count); one plan for all of them.
struct FSurfaceInfo_s;
void PS2HWD_DrawFans(void *surf, void *base, unsigned int nfans, unsigned int flags, const unsigned int *desc);
// The frame plan (PS2-HW-34): every polygon of the frame goes through PlanPolygon before the first batch is drawn; the driver notes which mip level each big texture needs.
void PS2HWD_PlanBegin(void);
void PS2HWD_PlanPolygon(struct GLMipmap_s *mipmap, const void *verts, unsigned int n); // verts: n FOutVector
void PS2HWD_PlanEnd(void);
unsigned int PS2HWD_ScanDirection(void); // 0 / 1 alternating with the frames: the order in which the batches of a frame run through the textures (PS2-HW-31)
void PS2HWD_BatchBegin(void); // polygons are collected: SetTexture only notes the texture
void PS2HWD_BatchDraw(void); // the collected polygons are drawn: SetTexture makes the texture resident (asking for its data again if needed)
void PS2HWD_BatchEnd(void);
void PS2HWD_TouchTexture(struct GLMipmap_s *mipmap);
boolean PS2HWD_ReadScreenRGB(INT32 slot, UINT8 *dst);
void PS2HWD_ProfExtra(unsigned int frames); // OPT10 HG: HWPROF3 line (geometry path counters of the window), resets them

// Diagnostics: frame number (of the driver) whose texture selections and draws are printed (-hwtrace N), -1 = off; flags = ps2hwd_dbg_flags (-hwdbg N).
void PS2HWD_SetTrace(int frame, int dbg_flags);

#endif
