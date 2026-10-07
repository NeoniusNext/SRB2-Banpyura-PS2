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
// PS2-HW-52: the sprite loop (HWR_DrawSprites) collects the polygons of opaque sprites and their shadows and draws them as batches
extern boolean hwr_sprite_batch; // HWR_ProcessPolygon flushes the batch at the first polygon that is not order independent
extern boolean hwr_sprite_shadow; // the polygon is a drop shadow
#endif

#endif
