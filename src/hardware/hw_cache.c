// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2023 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_cache.c
/// \brief load and convert graphics to the hardware format

#include "../doomdef.h"

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_drv.h"
#include "hw_batching.h"

#include "../doomstat.h"    //gamemode
#include "../i_video.h"     //rendermode
#include "../r_data.h"
#include "../r_textures.h"
#include "../r_state.h"
#include "../r_sky.h"
#include "../w_wad.h"
#include "../z_zone.h"
#ifdef PS2_PROFILE
#include "../m_argv.h" // -hwfbtex
#endif
#include "../v_video.h"
#include "../r_draw.h"
#include "../r_patch.h"
#include "../r_picformats.h"
#include "../p_setup.h"
#ifdef PS2_PROFILE
#include "../ps2/hw/ps2_hwd_dbg.h" // ps2hwd_dbg_flags: -hwdbg 0x1000000 checks the composition fast path against the original loops
#include "../ps2/ps2_texc.h" // OPT13 IZ (PS2-602, R2): composites prebuilt by the cooker

static boolean ps2_slow_composite; // the original column loops (the check of the fast path)
boolean ps2hwt_comp_old; // OPT13 RDRV: -hwcomp 0 = the composition as before (A/B): the raw lump read per patch use, the GL structures and the patch built and freed for every placement
typedef struct { UINT16 wad, lump; patch_t *p; } ps2_cpatch_t;
#define PS2_CPATCH_MAX 32 // distinct patches held while one texture is composed (a placement of the same patch again in the texture finds it)
unsigned int ps2hwt_mkpatch_n, ps2hwt_mkpatch_cyc; // OPT10: patches composed for the GS driver and the EE cycles it took (HWTEX lines)
static inline unsigned int ps2hwt_now(void)
{
	unsigned int v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}
#endif

// OPT12 HWDRV (PS2-HW-442): the data of a patch mipmap (sprites, HUD) between two selections: a cache block of the LRU kind (PU_CACHE, stamped with the frame by the tag change: it cannot go before the
// batch that collected the polygon has been drawn). PU_HWRCACHE_UNLOCKED goes at the next allocation that does not fit, and a patch has no way to be made again at draw time ("no data (purged)":
// the sprite is missing for the frame). ps2_hwd.c sets PU_HWRCACHE_UNLOCKED again with -hwkeep 1 (the old rule, A/B).
#ifdef PS2_PROFILE
INT32 ps2hwt_patchtag = PU_HWRCACHE_LRU;
#define HWR_PATCH_UNLOCKED(p) Z_ChangeTag((p), ps2hwt_patchtag)
#else
#define HWR_PATCH_UNLOCKED(p) Z_ChangeTag((p), PU_HWRCACHE_UNLOCKED)
#endif

INT32 patchformat = GL_TEXFMT_AP_88; // use alpha for holes
INT32 textureformat = GL_TEXFMT_P_8; // use chromakey for hole

RGBA_t mapPalette[256] = {0}; // the palette for the currently loaded level or menu etc.

// Returns a pointer to the palette which should be used for caching textures.
RGBA_t *HWR_GetTexturePalette(void)
{
	return HWR_ShouldUsePaletteRendering() ? mapPalette : pLocalPalette;
}

static INT32 format2bpp(GLTextureFormat_t format)
{
	if (format == GL_TEXFMT_RGBA)
		return 4;
	else if (format == GL_TEXFMT_ALPHA_INTENSITY_88 || format == GL_TEXFMT_AP_88)
		return 2;
	else
		return 1;
}

// This code was originally placed directly in HWR_DrawPatchInCache.
// It is now split from it for my sanity! (and the sanity of others)
// -- Monster Iestyn (13/02/19)
static void HWR_DrawColumnInCache(const column_t *patchcol, UINT8 *block, GLMipmap_t *mipmap,
								INT32 pblockheight, INT32 blockmodulo,
								fixed_t yfracstep, fixed_t scale_y,
								texpatch_t *originPatch, INT32 patchheight,
								INT32 bpp, RGBA_t *palette)
{
	fixed_t yfrac, position, count;
	UINT8 *dest;
	const UINT8 *source;
	INT32 originy = 0;

	// for writing a pixel to dest
	RGBA_t colortemp;
	UINT8 alpha;
	UINT8 texel;
	UINT16 texelu16;

	(void)patchheight; // This parameter is unused

	if (originPatch) // originPatch can be NULL here, unlike in the software version
		originy = originPatch->originy;

	for (unsigned i = 0; i < patchcol->num_posts; i++)
	{
		post_t *post = &patchcol->posts[i];
		source = patchcol->pixels + post->data_offset;
		count  = ((post->length * scale_y) + (FRACUNIT/2)) >> FRACBITS;
		position = originy + post->topdelta;

		yfrac = 0;
		if (position < 0)
		{
			yfrac = -position<<FRACBITS;
			count += (((position * scale_y) + (FRACUNIT/2)) >> FRACBITS);
			position = 0;
		}

		position = ((position * scale_y) + (FRACUNIT/2)) >> FRACBITS;

		if (position < 0)
			position = 0;

		if (position + count >= pblockheight)
			count = pblockheight - position;

		for (dest = block + (position*blockmodulo); count > 0; count--, dest += blockmodulo, yfrac += yfracstep)
		{
			texel = source[yfrac>>FRACBITS];
			alpha = 0xFF;

			// Make pixel transparent if chroma keyed
			if ((mipmap->flags & TF_CHROMAKEYED) && (texel == HWR_PATCHES_CHROMAKEY_COLORINDEX))
				alpha = 0x00;

			if (mipmap->colormap)
				texel = mipmap->colormap->data[texel];

			switch (bpp)
			{
				case 2:
				{
					texelu16 = *((UINT16*)dest);
					if ((originPatch != NULL) && (originPatch->style != AST_COPY))
					{
						if (originPatch->style == AST_TRANSLUCENT && originPatch->alpha < ASTTextureBlendingThreshold[0])
							continue;
						if (!(texelu16 & 0xFF00) && originPatch->alpha <= ASTTextureBlendingThreshold[1])
							continue;
						texel = ASTBlendPaletteIndexes(texelu16 & 0xFF, texel, originPatch->style, originPatch->alpha);
					}
					texelu16 = (UINT16)((alpha<<8) | texel);
					memcpy(dest, &texelu16, sizeof(UINT16));
					break;
				}
				case 3:
					colortemp = palette[texel];
					if ((originPatch != NULL) && (originPatch->style != AST_COPY))
					{
						RGBA_t rgbatexel;
						rgbatexel.rgba = *(UINT32 *)dest;
						colortemp.rgba = ASTBlendTexturePixel(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
					}
					memcpy(dest, &colortemp, sizeof(RGBA_t)-sizeof(UINT8));
					break;
				case 4:
					colortemp = palette[texel];
					colortemp.s.alpha = alpha;
					if ((originPatch != NULL) && (originPatch->style != AST_COPY))
					{
						RGBA_t rgbatexel;
						rgbatexel.rgba = *(UINT32 *)dest;
						colortemp.rgba = ASTBlendTexturePixel(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
					}
					memcpy(dest, &colortemp, sizeof(RGBA_t));
					break;
				// default is 1
				default:
					if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						*dest = ASTBlendPaletteIndexes(*dest, texel, originPatch->style, originPatch->alpha);
					else
						*dest = texel;
					break;
			}
		}
	}
}

static void HWR_DrawFlippedColumnInCache(const column_t *patchcol, UINT8 *block, GLMipmap_t *mipmap,
								INT32 pblockheight, INT32 blockmodulo,
								fixed_t yfracstep, fixed_t scale_y,
								texpatch_t *originPatch, INT32 patchheight,
								INT32 bpp, RGBA_t *palette)
{
	fixed_t yfrac, position, count;
	UINT8 *dest;
	const UINT8 *source;
	INT32 topdelta;
	INT32 originy = 0;

	// for writing a pixel to dest
	RGBA_t colortemp;
	UINT8 alpha;
	UINT8 texel;
	UINT16 texelu16;

	if (originPatch) // originPatch can be NULL here, unlike in the software version
		originy = originPatch->originy;

	for (unsigned i = 0; i < patchcol->num_posts; i++)
	{
		post_t *post = &patchcol->posts[i];
		source = patchcol->pixels + post->data_offset;
		topdelta = patchheight-post->length-post->topdelta;
		count  = ((post->length * scale_y) + (FRACUNIT/2)) >> FRACBITS;
		position = originy + topdelta;

		yfrac = (post->length-1) << FRACBITS;

		if (position < 0)
		{
			yfrac += position<<FRACBITS;
			count += (((position * scale_y) + (FRACUNIT/2)) >> FRACBITS);
			position = 0;
		}

		position = ((position * scale_y) + (FRACUNIT/2)) >> FRACBITS;

		if (position < 0)
			position = 0;

		if (position + count >= pblockheight)
			count = pblockheight - position;

		for (dest = block + (position*blockmodulo); count > 0; count--, dest += blockmodulo, yfrac -= yfracstep)
		{
			texel = source[yfrac>>FRACBITS];
			alpha = 0xFF;

			// Make pixel transparent if chroma keyed
			if ((mipmap->flags & TF_CHROMAKEYED) && (texel == HWR_PATCHES_CHROMAKEY_COLORINDEX))
				alpha = 0x00;

			if (mipmap->colormap)
				texel = mipmap->colormap->data[texel];

			switch (bpp)
			{
				case 2:
					texelu16 = *((UINT16*)dest);
					if ((originPatch != NULL) && (originPatch->style != AST_COPY))
					{
						if (originPatch->style == AST_TRANSLUCENT && originPatch->alpha < ASTTextureBlendingThreshold[0])
							continue;
						if (!(texelu16 & 0xFF00) && originPatch->alpha <= ASTTextureBlendingThreshold[1])
							continue;
						texel = ASTBlendPaletteIndexes(texelu16 & 0xFF, texel, originPatch->style, originPatch->alpha);
					}
					texelu16 = (UINT16)((alpha<<8) | texel);
					memcpy(dest, &texelu16, sizeof(UINT16));
					break;
				case 3:
					colortemp = palette[texel];
					if ((originPatch != NULL) && (originPatch->style != AST_COPY))
					{
						RGBA_t rgbatexel;
						rgbatexel.rgba = *(UINT32 *)dest;
						colortemp.rgba = ASTBlendTexturePixel(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
					}
					memcpy(dest, &colortemp, sizeof(RGBA_t)-sizeof(UINT8));
					break;
				case 4:
					colortemp = palette[texel];
					colortemp.s.alpha = alpha;
					if ((originPatch != NULL) && (originPatch->style != AST_COPY))
					{
						RGBA_t rgbatexel;
						rgbatexel.rgba = *(UINT32 *)dest;
						colortemp.rgba = ASTBlendTexturePixel(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
					}
					memcpy(dest, &colortemp, sizeof(RGBA_t));
					break;
				// default is 1
				default:
					if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						*dest = ASTBlendPaletteIndexes(*dest, texel, originPatch->style, originPatch->alpha);
					else
						*dest = texel;
					break;
			}
		}
	}
}

// Simplified patch caching function
// for use by sprites and other patches that are not part of a wall texture
// no alpha or flipping should be present since we do not want non-texture graphics to have them
// no offsets are used either
// -- Monster Iestyn (13/02/19)
static void HWR_DrawPatchInCache(GLMipmap_t *mipmap,
	INT32 pblockwidth, INT32 pblockheight,
	INT32 pwidth, INT32 pheight,
	const patch_t *realpatch)
{
	INT32 ncols;
	fixed_t xfrac, xfracstep;
	fixed_t yfracstep, scale_y;
	const column_t *patchcol;
	UINT8 *block = mipmap->data;
	INT32 bpp;
	INT32 blockmodulo;
	RGBA_t *palette;

	if (pwidth <= 0 || pheight <= 0)
		return;

	palette = HWR_GetTexturePalette();

	ncols = pwidth;

	// source advance
	xfrac = 0;
	xfracstep = FRACUNIT;
	yfracstep = FRACUNIT;
	scale_y   = FRACUNIT;

	bpp = format2bpp(mipmap->format);

	if (bpp < 1 || bpp > 4)
		I_Error("HWR_DrawPatchInCache: no drawer defined for this bpp (%d)\n",bpp);

	// NOTE: should this actually be pblockwidth*bpp?
	blockmodulo = pblockwidth*bpp;

#ifdef PS2_PROFILE
	// PS2-HW-38: as the composition of map textures (PS2-HW-36): the block is row-major and the posts are columns, written column by column every
	// texel lands in another cache line of the 8 KiB data cache (about 25 cycles per texel: 0.4 M cycles for a 128x128 sprite). The same bytes
	// (alpha 0xFF for every texel of a post, the transparent rest of the block stays as MakeBlock filled it) are written in bands of 64 rows.
	if (!ps2_slow_composite && (bpp == 1 || bpp == 2) && !(mipmap->flags & TF_CHROMAKEYED))
	{
		const UINT8 *cm = mipmap->colormap ? mipmap->colormap->data : NULL;
		INT32 y0, j;

		for (y0 = 0; y0 < pblockheight; y0 += 64)
		{
			const INT32 y1 = y0 + 64 > pblockheight ? pblockheight : y0 + 64;

			for (j = 0; j < pwidth; j++)
			{
				const column_t *pc = &realpatch->columns[j];
				UINT8 *dcol = block + (size_t)j * bpp;
				unsigned i;

				for (i = 0; i < pc->num_posts; i++)
				{
					const post_t *post = &pc->posts[i];
					INT32 position = (INT32)post->topdelta, end = position + (INT32)post->length, ys, y;
					const UINT8 *src;
					UINT8 *d;

					if (position < 0)
						position = 0; // the original starts the source at -position: handled by the offset below
					ys = position > y0 ? position : y0;
					if (end > pblockheight)
						end = pblockheight;
					if (end > y1)
						end = y1;
					if (ys >= end)
						continue;
					src = pc->pixels + post->data_offset + (ys - (INT32)post->topdelta);
					d = dcol + (size_t)ys * (size_t)blockmodulo;
					if (bpp == 2)
					{
						for (y = ys; y < end; y++, d += blockmodulo)
						{
							const UINT8 t = *src++;

							*(UINT16 *)(void *)d = (UINT16)(0xFF00u | (cm ? cm[t] : t));
						}
					}
					else
					{
						for (y = ys; y < end; y++, d += blockmodulo)
						{
							const UINT8 t = *src++;

							*d = cm ? cm[t] : t;
						}
					}
				}
			}
		}
		return;
	}
#endif

	// Draw each column to the block cache
	for (; ncols--; block += bpp, xfrac += xfracstep)
	{
		patchcol = &realpatch->columns[xfrac>>FRACBITS];

		HWR_DrawColumnInCache(patchcol, block, mipmap,
								pblockheight, blockmodulo,
								yfracstep, scale_y,
								NULL, pheight, // not that pheight is going to get used anyway...
								bpp, palette);
	}
}

// This function we use for caching patches that belong to textures
static void HWR_DrawTexturePatchInCache(GLMipmap_t *mipmap,
	INT32 pblockwidth, INT32 pblockheight,
	texture_t *texture, texpatch_t *patch,
	const patch_t *realpatch)
{
	INT32 x, x1, x2;
	INT32 col, ncols;
	fixed_t xfrac, xfracstep;
	fixed_t yfracstep, scale_y;
	const column_t *patchcol;
	UINT8 *block = mipmap->data;
	INT32 bpp;
	INT32 blockmodulo;
	INT32 width, height;
	RGBA_t *palette;
	// Column drawing function pointer.
	static void (*ColumnDrawerPointer)(const column_t *patchcol, UINT8 *block, GLMipmap_t *mipmap,
								INT32 pblockheight, INT32 blockmodulo,
								fixed_t yfracstep, fixed_t scale_y,
								texpatch_t *originPatch, INT32 patchheight,
								INT32 bpp, RGBA_t *palette);

	if (texture->width <= 0 || texture->height <= 0)
		return;

	palette = HWR_GetTexturePalette();

	ColumnDrawerPointer = (patch->flip & 2) ? HWR_DrawFlippedColumnInCache : HWR_DrawColumnInCache;

	x1 = patch->originx;
	width = realpatch->width;
	height = realpatch->height;
	x2 = x1 + width;

	if (x1 > texture->width || x2 < 0)
		return; // patch not located within texture's x bounds, ignore

	if (patch->originy > texture->height || (patch->originy + height) < 0)
		return; // patch not located within texture's y bounds, ignore

	// patch is actually inside the texture!
	// now check if texture is partly off-screen and adjust accordingly

	// left edge
	if (x1 < 0)
		x = 0;
	else
		x = x1;

	// right edge
	if (x2 > texture->width)
		x2 = texture->width;


	col = x * pblockwidth / texture->width;
	ncols = ((x2 - x) * pblockwidth) / texture->width;

	// source advance
	xfrac = 0;
	if (x1 < 0)
		xfrac = -x1<<FRACBITS;

	xfracstep = (texture->width << FRACBITS) / pblockwidth;
	yfracstep = (texture->height<< FRACBITS) / pblockheight;
	scale_y   = (pblockheight  << FRACBITS) / texture->height;

	bpp = format2bpp(mipmap->format);

	if (bpp < 1 || bpp > 4)
		I_Error("HWR_DrawTexturePatchInCache: no drawer defined for this bpp (%d)\n",bpp);

	// NOTE: should this actually be pblockwidth*bpp?
	blockmodulo = pblockwidth*bpp;

#ifdef PS2_PROFILE
	// PS2-HW-36: a P_8 texture is row-major and the posts are columns: written column by column every byte lands in another cache line
	// (about 25 EE cycles per texel, 1.2 M cycles for an average texture, and the GS driver asks for the texture again whenever the zone
	// has dropped it). The same bytes (1:1 copy, no colormap, no blend style, no vertical flip) are written in bands of 64 rows instead: the
	// lines of a band stay in the data cache while the columns go by.
	if (!ps2_slow_composite && bpp == 1 && !mipmap->colormap && patch->style == AST_COPY && xfracstep == FRACUNIT && yfracstep == FRACUNIT && !(patch->flip & 2))
	{
		const INT32 sx0 = xfrac >> FRACBITS, originy = patch->originy;
		UINT8 *colblock = block + col;
		INT32 y0, j;

		for (y0 = 0; y0 < pblockheight; y0 += 64)
		{
			const INT32 y1 = y0 + 64 > pblockheight ? pblockheight : y0 + 64;

			for (j = 0; j < ncols; j++)
			{
				const column_t *pc = (patch->flip & 1) ? &realpatch->columns[(width-1)-(sx0+j)] : &realpatch->columns[sx0+j];
				UINT8 *dcol = colblock + j;
				unsigned i;

				for (i = 0; i < pc->num_posts; i++)
				{
					const post_t *post = &pc->posts[i];
					INT32 position = originy + (INT32)post->topdelta, count = (INT32)post->length, srcoff = 0, ys, ye, y;
					const UINT8 *src;
					UINT8 *d;

					if (position < 0)
					{
						srcoff = -position;
						count += position;
						position = 0;
					}
					if (position + count > pblockheight)
						count = pblockheight - position;
					ys = position > y0 ? position : y0;
					ye = position + count < y1 ? position + count : y1;
					if (ys >= ye)
						continue;
					src = pc->pixels + post->data_offset + srcoff + (ys - position);
					d = dcol + (size_t)ys * (size_t)pblockwidth;
					for (y = ys; y < ye; y++, d += pblockwidth)
						*d = *src++;
				}
			}
		}
		return;
	}
#endif

	// Draw each column to the block cache
	for (block += col*bpp; ncols--; block += bpp, xfrac += xfracstep)
	{
		if (patch->flip & 1)
			patchcol = &realpatch->columns[(width-1)-(xfrac>>FRACBITS)];
		else
			patchcol = &realpatch->columns[xfrac>>FRACBITS];

		ColumnDrawerPointer(patchcol, block, mipmap,
								pblockheight, blockmodulo,
								yfracstep, scale_y,
								patch, height,
								bpp, palette);
	}
}

// PS2-140: try: NULL (and no data) when the zone has no room for the block, instead of the end of the run
static UINT8 *MakeBlockEx(GLMipmap_t *grMipmap, boolean try)
{
	UINT8 *block;
	INT32 bpp, i;
	UINT16 bu16 = ((0x00 <<8) | HWR_PATCHES_CHROMAKEY_COLORINDEX);
	INT32 blocksize = (grMipmap->width * grMipmap->height);

	bpp =  format2bpp(grMipmap->format);
#ifdef PS2
	if (try)
	{
		block = Z_TryMallocAlign((size_t)blocksize*bpp, PU_HWRCACHE, &(grMipmap->data), sizeof (void *));
		if (!block)
			return NULL;
	}
	else
#else
	(void)try;
#endif
	block = Z_Malloc(blocksize*bpp, PU_HWRCACHE, &(grMipmap->data));

	switch (bpp)
	{
		case 1: memset(block, HWR_PATCHES_CHROMAKEY_COLORINDEX, blocksize); break;
		case 2:
				// fill background with chromakey, alpha = 0
#ifdef PS2_PROFILE
				{
					// OPT10 (HT): four texels per store (when block is 8 byte aligned; the tail and the unaligned case go texel by texel)
					const UINT64 pat = (UINT64)bu16 * 0x0001000100010001ull;
					UINT64 *q = (UINT64 *)(void *)block;

					i = 0;
					if (((size_t)block & 7u) == 0)
						for (; i + 4 <= blocksize; i += 4)
							*q++ = pat;
					for (; i < blocksize; i++)
						memcpy(block+i*sizeof(UINT16), &bu16, sizeof(UINT16));
				}
#else
				for (i = 0; i < blocksize; i++)
				//[segabor]
					memcpy(block+i*sizeof(UINT16), &bu16, sizeof(UINT16));
#endif
				break;
		case 4: memset(block, 0x00, blocksize*sizeof(UINT32)); break;
	}

	return block;
}

static UINT8 *MakeBlock(GLMipmap_t *grMipmap)
{
	return MakeBlockEx(grMipmap, false);
}

#ifdef PS2
// PS2-140 (OPT10-S): the picture of one entry of a composite texture, built as HWR_GenerateTexture below builds it but for lumps of this size
// and more, and NULL when the zone has no room for the lump or the patch (the texture is then made without that patch: a patch of 585 KB..1 MB
// ended the run on the Match maps M3 and MG when the rest of the arena was taken). The raw lump is a scratch copy, freed before the patch is
// used, and only its first bytes are read to find the format; *dispose: the caller frees the patch after drawing it (else it stays cached).
#define HWR_TRYPATCH_MIN (64u<<10)

static patch_t *HWR_TryTexturePatch(const texture_t *texture, UINT16 wadnum, lumpnum_t lumpnum, boolean *dispose)
{
	const size_t lumplength = W_LumpLengthPwad(wadnum, lumpnum);
	UINT8 head[PNG_HEADER_SIZE + 8];
	UINT8 *pdata;
	patch_t *realpatch = NULL;

	*dispose = true;
	memset(head, 0, sizeof head);
	W_ReadLumpHeaderPwad(wadnum, lumpnum, head, lumplength < sizeof head ? lumplength : sizeof head, 0);

	if (texture->type != TEXTURETYPE_FLAT && !Picture_IsLumpPNG(head, lumplength))
	{
		// a Doom patch: used from the patch cache when it is there, else loaded and not kept (see PS2-146 below)
		realpatch = W_GetCachedPatchNumPwad(wadnum, lumpnum);
		if (realpatch)
		{
			*dispose = false;
			return realpatch;
		}
		return W_TryCachePatchNumPwad(wadnum, lumpnum, PU_PATCH);
	}

	pdata = Z_TryMallocAlign(lumplength ? lumplength : 1, PU_RENDERWORK, NULL, sizeof (void *));
	if (!pdata)
		return NULL;
	W_ReadLumpHeaderPwad(wadnum, lumpnum, pdata, 0, 0);
	if (Picture_IsLumpPNG(pdata, lumplength))
	{
#ifdef PS2_PROFILE
		if (Picture_IsLumpCooked(pdata, lumplength))
			realpatch = (patch_t *)Picture_TryCookedPatch(pdata, lumplength);
		else
#endif
			realpatch = (patch_t *)Picture_PNGConvert(pdata, PICFMT_PATCH, NULL, NULL, NULL, NULL, lumplength, NULL, 0);
	}
	else
		realpatch = (patch_t *)Picture_Convert(PICFMT_FLAT, pdata, PICFMT_PATCH, 0, NULL, texture->width, texture->height, 0, 0, 0);
	Z_Free(pdata);
	return realpatch;
}
#endif

//
// Create a composite texture from patches, adapt the texture size to a power of 2
// height and width for the hardware texture cache.
//
static void HWR_GenerateTexture(INT32 texnum, GLMapTexture_t *grtex, GLMipmap_t *mipmap)
{
	UINT8 *block;
	texture_t *texture;
	texpatch_t *patch;
	INT32 blockwidth, blockheight, blocksize;
#ifdef PS2
	INT32 missing; // PS2-140: patches that could not be read for lack of memory
#endif
#ifdef PS2_PROFILE
	const unsigned int gt0 = ps2hwt_now(); // OPT13 IZ (PS2-603): what making this texture cost, for the eviction (Z_SetRebuildCost)
	ps2_cpatch_t held[PS2_CPATCH_MAX]; // OPT13 RDRV: patches read for this texture, kept until it is composed
	INT32 nheld = 0, h;
#endif

	INT32 i;

	texture = textures[texnum];

	blockwidth = texture->width;
	blockheight = texture->height;
	blocksize = blockwidth * blockheight;
#if defined(PS2) && defined(PS2_PROFILE)
	// OPT13 IZ (PS2-602, R2): the pixels were made by the cooker from the same patches (TEXC.PAK): a decode of a stored LZ4 block instead of the composition below (4..28 M cycles, 260..320 M
	// when the zone dropped the patches). A texture the pack has no composite of for this definition (an add-on's, patches from other files) goes the way it always did.
	if (!ps2_slow_composite && mipmap->format == GL_TEXFMT_P_8 && PS2TexC_Present())
	{
		UINT8 *tb = Z_TryMallocAlign((size_t)blocksize, PU_HWRCACHE, &(mipmap->data), sizeof (void *));

		if (tb)
		{
			if (PS2TexC_Fetch(texnum, tb, (size_t)blocksize))
			{
				if (ps2hwd_dbg_flags & 0x1000000 /* HWDBG_COMPOSE */)
				{
					// the check: the same texture composed by the original column loops against the stored pixels
					static unsigned checked, bad;
					GLMipmap_t chk = *mipmap;
					GLMapTexture_t tmp = *grtex;

					chk.data = NULL;
					ps2_slow_composite = true;
					HWR_GenerateTexture(texnum, &tmp, &chk);
					ps2_slow_composite = false;
					checked++;
					if (!chk.data || memcmp(chk.data, tb, (size_t)blocksize))
					{
						bad++;
						CONS_Printf("HWC TEXC MISMATCH texture %d %.8s %dx%d (%u of %u checked differ)\n", (int)texnum, texture->name, (int)texture->width, (int)texture->height, bad, checked);
					}
					else if (!(checked & 63))
						CONS_Printf("HWC TEXC check: %u textures identical, %u differ\n", checked - bad, bad);
					Z_Free(chk.data);
				}
				grtex->scaleX = 1.0f/(texture->width*FRACUNIT);
				grtex->scaleY = 1.0f/(texture->height*FRACUNIT);
				Z_SetRebuildCost(&mipmap->data, ps2hwt_now() - gt0);
				return;
			}
			Z_Free(tb); // (the owner pointer, mipmap->data, is NULL again)
		}
	}
#endif
#ifdef PS2
	// PS2-140: a texture of 64 KB and more that does not fit has no data: the driver skips the draws that need it for a frame and the engine asks again
	block = MakeBlockEx(mipmap, (size_t)blocksize * format2bpp(mipmap->format) >= HWR_TRYPATCH_MIN);
	missing = 0;
	if (!block)
	{
		static unsigned ps2_noblock_reports;

		if (ps2_noblock_reports++ < 16)
			CONS_Alert(CONS_WARNING, "no room for texture %.8s (%dx%d)\n", texture->name, (int)texture->width, (int)texture->height);
		grtex->scaleX = 1.0f/(texture->width*FRACUNIT);
		grtex->scaleY = 1.0f/(texture->height*FRACUNIT);
		return;
	}
#else
	block = MakeBlock(mipmap);
#endif

	// Composite the columns together.
	for (i = 0, patch = texture->patches; i < texture->patchcount; i++, patch++)
	{
		UINT16 wadnum = patch->wad;
		lumpnum_t lumpnum = patch->lump;
#ifdef PS2
		if (W_LumpLengthPwad(wadnum, lumpnum) >= HWR_TRYPATCH_MIN)
		{
			boolean dispose;
			patch_t *tp = HWR_TryTexturePatch(texture, wadnum, lumpnum, &dispose);

			if (tp)
			{
				HWR_DrawTexturePatchInCache(mipmap, blockwidth, blockheight, texture, patch, tp);
				if (dispose)
					Patch_Free(tp);
			}
			else
				missing++;
			continue;
		}
#endif
#ifdef PS2_PROFILE
		// OPT13 RDRV: the raw lump is read here for a flat only (Picture_Convert below); for a Doom patch it was read in full (4 M cycles when the zone had dropped it) and never looked at
		// (W_CachePatchNumPwad reads the lump again, and W_GetPatchPwad converts a PNG lump itself, with the same call as the branch below)
		UINT8 *pdata = (ps2hwt_comp_old || texture->type == TEXTURETYPE_FLAT) ? W_CacheLumpNumPwad(wadnum, lumpnum, PU_CACHE) : NULL;
#else
		UINT8 *pdata = W_CacheLumpNumPwad(wadnum, lumpnum, PU_CACHE);
#endif
		patch_t *realpatch = NULL;
		boolean free_patch = true;
#ifdef PS2_PROFILE
		boolean loaded_here = false;
#endif

#ifndef NO_PNG_LUMPS
		size_t lumplength = W_LumpLengthPwad(wadnum, lumpnum);
		if (pdata && Picture_IsLumpPNG(pdata, lumplength))
			realpatch = (patch_t *)Picture_PNGConvert(pdata, PICFMT_PATCH, NULL, NULL, NULL, NULL, lumplength, NULL, 0);
		else
#endif
		if (texture->type == TEXTURETYPE_FLAT)
			realpatch = (patch_t *)Picture_Convert(PICFMT_FLAT, pdata, PICFMT_PATCH, 0, NULL, texture->width, texture->height, 0, 0, 0);
		else
		{
			// If this patch has already been loaded, we just use it from the cache.
			realpatch = W_GetCachedPatchNumPwad(wadnum, lumpnum);
			free_patch = false;

			// Otherwise, we load it here.
			if (realpatch == NULL)
			{
#ifdef PS2_PROFILE
				if (!ps2hwt_comp_old)
				{
					// OPT13 RDRV: a patch placed again in the same texture is built once (THROCK4: 81 placements of 23 patches, SKY4: 151 placements), and no GL structure is made
					// for a patch that is only composed (W_CachePatchNumPwad runs Patch_CreateGL, which allocates the GL patch and mipmap and Patch_Free frees them again)
					for (h = 0; h < nheld; h++)
						if (held[h].wad == wadnum && held[h].lump == lumpnum)
						{
							realpatch = held[h].p;
							break;
						}
					if (!realpatch)
					{
						realpatch = W_CachePatchNumPwadNoGL(wadnum, lumpnum, PU_PATCH);
						if (realpatch && nheld < PS2_CPATCH_MAX)
						{
							held[nheld].wad = wadnum;
							held[nheld].lump = lumpnum;
							held[nheld].p = realpatch;
							nheld++;
						}
						else
							loaded_here = true; // no room in the list: freed after this patch as before
					}
				}
				else
				{
					realpatch = W_CachePatchNumPwad(wadnum, lumpnum, PU_PATCH);
					loaded_here = true;
				}
#else
				realpatch = W_CachePatchNumPwad(wadnum, lumpnum, PU_PATCH);
#endif
			}
		}

		HWR_DrawTexturePatchInCache(mipmap, blockwidth, blockheight, texture, patch, realpatch);

		if (free_patch)
			Patch_Free(realpatch);
#ifdef PS2_PROFILE
		// PS2-146 (OPT10-S): a patch that was read only to be composed into this texture is not kept: as PU_PATCH ("static for the whole run") the
		// wall patches of every texture the player has seen stayed in the arena (2.7 MB in 198 blocks at frame 323 of MAP10, hardware renderer, where
		// the arena then ran out); a texture is composed again only after the GS pool or the texture cache dropped it
		else if (loaded_here)
			Patch_Free(realpatch);
#endif
	}
#ifdef PS2_PROFILE
	for (h = 0; h < nheld; h++)
		Patch_Free(held[h].p); // OPT13 RDRV: the patches this texture was composed from are not kept (PS2-146: the arena would fill with the patches of every texture seen)
#endif
#ifdef PS2
	if (missing)
	{
		// the texture is made of what could be read; with nothing read it is a flat grey (the palette ramp: 0 white .. 31 black), never the
		// chroma key (a wall that is not there) and never a stale block. The zone may drop the block; the next request tries again.
		static unsigned ps2_missing_reports;

		if (missing == texture->patchcount && format2bpp(mipmap->format) == 1)
			memset(block, 15, (size_t)blocksize);
		if (ps2_missing_reports++ < 16)
			CONS_Alert(CONS_WARNING, "no room for %d of %d patch(es) of texture %.8s (%dx%d)\n", (int)missing, (int)texture->patchcount, texture->name, (int)texture->width, (int)texture->height);
	}
#endif
	//Hurdler: not efficient at all but I don't remember exactly how HWR_DrawPatchInCache works :(
	if (format2bpp(mipmap->format)==4)
	{
		for (i = 3; i < blocksize*4; i += 4) // blocksize*4 because blocksize doesn't include the bpp
		{
			if (block[i] == 0)
			{
#ifdef PS2_PROFILE
				if (!(mipmap->flags & TF_TRANSPARENT))
					hwr_texsig++; // OPT13 IR
#endif
				mipmap->flags |= TF_TRANSPARENT;
				break;
			}
		}
	}

#ifdef PS2_PROFILE
	if (!ps2_slow_composite && (ps2hwd_dbg_flags & 0x1000000) /* HWDBG_COMPOSE */ && mipmap->format == GL_TEXFMT_P_8)
	{
		// the same texture composed by the original loops: the bytes must be the same
		static unsigned checked, bad;
		GLMipmap_t chk = *mipmap;
		GLMapTexture_t tmp = *grtex;

		chk.data = NULL;
		{
			const boolean svold = ps2hwt_comp_old;

			ps2_slow_composite = true;
			ps2hwt_comp_old = true; // OPT13 RDRV: the check composes with the original patch handling as well
			HWR_GenerateTexture(texnum, &tmp, &chk);
			ps2hwt_comp_old = svold;
			ps2_slow_composite = false;
		}
		checked++;
		if (!chk.data || memcmp(chk.data, mipmap->data, (size_t)blocksize))
		{
			bad++;
			CONS_Printf("HWC composite MISMATCH texture %d %.8s %dx%d (%u of %u checked differ)\n", (int)texnum, texture->name, (int)texture->width, (int)texture->height, bad, checked);
		}
		else if (!(checked & 63))
		{
			CONS_Printf("HWC composite check: %u textures identical, %u differ\n", checked - bad, bad);
		}
		Z_Free(chk.data);
	}
#endif
	grtex->scaleX = 1.0f/(texture->width*FRACUNIT);
	grtex->scaleY = 1.0f/(texture->height*FRACUNIT);
#ifdef PS2
	if (!ps2_slow_composite && mipmap->data)
		Z_SetRebuildCost(&mipmap->data, ps2hwt_now() - gt0); // OPT13 IZ (PS2-603): Z_MakeRoom frees the cheap textures before this one
#endif
}

// patch may be NULL if grMipmap has been initialised already and makebitmap is false
void HWR_MakePatch (const patch_t *patch, GLPatch_t *grPatch, GLMipmap_t *grMipmap, boolean makebitmap)
{
	if (grMipmap->width == 0)
	{
		grMipmap->width = grMipmap->height = 1;
		while (grMipmap->width < patch->width) grMipmap->width <<= 1;
		while (grMipmap->height < patch->height) grMipmap->height <<= 1;

		// no wrap around, no chroma key
		grMipmap->flags = 0;

		// setup the texture info
		grMipmap->format = patchformat;

		grPatch->max_s = (float)patch->width / (float)grMipmap->width;
		grPatch->max_t = (float)patch->height / (float)grMipmap->height;
#ifdef PS2_PROFILE
		grMipmap->ps2_uw = (UINT16)patch->width; // PS2-HW-39: the GS driver stores only the real part of the power of two block
		grMipmap->ps2_uh = (UINT16)patch->height;
#endif
	}

	Z_Free(grMipmap->data);
	grMipmap->data = NULL;

	if (makebitmap)
	{
#ifdef PS2_PROFILE
		unsigned int t0 = ps2hwt_now();
#endif
#ifdef PS2
		// PS2-140: a patch of 64 KB and more as a texture (a 1024x512 one is 1 MB) that does not fit has no data: the driver skips the draws that need it
		if (!MakeBlockEx(grMipmap, (size_t)grMipmap->width * grMipmap->height * format2bpp(grMipmap->format) >= HWR_TRYPATCH_MIN))
		{
			static unsigned ps2_nopatch_reports;

			if (ps2_nopatch_reports++ < 16)
				CONS_Alert(CONS_WARNING, "no room for a patch texture of %dx%d\n", (int)grMipmap->width, (int)grMipmap->height);
			return;
		}
#else
		MakeBlock(grMipmap);
#endif

		HWR_DrawPatchInCache(grMipmap,
			grMipmap->width, grMipmap->height,
			patch->width, patch->height,
			patch);
#ifdef PS2_PROFILE
		ps2hwt_mkpatch_n++;
		ps2hwt_mkpatch_cyc += ps2hwt_now() - t0;
		if ((ps2hwd_dbg_flags & 0x1000000) /* HWDBG_COMPOSE */ && !ps2_slow_composite && format2bpp(grMipmap->format) <= 2)
		{
			// the same patch by the original column loops: the bytes must be the same
			static unsigned checked, bad;
			GLMipmap_t chk = *grMipmap;
			size_t bytes = (size_t)grMipmap->width * grMipmap->height * format2bpp(grMipmap->format);

			chk.data = NULL;
			ps2_slow_composite = true;
			MakeBlock(&chk);
			HWR_DrawPatchInCache(&chk, chk.width, chk.height, patch->width, patch->height, patch);
			ps2_slow_composite = false;
			checked++;
			if (!chk.data || memcmp(chk.data, grMipmap->data, bytes))
			{
				bad++;
				CONS_Printf("HWC patch composite MISMATCH %dx%d (patch %dx%d) colormap %d (%u of %u checked differ)\n", (int)grMipmap->width, (int)grMipmap->height, (int)patch->width, (int)patch->height,
					grMipmap->colormap != NULL, bad, checked);
			}
			else if (!(checked & 31))
			{
				CONS_Printf("HWC patch composite check: %u patches identical, %u differ\n", checked - bad, bad);
			}
			Z_Free(chk.data);
		}
#endif
	}
}


// =================================================
//             CACHING HANDLING
// =================================================

static size_t gl_numtextures = 0; // Texture count
static GLMapTexture_t *gl_textures; // For all textures
static GLMapTexture_t *gl_flats; // For all (texture) flats, as normal flats don't need to be cached
boolean gl_maptexturesloaded = false;

#ifdef PS2_PROFILE
UINT32 hwr_texsig = 1; // OPT13 IR: a number that changes whenever a word of HWR_PS2_SideTexWord may have (texture animation: p_spec.c, textures added: r_textures.c, TF_TRANSPARENT set: here and in the driver)

// OPT11 (PS2-HW-80): ProcessSeg tests TF_TRANSPARENT of the (translated) texture's mipmap, which the driver sets when it makes the texture resident:
// an input of the geometry cache key. tex is a translated texture number (R_GetTextureNum).
UINT32 HWR_PS2_TexTransparent(INT32 tex)
{
	if (!gl_textures || tex < 0 || tex >= (signed)gl_numtextures)
		return 0;
	return (gl_textures[tex].mipmap.flags & TF_TRANSPARENT) ? 1u : 0u;
}

// The word of a side texture in the key of the cache: the translated number (texture animation) and the TF_TRANSPARENT bit of its mipmap; 0 for no texture. One call for
// what were two (R_GetTextureNum, HWR_PS2_TexTransparent) with their bounds tests. raw: the number in the sidedef.
UINT32 HWR_PS2_SideTexWord(INT32 raw)
{
	INT32 t;

	if (raw < 0 || raw >= numtextures)
		return 0;
	t = texturetranslation[raw];
	if (!t)
		return 0;
	return (UINT32)t | ((gl_textures && t > 0 && t < (signed)gl_numtextures && (gl_textures[t].mipmap.flags & TF_TRANSPARENT)) ? 0x80000000u : 0u);
}
#endif

void HWR_FreeTextureData(patch_t *patch)
{
	GLPatch_t *grPatch;

	if (!patch || !patch->hardware)
		return;

	grPatch = patch->hardware;

	if (vid.glstate == VID_GL_LIBRARY_LOADED)
		HWD.pfnDeleteTexture(grPatch->mipmap);
	if (grPatch->mipmap->data)
		Z_Free(grPatch->mipmap->data);
}

void HWR_FreeTexture(patch_t *patch)
{
	if (!patch)
		return;

	if (patch->hardware)
	{
		GLPatch_t *grPatch = patch->hardware;

		HWR_FreeTextureColormaps(patch);

		if (grPatch->mipmap)
		{
			HWR_FreeTextureData(patch);
			Z_Free(grPatch->mipmap);
		}

		Z_Free(patch->hardware);
	}

	patch->hardware = NULL;
}

// Called by HWR_FreePatchCache.
void HWR_FreeTextureColormaps(patch_t *patch)
{
	GLPatch_t *pat;

	// The patch must be valid, obviously
	if (!patch)
		return;

	pat = patch->hardware;
	if (!pat)
		return;

	// The mipmap must be valid, obviously
	while (pat->mipmap)
	{
		// Confusing at first, but pat->mipmap->nextcolormap
		// at the beginning of the loop is the first colormap
		// from the linked list of colormaps.
		GLMipmap_t *next = NULL;

		// No mipmap in this patch, break out of the loop.
		if (!pat->mipmap)
			break;

		// No colormap mipmaps either.
		if (!pat->mipmap->nextcolormap)
			break;

		// Set the first colormap to the one that comes after it.
		next = pat->mipmap->nextcolormap;
		pat->mipmap->nextcolormap = next->nextcolormap;

		// Free image data from memory.
		if (next->data)
			Z_Free(next->data);
		if (next->colormap)
			Z_Free(next->colormap);
		HWD.pfnDeleteTexture(next);

		// Free the old colormap mipmap from memory.
		free(next);
	}
}

static boolean FreeTextureCallback(void *mem)
{
	patch_t *patch = (patch_t *)mem;
	HWR_FreeTexture(patch);
	return false;
}

static boolean FreeColormapsCallback(void *mem)
{
	patch_t *patch = (patch_t *)mem;
	HWR_FreeTextureColormaps(patch);
	return false;
}

static void HWR_FreePatchCache(boolean freeall)
{
	boolean (*callback)(void *mem) = FreeTextureCallback;

	if (!freeall)
		callback = FreeColormapsCallback;

	Z_IterateTags(PU_PATCH, PU_PATCH_ROTATED, callback);
	Z_IterateTags(PU_SPRITE, PU_HUDGFX, callback);
}

// free all textures after each level
void HWR_ClearAllTextures(void)
{
#ifdef PS2_PROFILE
	HWR_GCacheFlush(); // OPT11 (PS2-HW-80): the cached polygons name texture records
#endif
	HWD.pfnClearMipMapCache(); // free references to the textures
	HWR_FreePatchCache(true);
#ifdef PS2_PROFILE
	HWR_ReleaseBatching(); // PS2-HW-79
#endif
}

void HWR_FreeColormapCache(void)
{
	HWR_FreePatchCache(false);
}

void HWR_InitMapTextures(void)
{
	gl_textures = NULL;
	gl_flats = NULL;
	gl_maptexturesloaded = false;
}

static void DeleteTextureMipmap(GLMipmap_t *grMipmap, boolean delete_mipmap)
{
	HWD.pfnDeleteTexture(grMipmap);

	if (delete_mipmap)
		Z_Free(grMipmap->data);
}

static void FreeMapTexture(GLMapTexture_t *tex, boolean delete_chromakeys)
{
	if (tex->mipmap.nextcolormap)
	{
		DeleteTextureMipmap(tex->mipmap.nextcolormap, delete_chromakeys);
		free(tex->mipmap.nextcolormap);
		tex->mipmap.nextcolormap = NULL;
	}

	DeleteTextureMipmap(&tex->mipmap, true);
}

void HWR_FreeMapTextures(void)
{
	size_t i;

#ifdef PS2_PROFILE
	HWR_GCacheFlush(); // OPT11 (PS2-HW-80): the texture records the cached polygons point to are freed below
#endif

	for (i = 0; i < gl_numtextures; i++)
	{
		FreeMapTexture(&gl_textures[i], true);
		FreeMapTexture(&gl_flats[i], false);
	}

	// now the heap don't have any 'user' pointing to our
	// texturecache info, we can free it
	if (gl_textures)
		free(gl_textures);
	if (gl_flats)
		free(gl_flats);
	gl_textures = NULL;
	gl_flats = NULL;
	gl_numtextures = 0;
	gl_maptexturesloaded = false;
}

void HWR_LoadMapTextures(size_t pnumtextures)
{
	// we must free it since numtextures may have changed
	HWR_FreeMapTextures();
#if defined(PS2) && defined(PS2_PROFILE)
	PS2TexC_Reset(); // OPT13 IZ (PS2-602): the stored composites are looked up by texture number
	Z_ClearRebuildCosts(); // OPT13 IZ (PS2-603): the owners of the old list are gone
#endif

	gl_numtextures = pnumtextures;
	gl_textures = calloc(gl_numtextures, sizeof(*gl_textures));
	gl_flats = calloc(gl_numtextures, sizeof(*gl_flats));
#ifdef PS2_PROFILE
	{
		// -hwfbtex N (test of the guard below): the N-th call (1 = the first) finds no memory for the tables
		static int failtex = -1, ncalls;

		if (failtex < 0)
			failtex = (M_CheckParm("-hwfbtex") && M_IsNextParm()) ? atoi(M_GetNextParm()) : 0;
		if (failtex && ++ncalls == failtex)
		{
			free(gl_flats);
			gl_flats = NULL;
		}
	}
#endif

	if (gl_textures == NULL || gl_flats == NULL)
	{
#ifdef PS2
		// PS2-HW-446 (OPT12 HWDRV): under the guard of ps2_hwfb.c (the hardware part of a level load, the first frame of a renderer switch) this is not the end of the game: the
		// tables are given back and the level goes on in software (the stab_run.sh hwfb chain ended here, at map 23, with the start before this change as well)
		free(gl_textures);
		free(gl_flats);
		gl_textures = gl_flats = NULL;
		gl_numtextures = 0;
		Z_GuardThrow("HWR_LoadMapTextures: ran out of memory for OpenGL textures"); // (no return when a guard is armed)
#endif
		I_Error("HWR_LoadMapTextures: ran out of memory for OpenGL textures");
	}

	gl_maptexturesloaded = true;
}

// --------------------------------------------------------------------------
// Make sure texture is downloaded and set it as the source
// --------------------------------------------------------------------------
#ifdef PS2_PROFILE
// PS2-HW-16: the GS pool (3 MiB) cannot hold the textures of a whole frame (GFZ1 asks for about 2.5 MiB of wall textures alone), so a
// texture is made resident when it is DRAWN, not when it is selected. While polygons are batched only the selection is recorded
// (HWR_SetCurrentTexture); HWR_RenderBatches calls SetTexture when a batch is drawn, and the driver asks for the data again
// (HWR_PS2_RegenerateMipmap) when the zone has dropped it. Without batching the texture is uploaded right away, as before.
GLMapTexture_t *HWR_GetTexture(INT32 tex, boolean chromakeyed)
{
	GLMapTexture_t *grtex;
	GLMipmap_t *grMipmap, *originalMipmap;

	if (tex < 0 || tex >= (signed)gl_numtextures)
		tex = 0;

	grtex = &gl_textures[tex];
	grMipmap = originalMipmap = &grtex->mipmap;

	if (!originalMipmap->downloaded)
	{
#ifdef PS2_PROFILE
		if (originalMipmap->flags & TF_TRANSPARENT)
			hwr_texsig++; // OPT13 IR: the flag is made again when the texture is next made resident
#endif
		originalMipmap->flags = TF_WRAPXY;
		originalMipmap->width = (UINT16)textures[tex]->width;
		originalMipmap->height = (UINT16)textures[tex]->height;
		originalMipmap->format = textureformat;
	}
	grtex->scaleX = 1.0f/(textures[tex]->width*FRACUNIT);
	grtex->scaleY = 1.0f/(textures[tex]->height*FRACUNIT);
	originalMipmap->regen_kind = 1;
	originalMipmap->regen_id = tex;

	// If chroma-keyed, create or use a different mipmap for the variant
	if (chromakeyed && !textures[tex]->transparency)
	{
		if (!originalMipmap->nextcolormap)
		{
			GLMipmap_t *newMipmap = calloc(1, sizeof (*grMipmap));
			if (newMipmap == NULL)
				I_Error("%s: Out of memory", "HWR_GetTexture");

			newMipmap->flags = originalMipmap->flags | TF_CHROMAKEYED;
			newMipmap->width = originalMipmap->width;
			newMipmap->height = originalMipmap->height;
			newMipmap->format = originalMipmap->format;
			newMipmap->regen_kind = 1;
			newMipmap->regen_id = tex;
			newMipmap->ps2_twin = originalMipmap; // PS2-HW-30
			originalMipmap->nextcolormap = newMipmap;
		}
		grMipmap = originalMipmap->nextcolormap;
	}

	if (!grMipmap->downloaded && !currently_batching)
		HWD.pfnSetTexture(grMipmap); // PS2-HW-32: the driver asks for the texels (HWR_PS2_RegenerateMipmap) when its data cache has not got them
	HWR_SetCurrentTexture(grMipmap);

	Z_ChangeTag(grMipmap->data, PU_HWRCACHE_UNLOCKED);

	return grtex;
}
#else
GLMapTexture_t *HWR_GetTexture(INT32 tex, boolean chromakeyed)
{
	if (tex < 0 || tex >= (signed)gl_numtextures)
	{
#ifdef PARANOIA
		I_Error("HWR_GetTexture: Invalid texture ID %d", tex);
#else
		tex = 0;
#endif
	}

	GLMapTexture_t *grtex = &gl_textures[tex];

	GLMipmap_t *grMipmap = &grtex->mipmap;
	GLMipmap_t *originalMipmap = grMipmap;

	if (!originalMipmap->downloaded)
	{
#ifdef PS2_PROFILE
		if (originalMipmap->flags & TF_TRANSPARENT)
			hwr_texsig++; // OPT13 IR: the flag is made again when the texture is next made resident
#endif
		originalMipmap->flags = TF_WRAPXY;
		originalMipmap->width = (UINT16)textures[tex]->width;
		originalMipmap->height = (UINT16)textures[tex]->height;
		originalMipmap->format = textureformat;
	}

	// If chroma-keyed, create or use a different mipmap for the variant
	if (chromakeyed && !textures[tex]->transparency)
	{
		// Allocate it if it wasn't already
		if (!originalMipmap->nextcolormap)
		{
			GLMipmap_t *newMipmap = calloc(1, sizeof (*grMipmap));
			if (newMipmap == NULL)
				I_Error("%s: Out of memory", "HWR_GetTexture");

			newMipmap->flags = originalMipmap->flags | TF_CHROMAKEYED;
			newMipmap->width = originalMipmap->width;
			newMipmap->height = originalMipmap->height;
			newMipmap->format = originalMipmap->format;
			originalMipmap->nextcolormap = newMipmap;
		}

		// Generate, upload and bind the variant texture instead of the original one
		grMipmap = originalMipmap->nextcolormap;
	}

	if (!grMipmap->data)
	{
		HWR_GenerateTexture(tex, grtex, grMipmap);
#ifdef PS2_PROFILE // diagnostics of the GS pool budget (docs/GATES/g1/opt3-H.md)
		if (grMipmap->width * grMipmap->height >= 128 * 1024)
			CONS_Printf("HWC big texture %d %.8s %dx%d\n", (int)tex, textures[tex]->name, (int)grMipmap->width, (int)grMipmap->height);
#endif
	}

	if (!grMipmap->downloaded)
		HWD.pfnSetTexture(grMipmap);
	HWR_SetCurrentTexture(grMipmap);

	Z_ChangeTag(grMipmap->data, PU_HWRCACHE_UNLOCKED);

	return grtex;
}
#endif

static void HWR_CacheRawFlat(GLMipmap_t *grMipmap, lumpnum_t flatlumpnum)
{
	size_t size = W_LumpLength(flatlumpnum);
	UINT16 pflatsize = R_GetFlatSize(size);

	// setup the texture info
	grMipmap->format = GL_TEXFMT_P_8;
	grMipmap->flags = TF_WRAPXY;

	grMipmap->width = pflatsize;
	grMipmap->height = pflatsize;

	// the flat raw data needn't be converted with palettized textures
	W_ReadLump(flatlumpnum, Z_Malloc(size, PU_HWRCACHE, &grMipmap->data));
}

// Download a Doom 'flat' to the hardware cache and make it ready for use
void HWR_GetRawFlat(lumpnum_t flatlumpnum)
{
	GLMipmap_t *grmip;
	patch_t *patch;

	if (flatlumpnum == LUMPERROR)
		return;

	patch = HWR_GetCachedGLPatch(flatlumpnum);
	grmip = ((GLPatch_t *)Patch_AllocateHardwarePatch(patch))->mipmap;
	if (!grmip->downloaded && !grmip->data)
		HWR_CacheRawFlat(grmip, flatlumpnum);

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	if (!grmip->downloaded)
		HWD.pfnSetTexture(grmip);
	HWR_SetCurrentTexture(grmip);

	// The system-memory data can be purged now.
	Z_ChangeTag(grmip->data, PU_HWRCACHE_UNLOCKED);
}

static void MakeLevelFlatMipmap(GLMipmap_t *grMipmap, INT32 texturenum, UINT16 flags)
{
	grMipmap->format = GL_TEXFMT_P_8;
	grMipmap->flags = flags;

	grMipmap->width  = (UINT16)textures[texturenum]->width;
	grMipmap->height = (UINT16)textures[texturenum]->height;
}

#ifdef PS2_PROFILE
// PS2-HW-16: as HWR_GetTexture. The chroma keyed variant owns its copy of the pixels (the original only shared the pointer of the
// first, which the zone may drop under it).
void HWR_GetLevelFlat(levelflat_t *levelflat, boolean chromakeyed)
{
	INT32 texturenum;
	GLMapTexture_t *grtex;
	GLMipmap_t *grMipmap, *originalMipmap;

	if (levelflat->type == LEVELFLAT_NONE || levelflat->texture_id < 0)
	{
		HWR_SetCurrentTexture(NULL);
		return;
	}

	texturenum = texturetranslation[levelflat->texture_id];
	grtex = &gl_flats[texturenum];
	grMipmap = originalMipmap = &grtex->mipmap;

	if (!originalMipmap->downloaded)
		MakeLevelFlatMipmap(originalMipmap, texturenum, TF_WRAPXY);
	originalMipmap->regen_kind = 2;
	originalMipmap->regen_id = texturenum;

	if (chromakeyed)
	{
		if (!originalMipmap->nextcolormap)
		{
			GLMipmap_t *newMipmap = calloc(1, sizeof (*grMipmap));
			if (newMipmap == NULL)
				I_Error("%s: Out of memory", "HWR_GetLevelFlat");
			MakeLevelFlatMipmap(newMipmap, texturenum, TF_WRAPXY | TF_CHROMAKEYED);
			newMipmap->regen_kind = 2;
			newMipmap->regen_id = texturenum;
			newMipmap->ps2_twin = originalMipmap; // PS2-HW-30
			originalMipmap->nextcolormap = newMipmap;
		}
		grMipmap = originalMipmap->nextcolormap;
	}

	if (!grMipmap->downloaded && !currently_batching)
		HWD.pfnSetTexture(grMipmap); // PS2-HW-32: as HWR_GetTexture
	HWR_SetCurrentTexture(grMipmap);

	Z_ChangeTag(grMipmap->data, PU_HWRCACHE_UNLOCKED);
}

void HWR_PS2_RegenerateMipmap(GLMipmap_t *m)
{
	if (m->data)
		return;
	if (m->regen_kind == 1)
	{
		if (m->regen_id >= 0 && (size_t)m->regen_id < gl_numtextures && gl_textures)
			HWR_GenerateTexture(m->regen_id, &gl_textures[m->regen_id], m);
	}
	else if (m->regen_kind == 2)
	{
		if (m->regen_id >= 0 && (size_t)m->regen_id < gl_numtextures)
		{
			// PS2-140: no room for the engine's flat or for this copy of it: no data, the driver skips the draws that need the texture
			size_t size = (size_t)m->width * m->height;
			const UINT8 *src = R_TryGetFlatForTexture((size_t)m->regen_id);
			void *dst;

			if (!src)
				return;
			dst = Z_TryMallocAlign(size, PU_HWRCACHE, &m->data, sizeof (void *));
			if (!dst)
				return;
			memcpy(dst, src, size);
		}
	}
}

#if defined(PS2) && defined(PS2_PROFILE)
// OPT13 IZ (PS2-602): the composition of texture `texnum` by the original loops, for PS2TexC_Check (-texccheck): into dest, bytes = width * height
static boolean HWR_PS2_ComposeForCheck(INT32 texnum, UINT8 *dest, size_t bytes)
{
	GLMipmap_t chk;
	GLMapTexture_t tmp;
	boolean ok;

	Z_FlushCache(); // (the level load, nothing held: the lump reads of the original composition are PU_CACHE blocks of this very frame, which the zone would not evict)
	memset(&chk, 0, sizeof chk);
	memset(&tmp, 0, sizeof tmp);
	chk.format = GL_TEXFMT_P_8;
	chk.width = (UINT16)textures[texnum]->width;
	chk.height = (UINT16)textures[texnum]->height;
	ps2_slow_composite = true;
	HWR_GenerateTexture(texnum, &tmp, &chk);
	ps2_slow_composite = false;
	ok = chk.data && (size_t)chk.width * chk.height == bytes;
	if (ok)
		memcpy(dest, chk.data, bytes);
	Z_Free(chk.data);
	return ok;
}

// OPT13 IZ (PS2-603, RF-3): the textures near the start of a level are made before the first frame (the screen is still the loading screen), nearest first, the sky before them, within a budget
// of EE cycles (-hwwarm M: M million, 0 off) and while the arena has room: the walls the player sees in the first seconds were composed in the middle of frames 54..80 of the demos (47..101 M
// cycles, 0.16..0.34 s). The data are ordinary cache blocks (tag change as HWR_GetTexture does it): the zone takes them back, oldest first, when it needs the room, and the driver asks for
// what was taken the way it always did. Nothing is uploaded here: which textures reach the GS is the frame plan's business.
static INT32 warm_budget = -1;

typedef struct { INT32 tex; UINT32 dist; } warm_t;

static int HWR_WarmCmp(const void *a, const void *b)
{
	const warm_t *x = a, *y = b;

	return x->dist < y->dist ? -1 : x->dist > y->dist ? 1 : 0;
}

static void HWR_WarmTexture(INT32 tex)
{
	GLMapTexture_t *grtex;
	GLMipmap_t *m;

	if (!gl_textures || tex < 0 || (size_t)tex >= gl_numtextures || !textures[tex])
		return;
	grtex = &gl_textures[tex];
	m = &grtex->mipmap;
	if (m->data || m->downloaded)
		return;
	m->flags = TF_WRAPXY;
	m->width = (UINT16)textures[tex]->width;
	m->height = (UINT16)textures[tex]->height;
	m->format = textureformat;
	m->regen_kind = 1;
	m->regen_id = tex;
	HWR_GenerateTexture(tex, grtex, m);
	if (m->data)
		Z_ChangeTag(m->data, PU_HWRCACHE_UNLOCKED);
}

static void HWR_PS2_WarmLevel(void)
{
	warm_t *list;
	UINT8 *seen;
	size_t i, n = 0;
	INT32 px = 0, py = 0, done = 0;
	const unsigned int t0 = ps2hwt_now();
	unsigned int spent;

	if (warm_budget < 0)
		warm_budget = (M_CheckParm("-hwwarm") && M_IsNextParm()) ? atoi(M_GetNextParm()) : 0;
	if (!warm_budget || !gl_textures || !gl_numtextures || !numlines)
		return;
	list = Z_TryMallocAlign(gl_numtextures * sizeof *list, PU_RENDERWORK, NULL, 0);
	seen = Z_TryMallocAlign(gl_numtextures, PU_RENDERWORK, NULL, 0);
	if (!list || !seen)
	{
		Z_Free(list);
		Z_Free(seen);
		return;
	}
	memset(seen, 0, gl_numtextures);
	if (playerstarts[0])
	{
		px = playerstarts[0]->x;
		py = playerstarts[0]->y;
	}
	list[n].tex = skytexture; // the sky first
	list[n++].dist = 0;
	if (skytexture >= 0 && (size_t)skytexture < gl_numtextures)
		seen[skytexture] = 1;
	for (i = 0; i < numlines; i++)
	{
		const line_t *ln = &lines[i];
		const INT32 mx = ((ln->v1->x >> FRACBITS) + (ln->v2->x >> FRACBITS)) / 2, my = ((ln->v1->y >> FRACBITS) + (ln->v2->y >> FRACBITS)) / 2;
		const INT32 dx = mx - px, dy = my - py;
		const UINT32 dist = (UINT32)(((INT64)dx * dx + (INT64)dy * dy) >> 8) + 1u;
		int sd;

		for (sd = 0; sd < 2; sd++)
		{
			const side_t *sidep;
			INT32 t[3], k;

			if (ln->sidenum[sd] == 0xFFFF)
				continue;
			sidep = &sides[ln->sidenum[sd]];
			t[0] = sidep->toptexture;
			t[1] = sidep->midtexture;
			t[2] = sidep->bottomtexture;
			for (k = 0; k < 3; k++)
				if (t[k] > 0 && (size_t)t[k] < gl_numtextures && !seen[t[k]])
				{
					seen[t[k]] = 1;
					list[n].tex = t[k];
					list[n++].dist = dist;
				}
		}
	}
	qsort(list + 1, n - 1, sizeof *list, HWR_WarmCmp);
	for (i = 0; i < n; i++)
	{
		if ((ps2hwt_now() - t0) > (unsigned int)warm_budget * 1000000u || Z_ArenaFree() < Z_RenderHeadroom() + (3u << 20))
			break;
		HWR_WarmTexture(list[i].tex);
		done++;
	}
	spent = ps2hwt_now() - t0;
	CONS_Printf("HWWARM %d of %u textures made before the first frame, %u cycles (budget %d M), arena free %u K\n", (int)done, (unsigned)n, spent, (int)warm_budget, (unsigned)(Z_ArenaFree() >> 10));
	Z_Free(list);
	Z_Free(seen);
}

// P_LoadLevel, the hardware renderer is on: what the level needs of TEXC.PAK is read before the first frame (the screen is still the loading screen)
void HWR_PS2_PrefetchLevel(void)
{
	static int checked = -1;

	if (checked < 0)
		checked = M_CheckParm("-texccheck") != 0;
	if (checked > 0)
	{
		checked = 0;
		PS2TexC_Check(HWR_PS2_ComposeForCheck);
	}
	PS2TexC_PrefetchLevel();
	HWR_PS2_WarmLevel();
}
#endif

// OPT10 (PS2-HW-38): the mip levels of a big flat (the 1 MiB cloud planes) are made from the engine's own converted flat, pinned while the driver
// reads it: no second copy of 1 MiB (two of them at once ran the 22 MiB arena out of a contiguous 1 MiB block)
// PS2-HW-38: can the rows of this flat be read straight from its lump? (a raw flat of the size of the texture, not resident as the engine's flat: the levels
// of a 1 MiB cloud plane are then made from bands of 64 rows, and no 1 MiB block is ever needed - the arena of the full build has none to give)
boolean HWR_PS2_FlatStreamable(const GLMipmap_t *m)
{
	const texture_t *t;
	const texpatch_t *patch;

	if (m->regen_kind != 2 || m->regen_id < 0 || (size_t)m->regen_id >= gl_numtextures || !textures[m->regen_id])
		return false;
	t = textures[m->regen_id];
	if (t->flat != NULL || t->type != TEXTURETYPE_FLAT || t->patchcount < 1)
		return false;
	patch = &t->patches[0];
	return W_LumpLengthPwad(patch->wad, patch->lump) == (size_t)m->width * m->height;
}

boolean HWR_PS2_FlatRows(const GLMipmap_t *m, UINT32 row0, UINT32 nrows, UINT8 *dest)
{
	const texpatch_t *patch = &textures[m->regen_id]->patches[0];
	size_t bytes = (size_t)m->width * nrows;

	return W_ReadLumpHeaderPwad(patch->wad, patch->lump, dest, bytes, (size_t)row0 * m->width) == bytes;
}

const UINT8 *HWR_PS2_FlatPin(const GLMipmap_t *m)
{
	UINT8 *p;

	if (m->regen_kind != 2 || m->regen_id < 0 || (size_t)m->regen_id >= gl_numtextures)
		return NULL;
	p = R_TryGetFlatForTexture((size_t)m->regen_id); // PS2-140: NULL when a texture used as a flat does not fit (the draws are skipped)
	if (p)
		Z_ChangeTag(p, PU_STATIC);
	return p;
}

void HWR_PS2_FlatUnpin(const UINT8 *p, size_t bytes)
{
	// a 1 MiB plane would stay in the cache (and, touched in this frame, out of the zone's reach) for nothing: the levels made from it are
	// in the driver's data cache, it is read again when a level has to be made again
	if (bytes >= 512 * 1024)
		Z_Free((void *)p);
	else
		Z_ChangeTag((void *)p, PU_CACHE);
}

void HWR_PS2_ReleaseMipmapData(GLMipmap_t *m)
{
	Z_ChangeTag(m->data, PU_HWRCACHE_UNLOCKED);
}

// PS2-HW-32: zero-copy uploads: the GS driver sends the texels with DMA references, so the zone must not move or purge them until the
// transfer is done (the driver locks the block, and unlocks it when the GIF DMA of the packet that references it has completed)
void HWR_PS2_LockData(void *data)
{
	Z_ChangeTag(data, PU_HWRCACHE);
}

// OPT12 HWDRV (PS2-HW-442): the tag of the driver's data cache blocks between two uses is Z_HWCacheTag(bytes): PU_CACHE (evicted least recently used first, under pressure) while the arena has
// room, else the old PU_HWRCACHE_UNLOCKED (freed by the next allocation that does not fit). -hwkeep 1 (ps2_hwd.c) is the old rule everywhere.
void HWR_PS2_UnlockData(void *data)
{
	Z_ChangeTag(data, PU_HWRCACHE_UNLOCKED); // (Z_ChangeTag makes it a cache block while Z_HWCacheTag allows)
}

void HWR_PS2_FreeData(void *data)
{
	Z_Free(data);
}

// a purgable zone block owned by *newuser (the driver's data cache: decimated levels of a texture); NULL when the zone has no room
void *HWR_PS2_AllocData(size_t bytes, void **newuser)
{
	return Z_TryMallocAlign(bytes, Z_HWCacheTag(bytes), newuser, 6);
}

// The driver takes the texels of a mipmap over (its data cache): the block stays a purgable zone block, owned by *newuser from now on, and
// the engine's mipmap forgets it (it composes the texture again when it needs the texels, after the driver gave up its copy)
void *HWR_PS2_StealData(GLMipmap_t *m, void **newuser)
{
	void *p = m->data;

	m->data = NULL;
	Z_SetUser(p, newuser);
	Z_ChangeTag(p, PU_HWRCACHE_UNLOCKED);
	return p;
}

// name of the map texture / level flat behind a mipmap (driver diagnostics: -hwtrace)
const char *HWR_PS2_TexName(const GLMipmap_t *m)
{
	static char name[16];

	if (m->regen_kind && m->regen_id >= 0 && (size_t)m->regen_id < gl_numtextures && textures[m->regen_id])
	{
		snprintf(name, sizeof name, "%c:%.8s", m->regen_kind == 1 ? 'T' : 'F', textures[m->regen_id]->name);
		return name;
	}
	return "patch";
}
#else
void HWR_GetLevelFlat(levelflat_t *levelflat, boolean chromakeyed)
{
	if (levelflat->type == LEVELFLAT_NONE || levelflat->texture_id < 0)
	{
		HWR_SetCurrentTexture(NULL);
		return;
	}

	INT32 texturenum = texturetranslation[levelflat->texture_id];

	GLMapTexture_t *grtex = &gl_flats[texturenum];

	GLMipmap_t *grMipmap = &grtex->mipmap;
	GLMipmap_t *originalMipmap = grMipmap;

	if (!originalMipmap->downloaded)
		MakeLevelFlatMipmap(originalMipmap, texturenum, TF_WRAPXY);

	if (!originalMipmap->data)
	{
		size_t size = originalMipmap->width * originalMipmap->height;
		memcpy(Z_Malloc(size, PU_HWRCACHE, &originalMipmap->data), R_GetFlatForTexture(texturenum), size);
#ifdef PS2_PROFILE // diagnostics of the GS pool budget (docs/GATES/g1/opt3-H.md)
		if (size >= 128 * 1024)
			CONS_Printf("HWC big flat %d %.8s %dx%d\n", (int)texturenum, textures[texturenum]->name, (int)originalMipmap->width, (int)originalMipmap->height);
#endif
	}

	// If chroma-keyed, create or use a different mipmap for the variant
	if (chromakeyed)
	{
		if (!originalMipmap->data)
		{
			HWR_SetCurrentTexture(NULL);
			return;
		}

		// Allocate it if it wasn't already
		if (!originalMipmap->nextcolormap)
		{
			GLMipmap_t *newMipmap = calloc(1, sizeof (*grMipmap));
			if (newMipmap == NULL)
				I_Error("%s: Out of memory", "HWR_GetLevelFlat");
			MakeLevelFlatMipmap(newMipmap, texturenum, TF_WRAPXY | TF_CHROMAKEYED);
			originalMipmap->nextcolormap = newMipmap;
		}

		// Upload and bind the variant texture instead of the original one
		grMipmap = originalMipmap->nextcolormap;

		// Use the original texture's pixel data
		// It can just be a pointer to it, since the r_opengl backend deals with the pixels
		// that are supposed to be transparent.
		grMipmap->data = originalMipmap->data;
	}

	if (!grMipmap->downloaded)
		HWD.pfnSetTexture(grMipmap);
	HWR_SetCurrentTexture(grMipmap);

	HWR_PATCH_UNLOCKED(grMipmap->data);
}

#endif

// --------------------+
// HWR_LoadPatchMipmap : Generates a patch into a mipmap, usually the mipmap inside the patch itself
// --------------------+
static void HWR_LoadPatchMipmap(patch_t *patch, GLMipmap_t *grMipmap)
{
	GLPatch_t *grPatch = patch->hardware;
	if (!grMipmap->downloaded && !grMipmap->data)
		HWR_MakePatch(patch, grPatch, grMipmap, true);

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	if (!grMipmap->downloaded)
		HWD.pfnSetTexture(grMipmap);
	HWR_SetCurrentTexture(grMipmap);

	// The system-memory data can be purged now.
	HWR_PATCH_UNLOCKED(grMipmap->data);
}

// ----------------------+
// HWR_UpdatePatchMipmap : Updates a mipmap.
// ----------------------+
static void HWR_UpdatePatchMipmap(patch_t *patch, GLMipmap_t *grMipmap)
{
	GLPatch_t *grPatch = patch->hardware;
	HWR_MakePatch(patch, grPatch, grMipmap, true);

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	// If it does have the texture, then call pfnUpdateTexture to update it
	if (!grMipmap->downloaded)
		HWD.pfnSetTexture(grMipmap);
	else
		HWD.pfnUpdateTexture(grMipmap);
	HWR_SetCurrentTexture(grMipmap);

	// The system-memory data can be purged now.
	HWR_PATCH_UNLOCKED(grMipmap->data);
}

// -----------------+
// HWR_GetPatch     : Downloads a patch to the hardware cache and make it ready for use
// -----------------+
void HWR_GetPatch(patch_t *patch)
{
	if (!patch->hardware)
		Patch_CreateGL(patch);
	HWR_LoadPatchMipmap(patch, ((GLPatch_t *)patch->hardware)->mipmap);
}

// -------------------+
// HWR_GetMappedPatch : Same as HWR_GetPatch for sprite color
// -------------------+
void HWR_GetMappedPatch(patch_t *patch, const UINT8 *colormap)
{
	GLPatch_t *grPatch;
	GLMipmap_t *grMipmap, *newMipmap;

	if (!patch->hardware)
		Patch_CreateGL(patch);
	grPatch = patch->hardware;

	if (colormap == colormaps || colormap == NULL)
	{
		// Load the default (green) color in hardware cache
		HWR_GetPatch(patch);
		return;
	}

	// search for the mipmap
	// skip the first (no colormap translated)
	for (grMipmap = grPatch->mipmap; grMipmap->nextcolormap; )
	{
		grMipmap = grMipmap->nextcolormap;
		if (grMipmap->colormap && grMipmap->colormap->source == colormap)
		{
			if (memcmp(grMipmap->colormap->data, colormap, 256 * sizeof(UINT8)))
			{
				M_Memcpy(grMipmap->colormap->data, colormap, 256 * sizeof(UINT8));
				HWR_UpdatePatchMipmap(patch, grMipmap);
			}
			else
				HWR_LoadPatchMipmap(patch, grMipmap);
			return;
		}
	}
	// not found, create it!
	// If we are here, the sprite with the current colormap is not already in hardware memory

	//BP: WARNING: don't free it manually without clearing the cache of harware renderer
	//              (it have a liste of mipmap)
	//    this malloc is cleared in HWR_FreeColormapCache
	//    (...) unfortunately z_malloc fragment alot the memory :(so malloc is better
	newMipmap = calloc(1, sizeof (*newMipmap));
	if (newMipmap == NULL)
		I_Error("%s: Out of memory", "HWR_GetMappedPatch");
	grMipmap->nextcolormap = newMipmap;

	newMipmap->colormap = Z_Calloc(sizeof(*newMipmap->colormap), PU_HWRPATCHCOLMIPMAP, NULL);
	newMipmap->colormap->source = colormap;
	M_Memcpy(newMipmap->colormap->data, colormap, 256 * sizeof(UINT8));

	HWR_LoadPatchMipmap(patch, newMipmap);
}

void HWR_UnlockCachedPatch(GLPatch_t *gpatch)
{
	if (!gpatch)
		return;

	HWR_PATCH_UNLOCKED(gpatch->mipmap->data);
}

patch_t *HWR_GetCachedGLPatchPwad(UINT16 wadnum, UINT16 lumpnum)
{
	lumpcache_t *lumpcache = wadfiles[wadnum]->patchcache;
	if (!lumpcache[lumpnum])
	{
		void *ptr = Patch_Create(0, 0);
		Z_SetUser(ptr, &lumpcache[lumpnum]);
		Patch_AllocateHardwarePatch(ptr);
	}
	return (patch_t *)(lumpcache[lumpnum]);
}

patch_t *HWR_GetCachedGLPatch(lumpnum_t lumpnum)
{
	return HWR_GetCachedGLPatchPwad(WADFILENUM(lumpnum),LUMPNUM(lumpnum));
}

// Need to do this because they aren't powers of 2
static void HWR_DrawFadeMaskInCache(GLMipmap_t *mipmap, INT32 pblockwidth, INT32 pblockheight,
	lumpnum_t fademasklumpnum, UINT16 fmwidth, UINT16 fmheight)
{
	INT32 i,j;
	fixed_t posx, posy, stepx, stepy;
	UINT8 *block = mipmap->data; // places the data directly into here
	UINT8 *flat;
	UINT8 *dest, *src, texel;
	RGBA_t col;
	RGBA_t *palette = HWR_GetTexturePalette();

	// Place the flats data into flat
	W_ReadLump(fademasklumpnum, Z_Malloc(W_LumpLength(fademasklumpnum),
		PU_HWRCACHE, &flat));

	stepy = ((INT32)fmheight<<FRACBITS)/pblockheight;
	stepx = ((INT32)fmwidth<<FRACBITS)/pblockwidth;
	posy = 0;
	for (j = 0; j < pblockheight; j++)
	{
		posx = 0;
		dest = &block[j*(mipmap->width)]; // 1bpp
		src = &flat[(posy>>FRACBITS)*SHORT(fmwidth)];
		for (i = 0; i < pblockwidth;i++)
		{
			// fademask bpp is always 1, and is used just for alpha
			texel = src[(posx)>>FRACBITS];
			col = palette[texel];
			*dest = col.s.red; // take the red level of the colour and use it for alpha, as fademasks do

			dest++;
			posx += stepx;
		}
		posy += stepy;
	}

	Z_Free(flat);
}

static void HWR_CacheFadeMask(GLMipmap_t *grMipmap, lumpnum_t fademasklumpnum)
{
	size_t size;
	UINT16 fmheight = 0, fmwidth = 0;

	// setup the texture info
	grMipmap->format = GL_TEXFMT_ALPHA_8; // put the correct alpha levels straight in so I don't need to convert it later
	grMipmap->flags = 0;

	size = W_LumpLength(fademasklumpnum);

	switch (size)
	{
		// None of these are powers of 2, so I'll need to do what is done for textures and make them powers of 2 before they can be used
		case 256000: // 640x400
			fmwidth = 640;
			fmheight = 400;
			break;
		case 64000: // 320x200
			fmwidth = 320;
			fmheight = 200;
			break;
		case 16000: // 160x100
			fmwidth = 160;
			fmheight = 100;
			break;
		case 4000: // 80x50 (minimum)
			fmwidth = 80;
			fmheight = 50;
			break;
		default: // Bad lump
			CONS_Alert(CONS_WARNING, "Fade mask lump of incorrect size, ignored\n"); // I should avoid this by checking the lumpnum in HWR_RunWipe
			break;
	}

	// Thankfully, this will still work for this scenario
	grMipmap->width  = fmwidth;
	grMipmap->height = fmheight;

	MakeBlock(grMipmap);

	HWR_DrawFadeMaskInCache(grMipmap, fmwidth, fmheight, fademasklumpnum, fmwidth, fmheight);

	// I DO need to convert this because it isn't power of 2 and we need the alpha
}


void HWR_GetFadeMask(lumpnum_t fademasklumpnum)
{
	patch_t *patch = HWR_GetCachedGLPatch(fademasklumpnum);
	GLMipmap_t *grmip = ((GLPatch_t *)Patch_AllocateHardwarePatch(patch))->mipmap;
	if (!grmip->downloaded && !grmip->data)
		HWR_CacheFadeMask(grmip, fademasklumpnum);

	HWD.pfnSetTexture(grmip);

	// The system-memory data can be purged now.
	Z_ChangeTag(grmip->data, PU_HWRCACHE_UNLOCKED);
}

// =================================================
//             PALETTE HANDLING
// =================================================

void HWR_SetPalette(RGBA_t *palette)
{
	if (HWR_ShouldUsePaletteRendering())
	{
		// set the palette for palette postprocessing

		if (cv_glpalettedepth.value == 16)
		{
			// crush to 16-bit rgb565, like software currently does in the standard configuration
			// Note: Software's screenshots have the 24-bit palette, but the screen gets
			// the 16-bit version! For making comparison screenshots either use an external screenshot
			// tool or set the palette depth to 24 bits.
			RGBA_t crushed_palette[256];
			int i;
			for (i = 0; i < 256; i++)
			{
				float fred = (float)(palette[i].s.red >> 3);
				float fgreen = (float)(palette[i].s.green >> 2);
				float fblue = (float)(palette[i].s.blue >> 3);
				crushed_palette[i].s.red = (UINT8)(fred / 31.0f * 255.0f);
				crushed_palette[i].s.green = (UINT8)(fgreen / 63.0f * 255.0f);
				crushed_palette[i].s.blue = (UINT8)(fblue / 31.0f * 255.0f);
				crushed_palette[i].s.alpha = 255;
			}
			HWD.pfnSetScreenPalette(crushed_palette);
		}
		else
		{
			HWD.pfnSetScreenPalette(palette);
		}

		// this part is responsible for keeping track of the palette OUTSIDE of a level.
		if (!(gamestate == GS_LEVEL || (gamestate == GS_TITLESCREEN && titlemapinaction)))
			HWR_SetMapPalette();
	}
	else
	{
		// set the palette for the textures
		HWD.pfnSetTexturePalette(palette);
		// reset mapPalette so next call to HWR_SetMapPalette will update everything correctly
		memset(mapPalette, 0, sizeof(mapPalette));
		// hardware driver will flush there own cache if cache is non paletized
		// now flush data texture cache so 32 bit texture are recomputed
		if (patchformat == GL_TEXFMT_RGBA || textureformat == GL_TEXFMT_RGBA)
		{
			Z_FreeTag(PU_HWRCACHE);
			Z_FreeTag(PU_HWRCACHE_UNLOCKED);
#ifdef PS2_PROFILE
			Z_FreeTag(PU_HWRCACHE_LRU); // OPT12 HWDRV (PS2-HW-442)
#endif
		}
	}
}

static void HWR_SetPaletteLookup(RGBA_t *palette)
{
	int r, g, b;
	UINT8 *lut = Z_Malloc(
		HWR_PALETTE_LUT_SIZE*HWR_PALETTE_LUT_SIZE*HWR_PALETTE_LUT_SIZE*sizeof(UINT8),
		PU_STATIC, NULL);
#define STEP_SIZE (256/HWR_PALETTE_LUT_SIZE)
	for (b = 0; b < HWR_PALETTE_LUT_SIZE; b++)
	{
		for (g = 0; g < HWR_PALETTE_LUT_SIZE; g++)
		{
			for (r = 0; r < HWR_PALETTE_LUT_SIZE; r++)
			{
				lut[b*HWR_PALETTE_LUT_SIZE*HWR_PALETTE_LUT_SIZE+g*HWR_PALETTE_LUT_SIZE+r] =
					NearestPaletteColor(r*STEP_SIZE, g*STEP_SIZE, b*STEP_SIZE, palette);
			}
		}
	}
#undef STEP_SIZE
	HWD.pfnSetPaletteLookup(lut);
	Z_Free(lut);
}

// Updates mapPalette to reflect the loaded level or other game state.
// Textures are flushed if needed.
// Call this function only in palette rendering mode.
void HWR_SetMapPalette(void)
{
	RGBA_t RGBA_converted[256];
	RGBA_t *palette;
	int i;

	if (!(gamestate == GS_LEVEL || (gamestate == GS_TITLESCREEN && titlemapinaction)))
	{
		// outside of a level, pMasterPalette should have PLAYPAL ready for us
		palette = pMasterPalette;
	}
	else
	{
		// in a level pMasterPalette might have a flash palette, but we
		// want the map's original palette.
		lumpnum_t lumpnum = W_GetNumForName(GetPalette());
		size_t palsize = W_LumpLength(lumpnum);
		UINT8 *RGB_data;
		if (palsize < 768) // 256 * 3
			I_Error("HWR_SetMapPalette: A programmer assumed palette lumps are at least 768 bytes long, but apparently this was a wrong assumption!\n");
		RGB_data = W_CacheLumpNum(lumpnum, PU_CACHE);
		// we got the RGB palette now, but we need it in RGBA format.
		for (i = 0; i < 256; i++)
		{
			RGBA_converted[i].s.red = *(RGB_data++);
			RGBA_converted[i].s.green = *(RGB_data++);
			RGBA_converted[i].s.blue = *(RGB_data++);
			RGBA_converted[i].s.alpha = 255;
		}
		palette = RGBA_converted;
	}

	// check if the palette has changed from the previous one
	if (memcmp(mapPalette, palette, sizeof(mapPalette)))
	{
		memcpy(mapPalette, palette, sizeof(mapPalette));
		// in palette rendering mode, this means that all rgba textures now have wrong colors
		// and the lookup table is outdated
		HWR_SetPaletteLookup(mapPalette);
		HWD.pfnSetTexturePalette(mapPalette);
		if (patchformat == GL_TEXFMT_RGBA || textureformat == GL_TEXFMT_RGBA)
		{
			Z_FreeTag(PU_HWRCACHE);
			Z_FreeTag(PU_HWRCACHE_UNLOCKED);
#ifdef PS2_PROFILE
			Z_FreeTag(PU_HWRCACHE_LRU); // OPT12 HWDRV (PS2-HW-442)
#endif
		}
	}
}

// Creates a hardware lighttable from the supplied lighttable.
// Returns the id of the hw lighttable, usable in FSurfaceInfo.
UINT32 HWR_CreateLightTable(UINT8 *lighttable, RGBA_t *hw_lighttable)
{
	UINT32 i;
	RGBA_t *palette = HWR_GetTexturePalette();

	// To make the palette index -> RGBA mapping easier for the shader,
	// the hardware lighttable is composed of RGBA colors instead of palette indices.
	for (i = 0; i < 256 * 32; i++)
		hw_lighttable[i] = palette[lighttable[i]];

	return HWD.pfnCreateLightTable(hw_lighttable);
}

// Updates a hardware lighttable of a given id from the supplied lighttable.
void HWR_UpdateLightTable(UINT32 id, UINT8 *lighttable, RGBA_t *hw_lighttable)
{
	UINT32 i;
	RGBA_t *palette = HWR_GetTexturePalette();

	for (i = 0; i < 256 * 32; i++)
		hw_lighttable[i] = palette[lighttable[i]];

	HWD.pfnUpdateLightTable(id, hw_lighttable);
}

// get hwr lighttable id for colormap, create it if it doesn't already exist
UINT32 HWR_GetLightTableID(extracolormap_t *colormap)
{
	boolean default_colormap = false;
	if (!colormap)
	{
		colormap = R_GetDefaultColormap(); // a place to store the hw lighttable id
		// alternatively could just store the id in a global variable if there are issues
		default_colormap = true;
	}

	UINT8 *colormap_pointer;

	if (default_colormap)
		colormap_pointer = colormaps; // don't actually use the data from the "default colormap"
	else
		colormap_pointer = colormap->colormap;

	// create hw lighttable if there isn't one
	if (colormap->gl_lighttable.data == NULL)
	{
		Z_Malloc(256 * 32 * sizeof(RGBA_t), PU_HWRLIGHTTABLEDATA, &colormap->gl_lighttable.data);
	}

	// Generate the texture for this light table
	if (!colormap->gl_lighttable.id)
	{
		colormap->gl_lighttable.id = HWR_CreateLightTable(colormap_pointer, colormap->gl_lighttable.data);
	}
	// Update the texture if it was directly changed by a script
	else if (colormap->gl_lighttable.needs_update)
	{
		HWR_UpdateLightTable(colormap->gl_lighttable.id, colormap_pointer, colormap->gl_lighttable.data);
	}

	colormap->gl_lighttable.needs_update = false;

	return colormap->gl_lighttable.id;
}

// Note: all hardware lighttable ids assigned before this
// call become invalid and must not be used.
void HWR_ClearLightTables(void)
{
#ifdef PS2_PROFILE
	HWR_GCacheFlush(); // OPT11 (PS2-HW-80): the cached surfaces hold light table ids
#endif
	Z_FreeTag(PU_HWRLIGHTTABLEDATA);

	if (vid.glstate == VID_GL_LIBRARY_LOADED)
		HWD.pfnClearLightTables();
}

#endif //HWRENDER
