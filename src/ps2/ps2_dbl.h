// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_dbl.h
/// \brief PS2-LOAD-22 (OPT12-LOAD): exact IEEE binary64 subtraction, comparison and M_RoundUp on the bit patterns, in 64-bit integer arithmetic.
///
/// The EE has no double precision hardware: every + - * / < > of a double is a libgcc call (unpack, align, add, round, pack: 150-250 cycles). R_GenerateLightTable
/// (r_data.c) does ~25 of them for each of the 26 112 channel steps of a colormap (40 M cycles per table; one at the start-up and one per level that has a colormap).
/// Its recurrence only needs "a - b", "|a| > |b|" and the M_RoundUp of the result; these are done here on the bits, with the same rounding (to nearest, ties to
/// even) as the library, and give the same bits for every pair of finite normal numbers and zeros. A result or an operand that is not such a number (infinity, NaN,
/// denormal, overflow, underflow) sets *bail: the caller then runs the original double code instead. tools/ps2/dbl_hosttest.c compares them with the hardware double
/// of the host (SSE2: the same IEEE rounding) over hundreds of millions of random and edge-case operands.

#ifndef __PS2_DBL__
#define __PS2_DBL__

#include <stdint.h>
#include <string.h>

#define DBL_SIGN 0x8000000000000000ull
#define DBL_FRAC 0x000FFFFFFFFFFFFFull
#define DBL_HIDDEN 0x0010000000000000ull

typedef uint64_t dblbits_t;

static inline dblbits_t Dbl_Bits(double d)
{
	dblbits_t b;

	memcpy(&b, &d, sizeof b);
	return b;
}

// zero (either sign) or a normal finite number
static inline int Dbl_Ok(dblbits_t a)
{
	const uint32_t e = (uint32_t)(a >> 52) & 0x7FF;

	return e != 0x7FF && (e != 0 || (a & DBL_FRAC) == 0);
}

// a + b, rounded to nearest even; *bail is set when an operand or the result is not zero or a normal finite number
static inline dblbits_t Dbl_Add(dblbits_t a, dblbits_t b, int *bail)
{
	dblbits_t ma = a & ~DBL_SIGN, mb = b & ~DBL_SIGN, f, fb, t;
	int ea, eb, d, e;
	uint32_t rb;

	if (!Dbl_Ok(a) || !Dbl_Ok(b))
	{
		*bail = 1;
		return 0;
	}
	if (ma < mb) // |a| >= |b|
	{
		t = a; a = b; b = t;
		t = ma; ma = mb; mb = t;
	}
	ea = (int)(ma >> 52);
	eb = (int)(mb >> 52);
	if (eb == 0) // b is zero
	{
		if (ea == 0) // both zero: -0 only for -0 + -0
			return a & b & DBL_SIGN;
		return a;
	}
	f = ((ma & DBL_FRAC) | DBL_HIDDEN) << 3; // three more bits: guard, round, sticky
	fb = ((mb & DBL_FRAC) | DBL_HIDDEN) << 3;
	d = ea - eb;
	if (d)
	{
		if (d > 60)
			fb = 1; // (only the sticky bit is left)
		else
			fb = (fb >> d) | ((fb & ((1ull << d) - 1)) != 0);
	}
	e = ea;
	if ((a ^ b) & DBL_SIGN) // different signs: subtract the smaller magnitude
	{
		f -= fb;
		if (f == 0)
			return 0; // x - x = +0
		while (f < (1ull << 55)) // cancellation: bring the leading one back to bit 55
		{
			if (f < (1ull << 47))
			{
				f <<= 8;
				e -= 8;
			}
			else
			{
				f <<= 1;
				e--;
			}
		}
	}
	else
	{
		f += fb;
		if (f & (1ull << 56))
		{
			f = (f >> 1) | (f & 1);
			e++;
		}
	}
	rb = (uint32_t)(f & 7);
	f >>= 3;
	if (rb > 4 || (rb == 4 && (f & 1)))
	{
		f++;
		if (f & (1ull << 53))
		{
			f >>= 1;
			e++;
		}
	}
	if (e <= 0 || e >= 0x7FF)
	{
		*bail = 1;
		return 0;
	}
	return (a & DBL_SIGN) | ((dblbits_t)e << 52) | (f & DBL_FRAC);
}

static inline dblbits_t Dbl_Sub(dblbits_t a, dblbits_t b, int *bail)
{
	return Dbl_Add(a, b ^ DBL_SIGN, bail);
}

// the magnitude: what `(x) < 0 ? -(x) : (x)` is for every finite x (for -0.0 it is -0.0 itself, which compares equal to +0.0)
static inline dblbits_t Dbl_Abs(dblbits_t a)
{
	return a & ~DBL_SIGN;
}

// |a| > |b| for zeros and finite numbers (the integer order of the magnitudes is the order of the values)
static inline int Dbl_AbsGreater(dblbits_t a, dblbits_t b)
{
	return (a & ~DBL_SIGN) > (b & ~DBL_SIGN);
}

#define DBL_255 0x406FE00000000000ull // 255.0

// M_RoundUp (m_misc.c) of the number with these bits (zero or a normal finite number):
//   > 255 -> 255, < 0 -> 0, otherwise (int)n <= (int)(n - 0.5) ? (int)n + 1 : (int)n  (so 0 <= n < 1 gives 1, and from 1 up it rounds half up)
static inline int Dbl_RoundUp(dblbits_t a)
{
	uint32_t e;
	int shift;
	dblbits_t m, one;

	if (a & DBL_SIGN)
		return (a & ~DBL_SIGN) ? 0 : 1; // -0.0 is not < 0
	if (a > DBL_255)
		return 255;
	e = (uint32_t)(a >> 52);
	if (e < 0x3FF)
		return 1; // 0 <= n < 1
	shift = 52 - (int)(e - 0x3FF); // fraction bits
	m = (a & DBL_FRAC) | DBL_HIDDEN;
	one = 1ull << (shift - 1);
	return (int)(m >> shift) + ((m & ((1ull << shift) - 1)) >= one ? 1 : 0);
}


// The colour steps of R_GenerateLightTable (r_data.c): for each of the 34 light levels and 256 palette entries the nearest palette colour of the three
// channel values, which then move one step to the fade colour:
//   out[p * 256 + i] = nearest(RoundUp(m[i][0]), RoundUp(m[i][1]), RoundUp(m[i][2]));
//   for p >= fadestart: m[i][c] = |m[i][c] - dest[c]| > |delta[i][c']| ? m[i][c] - delta[i][c] : dest[c]      (c' is 1 for c = 2: the original compares the blue
//   channel with the green step, a slip of the original that is kept)
// map0, delta, dest are the doubles of the original as bit patterns. Returns 0 (out is then not valid) when a value is not a zero or a normal finite number
// (fadeend == fadestart makes the deltas infinite, for one).
// A level's colour for a palette entry that is the same as that of the level before is not asked for again (the answer only depends on the three numbers).
typedef uint8_t (*dbl_nearest_t)(void *ctx, uint8_t r, uint8_t g, uint8_t b);

static inline int Dbl_LightSteps(const dblbits_t map0[256][3], const dblbits_t delta[256][3], const dblbits_t dest[3], unsigned fadestart,
	uint8_t *out, dbl_nearest_t nearest, void *ctx)
{
	dblbits_t m[256][3];
	uint8_t prev[256][4];
	int bail = 0, p, i, c;

	for (c = 0; c < 3; c++)
		if (!Dbl_Ok(dest[c]))
			return 0;
	for (i = 0; i < 256; i++)
		for (c = 0; c < 3; c++)
			if (!Dbl_Ok(map0[i][c]) || !Dbl_Ok(delta[i][c]))
				return 0;
	memcpy(m, map0, sizeof m);
	for (p = 0; p < 34; p++)
	{
		for (i = 0; i < 256; i++)
		{
			const uint8_t r = (uint8_t)Dbl_RoundUp(m[i][0]), g = (uint8_t)Dbl_RoundUp(m[i][1]), b = (uint8_t)Dbl_RoundUp(m[i][2]);

			if (p && prev[i][0] == r && prev[i][1] == g && prev[i][2] == b)
				*out++ = prev[i][3];
			else
			{
				const uint8_t n = nearest(ctx, r, g, b);

				prev[i][0] = r;
				prev[i][1] = g;
				prev[i][2] = b;
				prev[i][3] = n;
				*out++ = n;
			}
			if ((unsigned)p < fadestart)
				continue;
			if (Dbl_AbsGreater(Dbl_Sub(m[i][0], dest[0], &bail), delta[i][0]))
				m[i][0] = Dbl_Sub(m[i][0], delta[i][0], &bail);
			else
				m[i][0] = dest[0];
			if (Dbl_AbsGreater(Dbl_Sub(m[i][1], dest[1], &bail), delta[i][1]))
				m[i][1] = Dbl_Sub(m[i][1], delta[i][1], &bail);
			else
				m[i][1] = dest[1];
			if (Dbl_AbsGreater(Dbl_Sub(m[i][2], dest[2], &bail), delta[i][1]))
				m[i][2] = Dbl_Sub(m[i][2], delta[i][2], &bail);
			else
				m[i][2] = dest[2];
		}
		if (bail)
			return 0;
	}
	return 1;
}

#endif // __PS2_DBL__
