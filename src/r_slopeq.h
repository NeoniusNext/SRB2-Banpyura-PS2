// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_slopeq.h
/// \brief 64-bit fixed-point helpers that stand in for the double-precision math of the slope renderer
///
/// The EE has no double FPU (libgcc soft-float, ~70-300 cycles per operation, libm sin/cos several
/// thousand) and its single-precision FPU truncates. The per-plane slope setup needs ~1e-12 relative
/// accuracy because it subtracts world coordinates of 1e4..1e5, so it is done in 64-bit integers:
/// values are "Q40" (value * 2^40) or "Q62" (unit-less, value * 2^62); products go through a 128-bit
/// intermediate. Doubles only appear as stored data (slope vectors from p_slopes.c): they are decoded
/// from their IEEE bit patterns. Light-scale and span outputs use float.
/// PS2-16 candidate: numerical tolerance tests pass, strict equivalence fails. Explicit opt-in only;
/// see tools/ps2/math_slope_hosttest.py and math_draw_hosttest.py.

#ifndef __R_SLOPEQ__
#define __R_SLOPEQ__

#if defined(PS2_OPT_SLOPE) || defined(PS2_OPT_SEGS)

#include <string.h>
#include "m_fixed.h"
#include "m_vector.h"

#define RQ_L 40 // fractional bits of lengths
#define RQ_T 62 // fractional bits of unit-less trigonometric values

#ifdef _MSC_VER
#define RQ_INLINE static __forceinline
#else
#define RQ_INLINE static inline __attribute__((always_inline))
#endif

#define RQ_MAX ((INT64)0x7FFFFFFFFFFFFFFFll)

// number of leading zero bits of a non-zero 64-bit value
RQ_INLINE UINT32 RQ_Clz64(UINT64 x)
{
	const UINT32 hi = (UINT32)(x >> 32);
	return hi ? PS2_Clz(hi) : 32u + PS2_Clz((UINT32)x);
}

// 128-bit product of two non-negative 64-bit values
RQ_INLINE void RQ_Mul128(UINT64 a, UINT64 b, UINT64 *hi, UINT64 *lo)
{
	const UINT64 a0 = (UINT32)a, a1 = a >> 32, b0 = (UINT32)b, b1 = b >> 32;
	const UINT64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	const UINT64 mid = (p00 >> 32) + (UINT32)p01 + (UINT32)p10;
	*lo = (UINT32)p00 | (mid << 32);
	*hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

// (a * b) >> sh (1 <= sh <= 63), exact 128-bit product, truncated toward zero, saturated to INT64
RQ_INLINE INT64 RQ_MulShift(INT64 a, INT64 b, int sh)
{
	const UINT64 ua = a < 0 ? 0 - (UINT64)a : (UINT64)a;
	const UINT64 ub = b < 0 ? 0 - (UINT64)b : (UINT64)b;
	UINT64 hi, lo, r;
	RQ_Mul128(ua, ub, &hi, &lo);
	if (hi >> sh)
		r = (UINT64)RQ_MAX; // result does not fit
	else
	{
		r = (hi << (64 - sh)) | (lo >> sh);
		if (r > (UINT64)RQ_MAX)
			r = (UINT64)RQ_MAX;
	}
	return ((a < 0) != (b < 0)) ? -(INT64)r : (INT64)r;
}

// value of a float, scaled by 2^frac, truncated toward zero (no FPU)
RQ_INLINE INT64 RQ_FromFloat(float f, int frac)
{
	UINT32 bits;
	INT32 sh;
	UINT64 m;
	memcpy(&bits, &f, sizeof bits);
	if (((bits >> 23) & 0xFF) == 0)
		return 0;
	sh = (INT32)((bits >> 23) & 0xFF) - 127 - 23 + frac; // |f| * 2^frac = mant * 2^sh
	m = (bits & 0x7FFFFFu) | 0x800000u;
	if (sh >= 0)
		m = (sh > 39) ? (UINT64)RQ_MAX : (m << sh); // m < 2^24: 2^63 at most
	else
		m = (sh < -24) ? 0 : (m >> -sh);
	return (bits & 0x80000000u) ? -(INT64)m : (INT64)m;
}

// value of a double stored in memory, scaled by 2^frac, truncated toward zero (no FPU, no libgcc)
RQ_INLINE INT64 RQ_FromDouble(const double *d, int frac)
{
	UINT64 bits, m;
	INT32 sh;
	memcpy(&bits, d, sizeof bits);
	if (((bits >> 52) & 0x7FF) == 0)
		return 0;
	sh = (INT32)((bits >> 52) & 0x7FF) - 1023 - 52 + frac;
	m = (bits & 0xFFFFFFFFFFFFFull) | (1ull << 52);
	if (sh >= 0)
		m = (sh > 10) ? (UINT64)RQ_MAX : (m << sh);
	else
		m = (sh < -53) ? 0 : (m >> -sh);
	if (m > (UINT64)RQ_MAX)
		m = (UINT64)RQ_MAX;
	return (bits >> 63) ? -(INT64)m : (INT64)m;
}

// Rounds the unsigned magnitude to 24 significant bits (nearest, ties to even): *m is left in
// [2^23, 2^24] or 0 and the returned shift s gives value = *m * 2^s, like an IEEE single.
RQ_INLINE INT32 RQ_Round24(UINT64 *m)
{
	INT32 nb, sh;
	if (*m == 0)
		return 0;
	nb = 64 - (INT32)RQ_Clz64(*m);
	if (nb <= 24)
	{
		*m <<= (24 - nb);
		return nb - 24;
	}
	sh = nb - 24;
	{
		const UINT64 rem = *m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
		*m >>= sh;
		if (rem > half || (rem == half && (*m & 1)))
			(*m)++;
	}
	if (*m == (1ull << 24))
	{
		*m >>= 1;
		sh++;
	}
	return sh;
}

// (float)v for an integer, as a value (nearest even); the original `(float)INT64` conversion
RQ_INLINE INT64 RQ_RoundToFloat24(INT64 v)
{
	UINT64 m = v < 0 ? 0 - (UINT64)v : (UINT64)v;
	const INT32 s = RQ_Round24(&m);
	m = (s >= 0) ? (m << s) : (m >> -s);
	return v < 0 ? -(INT64)m : (INT64)m;
}

// q * 2^exp2 as a single-precision float, nearest even, flush to zero below the normal range
RQ_INLINE float RQ_ToFloat(INT64 q, int exp2)
{
	UINT64 m = q < 0 ? 0 - (UINT64)q : (UINT64)q;
	UINT32 bits;
	float f;
	INT32 e;
	if (q == 0)
		return 0.0f;
	e = RQ_Round24(&m) + exp2 + 23 + 127;
	if (e <= 0)
		return 0.0f;
	if (e >= 255)
		bits = 0x7F7FFFFFu;
	else
		bits = ((UINT32)e << 23) | ((UINT32)m & 0x7FFFFFu);
	if (q < 0)
		bits |= 0x80000000u;
	memcpy(&f, &bits, sizeof f);
	return f;
}

// ANG2RAD(a) = (float)(a * M_PI) / ANGLE_180 evaluated exactly as the C expression does (double product
// rounded to 53 bits, then to float, then an exact division by 2^31), but without the FPU.
RQ_INLINE float RQ_Ang2Rad(UINT32 a)
{
	const UINT64 pi53 = 0x1921FB54442D18ull; // M_PI = pi53 * 2^-51
	UINT64 hi, lo, m;
	INT32 nb, sh;
	if (a == 0)
		return 0.0f;
	RQ_Mul128(a, pi53, &hi, &lo); // < 2^85
	nb = hi ? 128 - (INT32)RQ_Clz64(hi) : 64 - (INT32)RQ_Clz64(lo);
	sh = 0;
	if (nb > 53) // double rounding, step 1: 53 significant bits
	{
		UINT64 rem, half;
		sh = nb - 53; // 1..32
		rem = lo & ((1ull << sh) - 1);
		half = 1ull << (sh - 1);
		m = (hi << (64 - sh)) | (lo >> sh); // hi < 2^21, so no bits are lost
		if (rem > half || (rem == half && (m & 1)))
			m++;
	}
	else
		m = lo;
	// m * 2^sh * 2^-51 / 2^31; step 2: 24 significant bits
	return RQ_ToFloat((INT64)m, sh - 51 - 31);
}

// sin/cos of a (non-negative) float angle in radians, as Q62, accurate to ~1e-16
// (argument reduction in Q60, then Taylor polynomials on [-pi/4, pi/4]).
static inline void RQ_SinCos(float ang, INT64 *sinq, INT64 *cosq)
{
	static const INT64 pio2 = 0x1921FB54442D1847ll; // pi/2 * 2^60
	static const INT64 pio4 = 0xC90FDAA22168C23ll;
	// 1/(2k+1)! and 1/(2k)! in Q62 (alternating signs applied in the Horner steps)
	static const INT64 sc[9] = { 0x4000000000000000ll, 0x0AAAAAAAAAAAAAABll, 0x0088888888888889ll, 0x0003403403403403ll, 0x00000B8EF1D2AB64ll, 0x0000001AE64567F5ll, 0x000000002C248C27ll, 0x000000000035CFE8ll, 0x00000000000032A6ll };
	static const INT64 cc[9] = { 0x4000000000000000ll, 0x2000000000000000ll, 0x02AAAAAAAAAAAAABll, 0x0016C16C16C16C17ll, 0x0000680680680680ll, 0x00000127E4FB778All, 0x000000023DDB1DFFll, 0x0000000003272E95ll, 0x0000000000035CFEll };
	INT64 x = RQ_FromFloat(ang, 60), r, r2, ps, pc, s, c;
	INT32 n;
	if (x < 0)
	{ // not produced by the renderer; keep the result right anyway
		RQ_SinCos(-ang, &s, &c);
		*sinq = -s;
		*cosq = c;
		return;
	}
	// quadrant by subtraction: x < 8 (Q60 saturates), at most 5 steps
	r = x;
	n = 0;
	while (r >= pio2)
	{
		r -= pio2;
		n++;
	}
	if (r > pio4)
	{
		r -= pio2;
		n++;
	}
	r2 = RQ_MulShift(r, r, 60); // Q60
	// sin(r) = r * sum (-1)^k r^2k / (2k+1)!   Horner from the top term, Q62 accumulators
	ps = sc[8];
	pc = cc[8];
	for (int k = 7; k >= 0; k--)
	{
		ps = sc[k] - RQ_MulShift(ps, r2, 60);
		pc = cc[k] - RQ_MulShift(pc, r2, 60);
	}
	s = RQ_MulShift(ps, r, 60); // Q62
	c = pc;
	switch (n & 3)
	{
		case 0: *sinq = s; *cosq = c; break;
		case 1: *sinq = c; *cosq = -s; break;
		case 2: *sinq = -s; *cosq = -c; break;
		default: *sinq = -c; *cosq = s; break;
	}
}


// ---------------------------------------------------------------------------------------------------
// Slope plane setup (R_SetSlopePlane / R_SetScaledSlopePlane of r_plane.c), all in integers.
// Lengths are Q40 map units (= fixed_t << 24); |coordinates| must stay below 2^22 units (the code saturates
// beyond that, unlike the double original that keeps working at 1e9 - nothing in the binary maps gets there).
// ---------------------------------------------------------------------------------------------------

typedef struct { INT64 ox, oy, oz; INT64 nx, ny; INT64 dz; } rq_slope_t; // origin Q40, normal direction Q62, zdelta Q48
typedef struct { INT64 x, y, z; } rq_vec_t; // Q40

// Q40 of an INT64 fixed value, clamped to the supported range
RQ_INLINE INT64 RQ_FixedToQ(INT64 f)
{
	const INT64 lim = (INT64)1 << 38;
	if (f > lim)
		f = lim;
	else if (f < -lim)
		f = -lim;
	return f * ((INT64)1 << 24);
}

// truncation toward zero of a Q40 value to fixed units
RQ_INLINE INT64 RQ_QToFixed(INT64 q)
{
	return q < 0 ? -((-q) >> 24) : (q >> 24);
}

// a*cos(ang) + b*sin(ang) (or a*sin + b*cos when swap) for float operands, Q40; the double expression of the plane
// offset rotation in R_FindPlane, without libm
RQ_INLINE INT64 RQ_Rotate(float a, float b, float ang, boolean swap)
{
	INT64 sn, cs;
	RQ_SinCos(ang, &sn, &cs);
	if (swap)
	{
		const INT64 t = sn;
		sn = cs;
		cs = t;
	}
	return RQ_MulShift(RQ_FromFloat(a, 40), cs, 62) + RQ_MulShift(RQ_FromFloat(b, 40), sn, 62);
}

RQ_INLINE void RQ_LoadSlope(rq_slope_t *q, const dvector3_t *origin, const dvector3_t *normdir, const double *zdelta)
{
	q->ox = RQ_FromDouble(&origin->x, 40);
	q->oy = RQ_FromDouble(&origin->y, 40);
	q->oz = RQ_FromDouble(&origin->z, 40);
	q->nx = RQ_FromDouble(&normdir->x, 62);
	q->ny = RQ_FromDouble(&normdir->y, 62);
	q->dz = RQ_FromDouble(zdelta, 48);
}

// R_GetSlopeZAt: height of the plane at the fixed position (x, y), Q40
RQ_INLINE INT64 RQ_SlopeZAt(const rq_slope_t *s, INT64 x, INT64 y)
{
	const INT64 px = RQ_FixedToQ(x) - s->ox;
	const INT64 py = RQ_FixedToQ(y) - s->oy;
	const INT64 dist = RQ_MulShift(px, s->nx, 62) + RQ_MulShift(py, s->ny, 62);
	return s->oz + RQ_MulShift(dist, s->dz, 48);
}

// Q62 trigonometric value to Q40
RQ_INLINE INT64 RQ_T2L(INT64 t)
{
	return t < 0 ? -((-t) >> 22) : (t >> 22);
}

// sign and 128-bit magnitude of a1*b1 + a2*b2 (all INT64, |a*b| < 2^126)
RQ_INLINE void RQ_MAdd128(INT64 a1, INT64 b1, INT64 a2, INT64 b2, int *neg, UINT64 *rhi, UINT64 *rlo)
{
	UINT64 h1, l1, h2, l2;
	const int neg1 = ((a1 < 0) != (b1 < 0));
	const int neg2 = ((a2 < 0) != (b2 < 0));
	RQ_Mul128(a1 < 0 ? 0 - (UINT64)a1 : (UINT64)a1, b1 < 0 ? 0 - (UINT64)b1 : (UINT64)b1, &h1, &l1);
	RQ_Mul128(a2 < 0 ? 0 - (UINT64)a2 : (UINT64)a2, b2 < 0 ? 0 - (UINT64)b2 : (UINT64)b2, &h2, &l2);
	if (neg1 == neg2)
	{
		*rlo = l1 + l2;
		*rhi = h1 + h2 + (*rlo < l1);
		*neg = neg1;
	}
	else if (h1 > h2 || (h1 == h2 && l1 >= l2))
	{
		*rlo = l1 - l2;
		*rhi = h1 - h2 - (l1 < l2);
		*neg = neg1;
	}
	else
	{
		*rlo = l2 - l1;
		*rhi = h2 - h1 - (l2 < l1);
		*neg = neg2;
	}
}

// (a1*b1 - a2*b2) * 2^exp2 * mul as a float, one rounding (nearest even). The cross products of the slope vectors
// cancel (world coordinates ~1e4 times unit vectors), so the 128-bit difference of the exact products is formed first.
RQ_INLINE float RQ_CrossToFloat(INT64 a1, INT64 b1, INT64 a2, INT64 b2, int exp2, float mul)
{
	UINT64 hi, lo, m;
	int neg, nb;
	INT32 e, s;
	UINT32 mulbits;
	RQ_MAdd128(a1, b1, a2, -b2, &neg, &hi, &lo);
	if (hi == 0 && lo == 0)
		return 0.0f;
	// value = m * 2^e with m normalised to 64 bits (the dropped low bits are far below the final rounding)
	if (hi)
	{
		nb = 128 - (int)RQ_Clz64(hi);
		s = nb - 64; // 1..64
		m = (s == 64) ? hi : ((hi << (64 - s)) | (lo >> s));
		e = s;
	}
	else
	{
		s = (INT32)RQ_Clz64(lo);
		m = lo << s;
		e = -s;
	}
	e += exp2;
	memcpy(&mulbits, &mul, sizeof mulbits);
	if (mulbits != 0x3F800000u) // 1.0f
	{
		UINT64 mh, ml;
		if (((mulbits >> 23) & 0xFF) == 0)
			return 0.0f;
		if (mulbits & 0x80000000u)
			neg = !neg;
		RQ_Mul128(m, (mulbits & 0x7FFFFFu) | 0x800000u, &mh, &ml); // 2^86 <= product < 2^88
		s = 128 - (int)RQ_Clz64(mh) - 64; // bit length of mh
		m = (mh << (64 - s)) | (ml >> s);
		e += s + (INT32)((mulbits >> 23) & 0xFF) - 127 - 23;
	}
	{ // 24-bit mantissa, nearest even
		const UINT64 rem = m & 0xFFFFFFFFFFull, half = 1ull << 39; // low 40 bits
		UINT32 bits;
		float f;
		m >>= 40;
		e += 40;
		if (rem > half || (rem == half && (m & 1)))
			m++;
		if (m == (1ull << 24))
		{
			m >>= 1;
			e++;
		}
		e += 23 + 127;
		if (e <= 0)
			return 0.0f;
		bits = e >= 255 ? 0x7F7FFFFFu : (((UINT32)e << 23) | ((UINT32)m & 0x7FFFFFu));
		if (neg)
			bits |= 0x80000000u;
		memcpy(&f, &bits, sizeof f);
		return f;
	}
}

// ds_lightscale: BASEVIDWIDTH^2 / vid.width / zeroheight / 21 * fovtan (PLANELIGHTFLOAT of r_draw.c), zeroheight in Q40
RQ_INLINE float RQ_LightScale(INT32 num, INT64 zeroheightq, fixed_t fovtan)
{
	return (float)num / RQ_ToFloat(zeroheightq, -40) / 21.0f * FixedToFloat(fovtan);
}

// floor((hi * 2^64 + lo) / d); saturates to UINT64_MAX when the quotient does not fit 64 bits
RQ_INLINE UINT64 RQ_Div128(UINT64 hi, UINT64 lo, UINT64 d)
{
	UINT64 q = 0, rem;
	int i;
	if (hi >= d)
		return ~(UINT64)0;
	if (hi == 0)
		return lo / d;
	rem = hi;
	i = 63;
	{
		// PS2-172: while the partial remainder (hi << j | the top j bits of lo) stays below d the loop below shifts and subtracts nothing
		// (quotient bit 0). With k = the largest shift for which (hi + 1) << k <= d the first k remainders are below d (each is at most
		// ((hi + 1) << j) - 1), so the loop starts k bits in: rem = hi << k | lo >> (64 - k), next quotient bit 63 - k. The slope-wall
		// ray hits divide a ~82-bit numerator by a ~50-bit denominator: about half of the 64 iterations are skipped.
		int k = (int)RQ_Clz64(hi + 1) - (int)RQ_Clz64(d);
		if (((hi + 1) << k) > d)
			k--;
#if defined(PS2_NEGCTL) && PS2_NEGCTL == 12 // negative control of tools/ps2/sw_hosttest.py: one iteration too many skipped
		k++;
#endif
		if (k > 0)
		{
			rem = (hi << k) | (lo >> (64 - k));
			i = 63 - k;
		}
	}
	for (; i >= 0; i--)
	{
		const UINT64 top = rem >> 63;
		rem = (rem << 1) | ((lo >> i) & 1);
		if (top || rem >= d)
		{
			rem -= d;
			q |= (UINT64)1 << i;
		}
	}
	return q;
}

// trunc((a1*b1 + a2*b2) / d) for d != 0, exact (saturated to +-INT64_MAX)
RQ_INLINE INT64 RQ_MulAddDiv(INT64 a1, INT64 b1, INT64 a2, INT64 b2, INT64 d)
{
	UINT64 hi, lo, q;
	int neg;
	RQ_MAdd128(a1, b1, a2, b2, &neg, &hi, &lo);
	if (d < 0)
	{
		d = -d;
		neg = !neg;
	}
	q = RQ_Div128(hi, lo, (UINT64)d);
	if (q > (UINT64)RQ_MAX)
		q = (UINT64)RQ_MAX;
	return neg ? -(INT64)q : (INT64)q;
}

// R_StoreWallRange: where the ray from the view point (vx, vy) at the fine angle with sine S and cosine C (fixed)
// crosses the line of the seg (x1,y1)-(x2,y2). The original solves the two line equations in double and truncates with
// DoubleToFixed; the rational formula is computed exactly, but double rounding can give a different last bit:
// P = v1 + t*(v2 - v1), t = (C*(vy-y1) - S*(vx-x1)) / (C*dy - S*dx).
// Parallel lines and results beyond fixed_t give INT32_MIN, as the x86 conversion of inf/NaN/out of range does.
RQ_INLINE void RQ_RayHitsSeg(INT32 x1, INT32 y1, INT32 x2, INT32 y2, INT32 vx, INT32 vy, INT32 S, INT32 C, INT32 *ox, INT32 *oy)
{
	const INT64 dx = (INT64)x2 - x1, dy = (INT64)y2 - y1;
	const INT64 den = (INT64)C * dy - (INT64)S * dx;
	const INT64 num = (INT64)C * ((INT64)vy - y1) - (INT64)S * ((INT64)vx - x1);
	INT64 x, y;
	if (den == 0)
	{
		*ox = *oy = INT32_MIN;
		return;
	}
	x = RQ_MulAddDiv(x1, den, num, dx, den);
	y = RQ_MulAddDiv(y1, den, num, dy, den);
	*ox = (x > INT32_MAX || x < INT32_MIN) ? INT32_MIN : (INT32)x;
	*oy = (y > INT32_MAX || y < INT32_MIN) ? INT32_MIN : (INT32)y;
}

// Inputs and outputs of RQ_SetSlopePlane
typedef struct
{
	const rq_slope_t *slope;
	fixed_t xpos, ypos, zpos;
	INT64 xoff, yoff; // fixed units; the slope origin is taken at (-xoff, yoff)
	UINT32 angle, plangle;
	fixed_t plsin, plcos; // FINESINE / FINECOSINE (plangle >> ANGLETOFINESHIFT)
	boolean scaled;
	float xscale, yscale;
	boolean lightonly; // ds_solidcolor || ds_fog: only the light vector is wanted
	int sfshift; // log2 of the su/sv premultiplication: 16 + (powersoftwo ? nflatshiftup : 0)
	float focallength;
} rq_planein_t;

typedef struct
{
	float su[3], sv[3], sz[3], light[3];
	INT64 zeroheight; // Q40
} rq_planeout_t;

// R_SetSlopePlane (scaled == false) and R_SetScaledSlopePlane (scaled == true): vectors of the tilted span drawers
static inline void RQ_SetSlopePlane(const rq_planein_t *in, rq_planeout_t *out)
{
	const rq_slope_t *s = in->slope;
	rq_vec_t o, v, u, lv, lu;
	INT64 sinq, cosq, height, vx, vy, zq;
	const UINT32 plangle = in->plangle;
	INT64 sn, cs;

	// R_SetSlopePlaneOrigin
	vx = RQ_RoundToFloat24((INT64)in->xpos + in->xoff);
	vy = RQ_RoundToFloat24((INT64)in->ypos - in->yoff);
	RQ_SinCos(RQ_Ang2Rad(0xC0000000u - in->angle), &sinq, &cosq);
	vx = RQ_FixedToQ(vx);
	vy = RQ_FixedToQ(vy);
	o.x = RQ_MulShift(vx, cosq, 62) - RQ_MulShift(vy, sinq, 62);
	o.z = RQ_MulShift(vx, sinq, 62) + RQ_MulShift(vy, cosq, 62);
	zq = RQ_FixedToQ(in->zpos);
	o.y = RQ_SlopeZAt(s, -in->xoff, in->yoff) - zq;

	height = RQ_SlopeZAt(s, in->xpos, in->ypos);
	out->zeroheight = height - zq;

	RQ_SinCos(RQ_Ang2Rad(0x80000000u - (in->angle + plangle)), &sinq, &cosq);
	sn = RQ_T2L(sinq);
	cs = RQ_T2L(cosq);

	// CalcSlopeLightVectors
	lv.x = cs;
	lv.z = sn;
	lu.x = sn;
	lu.z = -cs;
	lv.y = RQ_SlopeZAt(s, (fixed_t)((UINT32)in->xpos + (UINT32)in->plsin), (fixed_t)((UINT32)in->ypos + (UINT32)in->plcos)) - height;
	lu.y = RQ_SlopeZAt(s, (fixed_t)((UINT32)in->xpos + (UINT32)in->plcos), (fixed_t)((UINT32)in->ypos - (UINT32)in->plsin)) - height;

	// DoSlopeLightCrossProduct
	out->light[0] = RQ_CrossToFloat(lv.y, lu.z, lv.z, lu.y, -80, 1.0f);
	out->light[1] = RQ_CrossToFloat(lv.z, lu.x, lv.x, lu.z, -80, 1.0f);
	out->light[2] = RQ_CrossToFloat(lv.x, lu.y, lv.y, lu.x, -80, in->focallength);
	if (in->lightonly)
		return;

	if (!in->scaled)
	{
		v = lv;
		u = lu;
	}
	else
	{
		const INT64 xs = RQ_FromFloat(in->xscale, 40), ys = RQ_FromFloat(in->yscale, 40);
		INT64 sinp, cosp;
		float f;
		v.x = RQ_MulShift(ys, cosq, 62);
		v.z = RQ_MulShift(ys, sinq, 62);
		u.x = RQ_MulShift(xs, sinq, 62);
		u.z = -RQ_MulShift(xs, cosq, 62);
		RQ_SinCos(RQ_Ang2Rad(plangle), &sinp, &cosp); // the full angle, not the fine one
		// FloatToFixed(scale * sin/cos(ang)): the double product is narrowed to float before the (fixed_t) cast
		f = RQ_ToFloat(RQ_MulShift(ys, sinp, 62), -40);
		vx = (fixed_t)(f * 65536.0f);
		f = RQ_ToFloat(RQ_MulShift(ys, cosp, 62), -40);
		vy = (fixed_t)(f * 65536.0f);
		v.y = RQ_SlopeZAt(s, (fixed_t)((UINT32)in->xpos + (UINT32)vx), (fixed_t)((UINT32)in->ypos + (UINT32)vy)) - height;
		f = RQ_ToFloat(RQ_MulShift(xs, cosp, 62), -40);
		vx = (fixed_t)(f * 65536.0f);
		f = RQ_ToFloat(RQ_MulShift(xs, sinp, 62), -40);
		vy = (fixed_t)(f * 65536.0f);
		u.y = RQ_SlopeZAt(s, (fixed_t)((UINT32)in->xpos + (UINT32)vx), (fixed_t)((UINT32)in->ypos - (UINT32)vy)) - height;
	}

	// DoSlopeCrossProducts: su = origin x v, sv = origin x u, sz = v x u; z *= focallength; su, sv *= 2^sfshift
	{
		const int ex = -80 + in->sfshift;
		out->su[0] = RQ_CrossToFloat(o.y, v.z, o.z, v.y, ex, 1.0f);
		out->su[1] = RQ_CrossToFloat(o.z, v.x, o.x, v.z, ex, 1.0f);
		out->su[2] = RQ_CrossToFloat(o.x, v.y, o.y, v.x, ex, in->focallength);
		out->sv[0] = RQ_CrossToFloat(o.y, u.z, o.z, u.y, ex, 1.0f);
		out->sv[1] = RQ_CrossToFloat(o.z, u.x, o.x, u.z, ex, 1.0f);
		out->sv[2] = RQ_CrossToFloat(o.x, u.y, o.y, u.x, ex, in->focallength);
		out->sz[0] = RQ_CrossToFloat(v.y, u.z, v.z, u.y, -80, 1.0f);
		out->sz[1] = RQ_CrossToFloat(v.z, u.x, v.x, u.z, -80, 1.0f);
		out->sz[2] = RQ_CrossToFloat(v.x, u.y, v.y, u.x, -80, in->focallength);
	}
}

#endif // PS2_OPT_SLOPE || PS2_OPT_SEGS
#endif // __R_SLOPEQ__
