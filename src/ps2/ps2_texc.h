// SONIC ROBO BLAST 2 (PS2 port)
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_texc.h
/// \brief OPT13 IZ (PS2-602, R2): composite textures prebuilt by the cooker (TEXC.PAK, docs/PACK_FORMAT.md)

#ifndef __PS2_TEXC_H__
#define __PS2_TEXC_H__

#include "../doomtype.h"

#ifdef PS2_PROFILE

// The key of the definition of texture `texnum` (R_LoadTextures' texture_t): size, type, every patch (wad, lump, origin, flip, alpha, style) and the content identity of the pack each patch
// comes from (and of the translucency tables, for a translucent patch). 0 when the texture cannot have a prebuilt composite: not a composite, a single patch or a non-square flat (a square flat is a
// floor, the engine's own copy), a blend style other than copy and translucent (they find the nearest colour of the palette of the moment), a patch of something that is not a version 2 pack (an
// add-on, a file of a platform other than the cooked packs). The same function runs in the host engine of the cooker (-texcdump) and on the console.
UINT64 PS2TexC_Key(INT32 texnum);

// TEXC.PAK was found beside the program and is valid (opened at the first call). -notexc: never.
boolean PS2TexC_Present(void);

// The prebuilt pixels of texture `texnum` (bytes = width * height palette indices, exactly what HWR_GenerateTexture makes) into dest; false: the pack has none for this definition (or
// is damaged): the caller composes the texture from its patches. Takes the stored form from the level's prefetch when it is there, else reads the one lump from the pack.
boolean PS2TexC_Fetch(INT32 texnum, UINT8 *dest, size_t bytes);

// A level was loaded (hardware renderer on): reads the stored form of the textures it uses (sidedefs, the sky, the pictures of the animations they belong to) in one pass, sorted by position in
// the pack, into one long-lived block, so that no texture is made from the pack in the middle of a frame. Nothing when there is no room to spare.
void PS2TexC_PrefetchLevel(void);

// The list of textures was made again (HWR_LoadMapTextures): what is known of the old list goes.
void PS2TexC_Reset(void);

// HWPROF window line "HWTEXC ...": hits, misses, reads and cycles of the window (counters back to 0)
void PS2TexC_Prof(unsigned int frames);

// -texccheck: for every texture the pack has a composite of: the stored pixels against the original composition (HWR_GenerateTexture with the fast paths and the pack off). Prints the verdict.
// compose is the caller's function making the pixels of texture `texnum` from the patches into dest (bytes), false if it cannot.
void PS2TexC_Check(boolean (*compose)(INT32 texnum, UINT8 *dest, size_t bytes));

// The host engine of the cooker (-texcdump FILE after R_LoadTextures): composes every eligible texture and writes the dump tools/ps2/cook.py --texc packs; then the program ends.
void PS2TexC_AfterTextures(void);

#endif // PS2_PROFILE
#endif // __PS2_TEXC_H__
