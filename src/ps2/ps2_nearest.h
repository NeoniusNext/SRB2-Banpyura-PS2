// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_nearest.h
/// \brief PS2-LOAD-14 (OPT12-LOAD): the nearest palette colour (r_data.c NearestPaletteColor) without scanning all 256 entries, with the same answer.
///
/// NearestPaletteColor returns the lowest index among the palette entries with the smallest squared distance |c - p[i]|^2. The entries are sorted once by
/// s = r + g + b; for a query q, |c - p|^2 >= (s_q - s_p)^2 / 3 (Cauchy-Schwarz on the difference vector), so the walk away from s_q in both directions stops
/// as soon as (s_q - s_p)^2 > 3 * (best distance so far). A tie is decided by the lower index, which is what the first-hit scan of the original does.
/// tools/ps2/nearest_hosttest.c compares it with the original loop for all 2^24 colours on several palettes.

#ifndef __PS2_NEAREST__
#define __PS2_NEAREST__

#ifdef PS2_NEAREST_HOST // tools/ps2/nearest_hosttest.c: no engine headers
#include <stdint.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int boolean;
#define true 1
#define false 0
#else
#include "../doomtype.h"
#endif

typedef struct
{
	const void *pal; // the palette it was built from (256 entries of 4 bytes: red, green, blue, alpha)
	boolean valid;
	UINT32 copy[256]; // the colours it was built from (the answer depends on the colours, not on the pointer)
	UINT8 sr[256], sg[256], sb[256], sidx[256]; // palette sorted by red + green + blue (stable: equal sums in index order)
	UINT16 start[767]; // start[s] = first sorted position whose sum is >= s; start[766] = 256
	UINT32 mkey[4096]; // memo of the answers (the fade colormaps ask for 16 384 colours of which 6 183 differ): direct mapped on the colour, 0xFFFFFFFF = empty
	UINT8 mval[4096];
} ps2nearest_t;

void PS2Nearest_Build(ps2nearest_t *n, const void *palette);
UINT8 PS2Nearest_Find(ps2nearest_t *n, UINT8 r, UINT8 g, UINT8 b); // (fills the memo of n)
boolean PS2Nearest_Same(const ps2nearest_t *n, const void *palette); // has the palette still the colours the context was built from

#endif
