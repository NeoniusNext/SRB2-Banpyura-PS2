// Float PCM -> signed 16-bit, bit-exact to libvorbis' ov_read(): clamp(floor((double)(f * 32768.0f) + 0.5)).
// libvorbisfile does that per sample in double (__extendsfdf2, __adddf3, floor, __fixdfsi: about 300 EE cycles
// without an FPU for double); PS2_FloatToS16_ref is a handful of single-precision operations. Exact for every rounding mode:
// the truncation, the conversion back and the subtraction are all exactly representable, only comparisons decide.
// PS2_FloatToS16 on the EE (PS2-310) is a branch-free form with the same result for every input (tested on the EE: ABENCH selftest).
#ifndef PS2_PCMCONV_H
#define PS2_PCMCONV_H
#include <stdint.h>
#include <stddef.h>
#if defined(_EE) && defined(PS2_VORBIS_VU0)
#include "vorbis/ps2_vu0a.h"
extern volatile int ps2a_vu0_disable;
#endif

static inline int16_t PS2_FloatToS16_ref(float f)
{
	float p = f * 32768.0f;
	int t;
	float frac;

	// beyond these the result is clamped anyway; keeps (int)p in range
	if (p > 32768.0f)
		p = 32768.0f;
	else if (p < -32769.0f)
		p = -32769.0f;
	t = (int)p; // toward zero
	frac = p - (float)t; // exact, |frac| < 1, sign of p
	t += (frac >= 0.5f) - (frac < -0.5f); // floor(p + 0.5)
	return (int16_t)(t > 32767 ? 32767 : t < -32768 ? -32768 : t);
}

#ifdef _EE
// Branch-free form for the EE (PS2-310), exact in every rounding mode like the reference: t = (int)p truncates (a conversion, never a sum),
// frac = p - (float)t is exact, and the two comparisons of the reference are made on the bits of frac (frac is in (-1, 1)):
//   frac >= 0.5   <=>  bits in [0x3f000000, 0x3f800000)             -> (bits - 0x3f000000) < 0x00800000 (unsigned)
//   frac < -0.5   <=>  bits in [0xbf000001, 0xbf800000)             -> (bits - 0xbf000001) < 0x007fffff (unsigned)
// (negative zero and +-0 give neither). The FPU compare + branch pairs of the reference cost about 3 times as much. A saturated t (|p| >= 2^31)
// leaves a meaningless frac, but then the clamp decides anyway: t is clamped to +-32770 before the adjustment of at most one.
static inline int16_t PS2_FloatToS16(float f)
{
	float p = f * 32768.0f;
	int t = (int)p;
	float frac = p - (float)t;
	uint32_t b;
	int adj;
	__builtin_memcpy(&b, &frac, sizeof b);
	adj = (int)((b - 0x3f000000u) < 0x00800000u) - (int)((b - 0xbf000001u) < 0x007fffffu);
	t = t > 32770 ? 32770 : t < -32770 ? -32770 : t;
	t += adj;
	t = t > 32767 ? 32767 : t; // two separate selects: the compiler makes conditional moves of them
	t = t < -32768 ? -32768 : t;
	return (int16_t)t;
}

// Interleaved stereo (or, with l == r, duplicated mono): out[2*i] = conv(l[i]), out[2*i+1] = conv(r[i]).
// (A four-frame unrolled version measured slower on the EE, 803 against 786 cycles per output frame: register pressure.)
// PS2-316: with VU0 free and 16-byte aligned l, r and out, groups of four frames go through the VU0 kernel ps2a_conv (the same results for every input,
// checked by the ABENCH selftest); head, tail and unaligned calls use the scalar code.
static inline void PS2_FloatsToS16Stereo(int16_t *out, const float *l, const float *r, size_t n)
{
	size_t i;
#if defined(PS2_VORBIS_VU0)
	if (n >= 8 && !ps2a_vu0_disable && !(((uintptr_t)l | (uintptr_t)r | (uintptr_t)out) & 15))
	{
		ps2a_vu0_saved sv;
		if (ps2a_vu0_enter(&sv))
		{
			size_t g = n >> 2;
			ps2a_conv(l, r, out, (int)g);
			ps2a_vu0_leave(&sv);
			l += g * 4; r += g * 4; out += g * 8; n -= g * 4;
		}
	}
#endif
	for (i = 0; i < n; i++) { out[i * 2] = PS2_FloatToS16(l[i]); out[i * 2 + 1] = PS2_FloatToS16(r[i]); }
}
#else
static inline int16_t PS2_FloatToS16(float f) { return PS2_FloatToS16_ref(f); }
static inline void PS2_FloatsToS16Stereo(int16_t *out, const float *l, const float *r, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) { out[i * 2] = PS2_FloatToS16(l[i]); out[i * 2 + 1] = PS2_FloatToS16(r[i]); }
}
#endif
#endif
