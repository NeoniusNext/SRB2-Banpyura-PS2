// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2020-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_batching.c
/// \brief Draw call batching and related things.

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_batching.h"
#include "../i_system.h"
#include <limits.h>
#ifdef PS2
#include "../z_zone.h"
#include "../ps2/hw/ps2_hwd.h"
#include "hw_sort.h"
#include "../ps2/hw/ps2_hw_prof.h"
extern int ps2hwd_dbg_flags; // the driver's -hwdbg bits (ps2/hw/ps2_hwd.c)
#endif

// The texture for the next polygon given to HWR_ProcessPolygon.
// Set with HWR_SetCurrentTexture.
GLMipmap_t *current_texture = NULL;

boolean currently_batching = false;
#ifdef PS2_PROFILE
boolean hwr_sprite_batch = false;
boolean hwr_sprite_shadow = false;
#endif

FOutVector* finalVertexArray = NULL;// contains subset of sorted vertices and texture coordinates to be sent to gpu
UINT32* finalVertexIndexArray = NULL;// contains indexes for glDrawElements, taking into account fan->triangles conversion
//     NOTE have this alloced as 3x finalVertexArray size
#ifdef PS2_PROFILE // PS2-HW-06: 32 MiB of RAM; the arrays double when a frame needs more (see HWR_ProcessPolygon / HWR_RenderBatches)
int finalVertexArrayAllocSize = 8192;
#else
int finalVertexArrayAllocSize = 65536;
#endif
//GLubyte* colorArray = NULL;// contains color data to be sent to gpu, if needed
//int colorArrayAllocSize = 65536;
// not gonna use this for now, just sort by color and change state when it changes
// later maybe when using vertex attributes if it's needed

PolygonArrayEntry* polygonArray = NULL;// contains the polygon data from DrawPolygon, waiting to be processed
int polygonArraySize = 0;
UINT32* polygonIndexArray = NULL;// contains sorting pointers for polygonArray
#ifdef PS2_PROFILE
int polygonArrayAllocSize = 2048;
#else
int polygonArrayAllocSize = 65536;
#endif

FOutVector* unsortedVertexArray = NULL;// contains unsorted vertices and texture coordinates from DrawPolygon
int unsortedVertexArraySize = 0;
#ifdef PS2_PROFILE
int unsortedVertexArrayAllocSize = 8192;
#else
int unsortedVertexArrayAllocSize = 65536;
#endif

#ifdef PS2
// The EE backend consumes indexed triangles synchronously. Index the collected vertices directly;
// sorting changes only the triangle order, so no duplicate vertex storage or per-batch memcpy is needed.
#define HWR_BATCH_VERTICES unsortedVertexArray
#else
#define HWR_BATCH_VERTICES finalVertexArray
#endif

#ifdef PS2
// PS2-HW-31: the order of the batches. The pool of the GS holds less than the textures of a frame, so a frame uploads what is not resident,
// and the batches of one texture must be drawn together (the engine's key hashed the texture together with the light level / blend mode,
// scattering the batches of a texture over the frame: it was uploaded again for each of them). The key is the texture order (the map
// texture or flat number) first, the rest of the state below it. The direction of the scan alternates from frame to frame: the textures
// that stay in the pool at the end of a frame (the ones drawn last) are then the ones drawn first in the next, the best a cyclic scan can
// do when the pool is smaller than the set. The state change test below compares the real state, so equal keys of different states are safe.
static UINT32 HWR_PS2_TextureOrder(const GLMipmap_t *t)
{
	UINT32 id;
	const UINT32 ps2_scan_dir = PS2HWD_ScanDirection(); // the frame's parity, not the batching passes' (a skybox view batches twice per frame)

	if (!t)
		return 0x3FFEu;
	if (t->regen_kind == 1)
		id = (UINT32)t->regen_id & 0x1FFFu;
	else if (t->regen_kind == 2)
		id = 0x2000u | ((UINT32)t->regen_id & 0x1FFFu);
	else
		id = (((UINT32)(uintptr_t)t >> 4) * 2654435761u) >> 18; // patches: any fixed order
	return ps2_scan_dir ? 0x3FFFu - id : id;
}
#endif

// Enables batching mode. HWR_ProcessPolygon will collect polygons instead of passing them directly to the rendering backend.
static void *HWR_BatchResize(void *old, size_t bytes)
{
	void *p;
#ifdef PS2
	// Geometry is live until sorted submission completes: non-purgable, accounted zone storage.
	p = Z_ReallocAlign(old, bytes, PU_HWRBATCH, NULL, 4);
#else
	p = realloc(old, bytes);
#endif
	if (!p)
		I_Error("Hardware batch allocation failed (%lu bytes)", (unsigned long)bytes);
	return p;
}
#define HWR_BatchAlloc(bytes) HWR_BatchResize(NULL, (bytes))

static int HWR_BatchCapacity(int old, int required, size_t element)
{
	size_t cap = (size_t)old;
	const size_t limit = (size_t)INT_MAX < SIZE_MAX / element ? (size_t)INT_MAX : SIZE_MAX / element;
	if (required < 0 || (size_t)required > limit)
		I_Error("Hardware batch size overflow");
	while (cap < (size_t)required)
	{
#ifdef PS2_PROFILE
		size_t step = cap / 2; // 50%% spare capacity instead of 100%% on memory-limited targets
#else
		size_t step = cap;
#endif
		if (!step)
			step = 1;
		cap = step > limit - cap ? limit : cap + step;
	}
	return (int)cap;
}

// One draw call of the batch being built: triangle indices, or on the PS2 (first vertex, count) pairs of fans (see HWR_RenderBatches)
static void HWR_DrawBatch(FSurfaceInfo *surf, int count, FBITFIELD polyFlags)
{
#ifdef PS2
	PS2HWD_DrawFans(surf, HWR_BATCH_VERTICES, (unsigned int)count / 2, polyFlags, finalVertexIndexArray);
#else
	HWD.pfnDrawIndexedTriangles(surf, HWR_BATCH_VERTICES, count, polyFlags, finalVertexIndexArray);
#endif
}

// Call HWR_RenderBatches to render all the collected geometry.
void HWR_StartBatching(void)
{
	if (currently_batching)
		I_Error("Repeat call to HWR_StartBatching without HWR_RenderBatches");

	// init arrays if that has not been done yet
	if (!polygonArray)
	{
#ifndef PS2
		finalVertexArray = HWR_BatchAlloc(finalVertexArrayAllocSize * sizeof(FOutVector));
#endif
		finalVertexIndexArray = HWR_BatchAlloc(finalVertexArrayAllocSize * 3 * sizeof(UINT32));
		polygonArray = HWR_BatchAlloc(polygonArrayAllocSize * sizeof(PolygonArrayEntry));
		polygonIndexArray = HWR_BatchAlloc(polygonArrayAllocSize * sizeof(UINT32));
		unsortedVertexArray = HWR_BatchAlloc(unsortedVertexArrayAllocSize * sizeof(FOutVector));
	}

	currently_batching = true;
#ifdef PS2
	PS2HWD_BatchBegin();
#endif
}

// This replaces the direct calls to pfnSetTexture in cases where batching is available.
// The texture selection is saved for the next HWR_ProcessPolygon call.
// Doing this was easier than getting a texture pointer to HWR_ProcessPolygon.
void HWR_SetCurrentTexture(GLMipmap_t *texture)
{
    if (currently_batching)
    {
        current_texture = texture;
#ifdef PS2 // PS2-HW-16: the polygon is drawn after the BSP walk; its texture must not be evicted from the GS pool before that
        PS2HWD_TouchTexture(texture);
#endif
    }
    else
    {
        HWD.pfnSetTexture(texture);
    }
}

// If batching is enabled, this function collects the polygon data and the chosen texture
// for later use in HWR_RenderBatches. Otherwise the rendering backend is used to
// render the polygon immediately.
void HWR_ProcessPolygon(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags, int shader_target, boolean horizonSpecial)
{
    if (iNumPts < 3)
        return; // no triangles; do not advance the fan writer past its allocation
#ifdef PS2_PROFILE
	if (currently_batching && hwr_sprite_batch)
	{
		// PS2-HW-52: batched sprite polygons are drawn in texture order, not in depth order. That is the same picture for polygons that write the depth
		// buffer and are not blended (opaque sprites) and for the drop shadows (blended onto the floor without depth write: a shadow behind a sprite
		// fails the depth test against it, one in front of the floor patch it covers is drawn over the floor either way). The first polygon of
		// anything else (translucent or additive sprites, link draw, bounding boxes) draws what was collected first and goes the immediate way.
		// (Things are spawned with the blend mode AST_TRANSLUCENT, so a sprite at full alpha is PF_Translucent with alpha 255: the same as masked.)
		const FBITFIELD blending = PolyFlags & PF_Blending;
		const boolean opaque = (blending == PF_Masked || (blending == PF_Translucent && pSurf && pSurf->PolyColor.s.alpha == 0xFF)) && (PolyFlags & PF_Occlude)
			&& !(PolyFlags & (PF_Invisible | PF_NoDepthTest | PF_Corona | PF_Ripple | PF_WireFrame | PF_NoTexture | PF_Decal)) && !horizonSpecial;

		if (!opaque && !hwr_sprite_shadow)
		{
			HWP_SPAN_BEGIN(tflush);
			HWR_RenderBatches();
			HWP_SPAN_END(tflush, HWP_SP_FLUSH);
			hwr_sprite_batch = false;
			if (!(PolyFlags & PF_NoTexture) && current_texture)
				HWD.pfnSetTexture(current_texture); // what the polygon selected while batching only noted it
		}
	}
	HWC_ADD(HWC_PROC); // OPT10 HG: calls (HWC_PROC_BATCH: the batched ones)
	if (currently_batching)
		HWC_ADD(HWC_PROC_BATCH);
#endif
    if (currently_batching)
	{
		if (!pSurf)
			I_Error("Got a null FSurfaceInfo in batching");// nulls should not come in the stuff that batching currently applies to
		if (iNumPts > (FUINT)(INT_MAX - unsortedVertexArraySize) || polygonArraySize == INT_MAX)
			I_Error("Hardware batch geometry exceeds addressable storage");
		if (polygonArraySize == polygonArrayAllocSize)
		{
			polygonArrayAllocSize = HWR_BatchCapacity(polygonArrayAllocSize, polygonArraySize + 1, sizeof(PolygonArrayEntry));
			polygonArray = HWR_BatchResize(polygonArray, (size_t)polygonArrayAllocSize * sizeof(PolygonArrayEntry));
			polygonIndexArray = HWR_BatchResize(polygonIndexArray, (size_t)polygonArrayAllocSize * sizeof(UINT32));
		}

		if (unsortedVertexArraySize + (int)iNumPts > unsortedVertexArrayAllocSize)
		{
			unsortedVertexArrayAllocSize = HWR_BatchCapacity(unsortedVertexArrayAllocSize,
				unsortedVertexArraySize + (int)iNumPts, sizeof(FOutVector));
			unsortedVertexArray = HWR_BatchResize(unsortedVertexArray, (size_t)unsortedVertexArrayAllocSize * sizeof(FOutVector));
		}

		// add the polygon data to the arrays

		polygonArray[polygonArraySize].surf = *pSurf;
		polygonArray[polygonArraySize].vertsIndex = unsortedVertexArraySize;
		polygonArray[polygonArraySize].numVerts = iNumPts;
		polygonArray[polygonArraySize].polyFlags = PolyFlags;
		polygonArray[polygonArraySize].texture = current_texture;
		polygonArray[polygonArraySize].shader = (shader_target != SHADER_NONE) ? HWR_GetShaderFromTarget(shader_target) : shader_target;
		polygonArray[polygonArraySize].horizonSpecial = horizonSpecial;
		// default to polygonArraySize so we don't lose order on horizon lines
		// (yes, it's supposed to be negative, since we're sorting in that direction)
		polygonArray[polygonArraySize].hash = -polygonArraySize;
		polygonArraySize++;

		if (!(PolyFlags & PF_NoTexture) && !horizonSpecial)
		{
			// use FNV-1a to hash polygons for later sorting.
			UINT32 hash = 0x811c9dc5u; // FNV multiplication intentionally wraps modulo 2^32
#define DIGEST(h, x) h ^= (x); h *= 0x01000193
			if (current_texture)
			{
#ifdef PS2 // PS2-HW-22: the texture's identity, not its GS handle: nothing is uploaded while polygons are collected, so every texture that is not resident has handle 0
				DIGEST(hash, (UINT32)(uintptr_t)current_texture);
#else
				DIGEST(hash, current_texture->downloaded);
#endif
			}
			DIGEST(hash, PolyFlags);
			DIGEST(hash, pSurf->PolyColor.rgba);
			if (cv_glshaders.value && gl_shadersavailable)
			{
				DIGEST(hash, shader_target);
				DIGEST(hash, pSurf->TintColor.rgba);
				DIGEST(hash, pSurf->FadeColor.rgba);
				DIGEST(hash, pSurf->LightInfo.light_level);
				DIGEST(hash, pSurf->LightInfo.fade_start);
				DIGEST(hash, pSurf->LightInfo.fade_end);
			}
#undef DIGEST
			// remove the sign bit to ensure that skybox and horizon line comes first.
#ifdef PS2 // PS2-HW-31: the texture first (14 bits), then the state hash (16 bits)
			polygonArray[polygonArraySize-1].hash = (INT32)((HWR_PS2_TextureOrder(current_texture) << 16) | ((hash ^ (hash >> 16)) & 0xFFFFu));
#ifdef PS2_PROFILE
			if (hwr_sprite_batch && !hwr_sprite_shadow)
				polygonArray[polygonArraySize-1].hash |= 0x40000000; // PS2-HW-52: the shadows are drawn first, as each is drawn before its sprite
#endif
#else
			polygonArray[polygonArraySize-1].hash = (hash & INT32_MAX);
#endif
		}

		memcpy(&unsortedVertexArray[unsortedVertexArraySize], pOutVerts, iNumPts * sizeof(FOutVector));
		unsortedVertexArraySize += iNumPts;
	}
	else
	{
		HWD.pfnSetShader((shader_target != SHADER_NONE) ? HWR_GetShaderFromTarget(shader_target) : shader_target);
		HWD.pfnDrawPolygon(pSurf, pOutVerts, iNumPts, PolyFlags);
	}
}

#ifndef PS2
static int comparePolygons(const void *p1, const void *p2)
{
	unsigned int index1 = *(const unsigned int*)p1;
	unsigned int index2 = *(const unsigned int*)p2;
	PolygonArrayEntry* poly1 = &polygonArray[index1];
	PolygonArrayEntry* poly2 = &polygonArray[index2];
	return (poly1->hash > poly2->hash) - (poly1->hash < poly2->hash);
}
#endif

// This function organizes the geometry collected by HWR_ProcessPolygon calls into batches and uses
// the rendering backend to draw them.
void HWR_RenderBatches(void)
{
    int finalVertexWritePos = 0;// position in finalVertexArray
	int finalIndexWritePos = 0;// position in finalVertexIndexArray

	int polygonReadPos = 0;// position in polygonIndexArray

	int currentShader;
	int nextShader = 0;
	GLMipmap_t *currentTexture;
	GLMipmap_t *nextTexture = NULL;
	FBITFIELD currentPolyFlags = 0;
	FBITFIELD nextPolyFlags = 0;
	FSurfaceInfo currentSurfaceInfo;
	FSurfaceInfo nextSurfaceInfo;

	int i;
	boolean sorted = true;

    if (!currently_batching)
		I_Error("HWR_RenderBatches called without starting batching");

	nextSurfaceInfo.LightInfo.fade_end = 0;
	nextSurfaceInfo.LightInfo.fade_start = 0;
	nextSurfaceInfo.LightInfo.light_level = 0;

	currently_batching = false;// no longer collecting batches
#ifdef PS2_PROFILE
	HWP_SPAN_BEGIN(tb_init);
#endif
#ifdef PS2
	PS2HWD_BatchDraw(); // the textures are made resident now, as each batch is drawn
#endif
	if (!polygonArraySize)
	{
		ps_hw_numpolys.value.i = ps_hw_numcalls.value.i = ps_hw_numshaders.value.i
			= ps_hw_numtextures.value.i = ps_hw_numpolyflags.value.i
			= ps_hw_numcolors.value.i = 0;
#ifdef PS2
		PS2HWD_BatchEnd();
#endif
		return;// nothing to draw
	}
	// init stats vars
	ps_hw_numpolys.value.i = polygonArraySize;
	ps_hw_numcalls.value.i = ps_hw_numverts.value.i = 0;
	ps_hw_numshaders.value.i = ps_hw_numtextures.value.i
		= ps_hw_numpolyflags.value.i = ps_hw_numcolors.value.i = 1;
	// init polygonIndexArray
	for (i = 0; i < polygonArraySize; i++)
	{
		polygonIndexArray[i] = i;
		// Keep qsort's existing equal-key order: only bypass it for strictly ordered keys.
		if (i && polygonArray[i-1].hash >= polygonArray[i].hash)
			sorted = false;
	}

#ifdef PS2_PROFILE
	HWP_SPAN_END2(tb_init, HWP_B_INIT, HWP_KB_INIT);
#endif
	// sort polygons
#ifdef PS2_PROFILE
	HWP_SPAN_BEGIN(tb_sort);
#endif
	PS_START_TIMING(ps_hw_batchsorttime);
#ifdef PS2 // PS2-HW-19: stable radix sort of the polygon keys (qsort of ~2000 polygons costs about 1.3 M cycles)
	if (!sorted)
	{
		static UINT32 *rkeys, *rtmpk, *rtmpi;
		static int rcap;

		if (rcap < polygonArrayAllocSize)
		{
			rkeys = HWR_BatchResize(rkeys, (size_t)polygonArrayAllocSize * sizeof(UINT32));
			rtmpk = HWR_BatchResize(rtmpk, (size_t)polygonArrayAllocSize * sizeof(UINT32));
			rtmpi = HWR_BatchResize(rtmpi, (size_t)polygonArrayAllocSize * sizeof(UINT32));
			rcap = polygonArrayAllocSize;
		}
		for (i = 0; i < polygonArraySize; i++)
			rkeys[i] = (UINT32)polygonArray[i].hash ^ 0x80000000u; // the signed order of comparePolygons
		// PS2-HW-59: the batches of a frame are ~250 distinct keys among thousands of polygons: HWR_GroupSort32 (-hwdbg 8192 = the radix sort only)
		if ((ps2hwd_dbg_flags & 8192) || HWR_GroupSort32(rkeys, polygonIndexArray, rtmpk, rtmpi, (UINT32)polygonArraySize) < 0)
		{
			if (!HWR_RadixSort32(rkeys, polygonIndexArray, rtmpk, rtmpi, (UINT32)polygonArraySize))
				memcpy(polygonIndexArray, rtmpi, (size_t)polygonArraySize * sizeof(UINT32));
		}
		else
			memcpy(polygonIndexArray, rtmpi, (size_t)polygonArraySize * sizeof(UINT32));
	}
#else
	if (!sorted)
		qsort(polygonIndexArray, polygonArraySize, sizeof(unsigned int), comparePolygons);
#endif
	PS_STOP_TIMING(ps_hw_batchsorttime);
#ifdef PS2_PROFILE
	HWP_SPAN_END2(tb_sort, HWP_BATCHSORT, HWP_KB_SORT);
#endif
	// sort order
	// 1. shader
	// 2. texture
	// 3. polyflags
	// 4. colors + light level
	// not sure about what order of the last 2 should be, or if it even matters

#ifdef PS2 // PS2-HW-34: the driver plans the frame's textures (mip level, visibility) before the first batch is drawn
#ifdef PS2_PROFILE
	HWP_SPAN_BEGIN(tb_plan);
#endif
	PS2HWD_PlanBegin();
	for (i = 0; i < polygonArraySize; i++)
	{
		const PolygonArrayEntry *pa = &polygonArray[i];

		if (pa->texture && !(pa->polyFlags & PF_NoTexture))
			PS2HWD_PlanPolygon(pa->texture, &unsortedVertexArray[pa->vertsIndex], pa->numVerts);
	}
	PS2HWD_PlanEnd();
#ifdef PS2_PROFILE
	HWP_SPAN_END2(tb_plan, HWP_B_PLAN, HWP_KB_PLAN);
#endif
#endif

#ifdef PS2_PROFILE
	HWP_SPAN_BEGIN(tb_draw);
#endif
	PS_START_TIMING(ps_hw_batchdrawtime);

	currentShader = polygonArray[polygonIndexArray[0]].shader;
	currentTexture = polygonArray[polygonIndexArray[0]].texture;
	currentPolyFlags = polygonArray[polygonIndexArray[0]].polyFlags;
	currentSurfaceInfo = polygonArray[polygonIndexArray[0]].surf;
	// For now, will sort and track the colors. Vertex attributes could be used instead of uniforms
	// and a color array could replace the color calls.

	// set state for first batch

	if (cv_glshaders.value && gl_shadersavailable)
	{
		HWD.pfnSetShader(currentShader);
	}

	if (currentPolyFlags & PF_NoTexture)
		currentTexture = NULL;
    else
	    HWD.pfnSetTexture(currentTexture);

	while (1)// note: remember handling notexture polyflag as having texture number 0 (also in comparePolygons)
	{
		int firstIndex;
		int lastIndex;
		int fanIndex;

		boolean stopFlag = false;
		boolean changeState = false;
		boolean changeShader = false;
		boolean changeTexture = false;
		boolean changePolyFlags = false;
		boolean changeSurfaceInfo = false;

		// steps:
		// write vertices
		// check for changes or end, otherwise go back to writing
			// changes will affect the next vars and the change bools
			// end could set flag for stopping
		// execute draw call
		// could check ending flag here
		// change states according to next vars and change bools, updating the current vars and reseting the bools
		// reset write pos
		// repeat loop

		int index = polygonIndexArray[polygonReadPos++];
		int numVerts = polygonArray[index].numVerts;
		// before writing, check if there is enough room
		// using 'while' instead of 'if' here makes sure that there will *always* be enough room.
		// probably never will this loop run more than once though
#ifdef PS2_PROFILE
		// Bounded output scratch: draw a full chunk before copying the next fan, with state/order intact.
		if (numVerts > finalVertexArrayAllocSize - finalVertexWritePos && finalIndexWritePos)
		{
			HWR_DrawBatch(&currentSurfaceInfo, finalIndexWritePos, currentPolyFlags);
			ps_hw_numcalls.value.i++;
			ps_hw_numverts.value.i += finalIndexWritePos;
			finalVertexWritePos = finalIndexWritePos = 0;
		}
#endif
		if (numVerts > finalVertexArrayAllocSize - finalVertexWritePos)
		{
			if (numVerts > INT_MAX / 3 - finalVertexWritePos)
				I_Error("Hardware batch index count overflow");
			finalVertexArrayAllocSize = HWR_BatchCapacity(finalVertexArrayAllocSize,
				finalVertexWritePos + numVerts, sizeof(FOutVector) + 3 * sizeof(UINT32));
			if (finalVertexArrayAllocSize > INT_MAX / 3)
				finalVertexArrayAllocSize = INT_MAX / 3;
#ifndef PS2
			finalVertexArray = HWR_BatchResize(finalVertexArray, (size_t)finalVertexArrayAllocSize * sizeof(FOutVector));
#endif
			finalVertexIndexArray = HWR_BatchResize(finalVertexIndexArray, (size_t)finalVertexArrayAllocSize * 3 * sizeof(UINT32));
		}
		// write the vertices of the polygon
#ifndef PS2
		memcpy(&finalVertexArray[finalVertexWritePos], &unsortedVertexArray[polygonArray[index].vertsIndex],
			numVerts * sizeof(FOutVector));
#endif
		// write the indexes, pointing to the fan vertexes but in triangles format
#ifdef PS2
		firstIndex = polygonArray[index].vertsIndex;
#else
		firstIndex = finalVertexWritePos;
#endif
		lastIndex = firstIndex + numVerts;
		fanIndex = firstIndex + 2;
		finalVertexWritePos += numVerts;
#ifdef PS2 // PS2-HW-19: the driver draws the polygons as GS triangle fans: (first vertex, count) pairs instead of triangle indices
		finalVertexIndexArray[finalIndexWritePos++] = firstIndex;
		finalVertexIndexArray[finalIndexWritePos++] = numVerts;
		(void)lastIndex; (void)fanIndex;
#else
		while (fanIndex < lastIndex)
		{
			finalVertexIndexArray[finalIndexWritePos++] = firstIndex;
			finalVertexIndexArray[finalIndexWritePos++] = fanIndex - 1;
			finalVertexIndexArray[finalIndexWritePos++] = fanIndex++;
		}
#endif

		if (polygonReadPos >= polygonArraySize)
		{
			stopFlag = true;
		}
		else
		{
			// check if a state change is required, set the change bools and next vars
			int nextIndex = polygonIndexArray[polygonReadPos];
#ifdef PS2 // PS2-HW-22/31: equal keys of different states must still change the state: the key has 30 bits of which the state hash has 16
			if (polygonArray[index].hash != polygonArray[nextIndex].hash || polygonArray[index].texture != polygonArray[nextIndex].texture
				|| polygonArray[index].polyFlags != polygonArray[nextIndex].polyFlags
				|| polygonArray[index].surf.PolyColor.rgba != polygonArray[nextIndex].surf.PolyColor.rgba)
#else
			if (polygonArray[index].hash != polygonArray[nextIndex].hash)
#endif
			{
				nextShader = polygonArray[nextIndex].shader;
				nextTexture = polygonArray[nextIndex].texture;
				nextPolyFlags = polygonArray[nextIndex].polyFlags;
				nextSurfaceInfo = polygonArray[nextIndex].surf;
				if (nextPolyFlags & PF_NoTexture)
					nextTexture = 0;
				if (currentShader != nextShader && cv_glshaders.value && gl_shadersavailable)
				{
					changeState = true;
					changeShader = true;
				}
				if (currentTexture != nextTexture)
				{
					changeState = true;
					changeTexture = true;
				}
				if (currentPolyFlags != nextPolyFlags)
				{
					changeState = true;
					changePolyFlags = true;
				}
				if (cv_glshaders.value && gl_shadersavailable)
				{
					if (currentSurfaceInfo.PolyColor.rgba != nextSurfaceInfo.PolyColor.rgba ||
						currentSurfaceInfo.TintColor.rgba != nextSurfaceInfo.TintColor.rgba ||
						currentSurfaceInfo.FadeColor.rgba != nextSurfaceInfo.FadeColor.rgba ||
						currentSurfaceInfo.LightInfo.light_level != nextSurfaceInfo.LightInfo.light_level ||
						currentSurfaceInfo.LightInfo.fade_start != nextSurfaceInfo.LightInfo.fade_start ||
						currentSurfaceInfo.LightInfo.fade_end != nextSurfaceInfo.LightInfo.fade_end)
					{
						changeState = true;
						changeSurfaceInfo = true;
					}
				}
				else
				{
					if (currentSurfaceInfo.PolyColor.rgba != nextSurfaceInfo.PolyColor.rgba)
					{
						changeState = true;
						changeSurfaceInfo = true;
					}
				}
			}
		}

		if (changeState || stopFlag)
		{
			// execute draw call
            HWR_DrawBatch(&currentSurfaceInfo, finalIndexWritePos, currentPolyFlags);
			// update stats
			ps_hw_numcalls.value.i++;
			ps_hw_numverts.value.i += finalIndexWritePos;
			// reset write positions
			finalVertexWritePos = 0;
			finalIndexWritePos = 0;
		}
		else continue;

		// if we're here then either its time to stop or time to change state
		if (stopFlag) break;

		// change state according to change bools and next vars, update current vars and reset bools
		if (changeState)
		{
			if (changeShader)
			{
				HWD.pfnSetShader(nextShader);
				currentShader = nextShader;
				changeShader = false;

				ps_hw_numshaders.value.i++;
			}
			if (changeTexture)
			{
				// texture should be already ready for use from calls to SetTexture during batch collection
			    HWD.pfnSetTexture(nextTexture);
				currentTexture = nextTexture;
				changeTexture = false;

				ps_hw_numtextures.value.i++;
			}
			if (changePolyFlags)
			{
				currentPolyFlags = nextPolyFlags;
				changePolyFlags = false;

				ps_hw_numpolyflags.value.i++;
			}
			if (changeSurfaceInfo)
			{
				currentSurfaceInfo = nextSurfaceInfo;
				changeSurfaceInfo = false;

				ps_hw_numcolors.value.i++;
			}
		}
		// and that should be it?
	}
	// reset the arrays (set sizes to 0)
	polygonArraySize = 0;
	unsortedVertexArraySize = 0;
#ifdef PS2_PROFILE
	HWP_SPAN_END2(tb_draw, HWP_BATCHDRAW, HWP_KB_DRAW);
	{
		HWP_SPAN_BEGIN(tb_end);
#endif
#ifdef PS2
	PS2HWD_BatchEnd();
#endif
#ifdef PS2_PROFILE
		HWP_SPAN_END2(tb_end, HWP_B_END, HWP_KB_END);
	}
#endif

	PS_STOP_TIMING(ps_hw_batchdrawtime);
}


#endif // HWRENDER
