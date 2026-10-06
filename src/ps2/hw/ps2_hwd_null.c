// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// Copyright (C) 1999-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_hwd_null.c
/// \brief Logging stub of the GS hardware driver (same three entry points as ps2_hwd.c, see ps2_hwd.h).
///        Draws nothing: counts the calls the hardware renderer makes per frame and prints them, so that the
///        engine integration (mode switch, HWR_* call flow, 2D through HWR) can be tested before the GS driver.

#include <stdio.h>
#include <string.h>

#include "../../doomdef.h"
#include "../../console.h"
#include "../../hardware/hw_drv.h"
#include "../../hardware/hw_main.h"
#include "../../i_system.h"
#include "../../m_argv.h"

#include "ps2_hwd.h"

// one counter per hook; X(name, field) keeps counters, names and the table in one place
#define HOOKS(X) \
	X(SetTexturePalette) X(FinishUpdate) X(Draw2DLine) X(DrawPolygon) X(DrawIndexedTriangles) X(RenderSkyDome) \
	X(SetBlend) X(ClearBuffer) X(SetTexture) X(UpdateTexture) X(DeleteTexture) X(ReadScreenTexture) X(GClipRect) \
	X(ClearMipMapCache) X(SetSpecialState) X(DrawModel) X(CreateModelVBOs) X(SetTransform) X(GetTextureUsed) \
	X(PostImgRedraw) X(FlushScreenTextures) X(DoScreenWipe) X(DrawScreenTexture) X(MakeScreenTexture) \
	X(DrawScreenFinalTexture) X(InitShaders) X(LoadShader) X(CompileShader) X(SetShader) X(UnSetShader) \
	X(SetShaderInfo) X(SetPaletteLookup) X(CreateLightTable) X(UpdateLightTable) X(ClearLightTables) \
	X(SetScreenPalette)

enum
{
#define X(n) H_##n,
	HOOKS(X)
#undef X
	H_COUNT
};

static const char *const hook_names[H_COUNT] =
{
#define X(n) #n,
	HOOKS(X)
#undef X
};

static unsigned frame_calls[H_COUNT]; // since the last FinishUpdate
static unsigned total_calls[H_COUNT];
static unsigned frames;
static unsigned polys, polyverts; // vertices handed to DrawPolygon/DrawIndexedTriangles in this frame
static INT32 textures_used;
static boolean up;

#define HIT(n) (frame_calls[H_##n]++, total_calls[H_##n]++)

static void PrintCounters(const char *what, const unsigned *c)
{
	char line[512];
	size_t len = 0;
	int i;

	line[0] = '\0';
	for (i = 0; i < H_COUNT && len < sizeof line - 40; i++)
		if (c[i])
			len += (size_t)snprintf(line + len, sizeof line - len, " %s=%u", hook_names[i], c[i]);
	CONS_Printf("HWD-NULL %s:%s\n", what, line);
}

static boolean NullInit(void)
{
	up = true;
	frames = 0;
	CONS_Printf("HWD-NULL: Init\n");
	return true;
}

static void NullShutdown(void)
{
	if (!up)
		return;
	up = false;
	PrintCounters("total", total_calls);
	CONS_Printf("HWD-NULL: Shutdown after %u frames\n", frames);
}

static void NullSetTexturePalette(RGBA_t *ppal) { (void)ppal; HIT(SetTexturePalette); }

static void NullFinishUpdate(INT32 waitvbl)
{
	(void)waitvbl;
	HIT(FinishUpdate);
	frames++;
	if (frames <= 3 || frames % 350 == 0)
	{
		char what[64];
		snprintf(what, sizeof what, "frame %u (polys=%u verts=%u tex=%d)", frames, polys, polyverts, (int)textures_used);
		PrintCounters(what, frame_calls);
	}
	memset(frame_calls, 0, sizeof frame_calls);
	polys = polyverts = 0;
}

static void NullDraw2DLine(F2DCoord *v1, F2DCoord *v2, RGBA_t Color) { (void)v1; (void)v2; (void)Color; HIT(Draw2DLine); }

static void NullDrawPolygon(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags)
{
	(void)pSurf; (void)pOutVerts; (void)PolyFlags;
	HIT(DrawPolygon);
	polys++;
	polyverts += iNumPts;
}

static void NullDrawIndexedTriangles(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags, UINT32 *IndexArray)
{
	(void)pSurf; (void)pOutVerts; (void)PolyFlags; (void)IndexArray;
	HIT(DrawIndexedTriangles);
	polys++;
	polyverts += iNumPts;
}

static void NullRenderSkyDome(gl_sky_t *sky) { (void)sky; HIT(RenderSkyDome); }
static void NullSetBlend(FBITFIELD PolyFlags) { (void)PolyFlags; HIT(SetBlend); }
static void NullClearBuffer(FBOOLEAN ColorMask, FBOOLEAN DepthMask, FRGBAFloat *ClearColor) { (void)ColorMask; (void)DepthMask; (void)ClearColor; HIT(ClearBuffer); }
static void NullSetTexture(GLMipmap_t *TexInfo) { (void)TexInfo; HIT(SetTexture); }

static void NullUpdateTexture(GLMipmap_t *TexInfo)
{
	HIT(UpdateTexture);
	if (TexInfo && !TexInfo->downloaded)
		textures_used++;
	if (TexInfo)
		TexInfo->downloaded = 1; // any non-zero token: "the GPU has it", hw_cache.c only tests for zero
}

static void NullDeleteTexture(GLMipmap_t *TexInfo)
{
	HIT(DeleteTexture);
	if (TexInfo && TexInfo->downloaded)
	{
		TexInfo->downloaded = 0;
		textures_used--;
	}
}

static void NullReadScreenTexture(int tex, UINT8 *dst_data) { (void)tex; (void)dst_data; HIT(ReadScreenTexture); }
static void NullGClipRect(INT32 minx, INT32 miny, INT32 maxx, INT32 maxy, float nearclip) { (void)minx; (void)miny; (void)maxx; (void)maxy; (void)nearclip; HIT(GClipRect); }
static void NullClearMipMapCache(void) { HIT(ClearMipMapCache); }
static void NullSetSpecialState(hwdspecialstate_t IdState, INT32 Value) { (void)IdState; (void)Value; HIT(SetSpecialState); }

static void NullDrawModel(model_t *model, INT32 frameIndex, float duration, float tics, INT32 nextFrameIndex, FTransform *pos,
	float hscale, float vscale, UINT8 flipped, UINT8 hflipped, FSurfaceInfo *Surface)
{
	(void)model; (void)frameIndex; (void)duration; (void)tics; (void)nextFrameIndex; (void)pos;
	(void)hscale; (void)vscale; (void)flipped; (void)hflipped; (void)Surface;
	HIT(DrawModel);
}

static void NullCreateModelVBOs(model_t *model) { (void)model; HIT(CreateModelVBOs); }
static void NullSetTransform(FTransform *ptransform) { (void)ptransform; HIT(SetTransform); }
static INT32 NullGetTextureUsed(void) { HIT(GetTextureUsed); return textures_used * 4096; }
static void NullPostImgRedraw(float points[SCREENVERTS][SCREENVERTS][2]) { (void)points; HIT(PostImgRedraw); }
static void NullFlushScreenTextures(void) { HIT(FlushScreenTextures); }
static void NullDoScreenWipe(int wipeStart, int wipeEnd, FSurfaceInfo *surf, FBITFIELD polyFlags) { (void)wipeStart; (void)wipeEnd; (void)surf; (void)polyFlags; HIT(DoScreenWipe); }
static void NullDrawScreenTexture(int tex, FSurfaceInfo *surf, FBITFIELD polyflags) { (void)tex; (void)surf; (void)polyflags; HIT(DrawScreenTexture); }
static void NullMakeScreenTexture(int tex) { (void)tex; HIT(MakeScreenTexture); }
static void NullDrawScreenFinalTexture(int tex, int width, int height) { (void)tex; (void)width; (void)height; HIT(DrawScreenFinalTexture); }

// shaders do not exist on the GS
static boolean NullInitShaders(void) { HIT(InitShaders); return false; }
static void NullLoadShader(int slot, char *code, hwdshaderstage_t stage) { (void)slot; (void)code; (void)stage; HIT(LoadShader); }
static boolean NullCompileShader(int slot) { (void)slot; HIT(CompileShader); return false; }
static void NullSetShader(int slot) { (void)slot; HIT(SetShader); }
static void NullUnSetShader(void) { HIT(UnSetShader); }
static void NullSetShaderInfo(hwdshaderinfo_t info, INT32 value) { (void)info; (void)value; HIT(SetShaderInfo); }
static void NullSetPaletteLookup(UINT8 *lut) { (void)lut; HIT(SetPaletteLookup); }
static UINT32 NullCreateLightTable(RGBA_t *hw_lighttable) { (void)hw_lighttable; HIT(CreateLightTable); return 0; }
static void NullUpdateLightTable(UINT32 id, RGBA_t *hw_lighttable) { (void)id; (void)hw_lighttable; HIT(UpdateLightTable); }
static void NullClearLightTables(void) { HIT(ClearLightTables); }
static void NullSetScreenPalette(RGBA_t *palette) { (void)palette; HIT(SetScreenPalette); }

boolean PS2HWD_Init(void)
{
	return NullInit();
}

void PS2HWD_Shutdown(void)
{
	NullShutdown();
}

void PS2HWD_SetScreenSize(INT32 w, INT32 h)
{
	(void)w;
	(void)h;
}

void PS2HWD_FillDriver(struct hwdriver_s *drv)
{
	drv->pfnInit = PS2HWD_Init;
	drv->pfnSetTexturePalette = NullSetTexturePalette;
	drv->pfnFinishUpdate = NullFinishUpdate;
	drv->pfnDraw2DLine = NullDraw2DLine;
	drv->pfnDrawPolygon = NullDrawPolygon;
	drv->pfnDrawIndexedTriangles = NullDrawIndexedTriangles;
	drv->pfnRenderSkyDome = NullRenderSkyDome;
	drv->pfnSetBlend = NullSetBlend;
	drv->pfnClearBuffer = NullClearBuffer;
	drv->pfnSetTexture = NullSetTexture;
	drv->pfnUpdateTexture = NullUpdateTexture;
	drv->pfnDeleteTexture = NullDeleteTexture;
	drv->pfnReadScreenTexture = NullReadScreenTexture;
	drv->pfnGClipRect = NullGClipRect;
	drv->pfnClearMipMapCache = NullClearMipMapCache;
	drv->pfnSetSpecialState = NullSetSpecialState;
	drv->pfnDrawModel = NullDrawModel;
	drv->pfnCreateModelVBOs = NullCreateModelVBOs;
	drv->pfnSetTransform = NullSetTransform;
	drv->pfnGetTextureUsed = NullGetTextureUsed;
	drv->pfnShutdown = PS2HWD_Shutdown;
	drv->pfnPostImgRedraw = NullPostImgRedraw;
	drv->pfnFlushScreenTextures = NullFlushScreenTextures;
	drv->pfnDoScreenWipe = NullDoScreenWipe;
	drv->pfnDrawScreenTexture = NullDrawScreenTexture;
	drv->pfnMakeScreenTexture = NullMakeScreenTexture;
	drv->pfnDrawScreenFinalTexture = NullDrawScreenFinalTexture;
	drv->pfnInitShaders = NullInitShaders;
	drv->pfnLoadShader = NullLoadShader;
	drv->pfnCompileShader = NullCompileShader;
	drv->pfnSetShader = NullSetShader;
	drv->pfnUnSetShader = NullUnSetShader;
	drv->pfnSetShaderInfo = NullSetShaderInfo;
	drv->pfnSetPaletteLookup = NullSetPaletteLookup;
	drv->pfnCreateLightTable = NullCreateLightTable;
	drv->pfnUpdateLightTable = NullUpdateLightTable;
	drv->pfnClearLightTables = NullClearLightTables;
	drv->pfnSetScreenPalette = NullSetScreenPalette;
}
