// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 2018-2024 by Lactozilla.
// Copyright (C) 2019-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_picformats.h
/// \brief Patch generation.

#ifndef __R_PICFORMATS__
#define __R_PICFORMATS__

#include "r_defs.h"
#include "doomdef.h"

typedef enum
{
	PICFMT_NONE = 0,

	// Doom formats
	PICFMT_PATCH,
	PICFMT_FLAT,
	PICFMT_DOOMPATCH,

	// PNG
	PICFMT_PNG,

	// 16bpp
	PICFMT_PATCH16,
	PICFMT_FLAT16,
	PICFMT_DOOMPATCH16,

	// 32bpp
	PICFMT_PATCH32,
	PICFMT_FLAT32,
	PICFMT_DOOMPATCH32
} pictureformat_t;

typedef enum
{
	PICFLAGS_XFLIP                = 1,
	PICFLAGS_YFLIP                = 1<<1,
	PICFLAGS_USE_TRANSPARENTPIXEL = 1<<2
} pictureflags_t;

enum
{
	PICDEPTH_NONE = 0,
	PICDEPTH_8BPP = 8,
	PICDEPTH_16BPP = 16,
	PICDEPTH_32BPP = 32
};

void *Picture_Convert(
	pictureformat_t informat, void *picture, pictureformat_t outformat,
	size_t insize, size_t *outsize,
	INT32 inwidth, INT32 inheight, INT32 inleftoffset, INT32 intopoffset,
	pictureflags_t flags);

void *Picture_PatchConvert(
	pictureformat_t informat, void *picture, pictureformat_t outformat,
	size_t *outsize,
	INT32 inwidth, INT32 inheight, INT32 inleftoffset, INT32 intopoffset,
	pictureflags_t flags);
void *Picture_FlatConvert(
	pictureformat_t informat, void *picture, pictureformat_t outformat,
	size_t *outsize,
	INT32 inwidth, INT32 inheight,
	pictureflags_t flags);
void *Picture_GetPatchPixel(
	patch_t *patch, pictureformat_t informat,
	INT32 x, INT32 y,
	pictureflags_t flags);

void *Picture_TextureToFlat(size_t texnum);

INT32 Picture_FormatBPP(pictureformat_t format);
boolean Picture_IsPatchFormat(pictureformat_t format);
boolean Picture_IsInternalPatchFormat(pictureformat_t format);
boolean Picture_IsDoomPatchFormat(pictureformat_t format);
boolean Picture_IsFlatFormat(pictureformat_t format);
boolean Picture_CheckIfDoomPatch(softwarepatch_t *patch, size_t size);

// Structs
typedef enum
{
	ROTAXIS_X, // Roll (the default)
	ROTAXIS_Y, // Pitch
	ROTAXIS_Z  // Yaw
} rotaxis_t;

typedef struct
{
	INT32 x, y;
} spriteframepivot_t;

typedef struct
{
#ifdef PS2_DYNLIMITS
	spriteframepivot_t *pivot; // PS2-104: NULL until R_SpriteInfoPivot() allocates MAXFRAMENUM entries (2 KB per sprite that has pivots, not for every sprite and skin animation)
#else
	spriteframepivot_t pivot[MAXFRAMENUM];
#endif
	boolean available;
} spriteinfo_t;

#ifdef PS2_DYNLIMITS
spriteframepivot_t *R_SpriteInfoPivot(spriteinfo_t *info); // the pivot array of info, allocated on first use
void R_SpriteInfoCopy(spriteinfo_t *dst, const spriteinfo_t *src); // deep copy (what the memcpy of the fixed-size struct did)
void R_SpriteInfoFree(spriteinfo_t *info); // frees the pivot array of a temporary
#else
#define R_SpriteInfoPivot(info) ((info)->pivot)
#define R_SpriteInfoCopy(dst, src) M_Memcpy((dst), (src), sizeof (spriteinfo_t))
#define R_SpriteInfoFree(info) ((void)(info))
#endif

// PNG support
#define PNG_HEADER_SIZE 8

#ifdef PS2_PROFILE
// PS2-20: the PNG lumps of the packs are converted by the cooker into "cooked pictures" (8 byte marker
// + a Doom patch with the pixels, transparency and offsets the original Picture_PNGConvert produced).
// Without PS2_ZIPPNG (no libpng) these three names stand for the cooked-picture functions so the PNG-aware
// texture/patch code needs no change; with it (PS2-100) they are the real libpng functions, which also accept
// a cooked picture (Picture_IsLumpCooked tells the two apart for the code that streams the patch directly).
#undef NO_PNG_LUMPS
#ifndef PS2_ZIPPNG
#undef PICTURE_PNG_USELOOKUP
#define Picture_IsLumpPNG Picture_IsLumpCooked
#define Picture_PNGConvert Picture_CookedConvert
#define Picture_PNGDimensions Picture_CookedDimensions
#endif
boolean Picture_IsLumpCooked(const UINT8 *d, size_t s);
#endif

boolean Picture_IsLumpPNG(const UINT8 *d, size_t s);

#ifndef NO_PNG_LUMPS
void *Picture_PNGConvert(
	const UINT8 *png, pictureformat_t outformat,
	INT32 *w, INT32 *h,
	INT16 *topoffset, INT16 *leftoffset,
	size_t insize, size_t *outsize,
	pictureflags_t flags);
boolean Picture_PNGDimensions(UINT8 *png, INT32 *width, INT32 *height, INT16 *topoffset, INT16 *leftoffset, size_t size);

#if !defined(PS2_PROFILE) || defined(PS2_ZIPPNG)
#define PICTURE_PNG_USELOOKUP
#endif
#ifdef PS2_PROFILE
void *Picture_CookedConvert(
	const UINT8 *cooked, pictureformat_t outformat,
	INT32 *w, INT32 *h,
	INT16 *topoffset, INT16 *leftoffset,
	size_t insize, size_t *outsize,
	pictureflags_t flags);
boolean Picture_CookedDimensions(UINT8 *cooked, INT32 *width, INT32 *height, INT16 *topoffset, INT16 *leftoffset, size_t size);
#endif
#endif

// SpriteInfo
extern spriteinfo_t spriteinfo[NUMSPRITES];
void R_LoadSpriteInfoLumps(UINT16 wadnum, UINT16 numlumps);
void R_ParseSPRTINFOLump(UINT16 wadNum, UINT16 lumpNum);

#endif // __R_PICFORMATS__
