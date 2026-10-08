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
#include "../m_argv.h" // -hwpolyhash
extern int ps2hwd_dbg_flags; // the driver's -hwdbg bits (ps2/hw/ps2_hwd.c)
#else
#include "../ps2/hw/ps2_hw_prof.h" // no-op profiling macros for the PC build
#endif

// The texture for the next polygon given to HWR_ProcessPolygon.
// Set with HWR_SetCurrentTexture.
GLMipmap_t *current_texture = NULL;

boolean currently_batching = false;
#ifdef PS2_PROFILE
boolean hwr_grec_on = false; // OPT11: the geometry cache is recording (hw_gcache.inc)
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
// OPT11 (PS2-HW-85): what HWR_ProcessPolygon asked of the driver and the shader table for every polygon, once per batch (a frame's parity and the table of shader
// targets do not change while polygons are collected). -hwgo 32: asked for every polygon, as before.
static UINT32 hwr_scan_dir;
static int hwr_shader_of[NUMSHADERTARGETS];
static UINT16 hwr_shader_have;

static inline int HWR_ShaderOfTarget(int shader_target)
{
	if (shader_target == SHADER_NONE)
		return shader_target;
	if (hwr_geo_off & 32)
		return HWR_GetShaderFromTarget(shader_target);
	if (!(hwr_shader_have & (1u << shader_target)))
	{
		hwr_shader_of[shader_target] = HWR_GetShaderFromTarget(shader_target);
		hwr_shader_have |= (UINT16)(1u << shader_target);
	}
	return hwr_shader_of[shader_target];
}

#define HWR_PS2_NOTEX_ID 0xFFFFFFFFu
static UINT32 HWR_PS2_TextureId(const GLMipmap_t *t) // OPT11: the part of the order that does not depend on the frame (the geometry cache keeps it)
{
	if (!t)
		return HWR_PS2_NOTEX_ID;
	if (t->regen_kind == 1)
		return (UINT32)t->regen_id & 0x1FFFu;
	if (t->regen_kind == 2)
		return 0x2000u | ((UINT32)t->regen_id & 0x1FFFu);
	return (((UINT32)(uintptr_t)t >> 4) * 2654435761u) >> 18; // patches: any fixed order
}

static inline UINT32 HWR_PS2_OrderOf(UINT32 id, UINT32 scan_dir)
{
	if (id == HWR_PS2_NOTEX_ID)
		return 0x3FFEu;
	return scan_dir ? 0x3FFFu - id : id;
}

static UINT32 HWR_PS2_TextureOrder(const GLMipmap_t *t)
{
	return HWR_PS2_OrderOf(HWR_PS2_TextureId(t), (hwr_geo_off & 32) ? PS2HWD_ScanDirection() : hwr_scan_dir); // the frame's parity, not the batching passes' (a skybox view batches twice per frame)
}
#endif

#ifdef PS2
static UINT32 *rkeys, *rtmpk, *rtmpi; // the scratch of the radix sort of HWR_RenderBatches
static int rcap;
static UINT16 *bgrp; // PS2-HW-226: the bucket of every polygon (HWR_RenderBatchesV2)
static int bgcap;

// OPT11 round 2 (PS2-HW-224): the vertices of a polygon (3..16 of 20 bytes) into the batch's pool. newlib's memcpy takes the byte loop for a source or destination that is
// not 8 byte aligned (a pool of 20 byte vertices is aligned to 4 only for every second vertex): 70..150 cycles for the 80 bytes of a quad, 150 K cycles a frame in
// DEMO_001. Five words a vertex, no call. (The empty asm keeps the compiler from turning the loop back into a call of memcpy.)
static inline void HWR_CopyVerts(FOutVector *dst, const FOutVector *src, FUINT n)
{
	FUINT k;

	for (k = 0; k < n; k++)
	{
		const float x = src[k].x, y = src[k].y, z = src[k].z, s = src[k].s, t = src[k].t;

		dst[k].x = x;
		dst[k].y = y;
		dst[k].z = z;
		dst[k].s = s;
		dst[k].t = t;
		__asm__ volatile("" ::: "memory");
	}
}
#endif

#ifdef PS2_PROFILE
// OPT11 (GEOM): -hwpolyhash. The polygons that the engine hands to HWR_ProcessPolygon (batched or not, in the order of the calls: their number, flags,
// shader, the texture's identity, the surface and every vertex) are folded into one hash per frame, printed at the frame's end (HWPH lines). It does not
// depend on the GS, the plan, VRAM or timing: two builds whose engine side makes the same polygons print the same lines, the test of every change of
// the walls, planes, sprites and the batcher that is meant not to change a picture (tools/ps2/gm_polycmp.py compares two logs).
static int hwr_ph_on = -1;
static UINT32 hwr_ph_a, hwr_ph_b, hwr_ph_n;
static UINT32 hwr_ph_calls, hwr_ph_lo, hwr_ph_hi; // -hwpolyhash 2 LO HI: every polygon of the frames LO..HI is printed with its parts (HWPP lines), to find what differs between two runs

static inline void HWR_PH_W(UINT32 x)
{
	hwr_ph_a = (hwr_ph_a ^ x) * 16777619u;
	hwr_ph_b = (hwr_ph_b + x) * 0x85ebca6bu;
	hwr_ph_b ^= hwr_ph_b >> 15;
}

static void HWR_PolyHashAdd(const FSurfaceInfo *s, const FOutVector *v, FUINT n, FBITFIELD flags, int shader, boolean horizon)
{
	const UINT32 *w = (const UINT32 *)v;
	UINT32 i;

	if (hwr_ph_on == 2 && hwr_ph_calls >= hwr_ph_lo && hwr_ph_calls <= hwr_ph_hi)
	{
		UINT32 vh = 0x811c9dc5u, k;
		const UINT32 *vw = (const UINT32 *)v;

		for (k = 0; k < n * 5; k++)
			vh = (vh ^ vw[k]) * 16777619u;
		I_OutputMsg("HWPP c=%u i=%u n=%u fl=%x sh=%d tex=%d/%d/%ux%u/%x col=%x/%x/%x lt=%d/%d/%d/%d v=%08x\n", hwr_ph_calls, hwr_ph_n, (unsigned)n, (unsigned)flags, shader,
			(current_texture && !(flags & PF_NoTexture)) ? (int)current_texture->regen_kind : -1, (current_texture && !(flags & PF_NoTexture)) ? (int)current_texture->regen_id : -1,
			(current_texture && !(flags & PF_NoTexture)) ? (unsigned)current_texture->width : 0u, (current_texture && !(flags & PF_NoTexture)) ? (unsigned)current_texture->height : 0u,
			(current_texture && !(flags & PF_NoTexture)) ? (unsigned)(current_texture->flags & 0xFFFFu) : 0u,
			s ? (unsigned)s->PolyColor.rgba : 0u, s ? (unsigned)s->TintColor.rgba : 0u, s ? (unsigned)s->FadeColor.rgba : 0u,
			s ? (int)s->LightTableId : 0, s ? (int)s->LightInfo.light_level : 0, s ? (int)s->LightInfo.fade_start : 0, s ? (int)s->LightInfo.fade_end : 0, vh);
	}
	hwr_ph_n++;
	HWR_PH_W(n | ((UINT32)horizon << 16));
	HWR_PH_W(flags);
	HWR_PH_W((UINT32)shader);
	if (current_texture && !(flags & PF_NoTexture))
	{
		HWR_PH_W((UINT32)current_texture->regen_kind);
		HWR_PH_W((UINT32)current_texture->regen_id);
		HWR_PH_W(((UINT32)current_texture->width << 16) | current_texture->height);
		HWR_PH_W(current_texture->flags & 0xFFFFu);
	}
	else
		HWR_PH_W(0xFFFFFFFFu);
	if (s)
	{
		HWR_PH_W(s->PolyColor.rgba);
		if (flags & PF_ColorMapped) // (what the driver reads of a surface that is not lit by the shader is the colour only; the rest is stack garbage in some callers: found by -hwpolyhash 2 on DEMO_002)
		{
			HWR_PH_W(s->TintColor.rgba);
			HWR_PH_W(s->FadeColor.rgba);
			HWR_PH_W(s->LightTableId);
			HWR_PH_W(s->LightInfo.light_level);
			HWR_PH_W(s->LightInfo.fade_start);
			HWR_PH_W(s->LightInfo.fade_end);
		}
	}
	for (i = 0; i < n * 5; i++)
		HWR_PH_W(w[i]);
}

void HWR_PolyHashFrame(INT32 frame) // called at the end of every frame (ps2/i_video.c)
{
	if (hwr_ph_on < 0)
	{
		hwr_ph_on = M_CheckParm("-hwpolyhash") ? 1 : 0;
		if (hwr_ph_on && M_IsNextParm() && atoi(M_GetNextParm()) == 2)
		{
			hwr_ph_on = 2;
			hwr_ph_lo = M_IsNextParm() ? (UINT32)atoi(M_GetNextParm()) : 0;
			hwr_ph_hi = M_IsNextParm() ? (UINT32)atoi(M_GetNextParm()) : 0;
		}
	}
	if (!hwr_ph_on)
		return;
	hwr_ph_calls++;
	I_OutputMsg("HWPH f=%d n=%u h=%08x%08x\n", (int)frame, hwr_ph_n, hwr_ph_a, hwr_ph_b);
	hwr_ph_a = 0x811c9dc5u;
	hwr_ph_b = 0x9e3779b9u;
	hwr_ph_n = 0;
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

#ifdef PS2_PROFILE
// OPT10-HF (PS2-HW-79): the batch arrays grow to the busiest frame seen and are non-purgeable, so after the vertex-heavy maps (THZ2, ACZ1: 32 000
// vertices a frame) they stay 1.45 MB big (0.42 MB on the light maps) for the rest of the session: the next big level (12 MB of PU_LEVEL) then did not fit.
// Called on every level change (HWR_ClearAllTextures): the arrays are made again, at the start size, by the first frame of the next level.
void HWR_ReleaseBatching(void)
{
	if (currently_batching)
		return;
	Z_Free(finalVertexIndexArray);
	Z_Free(polygonArray);
	Z_Free(polygonIndexArray);
	Z_Free(unsortedVertexArray);
	Z_Free(rkeys);
	Z_Free(rtmpk);
	Z_Free(rtmpi);
	Z_Free(bgrp);
	bgrp = NULL;
	bgcap = 0;
	finalVertexIndexArray = NULL;
	polygonArray = NULL;
	polygonIndexArray = NULL;
	unsortedVertexArray = NULL;
	rkeys = rtmpk = rtmpi = NULL;
	rcap = 0;
	polygonArraySize = 0;
	unsortedVertexArraySize = 0;
	finalVertexArrayAllocSize = 8192;
	polygonArrayAllocSize = 2048;
	unsortedVertexArrayAllocSize = 8192;
}
#endif

// One draw call of the batch being built: triangle indices, or on the PS2 (first vertex, count) pairs of fans (see HWR_RenderBatches)
static void HWR_DrawBatch(FSurfaceInfo *surf, const UINT32 *indices, int count, FBITFIELD polyFlags)
{
#ifdef PS2
	HWP_SPAN_BEGIN(tdb);
	PS2HWD_DrawFans(surf, HWR_BATCH_VERTICES, (unsigned int)count / 3, polyFlags, indices); // PS2-HW-106: (first vertex, count, light level) per polygon
	HWP_SPAN_END2(tdb, HWP_B_DB, HWP_KB_DB);
#else
	HWD.pfnDrawIndexedTriangles(surf, HWR_BATCH_VERTICES, count, polyFlags, (UINT32 *)indices);
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
#ifdef PS2_PROFILE
	hwr_scan_dir = PS2HWD_ScanDirection();
	hwr_shader_have = 0;
#endif
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
#ifdef PS2_PROFILE
		if (hwr_grec_on)
			HWR_GCRecTex(texture);
#endif
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
	if (hwr_ph_on > 0)
		HWR_PolyHashAdd(pSurf, pOutVerts, iNumPts, PolyFlags, shader_target, horizonSpecial);
	if (hwr_grec_on)
		HWR_GCRecPoly(pSurf, pOutVerts, iNumPts, PolyFlags, shader_target, horizonSpecial); // OPT11: the geometry cache records what the BSP walk hands to the batch
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
#ifdef PS2_PROFILE
		polygonArray[polygonArraySize].shader = HWR_ShaderOfTarget(shader_target);
#else
		polygonArray[polygonArraySize].shader = (shader_target != SHADER_NONE) ? HWR_GetShaderFromTarget(shader_target) : shader_target;
#endif
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
#ifdef PS2 // PS2-HW-106: polygons the VU1 program lights by palette rows carry their own light level: the sectors' levels do not split the batches (248 -> 93 batches on a GFZ1 frame)
				if (!PS2HWD_PalLit(pSurf, PolyFlags, current_texture, shader_target))
				{
					DIGEST(hash, pSurf->LightInfo.light_level);
				}
#else
				DIGEST(hash, pSurf->LightInfo.light_level);
#endif
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

#ifdef PS2
		HWR_CopyVerts(&unsortedVertexArray[unsortedVertexArraySize], pOutVerts, iNumPts);
#else
		memcpy(&unsortedVertexArray[unsortedVertexArraySize], pOutVerts, iNumPts * sizeof(FOutVector));
#endif
		unsortedVertexArraySize += iNumPts;
	}
	else
	{
		HWD.pfnSetShader((shader_target != SHADER_NONE) ? HWR_GetShaderFromTarget(shader_target) : shader_target);
		HWD.pfnDrawPolygon(pSurf, pOutVerts, iNumPts, PolyFlags);
	}
}

#ifdef PS2_PROFILE
// OPT11 (PS2-HW-80): the state hash of a polygon of the batch: the same digest as HWR_ProcessPolygon makes, for the geometry cache (it keeps the result with the polygon)
// KEEP IN STEP with the DIGEST list of HWR_ProcessPolygon (a change there, e.g. the PalLit condition of the light level, must be made here as well; -hwgc 2 finds a difference)
UINT32 HWR_GCPolyHash(const GLMipmap_t *tex, const FSurfaceInfo *pSurf, FBITFIELD PolyFlags, int shader_target)
{
	UINT32 hash = 0x811c9dc5u;

#define DIGEST(h, x) h ^= (x); h *= 0x01000193
	if (tex)
	{
		DIGEST(hash, (UINT32)(uintptr_t)tex);
	}
	DIGEST(hash, PolyFlags);
	DIGEST(hash, pSurf->PolyColor.rgba);
	if (cv_glshaders.value && gl_shadersavailable)
	{
		DIGEST(hash, shader_target);
		DIGEST(hash, pSurf->TintColor.rgba);
		DIGEST(hash, pSurf->FadeColor.rgba);
		if (!PS2HWD_PalLit(pSurf, PolyFlags, tex, shader_target)) // PS2-HW-106: as in HWR_ProcessPolygon
		{
			DIGEST(hash, pSurf->LightInfo.light_level);
		}
		DIGEST(hash, pSurf->LightInfo.fade_start);
		DIGEST(hash, pSurf->LightInfo.fade_end);
	}
#undef DIGEST
	return (hash ^ (hash >> 16)) & 0xFFFFu;
}

UINT32 HWR_GCTexId(const GLMipmap_t *tex)
{
	return HWR_PS2_TextureId(tex);
}

// HWR_ProcessPolygon of a polygon the geometry cache made before (batching, not sprites): what the digest and the sort key need is in the record
void HWR_GCReplayPoly(const FSurfaceInfo *pSurf, const FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags, int shader_target, boolean horizonSpecial, UINT32 texid, UINT32 h16, UINT32 scan_dir)
{
	PolygonArrayEntry *pe;

	HWC_ADD(HWC_PROC);
	HWC_ADD(HWC_PROC_BATCH);
	if (hwr_ph_on > 0)
		HWR_PolyHashAdd(pSurf, pOutVerts, iNumPts, PolyFlags, shader_target, horizonSpecial);
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
		unsortedVertexArrayAllocSize = HWR_BatchCapacity(unsortedVertexArrayAllocSize, unsortedVertexArraySize + (int)iNumPts, sizeof(FOutVector));
		unsortedVertexArray = HWR_BatchResize(unsortedVertexArray, (size_t)unsortedVertexArrayAllocSize * sizeof(FOutVector));
	}
	pe = &polygonArray[polygonArraySize];
	pe->surf = *pSurf;
	pe->vertsIndex = unsortedVertexArraySize;
	pe->numVerts = iNumPts;
	pe->polyFlags = PolyFlags;
	pe->texture = current_texture;
	pe->shader = HWR_ShaderOfTarget(shader_target);
	pe->horizonSpecial = horizonSpecial;
	if (!(PolyFlags & PF_NoTexture) && !horizonSpecial)
		pe->hash = (INT32)((HWR_PS2_OrderOf(texid, scan_dir) << 16) | h16);
	else
		pe->hash = -polygonArraySize; // (to stay in order on horizon lines)
	polygonArraySize++;
	HWR_CopyVerts(&unsortedVertexArray[unsortedVertexArraySize], pOutVerts, iNumPts);
	unsortedVertexArraySize += iNumPts;
}
#endif

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

#ifdef PS2
// PS2-HW-34: the driver plans the frame's textures (mip level, visibility) before the first batch is drawn
static void HWR_PlanPass(void)
{
	int i;
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
}

// -hwgo 128: the batches the walk of HWR_RenderBatchesOld would make (a dry run over the sorted order: no drawing), to be compared with those of HWR_RenderBatchesV2
#define HWR_BCMP_MAX 8192
static UINT32 bcmp_new[HWR_BCMP_MAX][3], bcmp_old[HWR_BCMP_MAX][3]; // first polygon, polygons, hash of the state the batch is drawn with
static int bcmp_nnew, bcmp_nold;

static UINT32 HWR_BCmpHash(const FSurfaceInfo *s, FBITFIELD flags, GLMipmap_t *tex)
{
	UINT32 h = 0x811c9dc5u;

	h = (h ^ s->PolyColor.rgba) * 16777619u;
	h = (h ^ s->TintColor.rgba) * 16777619u;
	h = (h ^ s->FadeColor.rgba) * 16777619u;
	h = (h ^ (UINT32)s->LightInfo.light_level) * 16777619u;
	h = (h ^ (UINT32)s->LightInfo.fade_start) * 16777619u;
	h = (h ^ (UINT32)s->LightInfo.fade_end) * 16777619u;
	h = (h ^ s->LightTableId) * 16777619u;
	h = (h ^ flags) * 16777619u;
	h = (h ^ (UINT32)(uintptr_t)tex) * 16777619u;
	return h;
}

static int HWR_BatchesOldDry(const UINT32 *ti, int n)
{
	int p, nb = 0, bs = 0, vwrite = 0;
	int cshader = polygonArray[ti[0]].shader;
	GLMipmap_t *ctex = polygonArray[ti[0]].texture;
	FBITFIELD cflags = polygonArray[ti[0]].polyFlags;
	FSurfaceInfo cs = polygonArray[ti[0]].surf;

	if (cflags & PF_NoTexture)
		ctex = NULL;
	for (p = 0; p < n; p++)
	{
		const PolygonArrayEntry *e = &polygonArray[ti[p]];
		int stop = 0, change = 0;

		if ((int)e->numVerts > finalVertexArrayAllocSize - vwrite && p > bs)
		{
			if (nb < HWR_BCMP_MAX)
			{
				bcmp_old[nb][0] = (UINT32)bs;
				bcmp_old[nb][1] = (UINT32)(p - bs);
				bcmp_old[nb][2] = HWR_BCmpHash(&cs, cflags, ctex);
			}
			nb++;
			bs = p;
			vwrite = 0;
		}
		vwrite += (int)e->numVerts;
		if (p + 1 >= n)
			stop = 1;
		else
		{
			const PolygonArrayEntry *ne = &polygonArray[ti[p + 1]];

			if (e->hash != ne->hash || e->texture != ne->texture || e->polyFlags != ne->polyFlags || e->surf.PolyColor.rgba != ne->surf.PolyColor.rgba)
			{
				int nshader = ne->shader;
				GLMipmap_t *ntex = ne->texture;
				FBITFIELD nflags = ne->polyFlags;
				int cshd = 0, ctx = 0, cfl = 0, csf = 0;

				if (nflags & PF_NoTexture)
					ntex = 0;
				if (cshader != nshader && cv_glshaders.value && gl_shadersavailable)
				{
					change = 1;
					cshd = 1;
				}
				if (ctex != ntex)
				{
					change = 1;
					ctx = 1;
				}
				if (cflags != nflags)
				{
					change = 1;
					cfl = 1;
				}
				if (cv_glshaders.value && gl_shadersavailable)
				{
					if (cs.PolyColor.rgba != ne->surf.PolyColor.rgba || cs.TintColor.rgba != ne->surf.TintColor.rgba || cs.FadeColor.rgba != ne->surf.FadeColor.rgba
						|| cs.LightInfo.light_level != ne->surf.LightInfo.light_level || cs.LightInfo.fade_start != ne->surf.LightInfo.fade_start
						|| cs.LightInfo.fade_end != ne->surf.LightInfo.fade_end)
					{
						change = 1;
						csf = 1;
					}
				}
				else if (cs.PolyColor.rgba != ne->surf.PolyColor.rgba)
				{
					change = 1;
					csf = 1;
				}
				if (change)
				{
					if (nb < HWR_BCMP_MAX)
					{
						bcmp_old[nb][0] = (UINT32)bs;
						bcmp_old[nb][1] = (UINT32)(p + 1 - bs);
						bcmp_old[nb][2] = HWR_BCmpHash(&cs, cflags, ctex);
					}
					nb++;
					bs = p + 1;
					vwrite = 0;
					if (cshd)
						cshader = nshader;
					if (ctx)
						ctex = ntex;
					if (cfl)
						cflags = nflags;
					if (csf)
						cs = ne->surf;
				}
			}
		}
		if (stop)
		{
			if (nb < HWR_BCMP_MAX)
			{
				bcmp_old[nb][0] = (UINT32)bs;
				bcmp_old[nb][1] = (UINT32)(p + 1 - bs);
				bcmp_old[nb][2] = HWR_BCmpHash(&cs, cflags, ctex);
			}
			nb++;
		}
	}
	return nb;
}

// OPT11 round 2 (PS2-HW-226): the batches of a frame without the sorted index array. HWR_RenderBatches used to give every polygon an index, sort the indices by key
// (HWR_GroupSort32: a table lookup per polygon, a scatter of the indices), and then walk the sorted polygons comparing each with its neighbour to find the batch
// boundaries, writing the (first vertex, count, light) triple of each into the index array of the driver: five passes over the polygons, about 150 cycles each. The
// polygons with one key (texture order and state hash) are a bucket; the buckets are sorted (a few hundred), and one pass puts the triple of every polygon where it
// belongs (the bucket's range, in the order the polygons came), checking on the way that the polygon is in the state of the first of its bucket (the key has 30 bits:
// two states can collide, and the walk below would have cut the batch there; then this function gives up and the old walk runs). The state changes are looked for between
// the first polygons of consecutive buckets with the tests of the old walk. The same batches in the same order (-hwgo 64: the old walk; -hwgo 128: both, the polygon order
// compared). Returns 0 when it did nothing.
static int HWR_RenderBatchesV2(void)
{
	unsigned int first[HWR_GS_MAX], start[HWR_GS_MAX], cnt[HWR_GS_MAX];
	unsigned short border[HWR_GS_MAX];
	const int n = polygonArraySize;
	UINT32 *desc;
	int m, i, bi, pos, bstart;
	int currentShader, nextShader;
	GLMipmap_t *currentTexture;
	FBITFIELD currentPolyFlags;
	FSurfaceInfo currentSurfaceInfo;
	const PolygonArrayEntry *e;

	if (n < 1 || n > INT_MAX / 3)
		return 0;
	if (bgcap < polygonArrayAllocSize)
	{
		bgrp = HWR_BatchResize(bgrp, (size_t)polygonArrayAllocSize * sizeof(UINT16));
		bgcap = polygonArrayAllocSize;
	}
	{
		HWP_SPAN_BEGIN(tb_sort);

		m = HWR_GroupPlan((const unsigned char *)&polygonArray[0].hash, (unsigned int)sizeof(PolygonArrayEntry), (unsigned int)n, bgrp, first, start, cnt, border);
		if (m < 0)
			return 0; // more than HWR_GS_MAX distinct keys: the old walk (radix sort)
		if (n > finalVertexArrayAllocSize)
		{
			finalVertexArrayAllocSize = HWR_BatchCapacity(finalVertexArrayAllocSize, n, sizeof(FOutVector) + 3 * sizeof(UINT32));
			if (finalVertexArrayAllocSize > INT_MAX / 3)
				finalVertexArrayAllocSize = INT_MAX / 3;
			finalVertexIndexArray = HWR_BatchResize(finalVertexIndexArray, (size_t)finalVertexArrayAllocSize * 3 * sizeof(UINT32));
		}
		desc = finalVertexIndexArray;
		for (i = 0; i < n; i++)
		{
			const unsigned int g = bgrp[i];
			const PolygonArrayEntry *p = &polygonArray[i], *f = &polygonArray[first[g]];
			UINT32 *d;

			if (p->texture != f->texture || p->polyFlags != f->polyFlags || p->surf.PolyColor.rgba != f->surf.PolyColor.rgba)
				return 0; // two states with one key: the old walk cuts the batch between them
			d = desc + 3 * start[g]++;
			d[0] = p->vertsIndex;
			d[1] = (UINT32)p->numVerts;
			d[2] = (UINT32)p->surf.LightInfo.light_level; // PS2-HW-106
		}
		HWP_SPAN_END2(tb_sort, HWP_BATCHSORT, HWP_KB_SORT);
	}
	if (hwr_geo_off & 128)
	{
		// compared with the sorted index array of the old walk: the same polygon at every position
		static UINT32 *ref_idx;
		static int ref_cap;
		unsigned int bad = 0;

		if (ref_cap < n)
		{
			ref_idx = HWR_BatchResize(ref_idx, (size_t)n * sizeof(UINT32));
			ref_cap = n;
		}
		{
			UINT32 *k = HWR_BatchResize(NULL, (size_t)n * 3 * sizeof(UINT32));
			UINT32 *ti = k + n, *tk = ti + n;
			int j;

			for (j = 0; j < n; j++)
			{
				ref_idx[j] = (UINT32)j;
				k[j] = (UINT32)polygonArray[j].hash ^ 0x80000000u;
			}
			if (HWR_GroupSort32(k, ref_idx, tk, ti, (UINT32)n) == 0)
			{
				for (j = 0; j < n; j++)
					if (polygonArray[ti[j]].vertsIndex != desc[3 * j])
						bad++;
				bcmp_nold = HWR_BatchesOldDry(ti, n);
			}
			else
				bcmp_nold = -1;
			Z_Free(k);
		}
		if (bad)
			I_OutputMsg("HWBATCH2 %u polygons differ in order of %d\n", bad, n);
		bcmp_nnew = 0;
	}

	currently_batching = false; // no longer collecting batches
	PS2HWD_BatchDraw(); // the textures are made resident now, as each batch is drawn
	ps_hw_numpolys.value.i = n;
	ps_hw_numcalls.value.i = ps_hw_numverts.value.i = 0;
	ps_hw_numshaders.value.i = ps_hw_numtextures.value.i = ps_hw_numpolyflags.value.i = ps_hw_numcolors.value.i = 1;
	HWR_PlanPass();
	{
		HWP_SPAN_BEGIN(tb_draw);
		PS_START_TIMING(ps_hw_batchdrawtime);

		e = &polygonArray[first[border[0]]];
		currentShader = e->shader;
		currentTexture = e->texture;
		currentPolyFlags = e->polyFlags;
		currentSurfaceInfo = e->surf;
		if (cv_glshaders.value && gl_shadersavailable)
			HWD.pfnSetShader(currentShader);
		if (currentPolyFlags & PF_NoTexture)
			currentTexture = NULL;
		else
		{
			HWP_SPAN_BEGIN(tst);
			HWD.pfnSetTexture(currentTexture);
			HWP_SPAN_END2(tst, HWP_B_TEX, HWP_KB_TEX);
		}
		bstart = 0;
		pos = (int)cnt[border[0]];
		for (bi = 1; bi < m; bi++)
		{
			const unsigned int g = border[bi];
			boolean changeState = false, changeShader = false, changeTexture = false, changePolyFlags = false, changeSurfaceInfo = false;
			GLMipmap_t *nextTexture;
			FBITFIELD nextPolyFlags;
			const FSurfaceInfo *ns;

			e = &polygonArray[first[g]];
			nextShader = e->shader;
			nextTexture = e->texture;
			nextPolyFlags = e->polyFlags;
			ns = &e->surf;
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
				if (currentSurfaceInfo.PolyColor.rgba != ns->PolyColor.rgba ||
					currentSurfaceInfo.TintColor.rgba != ns->TintColor.rgba ||
					currentSurfaceInfo.FadeColor.rgba != ns->FadeColor.rgba ||
					currentSurfaceInfo.LightInfo.light_level != ns->LightInfo.light_level ||
					currentSurfaceInfo.LightInfo.fade_start != ns->LightInfo.fade_start ||
					currentSurfaceInfo.LightInfo.fade_end != ns->LightInfo.fade_end)
				{
					changeState = true;
					changeSurfaceInfo = true;
				}
			}
			else if (currentSurfaceInfo.PolyColor.rgba != ns->PolyColor.rgba)
			{
				changeState = true;
				changeSurfaceInfo = true;
			}
			if (changeState)
			{
				if (hwr_geo_off & 128)
				{
					if (bcmp_nnew < HWR_BCMP_MAX)
					{
						bcmp_new[bcmp_nnew][0] = (UINT32)bstart;
						bcmp_new[bcmp_nnew][1] = (UINT32)(pos - bstart);
						bcmp_new[bcmp_nnew][2] = HWR_BCmpHash(&currentSurfaceInfo, currentPolyFlags, currentTexture);
					}
					bcmp_nnew++;
				}
				HWR_DrawBatch(&currentSurfaceInfo, desc + 3 * bstart, 3 * (pos - bstart), currentPolyFlags);
				ps_hw_numcalls.value.i++;
				ps_hw_numverts.value.i += 3 * (pos - bstart);
				bstart = pos;
				if (changeShader)
				{
					HWD.pfnSetShader(nextShader);
					currentShader = nextShader;
					ps_hw_numshaders.value.i++;
				}
				if (changeTexture)
				{
					HWP_SPAN_BEGIN(tst);
					HWD.pfnSetTexture(nextTexture);
					HWP_SPAN_END2(tst, HWP_B_TEX, HWP_KB_TEX);
					currentTexture = nextTexture;
					ps_hw_numtextures.value.i++;
				}
				if (changePolyFlags)
				{
					currentPolyFlags = nextPolyFlags;
					ps_hw_numpolyflags.value.i++;
				}
				if (changeSurfaceInfo)
				{
					currentSurfaceInfo = *ns;
					ps_hw_numcolors.value.i++;
				}
			}
			pos += (int)cnt[g];
		}
		if (hwr_geo_off & 128)
		{
			int j, diff = 0;

			if (bcmp_nnew < HWR_BCMP_MAX)
			{
				bcmp_new[bcmp_nnew][0] = (UINT32)bstart;
				bcmp_new[bcmp_nnew][1] = (UINT32)(pos - bstart);
				bcmp_new[bcmp_nnew][2] = HWR_BCmpHash(&currentSurfaceInfo, currentPolyFlags, currentTexture);
			}
			bcmp_nnew++;
			if (bcmp_nold != bcmp_nnew)
				diff = 1;
			else
				for (j = 0; j < bcmp_nnew && j < HWR_BCMP_MAX && !diff; j++)
					if (bcmp_new[j][0] != bcmp_old[j][0] || bcmp_new[j][1] != bcmp_old[j][1] || bcmp_new[j][2] != bcmp_old[j][2])
						diff = j + 1;
			if (diff)
				I_OutputMsg("HWBATCH2 batches differ: new %d old %d first difference at %d (new %u/%u/%08x old %u/%u/%08x)\n", bcmp_nnew, bcmp_nold, diff - 1,
					diff > 0 && diff <= HWR_BCMP_MAX ? bcmp_new[diff - 1][0] : 0u, diff > 0 && diff <= HWR_BCMP_MAX ? bcmp_new[diff - 1][1] : 0u, diff > 0 && diff <= HWR_BCMP_MAX ? bcmp_new[diff - 1][2] : 0u,
					diff > 0 && diff <= HWR_BCMP_MAX ? bcmp_old[diff - 1][0] : 0u, diff > 0 && diff <= HWR_BCMP_MAX ? bcmp_old[diff - 1][1] : 0u, diff > 0 && diff <= HWR_BCMP_MAX ? bcmp_old[diff - 1][2] : 0u);
		}
		HWR_DrawBatch(&currentSurfaceInfo, desc + 3 * bstart, 3 * (pos - bstart), currentPolyFlags);
		ps_hw_numcalls.value.i++;
		ps_hw_numverts.value.i += 3 * (pos - bstart);
		polygonArraySize = 0;
		unsortedVertexArraySize = 0;
		HWP_SPAN_END2(tb_draw, HWP_BATCHDRAW, HWP_KB_DRAW);
		{
			HWP_SPAN_BEGIN(tb_end);
			PS2HWD_BatchEnd();
			HWP_SPAN_END2(tb_end, HWP_B_END, HWP_KB_END);
		}
		PS_STOP_TIMING(ps_hw_batchdrawtime);
	}
	return 1;
}
#endif

// This function organizes the geometry collected by HWR_ProcessPolygon calls into batches and uses
// the rendering backend to draw them.
static void HWR_RenderBatchesOld(void)
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
#ifdef PS2 // PS2-HW-225: the sort keys are made in the same pass (it was a second pass over the polygon array)
	if (rcap < polygonArrayAllocSize)
	{
		rkeys = HWR_BatchResize(rkeys, (size_t)polygonArrayAllocSize * sizeof(UINT32));
		rtmpk = HWR_BatchResize(rtmpk, (size_t)polygonArrayAllocSize * sizeof(UINT32));
		rtmpi = HWR_BatchResize(rtmpi, (size_t)polygonArrayAllocSize * sizeof(UINT32));
		rcap = polygonArrayAllocSize;
	}
	{
		INT32 prev = 0;

		for (i = 0; i < polygonArraySize; i++)
		{
			const INT32 h = polygonArray[i].hash;

			polygonIndexArray[i] = i;
			rkeys[i] = (UINT32)h ^ 0x80000000u; // the signed order of comparePolygons
			// Keep qsort's existing equal-key order: only bypass it for strictly ordered keys.
			if (i && prev >= h)
				sorted = false;
			prev = h;
		}
	}
#else
	for (i = 0; i < polygonArraySize; i++)
	{
		polygonIndexArray[i] = i;
		// Keep qsort's existing equal-key order: only bypass it for strictly ordered keys.
		if (i && polygonArray[i-1].hash >= polygonArray[i].hash)
			sorted = false;
	}
#endif

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

#ifdef PS2
	HWR_PlanPass();
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
	{
		HWP_SPAN_BEGIN(tst);
		HWD.pfnSetTexture(currentTexture);
		HWP_SPAN_END2(tst, HWP_B_TEX, HWP_KB_TEX);
	}

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
			HWR_DrawBatch(&currentSurfaceInfo, finalVertexIndexArray, finalIndexWritePos, currentPolyFlags);
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
		finalVertexIndexArray[finalIndexWritePos++] = (UINT32)polygonArray[index].surf.LightInfo.light_level; // PS2-HW-106
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
            HWR_DrawBatch(&currentSurfaceInfo, finalVertexIndexArray, finalIndexWritePos, currentPolyFlags);
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
				{
					HWP_SPAN_BEGIN(tst);
					HWD.pfnSetTexture(nextTexture);
					HWP_SPAN_END2(tst, HWP_B_TEX, HWP_KB_TEX);
				}
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

void HWR_RenderBatches(void)
{
#ifdef PS2
	if (currently_batching && polygonArraySize > 0 && !(hwr_geo_off & 64) && HWR_RenderBatchesV2())
		return;
#endif
	HWR_RenderBatchesOld();
}


#endif // HWRENDER
