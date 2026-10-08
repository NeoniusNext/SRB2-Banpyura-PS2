// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2020-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_batching.h
/// \brief Draw call batching and related things.

#ifndef __HWR_BATCHING_H__
#define __HWR_BATCHING_H__

#include "hw_defs.h"
#include "hw_data.h"
#include "hw_drv.h"

typedef struct
{
	FSurfaceInfo surf;// surf also has its own polyflags for some reason, but it seems unused
	unsigned int vertsIndex;// location of verts in unsortedVertexArray
	FUINT numVerts;
	FBITFIELD polyFlags;
	GLMipmap_t *texture;
	int shader;
	// this tells batching that the plane belongs to a horizon line and must be drawn in correct order with the skywalls
	boolean horizonSpecial;
	INT32 hash;
} PolygonArrayEntry;

extern boolean currently_batching; // between HWR_StartBatching and HWR_RenderBatches: polygons are collected, not drawn
void HWR_StartBatching(void);
#ifdef PS2_PROFILE
void HWR_ReleaseBatching(void); // PS2-HW-79: gives the batch arrays back on a level change
#endif
void HWR_SetCurrentTexture(GLMipmap_t *texture);
void HWR_ProcessPolygon(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags, int shader, boolean horizonSpecial);
void HWR_RenderBatches(void);
#ifdef PS2_PROFILE
// OPT11 (GEOM, PS2-HW-80): the geometry cache (hw_gcache.inc, included by hw_main.c). While a seg or a plane is calculated for the cache, the three
// sinks of the BSP walk (HWR_ProcessPolygon, HWR_SetCurrentTexture, HWR_AddTransparentWall) append what they are given to the record.
extern boolean hwr_grec_on;
extern int polygonArraySize, polygonArrayAllocSize, unsortedVertexArraySize, unsortedVertexArrayAllocSize; // the batch arrays (the cache looks at the room before a replay)
extern UINT32 hwr_geo_off; // -hwgo: OPT11 optimisations switched off (hw_gcache.inc)
void HWR_GCRecPoly(const FSurfaceInfo *surf, const FOutVector *verts, FUINT n, FBITFIELD flags, int shader, boolean horizon, UINT32 hash); // hash: the sort key HWR_ProcessPolygon made of the polygon
void HWR_GCRecTex(GLMipmap_t *texture);
UINT32 HWR_GCTexId(const GLMipmap_t *tex); // the frame independent part of the texture order
typedef struct { UINT8 op, n, hashed, pad; UINT16 size; INT16 target; UINT32 hash[2]; } gcphdr_t; // 16 bytes, followed by the PolygonArrayEntry of the polygon (15 words: what HWR_ProcessPolygon would have stored, the vertex index, the shader and the sort key are made at the replay) and n FOutVector. hash[]: the sort key for the two scan directions; target: the shader target
void HWR_GCBatchReserve(int npoly, int nvert); // room in the batch arrays for that many more polygons and vertices (it may allocate: the cache asks before it replays)
void HWR_GCReplayPoly(const gcphdr_t *h, UINT32 view); // HWR_ProcessPolygon of such a record (batching): the entry is copied, the texture of the entry is touched once per view
void HWR_GCHashes(const GLMipmap_t *tex, UINT32 h16, UINT32 out[2]); // the sort keys of a polygon of this texture and state hash for the scan directions 0 and 1
extern GLMipmap_t *current_texture; // the texture of the next polygon (HWR_SetCurrentTexture)
void HWR_GCacheAbandon(void); // a frame was abandoned half way (ps2_hwfb.c): no record is in progress any more
void HWR_GCacheFlush(void); // everything cached is dropped (a new level, the texture records or the light tables go away, a setting changed)
void HWR_PolyHashFrame(INT32 frame); // -hwpolyhash: the HWPH line of the frame just ended
UINT32 HWR_PS2_TexTransparent(INT32 tex); // hw_cache.c: TF_TRANSPARENT of the mipmap of a (translated) map texture
UINT32 HWR_PS2_SideTexWord(INT32 raw); // hw_cache.c: the word of a side texture in the key of the cache (translated number | transparent bit << 31)
#endif

#ifdef PS2_PROFILE
// PS2-HW-52: the sprite loop (HWR_DrawSprites) collects the polygons of opaque sprites and their shadows and draws them as batches
extern boolean hwr_sprite_batch; // HWR_ProcessPolygon flushes the batch at the first polygon that is not order independent
extern boolean hwr_sprite_shadow; // the polygon is a drop shadow
#endif

#endif
