// Float PCM -> signed 16-bit, bit-exact to libvorbis' ov_read(): clamp(floor((double)(f * 32768.0f) + 0.5)).
// libvorbisfile does that per sample in double (__extendsfdf2, __adddf3, floor, __fixdfsi: about 300 EE cycles
// without an FPU for double); here it is a handful of single-precision operations. Exact for every rounding mode:
// the truncation, the conversion back and the subtraction are all exactly representable, only comparisons decide.
#ifndef PS2_PCMCONV_H
#define PS2_PCMCONV_H
#include <stdint.h>

static inline int16_t PS2_FloatToS16(float f)
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
#endif
