// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_rdraw.h
/// \brief Bit-identical helpers of the software renderer for the R5900 (PS2-30): same results as the original
///        expressions for every input, without libgcc 64-bit calls. Tested by tools/ps2/rend_hosttest.py.

#ifndef __PS2_RDRAW__
#define __PS2_RDRAW__

#include <string.h>
#include "../doomtype.h"
#include "../m_fixed.h"
#include "../tables.h"

// SlopeDivEx ((UINT64)num<<3) / (den>>8) capped to SLOPERANGE, without the 64-bit division (libgcc __udivdi3,
// hundreds of cycles on the R5900). 8n = 8(q*d + r), so 8n/d = 8q + 8r/d exactly.
static inline UINT32 PS2_SlopeDivEx(UINT32 num, UINT32 den)
{
	UINT32 d, q, ans;

	if (den < 512)
		return SLOPERANGE;
	d = den >> 8;
	if (num < (1u << 29))
		ans = (num << 3) / d;
	else
	{
		q = num / d;
		if (q >= 512) // 8q alone exceeds SLOPERANGE
			return SLOPERANGE;
		ans = (q << 3) + (((num - q * d) << 3) / d);
	}
	return ans <= SLOPERANGE ? ans : SLOPERANGE;
}

// Fills n halfwords with v. The R5900 has no byte-store-free memset in newlib (about 1.5 cycles per byte); this writes
// 64 bits at a time after aligning, and is plain stores everywhere else (host tests, any alignment).
static inline void PS2_Fill16(UINT16 *p, UINT16 v, size_t n)
{
	UINT64 w = v;
	w |= w << 16;
	w |= w << 32;
	while (n && ((size_t)p & 7))
	{
		*p++ = v;
		n--;
	}
#if defined(_EE) && defined(__GNUC__)
	// PS2-172: one more 64-bit store aligns the pointer to 16 bytes, the bulk then goes out with 128-bit stores (32 bytes per pass: 2 sq
	// instead of 4 sd; the visplane clip reset writes 2 x 640 bytes for every new visplane)
	if (n >= 20)
	{
		if ((size_t)p & 8)
		{
			memcpy(p, &w, 8);
			p += 4;
			n -= 4;
		}
		{
			size_t blocks = n >> 4;
			UINT64 pat;

			n &= 15;
			__asm__ volatile(
				"pcpyld %[pat],%[w],%[w]\n\t"
				"1:\n\t"
				"sq %[pat],0(%[p])\n\t"
				"sq %[pat],16(%[p])\n\t"
				"addiu %[b],%[b],-1\n\t"
				"addiu %[p],%[p],32\n\t"
				"bnez %[b],1b\n\t"
				: [p] "+r"(p), [b] "+r"(blocks), [pat] "=&r"(pat)
				: [w] "r"(w)
				: "memory");
		}
	}
#endif
	while (n >= 16)
	{
		memcpy(p, &w, 8);
		memcpy(p + 4, &w, 8);
		memcpy(p + 8, &w, 8);
		memcpy(p + 12, &w, 8);
		p += 16;
		n -= 16;
	}
	while (n >= 4)
	{
		memcpy(p, &w, 8);
		p += 4;
		n -= 4;
	}
	while (n)
	{
		*p++ = v;
		n--;
	}
}

// 32x32 -> 64 signed product with the R5900 mult instruction; (INT64)a * b is a libgcc call there (no 64-bit multiplier).
static inline INT64 PS2_MulS32(INT32 a, INT32 b)
{
#if defined(_EE) && defined(__GNUC__)
	UINT32 lo, hi;
	__asm__("mult %0,%2,%3\n\tmfhi %1" :"=&r"(lo), "=r"(hi) : "r"(a), "r"(b) : "hi", "lo");
	return (INT64)(((UINT64)hi << 32) | lo);
#else
	return (INT64)a * b;
#endif
}

// FixedDiv(a, b) when the divisor is exactly one unit: the result is a unless |a| >= 2^30 (saturation)
static inline fixed_t PS2_FixedDivByUnit(fixed_t a, fixed_t b)
{
	if (b == FRACUNIT && a > -0x40000000 && a < 0x40000000)
		return a;
	return FixedDiv(a, b);
}

// PS2-160: R_IsPointInSector's edge test. For v1y < y <= v2y the original computes
//   (fixed_t)(v1x + (INT64)(v2x - v1x) * (y - v1y) / (v2y - v1y)) < x
// with a 64-bit division (libgcc __divdi3: the R5900 has no 64-bit divider). With a = v2x-v1x, b = y-v1y, d = v2y-v1y (int32 values,
// 0 < b <= d) and t = x - v1x the quotient q = trunc(a*b/d) satisfies q < t  <=>  a*b < t*d  (a >= 0)  and  a*b <= (t-1)*d  (a < 0):
// two exact 64-bit products (mult) and one compare. Guards: no wrap in a, t fits 32 bits (|t*d| < 2^63); otherwise the original expression.
static inline boolean PS2_EdgeXLess(fixed_t v1x, fixed_t v1y, fixed_t v2x, fixed_t v2y, fixed_t x, fixed_t y)
{
	const fixed_t a = (fixed_t)((UINT32)v2x - (UINT32)v1x);
	const fixed_t b = (fixed_t)((UINT32)y - (UINT32)v1y);
	const fixed_t d = (fixed_t)((UINT32)v2y - (UINT32)v1y);
	if (d > 0 && b > 0 && b <= d && (INT64)v2x - v1x == a)
	{
		const INT64 t = (INT64)x - v1x;
		if (t == (fixed_t)t)
		{
			const INT64 ab = PS2_MulS32(a, b);
			const INT64 td = PS2_MulS32((fixed_t)t, d);
			return a >= 0 ? ab < td : ab <= td - d;
		}
	}
	return (fixed_t)((INT64)v1x + (INT64)a * b / d) < x;
}

#endif
