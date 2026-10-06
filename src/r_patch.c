// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2020-2023 by Lactozilla.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_patch.c
/// \brief Patch generation.

#include "doomdef.h"
#include "r_patch.h"
#include "r_picformats.h"
#include "r_defs.h"
#include "z_zone.h"

#ifdef HWRENDER
#include "hardware/hw_glob.h"
#endif

//
// Creates a patch.
//

patch_t *Patch_Create(INT16 width, INT16 height)
{
	patch_t *patch = Z_Calloc(sizeof(patch_t), PU_PATCH, NULL);
	patch->width = width;
	patch->height = height;
	return patch;
}

#ifdef PS2_PROFILE
// PS2-OPT-03: a patch made from a Doom patch is ONE zone block (structure, columns, posts, pixels). The zone can then
// evict a sprite patch with a single owner pointer (the data arrays used to be separate blocks without an owner), and
// the three extra block headers and their 16-byte rounding per patch are saved (1339 patches on MAP11: ~100 KB).
#define PATCH_ALIGN(x) (((x) + 15u) & ~(size_t)15u)

patch_t *Patch_CreateFromDoomPatch(softwarepatch_t *source)
{
	size_t total_pixels = 0, total_posts = 0, coloff, postoff, pixoff;
	patch_t *patch;
	UINT8 *base;
	INT16 width;

	if (!source)
		return Patch_Create(0, 0);

	width = SHORT(source->width);
	Patch_CalcDataSizes(source, &total_pixels, &total_posts);

	coloff = PATCH_ALIGN(sizeof (patch_t));
	postoff = PATCH_ALIGN(coloff + sizeof (column_t) * (size_t)width);
	pixoff = PATCH_ALIGN(postoff + sizeof (post_t) * total_posts);

	patch = Z_Calloc(pixoff + total_pixels, PU_PATCH, NULL);
	base = (UINT8 *)patch;
	patch->width      = width;
	patch->height     = SHORT(source->height);
	patch->leftoffset = SHORT(source->leftoffset);
	patch->topoffset  = SHORT(source->topoffset);
	patch->columns = (column_t *)(base + coloff);
	patch->posts = (post_t *)(base + postoff);
	patch->pixels = base + pixoff;
	patch->embedded = 1;

	Patch_MakeColumns(source, patch->width, patch->width, patch->pixels, patch->columns, patch->posts, false);

	return patch;
}

// The zone may drop an embedded patch that nothing hangs off: no flats, no rotated copies, no hardware texture.
boolean Patch_IsEvictable(const void *p)
{
	const patch_t *patch = p;
	int i;

	if (!patch->embedded || patch->hardware)
		return false;
	for (i = 0; i < 4; i++)
		if (patch->flats[i])
			return false;
#ifdef ROTSPRITE
	if (patch->rotated)
		return false;
#endif
	return true;
}
#else
patch_t *Patch_CreateFromDoomPatch(softwarepatch_t *source)
{
	patch_t *patch = Patch_Create(0, 0);
	if (!source)
		return patch;

	patch->width      = SHORT(source->width);
	patch->height     = SHORT(source->height);
	patch->leftoffset = SHORT(source->leftoffset);
	patch->topoffset  = SHORT(source->topoffset);

	size_t total_pixels = 0;
	size_t total_posts = 0;

	Patch_CalcDataSizes(source, &total_pixels, &total_posts);

	patch->columns = Z_Calloc(sizeof(column_t) * patch->width, PU_PATCH_DATA, NULL);
	patch->posts = Z_Calloc(sizeof(post_t) * total_posts, PU_PATCH_DATA, NULL);
	patch->pixels = Z_Calloc(sizeof(UINT8) * total_pixels, PU_PATCH_DATA, NULL);

	Patch_MakeColumns(source, patch->width, patch->width, patch->pixels, patch->columns, patch->posts, false);

	return patch;
}
#endif

void Patch_CalcDataSizes(softwarepatch_t *source, size_t *total_pixels, size_t *total_posts)
{
	for (INT32 i = 0; i < source->width; i++)
	{
		doompost_t *src_posts = (doompost_t*)((UINT8 *)source + LONG(source->columnofs[i]));
		for (doompost_t *post = src_posts; post->topdelta != 0xff ;)
		{
			(*total_posts)++;
			(*total_pixels) += post->length;
			post = (doompost_t *)((UINT8 *)post + post->length + 4);
		}
	}
}

void Patch_MakeColumns(softwarepatch_t *source, size_t num_columns, INT16 width, UINT8 *pixels, column_t *columns, post_t *posts, boolean flip)
{
	column_t *column = flip ? columns + (num_columns - 1) : columns;

	for (size_t i = 0; i < num_columns; i++)
	{
		size_t prevdelta = 0;
		size_t data_offset = 0;

		column->pixels = pixels;
		column->posts = posts;
		column->num_posts = 0;

		if (i >= (unsigned)width)
			continue;

		doompost_t *src_posts = (doompost_t*)((UINT8 *)source + LONG(source->columnofs[i]));

		for (doompost_t *post = src_posts; post->topdelta != 0xff ;)
		{
			size_t topdelta = post->topdelta;
			if (topdelta <= prevdelta)
				topdelta += prevdelta;
			prevdelta = topdelta;

			posts->topdelta = topdelta;
			posts->length = (size_t)post->length;
			posts->data_offset = data_offset;

			memcpy(pixels, (UINT8 *)post + 3, post->length);

			data_offset += posts->length;
			pixels += posts->length;

			column->num_posts++;

			posts++;

			post = (doompost_t *)((UINT8 *)post + post->length + 4);
		}

		if (flip)
			column--;
		else
			column++;
	}
}

//
// Frees a patch from memory.
//

static void Patch_FreeData(patch_t *patch)
{
	INT32 i;

#ifdef HWRENDER
	if (patch->hardware)
		HWR_FreeTexture(patch);
#endif

	for (i = 0; i < 4; i++)
	{
		if (patch->flats[i])
			Z_Free(patch->flats[i]);
	}

#ifdef ROTSPRITE
	if (patch->rotated)
	{
		rotsprite_t *rotsprite = patch->rotated;

		for (i = 0; i < rotsprite->angles; i++)
		{
			if (rotsprite->patches[i])
				Patch_Free(rotsprite->patches[i]);
		}

		Z_Free(rotsprite->patches);
		Z_Free(rotsprite);
	}
#endif

#ifdef PS2_PROFILE
	if (patch->embedded)
		return; // the data is part of the patch block
#endif

	if (patch->pixels)
		Z_Free(patch->pixels);
	if (patch->columns)
		Z_Free(patch->columns);
	if (patch->posts)
		Z_Free(patch->posts);
}

void Patch_Free(patch_t *patch)
{
	Patch_FreeData(patch);
	Z_Free(patch);
}

//
// Frees patches with a tag range.
//

static boolean Patch_FreeTagsCallback(void *mem)
{
	patch_t *patch = (patch_t *)mem;
	Patch_FreeData(patch);
	return true;
}

void Patch_FreeTags(INT32 lowtag, INT32 hightag)
{
	Z_IterateTags(lowtag, hightag, Patch_FreeTagsCallback);
}

void Patch_GenerateFlat(patch_t *patch, pictureflags_t flags)
{
	UINT8 flip = (flags & (PICFLAGS_XFLIP | PICFLAGS_YFLIP));
	if (patch->flats[flip] == NULL)
		patch->flats[flip] = Picture_Convert(PICFMT_PATCH, patch, PICFMT_FLAT16, 0, NULL, 0, 0, 0, 0, flags);
}

#ifdef HWRENDER
//
// Allocates a hardware patch.
//

void *Patch_AllocateHardwarePatch(patch_t *patch)
{
	if (!patch->hardware)
	{
		GLPatch_t *grPatch = Z_Calloc(sizeof(GLPatch_t), PU_HWRPATCHINFO, &patch->hardware);
		grPatch->mipmap = Z_Calloc(sizeof(GLMipmap_t), PU_HWRPATCHINFO, &grPatch->mipmap);
	}
	return (void *)(patch->hardware);
}

//
// Creates a hardware patch.
//

void *Patch_CreateGL(patch_t *patch)
{
	GLPatch_t *grPatch = (GLPatch_t *)Patch_AllocateHardwarePatch(patch);
	if (!grPatch->mipmap->data) // Run HWR_MakePatch in all cases, to recalculate some things
		HWR_MakePatch(patch, grPatch, grPatch->mipmap, false);
	return grPatch;
}
#endif // HWRENDER
