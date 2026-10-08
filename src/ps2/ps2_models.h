// SONIC ROBO BLAST 2 (PS2 port)
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_models.h
/// \brief OPT11-MODEL: 3D models on the PS2. The cooked model pack MODELS.PAK (tools/ps2/cook_models.py), the models in the engine's tinyframe form made from
/// it in ONE zone block (reclaimable under memory pressure), 8 bit model textures (palette indices, index 255 = hole) and the skin colour blend maps.

#ifndef __PS2_MODELS__
#define __PS2_MODELS__

#include "../doomtype.h"
#include "../hardware/hw_model.h"
#include "../hardware/hw_data.h"
#include "../hardware/hw_md2.h"

#ifdef PS2_PROFILE

// MODELS.PAK next to the ELF (or in the home directory) can be opened; opened on the first call, the answer is remembered
boolean PS2Models_PackPresent(void);

// The models.dat lump of the pack (NULL if none); *size = its length. The copy is freed with free().
char *PS2Models_Dat(size_t *size);

// Reads the cooked model of models.dat's file name `rel` ("PLAY/SONIC.md3") into one zone block owned by *owner (= &md2->model).
// NULL when there is no cooked model or no room (*why = 0 none, 1 no memory, 2 damaged, 3 too many models were read in this frame: ask again in the next one): never an I_Error.
model_t *PS2Models_Load(const char *rel, void **owner, int *why);

// A model of the pack is in use this frame (not a candidate for the reclaim hook until the next frame)
void PS2Models_Touch(model_t *model);

// Frees a model made by PS2Models_Load (the block, its sprite 2 tables, readjusted texture coordinates): owner is cleared by Z_Free.
void PS2Models_Free(model_t *model);

// The size of a model made by PS2Models_Load (0 = unknown: a loose file): the farthest vertex from its origin over every frame, in 1/64 MD3 units (the units of the vertices)
float PS2Models_Radius(const model_t *model);

// True if `model` was made by PS2Models_Load
boolean PS2Models_Is(const model_t *model);

// The cooked texture of `rel` ("PLAY/SONIC.png"): 8 bit indices of the current texture palette into mm->data (zone block, PU_HWRMODELTEXTURE_UNLOCKED, owner &mm->data),
// mm->width/height/format/flags set. Returns false when the pack has no such texture or there is no room.
boolean PS2Models_LoadTexture(const char *rel, GLMipmap_t *mm);

// An RGBA picture (a loose PNG/PCX of a user) as an 8 bit model texture: the same quantizer the cooker uses. dst: w * h bytes. *holes = some texel is a hole.
void PS2Models_Quantize(const UINT8 *rgba, int w, int h, UINT8 *dst, boolean *holes);

// One colour (r, g, b of a texel, 0..255) as the index of the texture palette: what the quantizer makes of it (used by the skin colour blend)
UINT8 PS2Models_QuantizeOne(int r, int g, int b);

// Every model made by PS2Models_Load goes back to the zone (the tables of md2_t that own them are about to move or change)
void PS2Models_FreeAll(void);

// A hash of the texture palette: textures made for another palette are made again (map palettes)
UINT32 PS2Models_PaletteHash(void);

// The cooked blend map "rel" ("PLAY/SONIC_blend.png"): sparse runs, loaded into a temporary block. *w *h = size. NULL if there is none or no room.
typedef struct
{
	UINT16 w, h;
	UINT32 nruns, npix;
	const UINT32 *run; // nruns pairs: pixel position, length (low 16 bits)
	const UINT8 *px; // npix quads: alpha, brightness, average of the colour channels, 0
	void *block; // free with PS2Models_FreeBlend
} ps2_blend_t;
boolean PS2Models_LoadBlend(const char *rel, ps2_blend_t *out);
void PS2Models_FreeBlend(ps2_blend_t *b);

// Does the pack hold a blend map for `rel` (without loading it)
boolean PS2Models_HasBlend(const char *rel);

// statistics: alive, KiB alive, loads, frees, reclaims, loads refused for memory, damaged, cycles of all loads, cycles of the longest load
void PS2Models_Stats(unsigned int *out);
void PS2Models_Report(void);

#endif // PS2_PROFILE
#endif // __PS2_MODELS__
