// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_textures.c
/// \brief Texture generation.

#include "doomdef.h"
#include "g_game.h"
#include "i_video.h"
#include "r_local.h"
#include "r_sky.h"
#include "p_local.h"
#include "m_misc.h"
#include "r_data.h"
#include "r_textures.h"
#include "r_patch.h"
#include "r_picformats.h"
#include "w_wad.h"
#include "z_zone.h"
#include "p_setup.h" // levelflats
#include "byteptr.h"
#include "dehacked.h"
#include "ps2/ps2_loadprof.h" // PS2-LOAD-20 (-loadhash of the texture list)
#if defined(PS2) && defined(PS2_PROFILE)
#include "m_argv.h" // -flatstream / -flatcheck (PS2-180)
#endif

#ifdef HWRENDER
#include "hardware/hw_glob.h" // HWR_LoadMapTextures
#endif

#include <errno.h>

//
// TEXTURE_T CACHING
// When a texture is first needed, it counts the number of composite columns
//  required in the texture and allocates space for a column directory and
//  any new columns.
// The directory will simply point inside other patches if there is only one
//  patch in a given column, but any columns with multiple patches will have
//  new column_ts generated.
//

INT32 numtextures = 0; // total number of textures found,
// size of following tables

texture_t **textures = NULL;
column_t **texturecolumns; // columns for each texture
UINT8 **texturecache; // graphics data for each generated full-size texture

INT32 *texturewidth;
fixed_t *textureheight; // needed for texture pegging

INT32 *texturetranslation;

// Painfully simple texture id cacheing to make maps load faster. :3
static struct {
	char name[9];
	UINT32 hash;
	INT32 id;
	UINT8 type;
} *tidcache = NULL;
static INT32 tidcachelen = 0;

//
// MAPTEXTURE_T CACHING
// When a texture is first needed, it counts the number of composite columns
//  required in the texture and allocates space for a column directory and
//  any new columns.
// The directory will simply point inside other patches if there is only one
//  patch in a given column, but any columns with multiple patches will have
//  new column_ts generated.
//

//
// R_DrawColumnInCache
// Clip and draw a column from a patch into a cached post.
//
static void R_DrawColumnInCache(column_t *column, UINT8 *cache, texpatch_t *originPatch, INT32 cacheheight, INT32 patchheight, UINT8 *opaque_pixels)
{
#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_texmask)
#define R_OPAQUE_PACKED
#define R_MASK_BYTES(h) (((size_t)(h) + 7) >> 3)
#define R_MASK_TEST(mask, p) (((mask)[(size_t)(p) >> 3] >> ((p) & 7)) & 1)
#define R_MASK_SET(mask, p) ((mask)[(size_t)(p) >> 3] |= (UINT8)(1u << ((p) & 7)))
#define R_MASK_RANGE(mask, p, n) do { \
	size_t firstbyte = (size_t)(p) >> 3, lastbyte = (size_t)((p) + (n) - 1) >> 3; \
	UINT8 firstmask = (UINT8)(0xffu << ((p) & 7)); \
	UINT8 lastmask = (UINT8)(0xffu >> (7 - (((p) + (n) - 1) & 7))); \
	if (firstbyte == lastbyte) (mask)[firstbyte] |= (UINT8)(firstmask & lastmask); \
	else { (mask)[firstbyte] |= firstmask; \
		memset((mask) + firstbyte + 1, 0xff, lastbyte - firstbyte - 1); \
		(mask)[lastbyte] |= lastmask; } \
} while (0)
#endif
	INT32 count, position;
	UINT8 *source;
	INT32 originy = originPatch->originy;

	(void)patchheight; // This parameter is unused

	for (unsigned i = 0; i < column->num_posts; i++)
	{
		post_t *post = &column->posts[i];
		source = column->pixels + post->data_offset;
		count = post->length;
		position = originy + post->topdelta;

		if (position < 0)
		{
			count += position;
			source -= position; // start further down the column
			position = 0;
		}

		if (position + count > cacheheight)
			count = cacheheight - position;

		if (count > 0)
		{
			M_Memcpy(cache + position, source, count);
#ifdef R_OPAQUE_PACKED
			R_MASK_RANGE(opaque_pixels, position, count);
#else
			memset(opaque_pixels + position, true, count);
#endif
		}
	}
}

//
// R_DrawFlippedColumnInCache
// Similar to R_DrawColumnInCache; it draws the column inverted, however.
//
static void R_DrawFlippedColumnInCache(column_t *column, UINT8 *cache, texpatch_t *originPatch, INT32 cacheheight, INT32 patchheight, UINT8 *opaque_pixels)
{
	INT32 count, position;
	UINT8 *source, *dest;
	INT32 originy = originPatch->originy;
	INT32 topdelta;
#ifndef R_OPAQUE_PACKED
	UINT8 *is_opaque;
#endif

	for (unsigned i = 0; i < column->num_posts; i++)
	{
		post_t *post = &column->posts[i];
		topdelta = patchheight - post->length - post->topdelta;
		source = column->pixels + post->data_offset + (post->length - 1);
		count = post->length;
		position = originy + topdelta;

		if (position < 0)
		{
			count += position;
			source += position; // start further UP the column
			position = 0;
		}

		if (position + count > cacheheight)
			count = cacheheight - position;

		dest = cache + position;
#ifndef R_OPAQUE_PACKED
		is_opaque = opaque_pixels + position;
#endif

		if (count > 0)
		{
#ifdef R_OPAQUE_PACKED
			R_MASK_RANGE(opaque_pixels, position, count);
			for (; dest < cache + position + count; --source, dest++)
#else
			for (; dest < cache + position + count; --source, dest++, is_opaque++)
#endif
			{
				*dest = *source;
#ifndef R_OPAQUE_PACKED
				*is_opaque = true;
#endif
			}
		}
	}
}

//
// R_DrawBlendColumnInCache
// Draws a translucent column into the cache.
//
static void R_DrawBlendColumnInCache(column_t *column, UINT8 *cache, texpatch_t *originPatch, INT32 cacheheight, INT32 patchheight, UINT8 *opaque_pixels)
{
	INT32 count, position;
	UINT8 *source, *dest;
	INT32 originy = originPatch->originy;
#ifdef R_OPAQUE_PACKED
	INT32 is_opaque;
#else
	UINT8 *is_opaque;
#endif

	(void)patchheight; // This parameter is unused

	for (unsigned i = 0; i < column->num_posts; i++)
	{
		post_t *post = &column->posts[i];
		source = column->pixels + post->data_offset;
		count = post->length;
		position = originy + post->topdelta;

		if (position < 0)
		{
			count += position;
			source -= position; // start further down the column
			position = 0;
		}

		if (position + count > cacheheight)
			count = cacheheight - position;

		dest = cache + position;
#ifdef R_OPAQUE_PACKED
		is_opaque = position;
#else
		is_opaque = opaque_pixels + position;
#endif

		if (count > 0)
		{
			for (; dest < cache + position + count; source++, dest++, is_opaque++)
			{
#ifdef R_OPAQUE_PACKED
				if (originPatch->alpha <= ASTTextureBlendingThreshold[1] && !R_MASK_TEST(opaque_pixels, is_opaque))
#else
				if (originPatch->alpha <= ASTTextureBlendingThreshold[1] && !(*is_opaque))
#endif
					continue;
				*dest = ASTBlendPaletteIndexes(*dest, *source, originPatch->style, originPatch->alpha);
#ifdef R_OPAQUE_PACKED
				R_MASK_SET(opaque_pixels, is_opaque);
#else
				*is_opaque = true;
#endif
			}
		}
	}
}

//
// R_DrawBlendFlippedColumnInCache
// Similar to the one above except that the column is inverted.
//
static void R_DrawBlendFlippedColumnInCache(column_t *column, UINT8 *cache, texpatch_t *originPatch, INT32 cacheheight, INT32 patchheight, UINT8 *opaque_pixels)
{
	INT32 count, position;
	UINT8 *source, *dest;
	INT32 originy = originPatch->originy;
	INT32 topdelta;
#ifdef R_OPAQUE_PACKED
	INT32 is_opaque;
#else
	UINT8 *is_opaque;
#endif

	for (unsigned i = 0; i < column->num_posts; i++)
	{
		post_t *post = &column->posts[i];
		topdelta = patchheight - post->length - post->topdelta;
		source = column->pixels + post->data_offset + (post->length - 1);
		count = post->length;
		position = originy + topdelta;

		if (position < 0)
		{
			count += position;
			source += position; // start further UP the column
			position = 0;
		}

		if (position + count > cacheheight)
			count = cacheheight - position;

		dest = cache + position;
#ifdef R_OPAQUE_PACKED
		is_opaque = position;
#else
		is_opaque = opaque_pixels + position;
#endif

		if (count > 0)
		{
			for (; dest < cache + position + count; --source, dest++, is_opaque++)
			{
#ifdef R_OPAQUE_PACKED
				if (originPatch->alpha <= ASTTextureBlendingThreshold[1] && !R_MASK_TEST(opaque_pixels, is_opaque))
#else
				if (originPatch->alpha <= ASTTextureBlendingThreshold[1] && !(*is_opaque))
#endif
					continue;
				*dest = ASTBlendPaletteIndexes(*dest, *source, originPatch->style, originPatch->alpha);
#ifdef R_OPAQUE_PACKED
				R_MASK_SET(opaque_pixels, is_opaque);
#else
				*is_opaque = true;
#endif
			}
		}
	}
}

#ifdef PS2_PROFILE
// Consume the original post stream directly; only one column/post descriptor lives on the stack.
static void R_DrawRawPatchColumn(softwarepatch_t *raw, size_t bytes, INT32 colx, UINT8 *dest,
	texpatch_t *origin, INT32 cacheheight, UINT8 *mask,
	void (*drawer)(column_t *, UINT8 *, texpatch_t *, INT32, INT32, UINT8 *))
{
	size_t off = (UINT32)LONG(raw->columnofs[colx]), prevdelta = 0;
	column_t column = {0};
	post_t post = {0};
	column.num_posts = 1;
	column.posts = &post;
	for (;;)
	{
		UINT8 *source;
		if (off >= bytes)
			I_Error("R_GenerateTexture: truncated patch column");
		source = (UINT8 *)raw + off;
		if (source[0] == 0xff)
			break;
		if (bytes - off < 4 || source[1] > bytes - off - 4)
			I_Error("R_GenerateTexture: truncated patch post");
		post.topdelta = source[0];
		if (post.topdelta <= prevdelta)
			post.topdelta += prevdelta;
		prevdelta = post.topdelta;
		post.length = source[1];
		column.pixels = source + 3;
		drawer(&column, dest, origin, cacheheight, SHORT(raw->height), mask);
		off += post.length + 4;
	}
}

// Pixel streams can have any byte length. Align the internal arrays independently without adding padding
// to total_pixels: patch column offsets and the pixel copy still describe only actual pixel bytes.
static size_t R_TextureCacheLayout(size_t pixels, size_t width, size_t posts, size_t *columnofs, size_t *postofs)
{
	const size_t columnalign = _Alignof(column_t), postalign = _Alignof(post_t);
	*columnofs = (pixels + columnalign - 1) & ~(columnalign - 1);
	*postofs = (*columnofs + sizeof(column_t) * width + postalign - 1) & ~(postalign - 1);
	return *postofs + sizeof(post_t) * posts;
}
#endif

#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_TEXPLACE)
#define R_TEXTURE_WORK_TAG PU_RENDERWORK
#else
#define R_TEXTURE_WORK_TAG PU_STATIC
#endif

#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_TEXPOSTS) && !defined(TEXTURE_255_IS_TRANSPARENT)
#define R_COUNTED_POSTS
// The mask survives until every descriptor has been written, even if the pixel root moves.
static void R_WriteTexturePosts(post_t *posts, const UINT8 *mask, INT32 width, INT32 height)
{
	for (INT32 x = 0; x < width; x++)
	{
		post_t *post = NULL;
		for (INT32 y = 0; y < height; y++)
		{
#ifdef R_OPAQUE_PACKED
			boolean opaque = R_MASK_TEST(mask + x * R_MASK_BYTES(height), y);
#else
			boolean opaque = mask[x * height + y];
#endif
			if (!opaque)
				post = NULL;
			else if (!post)
			{
				post = posts++;
				post->topdelta = post->data_offset = (size_t)y;
				post->length = 1;
			}
			else
				post->length++;
		}
	}
}
#endif

#if defined(PS2) && defined(PS2_PROFILE)
// PS2-148 (OPT10-S): a composite texture that does not fit in the arena must not end the game ("Out of memory allocating 524288 bytes" on MAP11 after
// the levels before it, 4194304 bytes on MAPMG: 2048x2048 CLUDSSSS). The texture becomes a column of one colour: every column of the texture points at
// the same single post and the same run of `height` pixels (width columns of 12 bytes and one post instead of width*height bytes). It is an ordinary
// cache block with an owner: the zone may evict it, and the next use tries the real composite again.
#define R_COMPOSITE_TRY_MIN (64u << 10)
#define R_FALLBACK_PIXEL 15 // a grey of the palette ramp (0 white .. 31 black)
static unsigned r_fallbacktextures;

static UINT8 *R_FallbackTexture(size_t texnum)
{
	texture_t *texture = textures[texnum];
	const size_t width = (size_t)texture->width, height = (size_t)texture->height;
	size_t columnofs, postofs, blocksize;
	UINT8 *block;
	column_t *columns;
	post_t *post;
	size_t x;

	blocksize = R_TextureCacheLayout(height, width, 1, &columnofs, &postofs);
	block = Z_Calloc(blocksize, PU_CACHE, &texturecache[texnum]);
	memset(block, R_FALLBACK_PIXEL, height);
	columns = (column_t *)(block + columnofs);
	post = (post_t *)(block + postofs);
	post->topdelta = 0;
	post->length = (unsigned)height;
	post->data_offset = 0;
	for (x = 0; x < width; x++)
	{
		columns[x].num_posts = 1;
		columns[x].posts = post;
		columns[x].pixels = block;
	}
	texture->transparency = false;
	texturecolumns[texnum] = columns;
	if (r_fallbacktextures++ < 8)
		CONS_Alert(CONS_WARNING, "R_GenerateTexture: no room for texture %d (%dx%d): plain column instead\n", (int)texnum, (int)width, (int)height);
	return block;
}

// W_CacheLumpNumPwad for the scratch copy of a lump (freed by the caller): NULL when there is no room instead of ending the run
static UINT8 *R_TryReadLump(UINT16 wadnum, lumpnum_t lumpnum)
{
	const size_t len = W_LumpLengthPwad(wadnum, lumpnum);
	UINT8 *p = Z_TryMallocAlign(len ? len : 1, R_TEXTURE_WORK_TAG, NULL, 2);

	if (p)
		W_ReadLumpHeaderPwad(wadnum, lumpnum, p, 0, 0);
	return p;
}
#endif

//
// R_GenerateTexture
//
// Allocate space for full size texture, either single patch or 'composite'
// Build the full textures from patches.
// The texture caching system is a little more hungry of memory, but has
// been simplified for the sake of highcolor, dynamic lighting, & speed.
//
// This is not optimised, but it's supposed to be executed only once
// per level, when enough memory is available.
//
#if defined(PS2) && defined(PS2_PROFILE)
// OPT12-CORE diagnostics: -ztexlog prints every texture composite / flat built after the first second of a level (what the cache has to rebuild again and again)
extern tic_t leveltime;
static boolean R_TexLog(void)
{
	static int on = -1;

	if (on < 0)
		on = M_CheckParm("-ztexlog") != 0;
	return on && leveltime > 35;
}
#define R_TEXLOG(kind, texnum) do { if (R_TexLog()) I_OutputMsg("TEXGEN %s %d %s %dx%d t=%d\n", kind, (int)(texnum), textures[texnum]->name, (int)textures[texnum]->width, (int)textures[texnum]->height, (int)leveltime); } while (0)
#else
#define R_TEXLOG(kind, texnum) ((void)0)
#endif

UINT8 *R_GenerateTexture(size_t texnum)
{
	UINT8 *block;
	UINT8 *blocktex;
	UINT8 *temp_block;
	texture_t *texture;
	texpatch_t *patch;
	int x, x1, x2, i, width, height;
	size_t blocksize;
#ifdef PS2_PROFILE
	size_t columnofs, postofs;
#endif
	unsigned *column_posts;
	UINT8 *opaque_pixels;
	column_t *columns, *temp_columns;
	post_t *posts, *temp_posts = NULL;
	size_t total_posts = 0;
	size_t total_pixels = 0;
#ifdef R_COUNTED_POSTS
	boolean final_posts = false;
#endif

	I_Assert(texnum <= (size_t)numtextures);
	texture = textures[texnum];
	I_Assert(texture != NULL);
	R_TEXLOG("comp", texnum);

	// Just create a composite one
	if (texture->type == TEXTURETYPE_FLAT)
		goto multipatch;

	// single-patch textures can have holes in them and may be used on
	// 2sided lines so they need to be kept in 'packed' format
	// BUT this is wrong for skies and walls with over 255 pixels,
	// so check if there's holes and if not strip the posts.
	if (texture->patchcount == 1)
	{
		patch = &texture->patches[0];

		UINT16 wadnum = patch->wad;
		UINT16 lumpnum = patch->lump;
		UINT8 *pdata;
		softwarepatch_t *realpatch;

#ifndef NO_PNG_LUMPS
		UINT8 header[PNG_HEADER_SIZE];

		W_ReadLumpHeaderPwad(wadnum, lumpnum, header, PNG_HEADER_SIZE, 0);

		// Not worth converting
		if (Picture_IsLumpPNG(header, W_LumpLengthPwad(wadnum, lumpnum)))
			goto multipatch;
#endif

#ifdef PS2_PROFILE
		// PU_STATIC while in use (a PU_CACHE block could be purged by the allocations below), freed on every path
#if defined(PS2)
		pdata = R_TryReadLump(wadnum, lumpnum);
		if (!pdata)
			return R_FallbackTexture(texnum);
#else
		pdata = W_CacheLumpNumPwad(wadnum, lumpnum, R_TEXTURE_WORK_TAG);
#endif
#else
		pdata = W_CacheLumpNumPwad(wadnum, lumpnum, PU_CACHE);
#endif
		realpatch = (softwarepatch_t *)pdata;

		texture->transparency = false;

		// Check the patch for holes.
		if (texture->width > SHORT(realpatch->width) || texture->height > SHORT(realpatch->height))
			texture->transparency = true;
		else
		{
			UINT8 *colofs = (UINT8 *)realpatch->columnofs;
			for (x = 0; x < texture->width; x++)
			{
				doompost_t *col = (doompost_t *)((UINT8 *)realpatch + LONG(*(UINT32 *)&colofs[x<<2]));
				INT32 topdelta, prevdelta = -1, y = 0;
				while (col->topdelta != 0xff)
				{
					topdelta = col->topdelta;
					if (topdelta <= prevdelta)
						topdelta += prevdelta;
					prevdelta = topdelta;
					if (topdelta > y)
						break;
					y = topdelta + col->length + 1;
					col = (doompost_t *)((UINT8 *)col + col->length + 4);
				}
				if (y < texture->height)
					texture->transparency = true; // this texture is HOLEy! D:
			}
		}

		// If the patch uses transparency, we have to save it this way.
		if (texture->transparency)
		{
			texture->flip = patch->flip;

			Patch_CalcDataSizes(realpatch, &total_pixels, &total_posts);

#ifdef PS2_PROFILE
			blocksize = R_TextureCacheLayout(total_pixels, texture->width, total_posts, &columnofs, &postofs);
#else
			blocksize = (sizeof(column_t) * texture->width) + (sizeof(post_t) * total_posts) + (sizeof(UINT8) * total_pixels);
#endif
			texturememory += blocksize;

#if defined(PS2)
			block = Z_TryMallocAlign(blocksize, R_TEXTURE_WORK_TAG, &texturecache[texnum], 2);
			if (!block)
			{
				Z_Free(pdata);
				return R_FallbackTexture(texnum);
			}
			memset(block, 0, blocksize);
#else
			block = Z_Calloc(blocksize, R_TEXTURE_WORK_TAG, &texturecache[texnum]);
#endif
			blocktex = block;

#ifdef PS2_PROFILE
			columns = (column_t *)(block + columnofs);
			posts = (post_t *)(block + postofs);
#else
			columns = (column_t *)(block + (sizeof(UINT8) * total_pixels));
			posts = (post_t *)(block + (sizeof(UINT8) * total_pixels) + (sizeof(column_t) * texture->width));
#endif

			texturecolumns[texnum] = columns;

			// Handles flipping as well.
			// we can't as easily flip the patch vertically sadly though,
			//  we have wait until the texture itself is drawn to do that
			Patch_MakeColumns(realpatch, texture->width, texture->width, blocktex, columns, posts, patch->flip & 1);

			Z_Free(pdata);

			goto done;
		}

#ifdef PS2_PROFILE
		Z_Free(pdata);
#endif
		// Otherwise, do multipatch format.
	}

	// multi-patch textures (or 'composite')
	multipatch:
	texture->flip = 0;

	// To make things easier, I just allocate WxH always
	total_pixels = texture->width * texture->height;

#ifdef PS2_PROFILE
	// PS2-76: the biggest block first. The mask and the column array below are small, but they took the middle of the one hole
	// that would have held WxH (MAP11, 1024x512: 498 KB hole split by a 64 KB mask, "Out of memory allocating 524288 bytes")
#if defined(PS2)
	if (total_pixels >= R_COMPOSITE_TRY_MIN)
	{
		temp_block = Z_TryMallocAlign(total_pixels, R_TEXTURE_WORK_TAG, NULL, 2);
		if (!temp_block)
			return R_FallbackTexture(texnum);
		memset(temp_block, 0, total_pixels);
	}
	else
#endif
	temp_block = Z_Calloc(total_pixels, R_TEXTURE_WORK_TAG, NULL);
#endif
#ifdef R_OPAQUE_PACKED
#if defined(PS2)
	if ((size_t)texture->width * R_MASK_BYTES(texture->height) >= R_COMPOSITE_TRY_MIN)
	{
		opaque_pixels = Z_TryMallocAlign((size_t)texture->width * R_MASK_BYTES(texture->height), R_TEXTURE_WORK_TAG, NULL, 2);
		if (!opaque_pixels)
		{
			Z_Free(temp_block);
			return R_FallbackTexture(texnum);
		}
		memset(opaque_pixels, 0, (size_t)texture->width * R_MASK_BYTES(texture->height));
	}
	else
#endif
	opaque_pixels = Z_Calloc((size_t)texture->width * R_MASK_BYTES(texture->height), R_TEXTURE_WORK_TAG, NULL);
#else
	opaque_pixels = Z_Calloc(total_pixels * sizeof(UINT8), R_TEXTURE_WORK_TAG, NULL);
#endif
	temp_columns = Z_Calloc(sizeof(column_t) * texture->width, R_TEXTURE_WORK_TAG, NULL);
#ifndef PS2_PROFILE
	temp_block = Z_Calloc(total_pixels, R_TEXTURE_WORK_TAG, NULL);
#endif

#ifdef TEXTURE_255_IS_TRANSPARENT
	texture->transparency = false;

	// Transparency hack
	memset(temp_block, TRANSPARENTPIXEL, total_pixels);
#else
	texture->transparency = true;
#endif

	for (x = 0; x < texture->width; x++)
	{
		column_t *column = &temp_columns[x];
		column->num_posts = 0;
		column->posts = NULL;
		column->pixels = temp_block + (texture->height * x);
	}

	// Composite the columns together.
	for (i = 0, patch = texture->patches; i < texture->patchcount; i++, patch++)
	{
		static void (*columnDrawer)(column_t *, UINT8 *, texpatch_t *, INT32, INT32, UINT8 *);
		if (patch->style != AST_COPY)
			columnDrawer = (patch->flip & 2) ? R_DrawBlendFlippedColumnInCache : R_DrawBlendColumnInCache;
		else
			columnDrawer = (patch->flip & 2) ? R_DrawFlippedColumnInCache : R_DrawColumnInCache;

		UINT16 wadnum = patch->wad;
		lumpnum_t lumpnum = patch->lump;
#ifdef PS2_PROFILE
		// The raw lump is only needed by the PNG/flat converters: PU_STATIC while in use, released below
		UINT8 *pdata = NULL;
		boolean rawlump = (texture->type == TEXTURETYPE_FLAT);
		softwarepatch_t *rawpatch = NULL;
		size_t rawbytes = 0;
#else
		UINT8 *pdata = W_CacheLumpNumPwad(wadnum, lumpnum, PU_CACHE);
#endif
		patch_t *realpatch = NULL;
		boolean free_patch = true;

#ifdef PS2_PROFILE
#ifndef NO_PNG_LUMPS
		{
			UINT8 header[PNG_HEADER_SIZE];
			W_ReadLumpHeaderPwad(wadnum, lumpnum, header, PNG_HEADER_SIZE, 0);
			if (Picture_IsLumpPNG(header, W_LumpLengthPwad(wadnum, lumpnum)))
				rawlump = true;
		}
#endif
		if (rawlump)
		{
#if defined(PS2)
			pdata = R_TryReadLump(wadnum, lumpnum);
			if (!pdata)
			{
				Z_Free(temp_columns);
				Z_Free(opaque_pixels);
				Z_Free(temp_block);
				return R_FallbackTexture(texnum);
			}
#else
			pdata = W_CacheLumpNumPwad(wadnum, lumpnum, R_TEXTURE_WORK_TAG);
#endif
		}
#endif

#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_texstream)
		if (texture->type != TEXTURETYPE_FLAT)
		{
			realpatch = W_GetCachedPatchNumPwad(wadnum, lumpnum);
			free_patch = false;
			if (realpatch == NULL)
			{
#if defined(PS2)
				// PS2-178 (OPT11-STAB): the PNG probe above already read this lump into pdata; a second copy replaced the pointer and the first one stayed in the arena for
				// good (tag PU_RENDERWORK, nothing frees it: 541 KB for one composite texture of MAPFB, in the middle of the arena after the level, MAPM3 did not load after 65 maps)
				if (!pdata)
					pdata = R_TryReadLump(wadnum, lumpnum);
				if (!pdata)
				{
					Z_Free(temp_columns);
					Z_Free(opaque_pixels);
					Z_Free(temp_block);
					return R_FallbackTexture(texnum);
				}
#else
				pdata = W_CacheLumpNumPwad(wadnum, lumpnum, R_TEXTURE_WORK_TAG);
#endif
				rawbytes = W_LumpLengthPwad(wadnum, lumpnum);
				rawpatch = (softwarepatch_t *)pdata;
#ifdef PS2_ZIPPNG
				if (Picture_IsLumpPNG(pdata, rawbytes) && !Picture_IsLumpCooked(pdata, rawbytes))
				{
					// PS2-100: a real PNG (add-on): the generic converter (patch cache) builds the patch
					rawpatch = NULL;
					Z_Free(pdata);
					pdata = NULL;
					realpatch = W_CachePatchNumPwad(wadnum, lumpnum, PU_PATCH);
				}
				else
#endif
				{
#ifndef NO_PNG_LUMPS
				if (Picture_IsLumpPNG(pdata, rawbytes))
				{
					rawpatch = (softwarepatch_t *)(pdata + PNG_HEADER_SIZE);
					rawbytes -= PNG_HEADER_SIZE;
				}
#endif
				if (rawbytes < 8 || SHORT(rawpatch->width) <= 0
					|| (size_t)SHORT(rawpatch->width) > (rawbytes - 8) / sizeof(UINT32))
					I_Error("R_GenerateTexture: invalid patch column directory");
				}
			}
		}
		else
		{
#endif
#ifndef NO_PNG_LUMPS
		size_t lumplength = W_LumpLengthPwad(wadnum, lumpnum);
#ifdef PS2_PROFILE
		if (pdata && Picture_IsLumpPNG(pdata, lumplength))
#else
		if (Picture_IsLumpPNG(pdata, lumplength))
#endif
			realpatch = (patch_t *)Picture_PNGConvert(pdata, PICFMT_PATCH, NULL, NULL, NULL, NULL, lumplength, NULL, 0);
		else
#endif
		if (texture->type == TEXTURETYPE_FLAT)
			realpatch = (patch_t *)Picture_Convert(PICFMT_FLAT, pdata, PICFMT_PATCH, 0, NULL, texture->width, texture->height, 0, 0, PICFLAGS_USE_TRANSPARENTPIXEL);
		else
		{
			// If this patch has already been loaded, we just use it from the cache.
			realpatch = W_GetCachedPatchNumPwad(wadnum, lumpnum);
			free_patch = false;

			// Otherwise, we load it here.
			if (realpatch == NULL)
				realpatch = W_CachePatchNumPwad(wadnum, lumpnum, PU_PATCH);
		}
#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_texstream)
		}
#endif

#ifdef PS2_PROFILE
		if (!rawpatch)
			Z_Free(pdata); // the converted patch is a copy
#endif

		x1 = patch->originx;
#ifdef PS2_PROFILE
		width = rawpatch ? SHORT(rawpatch->width) : realpatch->width;
		height = rawpatch ? SHORT(rawpatch->height) : realpatch->height;
#else
		width = realpatch->width;
		height = realpatch->height;
#endif
		x2 = x1 + width;

		if (x1 > texture->width || x2 < 0)
		{
			if (free_patch)
				Patch_Free(realpatch);
#ifdef PS2_PROFILE
			if (rawpatch) Z_Free(pdata);
#endif
			continue; // patch not located within texture's x bounds, ignore
		}

		if (patch->originy > texture->height || (patch->originy + height) < 0)
		{
			if (free_patch)
				Patch_Free(realpatch);
#ifdef PS2_PROFILE
			if (rawpatch) Z_Free(pdata);
#endif
			continue; // patch not located within texture's y bounds, ignore
		}

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

		for (; x < x2; x++)
		{
			INT32 colx;

			if (patch->flip & 1)
				colx = (x1+width-1)-x;
			else
				colx = x-x1;

#ifdef PS2_PROFILE
			if (rawpatch)
			{
#ifdef R_OPAQUE_PACKED
				UINT8 *mask = opaque_pixels + x * R_MASK_BYTES(texture->height);
#else
				UINT8 *mask = opaque_pixels + x * texture->height;
#endif
				R_DrawRawPatchColumn(rawpatch, rawbytes, colx, temp_columns[x].pixels,
					patch, texture->height, mask, columnDrawer);
				continue;
			}
#endif
			column_t *patchcol = &realpatch->columns[colx];

			if (patchcol->num_posts > 0)
#ifdef R_OPAQUE_PACKED
				columnDrawer(patchcol, temp_columns[x].pixels, patch, texture->height, height, &opaque_pixels[x * R_MASK_BYTES(texture->height)]);
#else
				columnDrawer(patchcol, temp_columns[x].pixels, patch, texture->height, height, &opaque_pixels[x * texture->height]);
#endif
		}

		if (free_patch)
			Patch_Free(realpatch);
#ifdef PS2_PROFILE
		if (rawpatch) Z_Free(pdata);
#endif
	}

	// Now write the columns
	column_posts = Z_Calloc(sizeof(unsigned) * texture->width, R_TEXTURE_WORK_TAG, NULL);

#ifdef TEXTURE_255_IS_TRANSPARENT
	total_posts = texture->width;
	temp_posts = Z_Realloc(temp_posts, sizeof(post_t) * total_posts, PU_CACHE, NULL);
#endif

	for (x = 0; x < texture->width; x++)
	{
#ifndef R_COUNTED_POSTS
		post_t *post = NULL;
#endif

		column_t *column = &temp_columns[x];

#ifdef TEXTURE_255_IS_TRANSPARENT
		post = &temp_posts[x];
		post->topdelta = 0;
		post->length = texture->height;
		post->data_offset = 0;
		column_posts[x] = x;
		column->num_posts = 1;
#else
		boolean was_opaque = false;

		column_posts[x] = (unsigned)-1;

		for (INT32 y = 0; y < texture->height; y++)
		{
			// End span if we have a transparent pixel
#ifdef R_OPAQUE_PACKED
			if (!R_MASK_TEST(opaque_pixels + x * R_MASK_BYTES(texture->height), y))
#else
			if (!opaque_pixels[(x * texture->height) + y])
#endif
			{
				was_opaque = false;
				continue;
			}

			if (!was_opaque)
			{
				total_posts++;

#ifndef R_COUNTED_POSTS
				temp_posts = Z_Realloc(temp_posts, sizeof(post_t) * total_posts, PU_CACHE, NULL);
				post = &temp_posts[total_posts - 1];
				post->topdelta = (size_t)y;
				post->length = 0;
				post->data_offset = (size_t)y;
#endif
				if (column_posts[x] == (unsigned)-1)
					column_posts[x] = total_posts - 1;
				column->num_posts++;
			}

			was_opaque = true;

#ifndef R_COUNTED_POSTS
			post->length++;
#endif
		}
#endif
	}

#ifdef PS2_PROFILE
	// The scratch buffers below were never released (and temp_posts is PU_CACHE without an owner, so never
	// purged either): that leaked width*height bytes per composite texture.
#ifdef R_COUNTED_POSTS
	// Keep the mask only when it costs no more than the eliminated descriptor scratch.
#ifdef R_OPAQUE_PACKED
	final_posts = (size_t)texture->width * R_MASK_BYTES(texture->height) <= total_posts * sizeof(post_t);
#else
	final_posts = total_pixels <= total_posts * sizeof(post_t);
#endif
	if (!final_posts)
	{
		temp_posts = Z_Malloc(total_posts * sizeof(*temp_posts), PU_CACHE, NULL);
		R_WriteTexturePosts(temp_posts, opaque_pixels, texture->width, texture->height);
		Z_Free(opaque_pixels);
	}
#else
	Z_Free(opaque_pixels);
#endif
#endif

#ifdef PS2_PROFILE
	blocksize = R_TextureCacheLayout(total_pixels, texture->width, total_posts, &columnofs, &postofs);
#else
	blocksize = (sizeof(column_t) * texture->width) + (sizeof(post_t) * total_posts) + (sizeof(UINT8) * total_pixels);
#endif
	texturememory += blocksize;

#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_texreuse)
	// Release the adjacent scratch before extending pixels into their final self-contained cache.
	Z_Free(temp_columns);
	temp_columns = NULL;
#if defined(PS2)
	block = Z_TryReallocAlign(temp_block, blocksize, R_TEXTURE_WORK_TAG, &texturecache[texnum], 2);
	if (!block)
	{
		// no room to stretch the pixels into their final block (the old and the new block would both have to exist): drop the scratch, plain column
		Z_Free(temp_block);
#ifdef R_COUNTED_POSTS
		if (final_posts)
			Z_Free(opaque_pixels);
		else
			Z_Free(temp_posts);
#else
		Z_Free(temp_posts);
#endif
		Z_Free(column_posts);
		return R_FallbackTexture(texnum);
	}
#else
	block = Z_Realloc(temp_block, blocksize, R_TEXTURE_WORK_TAG, &texturecache[texnum]);
#endif
	temp_block = NULL;
#else
	block = Z_Calloc(blocksize, R_TEXTURE_WORK_TAG, &texturecache[texnum]);
#endif
	blocktex = block;

#if !defined(PS2_PROFILE) || defined(PS2_NOOPT_texreuse)
	memcpy(blocktex, temp_block, total_pixels);

	Z_Free(temp_block);
#endif

#ifdef PS2_PROFILE
	columns = (column_t *)(block + columnofs);
	posts = (post_t *)(block + postofs);
#else
	columns = (column_t *)(block + (sizeof(UINT8) * total_pixels));
	posts = (post_t *)(block + (sizeof(UINT8) * total_pixels) + (sizeof(column_t) * texture->width));
#endif

#if !defined(PS2_PROFILE) || defined(PS2_NOOPT_texreuse)
	memcpy(columns, temp_columns, sizeof(column_t) * texture->width);
#endif
#ifdef R_COUNTED_POSTS
	if (final_posts)
	{
		R_WriteTexturePosts(posts, opaque_pixels, texture->width, texture->height);
		Z_Free(opaque_pixels);
	}
	else
#endif
		memcpy(posts, temp_posts, sizeof(post_t) * total_posts);

	texturecolumns[texnum] = columns;

#if defined(PS2_PROFILE) && !defined(PS2_NOOPT_texreuse)
	// Prefix indices determine every column's post count, including empty columns.
	{
		unsigned nextpost = total_posts;
		for (x = texture->width - 1; x >= 0; x--)
		{
			column_t *column = &columns[x];
			if (column_posts[x] != (unsigned)-1)
			{
				column->num_posts = nextpost - column_posts[x];
				column->posts = &posts[column_posts[x]];
				nextpost = column_posts[x];
			}
			column->pixels = blocktex + (texture->height * x);
		}
	}
#else
	for (x = 0; x < texture->width; x++)
	{
		column_t *column = &columns[x];
		if (column->num_posts > 0)
			column->posts = &posts[column_posts[x]];
		column->pixels = blocktex + (texture->height * x);
	}
#endif

#ifdef PS2_PROFILE
	Z_Free(temp_columns);
	Z_Free(temp_posts);
	Z_Free(column_posts);
#endif

done:
	// Now that the texture has been built in column cache, it is purgable from zone memory.
	Z_ChangeTag(block, PU_CACHE);
	return blocktex;
}

#if defined(PS2) && defined(PS2_PROFILE)
// PS2-180 (OPT11-STAB): the flat of a big texture straight from its raw patches.
// The generic route makes a texture that is used as a flat twice: the composite (W x H pixels, its posts and a mask) and then the flat (W x H again), so both sit in the arena at
// once, and a composite that was just used is protected from the eviction that could make room for the flat. CLUDSSSS of MAPMG is 2048 x 2048 (four 1024 x 1024 raw patches):
// the arena has room for one 4.2 MB block, not for two (software: "Not enough memory to draw map MAPMG: 4194304 bytes (PU_RENDERWORK)", the map did not open; hardware: the draws of the
// plane were skipped).
// Here the flat is written column by column from the patches themselves, read from the pack in windows of 32 KB: the same pixels as the composite route (the patches back to front, the
// pixels of a post as they are, what no post covers is TRANSPARENTPIXEL) and nothing but the flat in the arena. Only for plain patches (raw, not PNG and not cooked, copy style, no flips);
// every other texture takes the generic route. -flatcheck compares the two routes for every texture used as a flat (self-test), -flatstream takes this route for all of them.
#define R_STREAM_MIN (1024u * 1024u) // texels of a texture that takes this route
#define R_STREAM_WINDOW 32768u
static int r_flatstream_mode = -1; // -flatstream: 1, -flatcheck: 2, otherwise 0

static int R_FlatStreamMode(void)
{
	if (r_flatstream_mode < 0)
		r_flatstream_mode = M_CheckParm("-flatcheck") ? 2 : M_CheckParm("-flatstream") ? 1 : 0;
	return r_flatstream_mode;
}

static boolean R_TextureStreamable(const texture_t *texture)
{
	INT32 i;

	if (texture->type == TEXTURETYPE_FLAT || texture->patchcount < 1)
		return false;
	for (i = 0; i < texture->patchcount; i++)
	{
		const texpatch_t *patch = &texture->patches[i];
		const size_t len = W_LumpLengthPwad(patch->wad, patch->lump);
		UINT8 header[PNG_HEADER_SIZE];

		if (patch->style != AST_COPY || patch->flip || len < 8 + PNG_HEADER_SIZE)
			return false;
		W_ReadLumpHeaderPwad(patch->wad, patch->lump, header, PNG_HEADER_SIZE, 0);
		if (Picture_IsLumpPNG(header, len)) // PNG and the cooked pictures of the packs
			return false;
	}
	return true;
}

// the flat of texture `texnum`, a new PU_RENDERWORK block (the caller gives it an owner); NULL: not possible or no room (nothing is left allocated)
static UINT8 *R_StreamTextureToFlat(size_t texnum)
{
	const texture_t *texture = textures[texnum];
	const INT32 width = texture->width, height = texture->height;
	const size_t flatsize = (size_t)width * (size_t)height;
	UINT8 *flat, *win;
	INT32 i;
	boolean ok = true;

	if (!R_TextureStreamable(texture))
		return NULL;
	flat = Z_TryMallocAlign(flatsize, PU_RENDERWORK, NULL, sizeof (void *));
	if (!flat)
		return NULL;
	win = Z_TryMallocAlign(R_STREAM_WINDOW, PU_RENDERWORK, NULL, 2);
	if (!win)
	{
		Z_Free(flat);
		return NULL;
	}
	memset(flat, TRANSPARENTPIXEL, flatsize);

	for (i = 0; ok && i < texture->patchcount; i++)
	{
		const texpatch_t *patch = &texture->patches[i];
		const size_t len = W_LumpLengthPwad(patch->wad, patch->lump);
		INT16 head[4]; // width, height, leftoffset, topoffset (INT16 array: no unaligned halfword loads on the EE)
		UINT32 *colofs;
		INT32 pw, ph, x, x1, x2;
		size_t winlo = 0, winhi = 0;

		W_ReadLumpHeaderPwad(patch->wad, patch->lump, head, sizeof head, 0);
		pw = SHORT(head[0]);
		ph = SHORT(head[1]);
		if (pw <= 0 || ph <= 0 || (size_t)pw > (len - 8) / sizeof (UINT32))
		{
			ok = false;
			break;
		}
		x1 = patch->originx;
		x2 = x1 + pw;
		if (x1 > width || x2 < 0)
			continue; // not inside the texture (as R_GenerateTexture)
		if (patch->originy > height || patch->originy + ph < 0)
			continue;
		colofs = Z_TryMallocAlign((size_t)pw * sizeof (UINT32), PU_RENDERWORK, NULL, sizeof (UINT32));
		if (!colofs)
		{
			ok = false;
			break;
		}
		if (W_ReadLumpHeaderPwad(patch->wad, patch->lump, colofs, (size_t)pw * sizeof (UINT32), 8) != (size_t)pw * sizeof (UINT32))
			ok = false;
		x = x1 < 0 ? 0 : x1;
		if (x2 > width)
			x2 = width;
		for (; ok && x < x2; x++)
		{
			size_t off = (size_t)(UINT32)LONG(colofs[x - x1]), prevdelta = 0;

			for (;;)
			{
				const UINT8 *post;
				INT32 count, position;
				size_t topdelta;
				const UINT8 *source;
				UINT8 *dest;

				// the post header (and its pixels) must be in the window: reload it at this post when it is not
				if (off < winlo || off + 4 > winhi || (off + 4 <= winhi && win[off - winlo] != 0xff && off + 4 + win[off - winlo + 1] > winhi))
				{
					size_t n = len > off ? len - off : 0;

					if (n > R_STREAM_WINDOW)
						n = R_STREAM_WINDOW;
					if (n == 0 || W_ReadLumpHeaderPwad(patch->wad, patch->lump, win, n, off) != n)
					{
						ok = false; // truncated column
						break;
					}
					winlo = off;
					winhi = off + n;
				}
				post = win + (off - winlo);
				if (post[0] == 0xff)
					break;
				if (winhi - off < 4 || (size_t)post[1] > winhi - off - 4)
				{
					ok = false; // truncated post
					break;
				}
				topdelta = post[0];
				if (topdelta <= prevdelta)
					topdelta += prevdelta;
				prevdelta = topdelta;
				count = post[1];
				source = post + 3;
				position = patch->originy + (INT32)topdelta;
				if (position < 0)
				{
					count += position;
					source -= position;
					position = 0;
				}
				if (position + count > height)
					count = height - position;
				dest = flat + (size_t)position * (size_t)width + (size_t)x;
				for (; count > 0; count--, source++, dest += width)
					*dest = *source;
				off += (size_t)post[1] + 4;
			}
			if (!ok)
				break;
		}
		Z_Free(colofs);
	}
	Z_Free(win);
	if (!ok)
	{
		Z_Free(flat);
		return NULL;
	}
	return flat;
}

// the flats of 1 MiB and more that the level uses, built first (at the start of the level, when the arena still has a big hole; see R_GetFlatForTexture)
void R_LoadBigFlats(void)
{
	size_t i;

	for (i = 0; i < numlevelflats; i++)
	{
		size_t texnum;
		const texture_t *texture;

		if (levelflats[i].type == LEVELFLAT_NONE)
			continue;
		texnum = (size_t)R_GetTextureNumForFlat(&levelflats[i]);
		if (texnum >= (size_t)numtextures)
			continue;
		texture = textures[texnum];
		if (texture->flat == NULL && texture->type != TEXTURETYPE_FLAT && (size_t)texture->width * (size_t)texture->height >= R_STREAM_MIN && R_TextureStreamable(texture))
			(void)R_GetFlatForTexture(texnum);
	}
}
#endif

UINT8 *R_GetFlatForTexture(size_t texnum)
{
	if (texnum >= (unsigned)numtextures)
		return NULL;

	texture_t *texture = textures[texnum];
	if (texture->flat != NULL)
	{
#ifdef PS2_PROFILE
		Z_Touch(texture->flat); // PS2-OPT-03: used in this frame (the zone evicts the least recently used cache first)
#endif
		return texture->flat;
	}

	// Special case: Textures that are flats don't need to be converted FROM a texture INTO a flat.
	if (texture->type == TEXTURETYPE_FLAT)
	{
		texpatch_t *patch = &texture->patches[0];
		UINT16 wadnum = patch->wad;
		lumpnum_t lumpnum = patch->lump;
#ifdef PS2_PROFILE
		// PU_STATIC while in use: the Z_Malloc below may purge PU_CACHE blocks (freed again at the end)
		UINT8 *pdata = W_CacheLumpNumPwad(wadnum, lumpnum, R_TEXTURE_WORK_TAG);
#else
		UINT8 *pdata = W_CacheLumpNumPwad(wadnum, lumpnum, PU_CACHE);
#endif
		size_t lumplength = W_LumpLengthPwad(wadnum, lumpnum);

#ifndef NO_PNG_LUMPS
		if (Picture_IsLumpPNG(pdata, lumplength))
			texture->flat = Picture_PNGConvert(pdata, PICFMT_FLAT, NULL, NULL, NULL, NULL, lumplength, NULL, 0);
		else
#endif
		{
#ifdef PS2_PROFILE
#ifdef PS2_NOOPT_flattransfer
			texture->flat = Z_Malloc(lumplength, PU_CACHE, &texture->flat);
			memcpy(texture->flat, pdata, lumplength);
#else
			texture->flat = W_TakeLumpNumPwad(wadnum, lumpnum, PU_CACHE, (void **)&texture->flat);
#endif
#else
			texture->flat = Z_Malloc(lumplength, PU_STATIC, NULL);
			memcpy(texture->flat, pdata, lumplength);
#endif
		}

#ifndef PS2_PROFILE
		Z_SetUser(texture->flat, &texture->flat);
#endif

#ifdef PS2_PROFILE
		if (texture->flat != pdata)
		{
			Z_SetUser(texture->flat, (void **)&texture->flat);
			Z_ChangeTag(texture->flat, PU_CACHE);
			Z_Free(pdata);
		}
#else
		Z_Free(pdata);
#endif
	}
	else
	{
#if defined(PS2) && defined(PS2_PROFILE)
		// PS2-180: a big texture (or every one with -flatstream) is made straight from its patches (no composite next to the flat)
		boolean streamed = false;

		if ((R_FlatStreamMode() == 1 || (R_FlatStreamMode() == 0 && (size_t)texture->width * (size_t)texture->height >= R_STREAM_MIN)) && R_TextureStreamable(texture))
		{
			texture->flat = R_StreamTextureToFlat(texnum);
			streamed = texture->flat != NULL;
		}
		if (!streamed)
#endif
		{
		R_TEXLOG("flat", texnum);
		texture->flat = (UINT8 *)Picture_TextureToFlat(texnum);
		}
#ifdef PS2_PROFILE
		// PS2-OPT-03: a texture used as a flat was converted into a PU_STATIC block that nothing ever released
		// (MAP11: 50 blocks, 1.4 MB); like the plain flats it is a cache block now and is built again when it was evicted
		Z_SetUser(texture->flat, (void **)&texture->flat);
#if defined(PS2)
		// PS2-180: a flat of 1 MiB and more lives as long as the level (a cache block of this size cannot be made again once the arena is fragmented: MAPMG, 4.2 MB between 64 KB flats)
		Z_ChangeTag(texture->flat, streamed ? PU_LEVEL : PU_CACHE);
		if (streamed)
			CONS_Printf("FLATSTREAM texture %d %.8s (%dx%d): the flat is made from its patches, kept for the level\n", (int)texnum, texture->name, texture->width, texture->height);
		else
#else
		Z_ChangeTag(texture->flat, PU_CACHE);
#endif
		R_ReleaseTextureCache((INT32)texnum); // conversion copied every column; only the independent flat is still held
#if defined(PS2)
		if (R_FlatStreamMode() == 2 && !streamed && R_TextureStreamable(texture)) // -flatcheck: the same texture by the other route, compared
		{
			UINT8 *other = R_StreamTextureToFlat(texnum);
			const size_t size = (size_t)texture->width * (size_t)texture->height;

			if (!other)
				CONS_Printf("FLATCHECK texture %d (%dx%d): no second copy\n", (int)texnum, texture->width, texture->height);
			else
			{
				size_t k, diff = 0;

				for (k = 0; k < size; k++)
					diff += ((UINT8 *)texture->flat)[k] != other[k];
				CONS_Printf("FLATCHECK texture %d %.8s (%dx%d): %s%s%u texels differ\n", (int)texnum, texture->name, texture->width, texture->height, diff ? "DIFFER " : "equal ", "", (unsigned)diff);
				Z_Free(other);
			}
		}
#endif
#endif
	}

	flatmemory += texture->width * texture->height;

	return texture->flat;
}

#if defined(PS2) && defined(PS2_PROFILE)
// PS2-140 (OPT10-S): R_GetFlatForTexture for the hardware renderer, which can do without a flat (the draws that need it are skipped for a frame, the
// driver says so): a texture that is used as a flat and does not fit once converted (two 1 MiB blocks at a time on MAPMG: the composite and the flat)
// is NULL here instead of the end of the run. Flats that are lumps take the original path (the driver reads those in bands from the lump).
UINT8 *R_TryGetFlatForTexture(size_t texnum)
{
	texture_t *texture;
	UINT8 *flat;

	if (texnum >= (unsigned)numtextures)
		return NULL;
	texture = textures[texnum];
	if (texture->flat != NULL || texture->type == TEXTURETYPE_FLAT)
		return R_GetFlatForTexture(texnum);

	flat = NULL;
	if ((R_FlatStreamMode() == 1 || (R_FlatStreamMode() != 2 && (size_t)texture->width * (size_t)texture->height >= R_STREAM_MIN)) && R_TextureStreamable(texture))
		flat = R_StreamTextureToFlat(texnum); // PS2-180: no composite next to the flat
	if (flat)
	{
		texture->flat = flat;
		Z_SetUser(flat, (void **)&texture->flat);
		Z_ChangeTag(flat, PU_CACHE);
		flatmemory += texture->width * texture->height;
		return flat;
	}
	flat = (UINT8 *)Picture_TryTextureToFlat(texnum);
	if (!flat)
		return NULL;
	texture->flat = flat;
	Z_SetUser(flat, (void **)&texture->flat);
	Z_ChangeTag(flat, PU_CACHE);
	R_ReleaseTextureCache((INT32)texnum);
	flatmemory += texture->width * texture->height;
	return flat;
}
#endif

//
// R_GetTextureNum
//
// Returns the actual texture id that we should use.
// This can either be texnum, the current frame for texnum's anim (if animated),
// or 0 if not valid.
//
INT32 R_GetTextureNum(INT32 texnum)
{
	if (texnum < 0 || texnum >= numtextures)
		return 0;
	return texturetranslation[texnum];
}

//
// R_CheckTextureCache
//
// Use this if you need to make sure the texture is cached before R_GetColumn calls
// e.g.: midtextures and FOF walls
//
void R_CheckTextureCache(INT32 tex)
{
	if (!texturecache[tex])
		R_GenerateTexture(tex);
#ifdef PS2_PROFILE
	else
		Z_Touch(texturecache[tex]); // PS2-OPT-03: cache hit: used in this frame
#endif
}

#ifdef PS2_PROFILE
void R_ReleaseTextureCache(INT32 tex)
{
	if (tex > 0 && tex < numtextures)
		Z_ReleaseCache(texturecache[tex]);
}

void R_ReleaseFlatCache(INT32 tex)
{
	if (tex >= 0 && tex < numtextures)
		Z_ReleaseCache(textures[tex]->flat);
}
#endif

column_t *R_GetColumn(fixed_t tex, INT32 col)
{
	INT32 width = texturewidth[tex];
#ifdef PS2_PROFILE
	if (!texturecache[tex]) // the zone may have evicted it (PU_CACHE): rebuild
		R_GenerateTexture(tex);
	else
		Z_Touch(texturecache[tex]); // PS2-OPT-03: cache hit: used in this frame
#endif
	if (width & (width - 1))
		col = (UINT32)col % width;
	else
		col &= (width - 1);

	return &texturecolumns[tex][col];
}

INT32 R_GetTextureNumForFlat(levelflat_t *levelflat)
{
	return texturetranslation[levelflat->texture_id];
}

void *R_GetFlat(levelflat_t *levelflat)
{
	if (levelflat->type == LEVELFLAT_NONE)
		return NULL;

	return R_GetFlatForTexture(R_GetTextureNumForFlat(levelflat));
}

//
// Checks if the current flat's dimensions are powers of two
//
boolean R_CheckPowersOfTwo(void)
{
	if (ds_flatwidth > 2048 || ds_flatheight > 2048)
		return false;

	boolean wpow2 = !(ds_flatwidth & (ds_flatwidth - 1));
	boolean hpow2 = !(ds_flatheight & (ds_flatheight - 1));

	return ds_flatwidth == ds_flatheight && wpow2 && hpow2;
}

//
// Checks if the current flat's dimensions are 1x1
//
boolean R_CheckSolidColorFlat(void)
{
	return ds_flatwidth == 1 && ds_flatheight == 1;
}

//
// Returns the flat size corresponding to the length of a lump
//
UINT16 R_GetFlatSize(size_t length)
{
	switch (length)
	{
		case 4194304: // 2048x2048 lump
			return 2048;
		case 1048576: // 1024x1024 lump
			return 1024;
		case 262144:// 512x512 lump
			return 512;
		case 65536: // 256x256 lump
			return 256;
		case 16384: // 128x128 lump
			return 128;
		case 1024: // 32x32 lump
			return 32;
		case 256: // 16x16 lump
			return 16;
		case 64: // 8x8 lump
			return 8;
		case 16: // 4x4 lump
			return 4;
		case 4: // 2x2 lump
			return 2;
		case 1: // 1x1 lump
			return 1;
		default: // 64x64 lump
			return 64;
	}
}

//
// Determines a flat's width bits from its size
//
UINT8 R_GetFlatBits(INT32 size)
{
	switch (size)
	{
		case 2048: return 11;
		case 1024: return 10;
		case 512:  return 9;
		case 256:  return 8;
		case 128:  return 7;
		case 32:   return 5;
		case 16:   return 4;
		case 8:    return 3;
		case 4:    return 2;
		case 2:    return 1;
		case 1:    return 0;
		default:   return 6; // 64x64
	}
}

void R_SetFlatVars(size_t length)
{
	UINT16 size = R_GetFlatSize(length);
	UINT8 bits = R_GetFlatBits(size);

	ds_flatwidth = ds_flatheight = size;

	if (bits == 0)
		return;

	nflatshiftup = 16 - bits;
	nflatxshift = 16 + nflatshiftup;
	nflatyshift = nflatxshift - bits;
	nflatmask = (size - 1) * size;
}

//
// Empty the texture cache (used for load wad at runtime)
//
void R_FlushTextureCache(void)
{
	INT32 i;

	if (numtextures)
		for (i = 0; i < numtextures; i++)
		{
			Z_Free(textures[i]->flat);
			Z_Free(texturecache[i]);
		}
}

// Need these prototypes for later; defining them here instead of r_textures.h so they're "private"
int R_CountTexturesInTEXTURESLump(UINT16 wadNum, UINT16 lumpNum);
void R_ParseTEXTURESLump(UINT16 wadNum, UINT16 lumpNum, INT32 *index);

static void R_AddSinglePatchTexture(INT32 i, UINT16 wadnum, UINT16 lumpnum, INT16 width, INT16 height, unsigned type)
{
	texture_t *texture = Z_Calloc(sizeof(texture_t) + sizeof(texpatch_t), PU_STATIC, NULL);

	// Set texture properties.
	M_Memcpy(texture->name, W_CheckNameForNumPwad(wadnum, lumpnum), sizeof(texture->name));
	texture->hash = quickncasehash(texture->name, 8);

	texture->width = width;
	texture->height = height;

	texture->type = type;
	texture->patchcount = 1;
	texture->flip = 0;

	// Allocate information for the texture's patches.
	texpatch_t *patch = &texture->patches[0];

	patch->originx = patch->originy = 0;
	patch->wad = wadnum;
	patch->lump = lumpnum;
	patch->flip = 0;

	texturewidth[i] = texture->width;
	textureheight[i] = texture->height << FRACBITS;

	textures[i] = texture;
}

static INT32
Rloadflats (INT32 i, INT32 w)
{
	UINT16 j, numlumps = 0;
	UINT16 texstart = 0, texend = 0;
	UINT32 *list = NULL;

	// Get every lump inside the Flats/ folder
	if (W_FileHasFolders(wadfiles[w]))
	{
		W_GetFolderLumpsPwad("Flats/", (UINT16)w, &list, NULL, &numlumps);
	}
	else
	{
		// Else, get the lumps between F_START/F_END markers
		texstart = W_CheckNumForMarkerStartPwad("F_START", (UINT16)w, 0);
		texend = W_CheckNumForNamePwad("F_END", (UINT16)w, texstart);
		if (texstart == INT16_MAX || texend == INT16_MAX)
			return i;

		numlumps = texend - texstart;
	}

	// Now add every entry as a texture
	for (j = 0; j < numlumps; j++)
	{
		UINT16 wadnum = list ? WADFILENUM(list[j]) : (UINT16)w;
		UINT16 lumpnum = list ? LUMPNUM(list[j]) : (texstart + j);

		size_t lumplength = W_LumpLengthPwad(wadnum, lumpnum);
		size_t flatsize = R_GetFlatSize(lumplength);

		INT16 width = flatsize, height = flatsize;

#ifndef NO_PNG_LUMPS
		UINT8 header[PNG_HEADER_SIZE];

		W_ReadLumpHeaderPwad(wadnum, lumpnum, header, sizeof header, 0);

		if (Picture_IsLumpPNG(header, lumplength))
		{
			INT32 texw, texh;
			UINT8 *flatlump = W_CacheLumpNumPwad(wadnum, lumpnum, PU_CACHE);
			if (Picture_PNGDimensions((UINT8 *)flatlump, &texw, &texh, NULL, NULL, lumplength))
			{
				width = (INT16)texw;
				height = (INT16)texh;
			}
			else
			{
				width = 1;
				height = 1;
			}
			Z_Free(flatlump);
		}
#endif

		// printf("\"%s\" (wad: %u, lump: %u) is a flat, dimensions %d x %d\n",W_CheckNameForNumPwad(wadnum,lumpnum),wadnum,lumpnum,width,height);

		R_AddSinglePatchTexture(i, wadnum, lumpnum, width, height, TEXTURETYPE_FLAT);

		i++;
	}

	if (list)
		Z_Free(list);

	return i;
}

#define TX_START "TX_START"
#define TX_END "TX_END"

static INT32
Rloadtextures (INT32 i, INT32 w)
{
	UINT16 j, numlumps = 0;
	UINT16 texstart = 0, texend = 0;
	UINT16 texturesLumpPos;
	UINT32 *list = NULL;

	// Get every lump inside the Textures/ folder
	if (W_FileHasFolders(wadfiles[w]))
	{
		W_GetFolderLumpsPwad("Textures/", (UINT16)w, &list, NULL, &numlumps);

		texturesLumpPos = W_CheckNumForNamePwad("TEXTURES", (UINT16)w, 0);
		while (texturesLumpPos != INT16_MAX)
		{
			R_ParseTEXTURESLump(w, texturesLumpPos, &i);
			texturesLumpPos = W_CheckNumForNamePwad("TEXTURES", (UINT16)w, texturesLumpPos + 1);
		}
	}
	// Else, get the lumps between TX_START/TX_END markers
	else
	{
		texturesLumpPos = W_CheckNumForNamePwad("TEXTURES", (UINT16)w, 0);
		if (texturesLumpPos != INT16_MAX)
			R_ParseTEXTURESLump(w, texturesLumpPos, &i);

		texstart = W_CheckNumForMarkerStartPwad(TX_START, (UINT16)w, 0);
		texend = W_CheckNumForNamePwad(TX_END, (UINT16)w, texstart);
		if (texstart == INT16_MAX || texend == INT16_MAX)
			return i;

		numlumps = texend - texstart;
	}

	// Now add every entry as a texture
	for (j = 0; j < numlumps; j++)
	{
		UINT16 wadnum = list ? WADFILENUM(list[j]) : (UINT16)w;
		UINT16 lumpnum = list ? LUMPNUM(list[j]) : (texstart + j);

		INT16 width = 0, height = 0;

		if (!W_ReadPatchHeaderPwad(wadnum, lumpnum, &width, &height, NULL, NULL))
		{
			width = 1;
			height = 1;
		}

		// printf("\"%s\" (wad: %u, lump: %u) is a single patch, dimensions %d x %d\n",W_CheckNameForNumPwad(wadnum,lumpnum),wadnum,lumpnum,width,height);

		R_AddSinglePatchTexture(i, wadnum, lumpnum, width, height, TEXTURETYPE_SINGLEPATCH);

		i++;
	}

	if (list)
		Z_Free(list);

	return i;
}

static INT32
count_range
(		const char * marker_start,
		const char * marker_end,
		const char * folder,
		UINT16 wadnum)
{
	INT32 count = 0;

	if (W_FileHasFolders(wadfiles[wadnum]))
		count += W_CountFolderLumpsPwad(folder, wadnum);
	else
	{
		UINT16 texstart = W_CheckNumForMarkerStartPwad(marker_start, wadnum, 0);
		UINT16 texend = W_CheckNumForNamePwad(marker_end, wadnum, texstart);

		if (texstart != INT16_MAX && texend != INT16_MAX)
			count += (texend - texstart);
	}

	return count;
}

static INT32 R_CountTextures(UINT16 wadnum)
{
	UINT16 texturesLumpPos;
	INT32 count = 0;

	// Load patches and textures.

	// Get the number of textures to check.
	// NOTE: Make SURE the system does not process
	// the markers.
	// This system will allocate memory for all duplicate/patched textures even if it never uses them,
	// but the alternative is to spend a ton of time checking and re-checking all previous entries just to skip any potentially patched textures.

	count += count_range("F_START", "F_END", "flats/", wadnum);

	// Count the textures from TEXTURES lumps
	texturesLumpPos = W_CheckNumForNamePwad("TEXTURES", wadnum, 0);

	while (texturesLumpPos != INT16_MAX)
	{
		count += R_CountTexturesInTEXTURESLump(wadnum, texturesLumpPos);
		texturesLumpPos = W_CheckNumForNamePwad("TEXTURES", wadnum, texturesLumpPos + 1);
	}

	// Count single-patch textures
	count += count_range(TX_START, TX_END, "textures/", wadnum);

	return count;
}

static void
recallocuser
(		void * user,
		size_t old,
		size_t new)
{
	char *p = Z_Realloc(*(void**)user,
			new, PU_STATIC, user);

	if (new > old)
		memset(&p[old], 0, (new - old));
}

static void R_AllocateTextures(INT32 add)
{
	const INT32 newtextures = (numtextures + add);
	const size_t newsize = newtextures * sizeof (void*);
	const size_t oldsize = numtextures * sizeof (void*);

	INT32 i;

	// Allocate memory and initialize to 0 for all the textures we are initialising.
	recallocuser(&textures, oldsize, newsize);

	// Allocate texture column offset table.
	recallocuser(&texturecolumns, oldsize, newsize);
	// Allocate texture referencing cache.
	recallocuser(&texturecache, oldsize, newsize);
	// Allocate texture width table.
	recallocuser(&texturewidth, oldsize, newsize);
	// Allocate texture height table.
	recallocuser(&textureheight, oldsize, newsize);
	// Create translation table for global animation.
	Z_Realloc(texturetranslation, (newtextures + 1) * sizeof(*texturetranslation), PU_STATIC, &texturetranslation);

	for (i = 0; i < numtextures; ++i)
	{
		// R_FlushTextureCache relies on the user for
		// Z_Free, texturecache has been reallocated so the
		// user is now garbage memory.
		Z_SetUser(texturecache[i],
				(void**)&texturecache[i]);
	}

	while (i < newtextures)
	{
		texturetranslation[i] = i;
		i++;
	}
}

static INT32 R_DefineTextures(INT32 i, UINT16 w)
{
	i = Rloadflats(i, w);
	return Rloadtextures(i, w);
}

static void R_FinishLoadingTextures(INT32 add)
{
	numtextures += add;

#ifdef HWRENDER
	if (rendermode == render_opengl)
		HWR_LoadMapTextures(numtextures);
#endif
}

#ifdef PS2_PROFILE
// PS2-LOAD-20 (-loadprof -loadhash): a hash of the texture list as R_LoadTextures built it (names, sizes, types, every patch: wad, lump, origin, flip, alpha,
// style; texturewidth, textureheight, texturetranslation), printed as "LHASH tex.all"; two ELFs that load the same files must print the same line.
static void R_TexturesHash(const char *label)
{
	ps2lp_hash_t hs = { { 2166136261u, 0x811C9DC5u ^ 0xA5A5A5A5u } };
	INT32 i;
	int k;
	char lab[24];

	if (!ps2lp_on || !M_CheckParm("-loadhash"))
		return;
	PS2LP_H32(&hs, (UINT32)numtextures);
	for (i = 0; i < numtextures; i++)
	{
		const texture_t *t = textures[i];

		for (k = 0; k < 8; k++)
			PS2LP_H32(&hs, (UINT32)(UINT8)t->name[k]);
		PS2LP_H32(&hs, t->hash);
		PS2LP_H32(&hs, t->type);
		PS2LP_H32(&hs, (UINT32)t->width);
		PS2LP_H32(&hs, (UINT32)t->height);
		PS2LP_H32(&hs, t->flip);
		PS2LP_H32(&hs, (UINT32)t->patchcount);
		for (k = 0; k < t->patchcount; k++)
		{
			const texpatch_t *p = &t->patches[k];

			PS2LP_H32(&hs, (UINT32)(UINT16)p->originx);
			PS2LP_H32(&hs, (UINT32)(UINT16)p->originy);
			PS2LP_H32(&hs, p->wad);
			PS2LP_H32(&hs, p->lump);
			PS2LP_H32(&hs, p->flip);
			PS2LP_H32(&hs, p->alpha);
			PS2LP_H32(&hs, (UINT32)p->style);
		}
		PS2LP_H32(&hs, (UINT32)texturewidth[i]);
		PS2LP_H32(&hs, (UINT32)textureheight[i]);
		PS2LP_H32(&hs, (UINT32)texturetranslation[i]);
	}
	strcpy(lab, label);
	PS2LP_HashPrint(lab, &hs);
}
#endif

//
// R_LoadTextures
// Initializes the texture list with the textures from the world map.
//
void R_LoadTextures(void)
{
	INT32 i, w;
	INT32 newtextures = 0;

	for (w = 0; w < numwadfiles; w++)
	{
		newtextures += R_CountTextures((UINT16)w);
	}

	// If no textures found by this point, bomb out
	if (!newtextures)
		I_Error("No textures detected in any WADs!\n");

	R_AllocateTextures(newtextures);

	for (i = 0, w = 0; w < numwadfiles; w++)
	{
		i = R_DefineTextures(i, w);
	}

	R_FinishLoadingTextures(newtextures);
#ifdef PS2_PROFILE
	R_TexturesHash("tex.boot");
#endif
}

void R_LoadTexturesPwad(UINT16 wadnum)
{
	INT32 newtextures = R_CountTextures(wadnum);

	R_AllocateTextures(newtextures);
	R_DefineTextures(numtextures, wadnum);
	R_FinishLoadingTextures(newtextures);
#ifdef PS2_PROFILE
	R_TexturesHash("tex.pwad");
#endif
}

static lumpnum_t W_GetTexPatchLumpNum(const char *name)
{
	// Flats as a texture patch crashes horribly, and flats
	// can share the same name as textures.

	// But even if they worked, we want to prioritize:
	// Patches -> Textures -> anything else

	enum
	{
		USE_PATCHES,
		USE_TEXTURES,
		USE__MAX,
	};

	lumpnum_t lump = LUMPERROR;
	INT32 lump_type_it;


	for (lump_type_it = 0; lump_type_it < USE__MAX; lump_type_it++)
	{
		INT32 i;

		for (i = numwadfiles - 1; i >= 0; i--) // Scan wad files backwards so patched lumps take precedent
		{
			lumpnum_t start = LUMPERROR;
			lumpnum_t end = LUMPERROR;

			switch (wadfiles[i]->type)
			{
				case RET_WAD:
					if (lump_type_it == USE_PATCHES)
					{
						if ((start = W_CheckNumForMarkerStartPwad("P_START", (UINT16)i, 0)) == INT16_MAX)
							continue;
						else if ((end = W_CheckNumForNamePwad("P_END", (UINT16)i, start)) == INT16_MAX)
							continue;
					}
					else if (lump_type_it == USE_TEXTURES)
					{
						if ((start = W_CheckNumForMarkerStartPwad("TX_START", (UINT16)i, 0)) == INT16_MAX)
							continue;
						else if ((end = W_CheckNumForNamePwad("TX_END", (UINT16)i, start)) == INT16_MAX)
							continue;
					}
					break;
				case RET_PK3:
				case RET_FOLDER:
					if (lump_type_it == USE_PATCHES)
					{
						if ((start = W_CheckNumForFolderStartPK3("Patches/", i, 0)) == INT16_MAX)
							continue;
						if ((end = W_CheckNumForFolderEndPK3("Patches/", i, start)) == INT16_MAX)
							continue;
					}
					else if (lump_type_it == USE_TEXTURES)
					{
						if ((start = W_CheckNumForFolderStartPK3("Textures/", i, 0)) == INT16_MAX)
							continue;
						if ((end = W_CheckNumForFolderEndPK3("Textures/", i, start)) == INT16_MAX)
							continue;
					}
					break;
				default:
					continue;
			}

			// Now find lump with specified name in that range.
			lump = W_CheckNumForNamePwad(name, (UINT16)i, start);
			if (lump < end)
			{
				lump += (i<<16); // found it, in our constraints
				break;
			}
			lump = LUMPERROR;
		}

		if (lump != LUMPERROR)
		{
			break;
		}
	}

	if (lump == LUMPERROR)
	{
		// Use whatever else you can find.
		return W_CheckNumForPatchName(name);
	}

	return lump;
}

static texpatch_t *R_ParsePatch(boolean actuallyLoadPatch)
{
	char *texturesToken;
	size_t texturesTokenLength;
	char *endPos;
	char *patchName = NULL;
	INT16 patchXPos;
	INT16 patchYPos;
	UINT8 flip = 0;
	UINT8 alpha = 255;
	enum patchalphastyle style = AST_COPY;
	texpatch_t *resultPatch = NULL;
	lumpnum_t patchLumpNum;

	// Patch identifier
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where patch name should be");
	}
	texturesTokenLength = strlen(texturesToken);
	if (texturesTokenLength>8)
	{
		I_Error("Error parsing TEXTURES lump: Patch name \"%s\" exceeds 8 characters",texturesToken);
	}
	else
	{
		if (patchName != NULL)
		{
			Z_Free(patchName);
		}
		patchName = (char *)Z_Malloc((texturesTokenLength+1)*sizeof(char),PU_STATIC,NULL);
		M_Memcpy(patchName,texturesToken,texturesTokenLength*sizeof(char));
		patchName[texturesTokenLength] = '\0';
	}

	// Comma 1
	M_FreeToken(texturesToken);
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where comma after \"%s\"'s patch name should be",patchName);
	}
	if (strcmp(texturesToken,",")!=0)
	{
		I_Error("Error parsing TEXTURES lump: Expected \",\" after %s's patch name, got \"%s\"",patchName,texturesToken);
	}

	// XPos
	M_FreeToken(texturesToken);
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where patch \"%s\"'s x coordinate should be",patchName);
	}
	endPos = NULL;
#ifndef AVOID_ERRNO
	errno = 0;
#endif
	patchXPos = strtol(texturesToken,&endPos,10);
	(void)patchXPos; //unused for now
	if (endPos == texturesToken // Empty string
		|| *endPos != '\0' // Not end of string
#ifndef AVOID_ERRNO
		|| errno == ERANGE // Number out-of-range
#endif
		)
	{
		I_Error("Error parsing TEXTURES lump: Expected an integer for patch \"%s\"'s x coordinate, got \"%s\"",patchName,texturesToken);
	}

	// Comma 2
	M_FreeToken(texturesToken);
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where comma after patch \"%s\"'s x coordinate should be",patchName);
	}
	if (strcmp(texturesToken,",")!=0)
	{
		I_Error("Error parsing TEXTURES lump: Expected \",\" after patch \"%s\"'s x coordinate, got \"%s\"",patchName,texturesToken);
	}

	// YPos
	M_FreeToken(texturesToken);
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where patch \"%s\"'s y coordinate should be",patchName);
	}
	endPos = NULL;
#ifndef AVOID_ERRNO
	errno = 0;
#endif
	patchYPos = strtol(texturesToken,&endPos,10);
	(void)patchYPos; //unused for now
	if (endPos == texturesToken // Empty string
		|| *endPos != '\0' // Not end of string
#ifndef AVOID_ERRNO
		|| errno == ERANGE // Number out-of-range
#endif
		)
	{
		I_Error("Error parsing TEXTURES lump: Expected an integer for patch \"%s\"'s y coordinate, got \"%s\"",patchName,texturesToken);
	}
	M_FreeToken(texturesToken);

	// Patch parameters block (OPTIONAL)
	// added by Monster Iestyn (22/10/16)

	// Left Curly Brace
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
		; // move on and ignore, R_ParseTextures will deal with this
	else
	{
		if (strcmp(texturesToken,"{")==0)
		{
			M_FreeToken(texturesToken);
			texturesToken = M_GetTokenPooled(NULL);
			if (texturesToken == NULL)
			{
				I_Error("Error parsing TEXTURES lump: Unexpected end of file where patch \"%s\"'s parameters should be",patchName);
			}
			while (strcmp(texturesToken,"}")!=0)
			{
				if (stricmp(texturesToken, "ALPHA")==0)
				{
					M_FreeToken(texturesToken);
					texturesToken = M_GetTokenPooled(NULL);
					alpha = 255*strtof(texturesToken, NULL);
				}
				else if (stricmp(texturesToken, "STYLE")==0)
				{
					M_FreeToken(texturesToken);
					texturesToken = M_GetTokenPooled(NULL);
					if (stricmp(texturesToken, "TRANSLUCENT")==0)
						style = AST_TRANSLUCENT;
					else if (stricmp(texturesToken, "ADD")==0)
						style = AST_ADD;
					else if (stricmp(texturesToken, "SUBTRACT")==0)
						style = AST_SUBTRACT;
					else if (stricmp(texturesToken, "REVERSESUBTRACT")==0)
						style = AST_REVERSESUBTRACT;
					else if (stricmp(texturesToken, "MODULATE")==0)
						style = AST_MODULATE;
				}
				else if (stricmp(texturesToken, "FLIPX")==0)
					flip |= 1;
				else if (stricmp(texturesToken, "FLIPY")==0)
					flip |= 2;
				M_FreeToken(texturesToken);

				texturesToken = M_GetTokenPooled(NULL);
				if (texturesToken == NULL)
				{
					I_Error("Error parsing TEXTURES lump: Unexpected end of file where patch \"%s\"'s parameters or right curly brace should be",patchName);
				}
			}
		}
		else
		{
			 // this is not what we wanted...
			 // undo last read so R_ParseTextures can re-get the token for its own purposes
			M_UnGetToken();
		}
		M_FreeToken(texturesToken);
	}

	if (actuallyLoadPatch == true)
	{
		// Check lump exists
		patchLumpNum = W_GetTexPatchLumpNum(patchName);
		// If so, allocate memory for texpatch_t and fill 'er up
		resultPatch = (texpatch_t *)Z_Malloc(sizeof(texpatch_t),PU_STATIC,NULL);
		resultPatch->originx = patchXPos;
		resultPatch->originy = patchYPos;
		resultPatch->lump = LUMPNUM(patchLumpNum);
		resultPatch->wad = WADFILENUM(patchLumpNum);
		resultPatch->flip = flip;
		resultPatch->alpha = alpha;
		resultPatch->style = style;
		// Clean up a little after ourselves
		Z_Free(patchName);
		// Then return it
		return resultPatch;
	}
	else
	{
		Z_Free(patchName);
		return NULL;
	}
}

static texture_t *R_ParseTexture(boolean actuallyLoadTexture)
{
	char *texturesToken;
	size_t texturesTokenLength;
	char *endPos;
	INT32 newTextureWidth;
	INT32 newTextureHeight;
	texture_t *resultTexture = NULL;
	texpatch_t *newPatch;
	char newTextureName[9]; // no longer dynamically allocated

	// Texture name
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where texture name should be");
	}
	texturesTokenLength = strlen(texturesToken);
	if (texturesTokenLength>8)
	{
		I_Error("Error parsing TEXTURES lump: Texture name \"%s\" exceeds 8 characters",texturesToken);
	}
	else
	{
		memset(&newTextureName, 0, 9);
		M_Memcpy(newTextureName, texturesToken, texturesTokenLength);
		// ^^ we've confirmed that the token is <= 8 characters so it will never overflow a 9 byte char buffer
		strupr(newTextureName); // Just do this now so we don't have to worry about it
	}
	M_FreeToken(texturesToken);

	// Comma 1
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where comma after texture \"%s\"'s name should be",newTextureName);
	}
	else if (strcmp(texturesToken,",")!=0)
	{
		I_Error("Error parsing TEXTURES lump: Expected \",\" after texture \"%s\"'s name, got \"%s\"",newTextureName,texturesToken);
	}
	M_FreeToken(texturesToken);

	// Width
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where texture \"%s\"'s width should be",newTextureName);
	}
	endPos = NULL;
#ifndef AVOID_ERRNO
	errno = 0;
#endif
	newTextureWidth = strtol(texturesToken,&endPos,10);
	if (endPos == texturesToken // Empty string
		|| *endPos != '\0' // Not end of string
#ifndef AVOID_ERRNO
		|| errno == ERANGE // Number out-of-range
#endif
		|| newTextureWidth < 0) // Number is not positive
	{
		I_Error("Error parsing TEXTURES lump: Expected a positive integer for texture \"%s\"'s width, got \"%s\"",newTextureName,texturesToken);
	}
	M_FreeToken(texturesToken);

	// Comma 2
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where comma after texture \"%s\"'s width should be",newTextureName);
	}
	if (strcmp(texturesToken,",")!=0)
	{
		I_Error("Error parsing TEXTURES lump: Expected \",\" after texture \"%s\"'s width, got \"%s\"",newTextureName,texturesToken);
	}
	M_FreeToken(texturesToken);

	// Height
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where texture \"%s\"'s height should be",newTextureName);
	}
	endPos = NULL;
#ifndef AVOID_ERRNO
	errno = 0;
#endif
	newTextureHeight = strtol(texturesToken,&endPos,10);
	if (endPos == texturesToken // Empty string
		|| *endPos != '\0' // Not end of string
#ifndef AVOID_ERRNO
		|| errno == ERANGE // Number out-of-range
#endif
		|| newTextureHeight < 0) // Number is not positive
	{
		I_Error("Error parsing TEXTURES lump: Expected a positive integer for texture \"%s\"'s height, got \"%s\"",newTextureName,texturesToken);
	}
	M_FreeToken(texturesToken);

	// Left Curly Brace
	texturesToken = M_GetTokenPooled(NULL);
	if (texturesToken == NULL)
	{
		I_Error("Error parsing TEXTURES lump: Unexpected end of file where open curly brace for texture \"%s\" should be",newTextureName);
	}
	if (strcmp(texturesToken,"{")==0)
	{
		if (actuallyLoadTexture)
		{
			// Allocate memory for a zero-patch texture. Obviously, we'll be adding patches momentarily.
			resultTexture = (texture_t *)Z_Calloc(sizeof(texture_t),PU_STATIC,NULL);
			M_Memcpy(resultTexture->name, newTextureName, 8);
			resultTexture->hash = quickncasehash(newTextureName, 8);
			resultTexture->width = newTextureWidth;
			resultTexture->height = newTextureHeight;
			resultTexture->type = TEXTURETYPE_COMPOSITE;
		}
		M_FreeToken(texturesToken);
		texturesToken = M_GetTokenPooled(NULL);
		if (texturesToken == NULL)
		{
			I_Error("Error parsing TEXTURES lump: Unexpected end of file where patch definition for texture \"%s\" should be",newTextureName);
		}
		while (strcmp(texturesToken,"}")!=0)
		{
			if (stricmp(texturesToken, "PATCH")==0)
			{
				M_FreeToken(texturesToken);
				if (resultTexture)
				{
					// Get that new patch
					newPatch = R_ParsePatch(true);
					// Make room for the new patch
					resultTexture = Z_Realloc(resultTexture, sizeof(texture_t) + (resultTexture->patchcount+1)*sizeof(texpatch_t), PU_STATIC, NULL);
					// Populate the uninitialized values in the new patch entry of our array
					M_Memcpy(&resultTexture->patches[resultTexture->patchcount], newPatch, sizeof(texpatch_t));
					// Account for the new number of patches in the texture
					resultTexture->patchcount++;
					// Then free up the memory assigned to R_ParsePatch, as it's unneeded now
					Z_Free(newPatch);
				}
				else
				{
					R_ParsePatch(false);
				}
			}
			else
			{
				I_Error("Error parsing TEXTURES lump: Expected \"PATCH\" in texture \"%s\", got \"%s\"",newTextureName,texturesToken);
			}

			texturesToken = M_GetTokenPooled(NULL);
			if (texturesToken == NULL)
			{
				I_Error("Error parsing TEXTURES lump: Unexpected end of file where patch declaration or right curly brace for texture \"%s\" should be",newTextureName);
			}
		}
		if (resultTexture && resultTexture->patchcount == 0)
		{
			I_Error("Error parsing TEXTURES lump: Texture \"%s\" must have at least one patch",newTextureName);
		}
	}
	else
	{
		I_Error("Error parsing TEXTURES lump: Expected \"{\" for texture \"%s\", got \"%s\"",newTextureName,texturesToken);
	}
	M_FreeToken(texturesToken);

	if (actuallyLoadTexture) return resultTexture;
	else return NULL;
}

// Parses the TEXTURES lump... but just to count the number of textures.
int R_CountTexturesInTEXTURESLump(UINT16 wadNum, UINT16 lumpNum)
{
	char *texturesLump;
	size_t texturesLumpLength;
	char *texturesText;
	UINT32 numTexturesInLump = 0;
	char *texturesToken;

	// Since lumps AREN'T \0-terminated like I'd assumed they should be, I'll
	// need to make a space of memory where I can ensure that it will terminate
	// correctly. Start by loading the relevant data from the WAD.
	texturesLump = (char *)W_CacheLumpNumPwad(wadNum, lumpNum, PU_STATIC);
	// If that didn't exist, we have nothing to do here.
	if (texturesLump == NULL) return 0;
	// If we're still here, then it DOES exist; figure out how long it is, and allot memory accordingly.
	texturesLumpLength = W_LumpLengthPwad(wadNum, lumpNum);
	texturesText = (char *)Z_Malloc((texturesLumpLength+1)*sizeof(char),PU_STATIC,NULL);
	// Now move the contents of the lump into this new location.
	memmove(texturesText,texturesLump,texturesLumpLength);
	// Make damn well sure the last character in our new memory location is \0.
	texturesText[texturesLumpLength] = '\0';
	// Finally, free up the memory from the first data load, because we really
	// don't need it.
	Z_Free(texturesLump);

	texturesToken = M_GetTokenPooled(texturesText);
	while (texturesToken != NULL)
	{
		if (stricmp(texturesToken, "WALLTEXTURE") == 0 || stricmp(texturesToken, "TEXTURE") == 0)
		{
			numTexturesInLump++;
			M_FreeToken(texturesToken);
			R_ParseTexture(false);
		}
		else
		{
			I_Error("Error parsing TEXTURES lump: Expected \"WALLTEXTURE\" or \"TEXTURE\", got \"%s\"",texturesToken);
		}
		texturesToken = M_GetTokenPooled(NULL);
	}
	M_FreeToken(texturesToken);
	Z_Free((void *)texturesText);

	return numTexturesInLump;
}

// Parses the TEXTURES lump... for real, this time.
void R_ParseTEXTURESLump(UINT16 wadNum, UINT16 lumpNum, INT32 *texindex)
{
	char *texturesLump;
	size_t texturesLumpLength;
	char *texturesText;
	char *texturesToken;
	texture_t *newTexture;

	I_Assert(texindex != NULL);

	// Since lumps AREN'T \0-terminated like I'd assumed they should be, I'll
	// need to make a space of memory where I can ensure that it will terminate
	// correctly. Start by loading the relevant data from the WAD.
	texturesLump = (char *)W_CacheLumpNumPwad(wadNum, lumpNum, PU_STATIC);
	// If that didn't exist, we have nothing to do here.
	if (texturesLump == NULL) return;
	// If we're still here, then it DOES exist; figure out how long it is, and allot memory accordingly.
	texturesLumpLength = W_LumpLengthPwad(wadNum, lumpNum);
	texturesText = (char *)Z_Malloc((texturesLumpLength+1)*sizeof(char),PU_STATIC,NULL);
	// Now move the contents of the lump into this new location.
	memmove(texturesText,texturesLump,texturesLumpLength);
	// Make damn well sure the last character in our new memory location is \0.
	texturesText[texturesLumpLength] = '\0';
	// Finally, free up the memory from the first data load, because we really
	// don't need it.
	Z_Free(texturesLump);

	texturesToken = M_GetTokenPooled(texturesText);
	while (texturesToken != NULL)
	{
		if (stricmp(texturesToken, "WALLTEXTURE") == 0 || stricmp(texturesToken, "TEXTURE") == 0)
		{
			M_FreeToken(texturesToken);
			// Get the new texture
			newTexture = R_ParseTexture(true);
			// Store the new texture
			textures[*texindex] = newTexture;
			texturewidth[*texindex] = newTexture->width;
			textureheight[*texindex] = newTexture->height << FRACBITS;
			// Increment i back in R_LoadTextures()
			(*texindex)++;
		}
		else
		{
			I_Error("Error parsing TEXTURES lump: Expected \"WALLTEXTURE\" or \"TEXTURE\", got \"%s\"",texturesToken);
		}
		texturesToken = M_GetTokenPooled(NULL);
	}
	M_FreeToken(texturesToken);
	Z_Free((void *)texturesText);
}

void R_ClearTextureNumCache(boolean btell)
{
	if (tidcache)
		Z_Free(tidcache);
	tidcache = NULL;
	if (btell)
		CONS_Debug(DBG_SETUP, "Fun Fact: There are %d textures used in this map.\n", tidcachelen);
	tidcachelen = 0;
}

static void AddTextureToCache(const char *name, UINT32 hash, INT32 id, UINT8 type)
{
	tidcachelen++;
	Z_Realloc(tidcache, tidcachelen * sizeof(*tidcache), PU_STATIC, &tidcache);
	strncpy(tidcache[tidcachelen-1].name, name, 8);
	tidcache[tidcachelen-1].name[8] = '\0';
#ifndef ZDEBUG
	CONS_Debug(DBG_SETUP, "texture #%s: %s\n", sizeu1(tidcachelen), tidcache[tidcachelen-1].name);
#endif
	tidcache[tidcachelen-1].hash = hash;
	tidcache[tidcachelen-1].id = id;
	tidcache[tidcachelen-1].type = type;
}

//
// R_CheckTextureNumForName
//
// Check whether texture is available. Filter out NoTexture indicator.
//
INT32 R_CheckTextureNumForName(const char *name)
{
	INT32 i;
	UINT32 hash;

	// "NoTexture" marker.
	if (name[0] == '-')
		return 0;

	hash = quickncasehash(name, 8);

	for (i = 0; i < tidcachelen; i++)
		if (tidcache[i].hash == hash && !strncasecmp(tidcache[i].name, name, 8))
			return tidcache[i].id;

	// Need to parse the list backwards, so textures loaded more recently are used in lieu of ones loaded earlier
	for (i = numtextures - 1; i >= 0; i--)
		if (textures[i]->hash == hash && !strncasecmp(textures[i]->name, name, 8))
		{
			AddTextureToCache(name, hash, i, textures[i]->type);
			return i;
		}

	return -1;
}

//
// R_CheckTextureNameForNum
//
// because sidedefs use numbers and sometimes you want names
// returns no texture marker if no texture was found
//
const char *R_CheckTextureNameForNum(INT32 num)
{
	if (num > 0 && num < numtextures)
		return textures[num]->name;

	return "-";
}

//
// R_TextureNameForNum
//
// calls R_CheckTextureNameForNum and returns REDWALL if result is a no texture marker
//
const char *R_TextureNameForNum(INT32 num)
{
	const char *result = R_CheckTextureNameForNum(num);

	if (strcmp(result, "-") == 0)
		return "REDWALL";

	return result;
}

//
// R_TextureNumForName
//
// Calls R_CheckTextureNumForName, aborts with error message.
//
INT32 R_TextureNumForName(const char *name)
{
	const INT32 i = R_CheckTextureNumForName(name);

	if (i == -1)
	{
		static INT32 redwall = -2;
		CONS_Debug(DBG_SETUP, "WARNING: R_TextureNumForName: %.8s not found\n", name);
		if (redwall == -2)
			redwall = R_CheckTextureNumForName("REDWALL");
		if (redwall != -1)
			return redwall;
		return 1;
	}
	return i;
}

// Like R_CheckTextureNumForName, but only looks in the flat namespace specifically.
INT32 R_CheckFlatNumForName(const char *name)
{
	INT32 i;
	UINT32 hash;

	// "NoTexture" marker.
	if (name[0] == '-')
		return 0;

	hash = quickncasehash(name, 8);

	for (i = 0; i < tidcachelen; i++)
		if (tidcache[i].type == TEXTURETYPE_FLAT && tidcache[i].hash == hash && !strncasecmp(tidcache[i].name, name, 8))
			return tidcache[i].id;

	for (i = numtextures - 1; i >= 0; i--)
		if (textures[i]->hash == hash && !strncasecmp(textures[i]->name, name, 8) && textures[i]->type == TEXTURETYPE_FLAT)
		{
			AddTextureToCache(name, hash, i, TEXTURETYPE_FLAT);
			return i;
		}

	return -1;
}
