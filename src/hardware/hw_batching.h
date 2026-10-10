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
int HWR_FrPolyOutside(const FOutVector *v, unsigned int n); // OPT12 HWFRONT (hw_front.inc): all vertices outside one side of the view volume
#ifdef PS2_HWDETAIL
extern int hwr_fr_src; // OPT12: what the BSP walk is making (hw_front.inc census): 1 wall, 2 plane
void HWR_FrCensusPoly(const FOutVector *v, unsigned int n); // OPT12 HWFRONT (hw_front.inc)
#endif
void HWR_RenderBatches(void);
#ifdef PS2_PROFILE
// OPT11 (GEOM, PS2-HW-80): the geometry cache (hw_gcache.inc, included by hw_main.c). While a seg or a plane is calculated for the cache, the three
// sinks of the BSP walk (HWR_ProcessPolygon, HWR_SetCurrentTexture, HWR_AddTransparentWall) append what they are given to the record.
extern boolean hwr_grec_on;
extern int polygonArraySize, polygonArrayAllocSize, unsortedVertexArraySize, unsortedVertexArrayAllocSize; // the batch arrays (the cache looks at the room before a replay)
extern UINT32 hwr_geo_off; // -hwgo: OPT11 optimisations switched off (hw_gcache.inc)
void HWR_GCRecPoly(const FSurfaceInfo *surf, const FOutVector *verts, FUINT n, FBITFIELD flags, int shader, boolean horizon);
void HWR_GCRecTex(GLMipmap_t *texture);
typedef struct { UINT8 op, n, horizon, pad; UINT16 size; INT16 target; FBITFIELD flags; GLMipmap_t *tex; INT32 src, tnum; } gcphdr_t; // 24 bytes (src: 0, or the number + 1 of the side whose animated texture tnum the polygon takes: 3D floors), followed by an FSurfaceInfo and n FOutVector: what the cache keeps of one HWR_ProcessPolygon
// OPT13 IR (RF-1, stored records v3, PS2-HW-480..): the record of a polygon that was collected as a block (hw_pbatch.inc): the interned state of its bucket, the box around it, the words of its header
// quadwords and its vertices (x y z s t). The replay puts the block into the pool of the batch without HWR_ProcessPolygon (HWR_GCReplayRun).
typedef struct { UINT8 op, n, ext, fl; UINT16 size, state; INT32 bb[6]; UINT32 light; float ex[4]; } gcbk_t; // 52 bytes (op, vertices, the texture repeats (ex holds the extent of s, t), flags: 1 the box is not valid; size of the whole op; state; box x0 x1 y0 y1 z0 z1 (whole map units, rounded outwards; flag 1: no box); the light level of the block as the float of its header), followed by n * 5 words
typedef struct { INT32 a, b, c, nd; UINT32 ox, oy, oz; } hwr_fbox_t; // a plane of the view volume (hw_front.inc) made for the integer test of a box x0 x1 y0 y1 z0 z1: a, b, c in 1/16384, nd: minus the distance of the origin with the margins, in the same unit; ox, oy, oz: the indices of the corner that is nearest the inside
extern hwr_fbox_t hwr_fbox[5];
extern int hwr_fbox_on;
#define GCOP_BLK 4 // the op of a polygon block in a record (2 and 3 are the earlier polygon call and the translucent wall: hw_gcache.inc)
#define GCBX_HDR 32u // the header of an op that holds the block of a plane (fl bit 1): op n ext fl size state and the box, then the block (two quadwords and two a vertex) on a 16 byte boundary
#define GCBX_SIZE(n) (GCBX_HDR + 16u * (2u + 2u * (UINT32)(n)))
#define GCBK_SIZE(n) ((UINT32)sizeof(gcbk_t) + 20u * (UINT32)(n)) // (rounded up to 16 in the record)
const UINT8 *HWR_GCReplayRun(const UINT8 *p, const UINT8 *end, UINT32 view, GLMipmap_t **lasttex); // the replay of the polygon blocks that follow each other from p; returns the first op that is not one
boolean HWR_PBRoomFor(UINT32 npoly, UINT32 nvert); // the pool and the buckets have room for that many more polygons and vertices (false: the replay of a record is not started); may grow the pool
UINT32 HWR_PBCount(void); // the number of polygons collected in the batch so far
UINT32 HWR_GCVerifyPool(UINT32 a, UINT32 b, UINT32 c, const char *what, UINT32 id); // check mode: the polygons [a, b) (the replay) and [b, c) (the calculation) are the same blocks in the same buckets (number of differences)
void HWR_PBNoCull(int on); // the replay does not test the box (check mode)
void HWR_PBCullView(void); // once per view: the gate of the box test
extern UINT32 hwr_gv_off; // -hwgv: OPT13 IR switches (hw_gcache.inc)
boolean HWR_GCRecBlkOk(FBITFIELD flags, boolean horizon); // gcache: may the polygon the walk is making be recorded as a block?
void HWR_PBStateFlush(void); // the interned states are dropped (the cache was flushed)
void HWR_GCRecBlk(UINT32 state, UINT32 light, const float *ex, UINT32 ext, const FOutVector *verts, FUINT n); // gcache: appends the record of a polygon block
void HWR_GCBatchReserve(int npoly, int nvert); // room in the batch arrays of the old collection for that many more polygons and vertices (it may allocate: the cache asks before it replays)
void HWR_GCReplayPoly(const gcphdr_t *h, UINT32 view); // HWR_ProcessPolygon of such a record (the texture is made current and touched once per view)
extern GLMipmap_t *current_texture; // the texture of the next polygon (HWR_SetCurrentTexture)
void HWR_GCacheAbandon(void); // a frame was abandoned half way (ps2_hwfb.c): no record is in progress any more
void HWR_GCacheFlush(void); // everything cached is dropped (a new level, the texture records or the light tables go away, a setting changed)
void HWR_PolyHashFrame(INT32 frame); // -hwpolyhash: the HWPH line of the frame just ended
UINT32 HWR_PS2_TexTransparent(INT32 tex); // hw_cache.c: TF_TRANSPARENT of the mipmap of a (translated) map texture
extern UINT32 hwr_texsig; // hw_cache.c: bumped whenever a word of HWR_PS2_SideTexWord may have changed
UINT32 HWR_PS2_SideTexWord(INT32 raw); // hw_cache.c: the word of a side texture in the key of the cache (translated number | transparent bit << 31)
#endif

#ifdef PS2_PROFILE
// PS2-HW-52: the sprite loop (HWR_DrawSprites) collects the polygons of opaque sprites and their shadows and draws them as batches
extern boolean hwr_sprite_batch; // HWR_ProcessPolygon flushes the batch at the first polygon that is not order independent
extern boolean hwr_sprite_shadow; // the polygon is a drop shadow
#endif

#endif
