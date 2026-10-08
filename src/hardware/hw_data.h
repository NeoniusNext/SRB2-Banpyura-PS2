// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2023 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_data.h
/// \brief defines structures and exports for the hardware interface used by Sonic Robo Blast 2

#ifndef _HWR_DATA_
#define _HWR_DATA_

#if defined (_WIN32) && !defined (__CYGWIN__)
//#define WIN32_LEAN_AND_MEAN
#define RPC_NO_WINDOWS_H
#include <windows.h>
#endif

#include "../doomdef.h"
#include "../screen.h"


// ==========================================================================
//                                                               TEXTURE INFO
// ==========================================================================

typedef enum GLTextureFormat_e
{
	GL_TEXFMT_P_8                 = 0x01, /* 8-bit palette */
	GL_TEXFMT_AP_88               = 0x02, /* 8-bit alpha, 8-bit palette */

	GL_TEXFMT_RGBA                = 0x10, /* 32 bit RGBA! */

	GL_TEXFMT_ALPHA_8             = 0x20, /* (0..0xFF) alpha     */
	GL_TEXFMT_INTENSITY_8         = 0x21, /* (0..0xFF) intensity */
	GL_TEXFMT_ALPHA_INTENSITY_88  = 0x22,
} GLTextureFormat_t;

// Colormap structure for mipmaps.
struct GLColormap_s
{
	const UINT8 *source;
	UINT8 data[256];
};
typedef struct GLColormap_s GLColormap_t;


// Texture information (misleadingly named "mipmap" all over the code.)
// The *data pointer holds the address of the graphics data cached in heap memory.
// NULL if the texture is not in SRB2's heap cache.
struct GLMipmap_s
{
	// for UpdateTexture
	GLTextureFormat_t     format;
	void                 *data;

	UINT32                flags;
	UINT16                height;
	UINT16                width;
	UINT32                downloaded; // The GPU has this texture.

	struct GLMipmap_s    *nextcolormap;
	struct GLColormap_s  *colormap;

#ifdef PS2_PROFILE
	// PS2-HW-16: the GS driver asks for the data of a map texture / level flat again when it is drawn and the zone dropped it
	// (HWR_PS2_RegenerateMipmap): kind 0 = no way (patches stay as they are), 1 = map texture `regen_id`, 2 = level flat `regen_id`
	INT32                 regen_id;
	UINT8                 regen_kind;
	// PS2-HW-30: the other CLUT variant (chroma keyed / plain) of a map texture or flat: it has the same pixels, the GS driver keeps one
	// VRAM image for both. Set on the chroma keyed variant (-> original); the original reaches it through nextcolormap.
	struct GLMipmap_s    *ps2_twin;
	UINT8                 ps2_h255; // 0 unknown, 1 no texel has index 255, 2 some has (cached: scanning 512 KiB costs 0.6 M cycles)
	// PS2-HW-34: what the batches of this frame need of the texture (ps2_hw_plan.inc): on the original mipmap of a variant pair
	UINT32                ps2_planfr; // driver frame + 1 the plan belongs to (0 = none)
	UINT8                 ps2_want; // the mip level (0 = full size) the frame plan resolved for the texture (OPT10: ps2_hw_plan.inc)
	UINT16                ps2_pi; // OPT10: index of the texture's record in the driver's frame plan (valid while ps2_planfr is the current frame)
	UINT16                ps2_uw, ps2_uh; // OPT10 (PS2-HW-39): the real size of a patch (the mipmap is its power of two size): only that part is stored in VRAM
	UINT8                 ps2_ihint; // OPT10: the level the budgeted plan gave the texture for its immediate draws (translucent planes, sky), valid for 2 frames after ps2_ihfr
	UINT32                ps2_ihfr, ps2_imfr; // driver frame + 1 of the plan that set the hint / of the last immediate draw of the texture
	UINT8                 ps2_vis; // some polygon of the frame is visible at all
	UINT32                ps2_full_fr; // driver frame + 1 of the last draw that needed the full size without a plan (the next plans keep the full size)
	UINT32                ps2_gcv; // OPT11 (PS2-HW-80): the view of the geometry cache whose replay last made this texture the current one (once per view is enough)
	UINT8                 ps2_nup, ps2_drop; // OPT12 HWDRV (PS2-HW-441): uploads of this texture so far (saturating) / why its VRAM image was dropped last (1 evicted, 2 upgrade, 3 downgrade, 4 cap, 5 other)
	UINT8                 ps2_keep; // OPT12 HWDRV (PS2-HW-442): the driver keeps the data of this texture alive (ps2_hw_tex.inc keep_*)
	UINT8                 ps2_cost; // OPT12 HWDRV (PS2-HW-442): what the last make-again of its data cost, in units of 2^18 cycles (0 = never made again, 255 = more)
#endif
};
typedef struct GLMipmap_s GLMipmap_t;


//
// Level textures, as cached for hardware rendering.
//
struct GLMapTexture_s
{
	GLMipmap_t  mipmap;
	float       scaleX; // Used for scaling textures on walls
	float       scaleY;
};
typedef struct GLMapTexture_s GLMapTexture_t;


// Patch information for the hardware renderer.
struct GLPatch_s
{
	GLMipmap_t *mipmap; // Texture data. Allocated whenever the patch is.
	float       max_s, max_t;
};
typedef struct GLPatch_s GLPatch_t;

#endif //_HWR_DATA_
