// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2023 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_state.h
/// \brief Refresh/render internal state variables (global)

#ifndef __R_STATE__
#define __R_STATE__

// Need data structure definitions.
#include "d_player.h"
#include "r_data.h"

#ifdef __GNUG__
#pragma interface
#endif

//
// Refresh internal data structures, for rendering.
//

// needed for pre rendering (fracs)
typedef struct
{
#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_SPRCACHE)
	// Sprite headers supply signed 16-bit pixels. All cached fixed-point
	// values have zero fractional bits, including the adjusted top offset.
	INT16 width_pixels;
	INT16 offset_pixels;
	INT16 topoffset_pixels;
	INT16 height_pixels;
#else
	fixed_t width;
	fixed_t offset;
	fixed_t topoffset;
	fixed_t height;
#endif
} sprcache_t;

#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_SPRCACHE)
#define SPRCACHE_WIDTH(c) ((fixed_t)(c).width_pixels * FRACUNIT)
#define SPRCACHE_OFFSET(c) ((fixed_t)(c).offset_pixels * FRACUNIT)
#define SPRCACHE_TOPOFFSET(c) ((fixed_t)(c).topoffset_pixels * FRACUNIT)
#define SPRCACHE_HEIGHT(c) ((fixed_t)(c).height_pixels * FRACUNIT)
#else
#define SPRCACHE_WIDTH(c) ((c).width)
#define SPRCACHE_OFFSET(c) ((c).offset)
#define SPRCACHE_TOPOFFSET(c) ((c).topoffset)
#define SPRCACHE_HEIGHT(c) ((c).height)
#endif

extern sprcache_t *spritecachedinfo;

extern lighttable_t *colormaps;
extern lighttable_t *fadecolormap;

// Boom colormaps.
extern extracolormap_t *extra_colormaps;

// for global animation
extern INT32 *texturetranslation;

// Sprites
extern size_t numspritelumps, max_spritelumps;

//
// Lookup tables for map data.
//
extern boolean udmf;

extern size_t numsprites;
extern spritedef_t *sprites;

extern size_t numvertexes;
extern vertex_t *vertexes;

extern size_t numsegs;
extern seg_t *segs;

extern size_t numsectors;
extern sector_t *sectors;
extern spawnsector_t *spawnsectors;

extern size_t numsubsectors;
extern subsector_t *subsectors;

extern size_t numnodes;
extern node_t *nodes;

extern size_t numlines;
extern line_t *lines;
extern spawnline_t *spawnlines;

extern size_t numsides;
extern side_t *sides;
extern spawnside_t *spawnsides;

//
// POV data.
//
extern fixed_t viewx, viewy, viewz;
extern angle_t viewangle, aimingangle;
extern sector_t *viewsector;
extern player_t *viewplayer;
extern mobj_t *r_viewmobj;

extern consvar_t cv_allowmlook;
extern consvar_t cv_maxportals;

extern angle_t clipangle;
extern angle_t doubleclipangle;

extern INT32 viewangletox[FINEANGLES/2];
extern angle_t xtoviewangle[MAXVIDWIDTH+1];

extern fixed_t rw_distance;
extern angle_t rw_normalangle;

// angle to line origin
extern angle_t rw_angle1;

#endif
