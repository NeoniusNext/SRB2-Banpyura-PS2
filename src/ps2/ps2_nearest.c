// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_nearest.c
/// \brief PS2-LOAD-14 (OPT12-LOAD): exact nearest palette colour with a luma-sorted search, see ps2_nearest.h

#include "ps2_nearest.h"

#define RGBMASK 0x00FFFFFFu // little endian: red | green << 8 | blue << 16 | alpha << 24

static inline UINT32 Word(const void *palette, int i)
{
	const UINT8 *p = (const UINT8 *)palette + i * 4;

	return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16);
}

void PS2Nearest_Build(ps2nearest_t *n, const void *palette)
{
	UINT16 count[768];
	int i, s;

	n->pal = palette;
	for (s = 0; s < 768; s++)
		count[s] = 0;
	for (i = 0; i < 256; i++)
	{
		const UINT32 w = Word(palette, i);

		n->copy[i] = w;
		count[(w & 255) + ((w >> 8) & 255) + ((w >> 16) & 255)]++;
	}
	{ // start[s] = number of entries with a smaller sum; count[] becomes the next free position of each sum
		UINT16 pos = 0;

		for (s = 0; s < 767; s++)
		{
			const UINT16 c = s < 766 ? count[s] : 0;

			n->start[s] = pos;
			count[s] = pos;
			pos = (UINT16)(pos + c);
		}
	}
	for (i = 0; i < 256; i++) // in index order: stable
	{
		const UINT32 w = n->copy[i];
		const int sum = (int)(w & 255) + (int)((w >> 8) & 255) + (int)((w >> 16) & 255);
		const UINT16 at = count[sum]++;

		n->sr[at] = (UINT8)w;
		n->sg[at] = (UINT8)(w >> 8);
		n->sb[at] = (UINT8)(w >> 16);
		n->sidx[at] = (UINT8)i;
	}
	n->valid = true;
}

boolean PS2Nearest_Same(const ps2nearest_t *n, const void *palette)
{
	int i;

	if (!n->valid)
		return false;
	for (i = 0; i < 256; i++)
		if (Word(palette, i) != n->copy[i])
			return false;
	return true;
}

UINT8 PS2Nearest_Find(const ps2nearest_t *n, UINT8 r, UINT8 g, UINT8 b)
{
	const int qs = (int)r + g + b;
	int up = n->start[qs], dn = up - 1;
	int best = 256 * 256 * 4, bestidx = 256; // (the original starts at this distortion too: every real one is smaller)

	for (;;)
	{
		const int du = up < 256 ? (int)n->sr[up] + n->sg[up] + n->sb[up] - qs : 100000;
		const int dd = dn >= 0 ? qs - ((int)n->sr[dn] + n->sg[dn] + n->sb[dn]) : 100000;
		const int d = du <= dd ? du : dd; // the next entry in order of distance in sum
		int at, dr, dg, db, dist;

		if (d >= 100000 || d * d > 3 * best)
			break; // nothing left, or everything left is further away in sum than the best is in distance
		at = du <= dd ? up++ : dn--;
		dr = (int)r - n->sr[at];
		dg = (int)g - n->sg[at];
		db = (int)b - n->sb[at];
		dist = dr * dr + dg * dg + db * db;
		if (dist < best || (dist == best && n->sidx[at] < bestidx))
		{
			best = dist;
			bestidx = n->sidx[at];
		}
	}
	return (UINT8)bestidx;
}
