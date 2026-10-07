// OPT10 (HT, PS2-HW-70): cutting a polygon along t for a texture of more than 1024 rows, which the driver stores as two images (the rows
// [0, 1024) and the rest: the GS takes 1024 rows of texture at most). Pure code (vertices are SPLIT_VERT: x, y, z, s, t floats), also compiled by
// the host test tools/ps2/hw_split_hosttest.py.
//
// The polygon is cut in world space, where the texture mapping is affine, so the parts on the two sides of a cut get the very same vertices (no
// crack). Piece p holds the rows lo_t[p] .. hi_t[p] of one repeat of the texture (fractions of its height, lo_t[0] = 0, hi_t[1] = 1, hi_t[0] =
// lo_t[1]); a point of a part of piece p in repeat `cell` gets the texture coordinate t' = (t - (cell + lo_t[p])) * tk[p] (0 at the first row of the
// piece, 1 after its last row), tk[p] = 1 / (hi_t[p] - lo_t[p]). A texture that repeats (wrap) is cut at every repeat as well. Without wrap the
// texture is clamped at its ends: everything above the boundary belongs to piece 0 (clamped at its top row) and everything below it to piece 1.

#ifndef __PS2_HW_SPLIT_H__
#define __PS2_HW_SPLIT_H__

#include <math.h>

#define SPLIT_MAXREPEATS 32 // repeats of the texture in one polygon that are drawn (a texture of 1536 rows repeated more often is not in the game)

// the part of polygon in[0..n) with t >= bound (keep_ge) or t <= bound: at most n + 1 vertices of a convex polygon; the vertices made by the cut have t == bound
static int split_clip(const SPLIT_VERT *in, int n, SPLIT_VERT *out, float bound, int keep_ge)
{
	int i, no = 0;

	for (i = 0; i < n; i++)
	{
		const SPLIT_VERT *a = &in[i], *b = &in[i + 1 == n ? 0 : i + 1];
		const float da = keep_ge ? a->t - bound : bound - a->t, db = keep_ge ? b->t - bound : bound - b->t;

		if (da >= 0.0f)
			out[no++] = *a;
		if ((da > 0.0f && db < 0.0f) || (da < 0.0f && db > 0.0f))
		{
			const float k = da / (da - db); // the same number from both sides of the cut
			SPLIT_VERT *o = &out[no++];

			o->x = a->x + (b->x - a->x) * k;
			o->y = a->y + (b->y - a->y) * k;
			o->z = a->z + (b->z - a->z) * k;
			o->s = a->s + (b->s - a->s) * k;
			o->t = bound;
		}
	}
	return no;
}

typedef void (*split_emit_fn)(void *ctx, int piece, int cell, const SPLIT_VERT *v, int n);

// Calls emit(ctx, piece, cell, part, count) for every part (a convex polygon of at least 3 vertices, its t already the piece's own). buf0 / buf1: room
// for n + 2 vertices each. Returns the number of parts.
static int split_polygon(const SPLIT_VERT *v, int n, int wrap, const float lo_t[2], const float hi_t[2], const float tk[2], SPLIT_VERT *buf0, SPLIT_VERT *buf1, split_emit_fn emit, void *ctx)
{
	float tmin, tmax;
	int ka, kb, k, p, i, parts = 0;

	if (n < 3)
		return 0;
	tmin = tmax = v[0].t;
	for (i = 1; i < n; i++)
	{
		if (v[i].t < tmin)
			tmin = v[i].t;
		if (v[i].t > tmax)
			tmax = v[i].t;
	}
	ka = wrap ? (int)floorf(tmin) : 0;
	kb = wrap ? (int)floorf(tmax) : 0;
	if (kb - ka >= SPLIT_MAXREPEATS)
		kb = ka + SPLIT_MAXREPEATS - 1;
	for (k = ka; k <= kb; k++)
		for (p = 0; p < 2; p++)
		{
			const float lo = (float)k + lo_t[p], hi = (float)k + hi_t[p];
			const SPLIT_VERT *src = v;
			SPLIT_VERT *dst = buf0;
			int m = n;

			if (wrap || p) // the part at or above the start of the piece
			{
				m = split_clip(src, m, dst, lo, 1);
				src = dst;
				dst = buf1;
				if (m < 3)
					continue;
			}
			if (wrap || !p) // the part at or below its end
			{
				m = split_clip(src, m, dst, hi, 0);
				src = dst;
				if (m < 3)
					continue;
			}
			for (i = 0; i < m; i++)
				((SPLIT_VERT *)src)[i].t = (src[i].t - lo) * tk[p]; // src is buf0 or buf1 here, never v: every piece has a bound
			emit(ctx, p, k, src, m);
			parts++;
		}
	return parts;
}

#endif
