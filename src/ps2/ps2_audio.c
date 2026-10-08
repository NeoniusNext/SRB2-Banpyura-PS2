// PS2 PCM support: original-rate samples, fixed-point cursors, saturating output.
#include "ps2_audio.h"
#include <string.h>

#ifdef _MSC_VER // the host tests
#define ALWAYS_INLINE __forceinline
#define ASSUME_ALIGNED(p, n) (p)
#else
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define ASSUME_ALIGNED(p, n) __builtin_assume_aligned(p, n)
#endif

static uint32_t LE16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t LE32(const uint8_t *p) { return LE16(p) | LE16(p+2) << 16; }

size_t PS2_AudioRead(const ps2_audio_input *in, size_t off, void *dst, size_t n)
{
	if (!in || !in->read_at || off >= in->size || !n) return 0;
	if (n > in->size - off) n = in->size - off;
	return in->read_at(in->user, off, dst, n);
}

static int ChunkFits(const ps2_audio_input *in, size_t pos, size_t end)
{
	uint8_t h[8];
	unsigned i;
	if (pos > end || end-pos < 8 || PS2_AudioRead(in,pos,h,8) != 8) return 0;
	for (i = 0; i < 4; i++) if (h[i] < 32 || h[i] > 126) return 0;
	return LE32(h+4) <= end-pos-8;
}

int PS2_ParsePCM(const ps2_audio_input *in, ps2_pcm *pcm, int allow_dmx)
{
	uint8_t h[24];
	size_t pos, end, data = 0, bytes = 0;
	unsigned bits = 0, channels = 0;
	uint32_t rate = 0;
	memset(pcm, 0, sizeof *pcm);
	if (PS2_AudioRead(in, 0, h, 8) != 8) return 0;
	if (allow_dmx && LE16(h) == 3)
	{
		rate = LE16(h+2); bytes = LE32(h+4);
		if (!rate || !bytes || bytes > in->size - 8) return 0;
		pcm->offset = 8; pcm->frames = (uint32_t)bytes;
		pcm->rate = rate; pcm->bits = 8; pcm->channels = 1;
		return 1;
	}
	if (PS2_AudioRead(in, 8, h+8, 4) != 4) return 0;
	if (memcmp(h, "RIFF", 4) || memcmp(h+8, "WAVE", 4)) return 0;
	end = (size_t)LE32(h+4);
	if (end < 4 || end > in->size - 8) return 0;
	end += 8;
	for (pos = 12; pos < end;)
	{
		size_t n;
		if (end - pos < 8 || PS2_AudioRead(in, pos, h, 8) != 8) return 0;
		n = LE32(h+4); pos += 8;
		if (n > end - pos) return 0;
		if (!memcmp(h, "fmt ", 4))
		{
			if (n < 16 || PS2_AudioRead(in, pos, h, 16) != 16 || LE16(h) != 1) return 0;
			channels = LE16(h+2); rate = LE32(h+4); bits = LE16(h+14);
			if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16)
				|| rate < 4000 || rate > 96000 || LE16(h+12) != channels * (bits/8)) return 0;
		}
		else if (!memcmp(h, "data", 4)) { data = pos; bytes = n; }
		if (n == end - pos) { pos = end; break; }
		// Some stock PCM WAVs omit the odd data-chunk pad before LIST.
		// Prefer standard padding; accept omission only with a bounded,
		// plausible following chunk, never by extending the RIFF boundary.
		if ((n & 1) && !ChunkFits(in,pos+n+1,end) && ChunkFits(in,pos+n,end))
		{ pos += n; continue; }
		if (n + (n & 1) > end - pos) return 0;
		pos += n + (n & 1);
	}
	if (!bits || !bytes || bytes % (channels * (bits/8))) return 0;
	pcm->offset = data; pcm->frames = (uint32_t)(bytes / (channels * (bits/8)));
	pcm->rate = rate; pcm->bits = bits; pcm->channels = channels;
	return pcm->frames != 0;
}

int16_t PS2_PCMValue(const uint8_t *p, unsigned bits)
{
	return bits == 8 ? (int16_t)(((int)p[0] - 128) * 256) : (int16_t)LE16(p);
}

void PS2_MixerInit(ps2_mixer *m) { memset(m, 0, sizeof *m); m->volume = 31; }

static ps2_voice *Voice(ps2_mixer *m, int h)
{
	ps2_voice *v;
	if (h < 0) return NULL;
	v = &m->voices[h & (PS2_AUDIO_CHANNELS-1)];
	return v->sample && v->generation == (uint32_t)h / PS2_AUDIO_CHANNELS ? v : NULL;
}

void PS2_MixerParams(ps2_mixer *m, int h, unsigned vol, unsigned pan, unsigned pitch)
{
	ps2_voice *v = Voice(m, h);
	uint32_t rate;
	if (!v) return;
	v->volume = vol > 255 ? 255 : vol; v->pan = pan > 255 ? 255 : pan;
	rate = v->sample->pcm.rate;
	if (!pitch) pitch = 1;
	// These exact source-rate ratios avoid the EE's software 64-bit divide.
	if (rate == PS2_AUDIO_RATE) v->step = (uint32_t)pitch << 9;
	else if (rate == PS2_AUDIO_RATE / 2) v->step = (uint32_t)pitch << 8;
	else if (rate == PS2_AUDIO_RATE * 2) v->step = (uint32_t)pitch << 10;
	else v->step = (uint32_t)((uint64_t)rate * 65536 * pitch
		/ (PS2_AUDIO_RATE * 128u));
}

int PS2_MixerStart(ps2_mixer *m, const ps2_sample *s, int ch, unsigned vol, unsigned pan, unsigned pitch)
{
	ps2_voice *v;
	int h;
	if (!s || !s->bytes || !s->pcm.frames || ch < 0 || ch >= PS2_AUDIO_CHANNELS) return -1;
	v = &m->voices[ch];
	v->generation = v->generation % 0x1ffffffu + 1;
	v->sample = s; v->frame = v->fraction = 0;
	h = (int)(v->generation * PS2_AUDIO_CHANNELS + ch);
	PS2_MixerParams(m, h, vol, pan, pitch);
	return h;
}

int PS2_MixerPlaying(const ps2_mixer *m, int h) { return Voice((ps2_mixer *)m, h) != NULL; }
void PS2_MixerStop(ps2_mixer *m, int h) { ps2_voice *v = Voice(m, h); if (v) v->sample = NULL; }
void PS2_MixerForget(ps2_mixer *m, const ps2_sample *s)
{
	unsigned i; for (i = 0; i < PS2_AUDIO_CHANNELS; i++)
		if (m->voices[i].sample == s) m->voices[i].sample = NULL;
}

static int16_t Clip(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
// Eight signed words become eight saturated halfwords. No early saturation:
// all 64 voices have already been added to the 32-bit accumulator.
static __attribute__((noinline)) size_t ClipMMI(int16_t *out, const int32_t *accum, size_t samples)
{
	size_t groups = samples / 8;
	if (!groups || ((uintptr_t)out & 15)) return 0;
	__asm__ volatile(
		"li $8,32767\n\t"
		"pextlw $8,$8,$8\n\t"
		"pcpyld $8,$8,$8\n\t"
		"li $9,-32768\n\t"
		"pextlw $9,$9,$9\n\t"
		"pcpyld $9,$9,$9\n\t"
		"1:\n\t"
		"lq $10,0(%1)\n\t"
		"lq $11,16(%1)\n\t"
		"pmaxw $10,$10,$9\n\t"
		"pmaxw $11,$11,$9\n\t"
		"pminw $10,$10,$8\n\t"
		"pminw $11,$11,$8\n\t"
		"ppach $10,$11,$10\n\t"
		"sq $10,0(%0)\n\t"
		"addiu %1,%1,32\n\t"
		"addiu %0,%0,16\n\t"
		"addiu %2,%2,-1\n\t"
		"bnez %2,1b\n\t"
		"nop\n\t"
		: "+r"(out), "+r"(accum), "+r"(groups)
		: : "$8", "$9", "$10", "$11", "memory");
	return samples & ~(size_t)7;
}
#endif

// x / 256 and x / 32768 with C's truncation toward zero (the result of the original `a * b / 256`), as shifts
static inline int32_t Div256(int32_t x) { return (x + ((x >> 31) & 255)) >> 8; }
static inline int32_t Div32768(int32_t x) { return (x + ((x >> 31) & 32767)) >> 15; }

static ALWAYS_INLINE int Sample16(const uint8_t *p, int aligned)
{
	int16_t v;
	// the EE and the host are little endian; memcpy of an aligned halfword is one lh, no aliasing violation
	if (aligned) { memcpy(&v, ASSUME_ALIGNED(p, 2), sizeof v); return v; }
	return (int16_t)(p[0] | (unsigned)p[1] << 8);
}

#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
// Signed PMULTH products fit 32 bits: |sample * gain| <= 32768 * 256.
// Add 255 to negative 16-bit products before shifting to preserve C division.
#define MMI_MIX(load, setup, divide, stride) \
	__asm__ volatile( \
		"lq $9,0(%3)\n\t" setup \
		"1:\n\t" load \
		"pmulth $zero,$8,$9\n\t" \
		"pmflo $10\n\t" \
		"pmfhi $11\n\t" \
		"pcpyld $12,$11,$10\n\t" \
		"pcpyud $13,$10,$11\n\t" divide \
		"lq $10,0(%1)\n\t" \
		"lq $11,16(%1)\n\t" \
		"paddw $12,$12,$10\n\t" \
		"paddw $13,$13,$11\n\t" \
		"sq $12,0(%1)\n\t" \
		"sq $13,16(%1)\n\t" \
		"addiu %0,%0," stride "\n\t" \
		"addiu %1,%1,32\n\t" \
		"addiu %2,%2,-1\n\t" \
		"bnez %2,1b\n\t" \
		"nop\n\t" \
		: "+r"(data), "+r"(accum), "+r"(groups) \
		: "r"(gains) \
		: "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "hi", "lo", "memory")
#define MMI_DIV256 \
	"psraw $14,$12,31\n\t" \
	"psraw $15,$13,31\n\t" \
	"psrlw $14,$14,24\n\t" \
	"psrlw $15,$15,24\n\t" \
	"paddw $12,$12,$14\n\t" \
	"paddw $13,$13,$15\n\t" \
	"psraw $12,$12,8\n\t" \
	"psraw $13,$13,8\n\t"
#define MMI_BIAS128 \
	"li $15,128\n\t" \
	"pextlh $15,$15,$15\n\t" \
	"pextlw $15,$15,$15\n\t" \
	"pcpyld $15,$15,$15\n\t"
static __attribute__((noinline)) size_t MixNativeMMI(int32_t *accum, const uint8_t *data,
	size_t block, int bits, int stereo, int left, int right)
{
	uint16_t gains[8] __attribute__((aligned(16)));
	size_t groups = block / 4;
	unsigned i, alignment = (unsigned)(bits / 8) * (stereo ? 8u : 4u);
	if (!groups || left < 0 || left > 256 || right < 0 || right > 256
		|| ((uintptr_t)data & (alignment - 1))) return 0;
	for (i = 0; i < 8; i += 2) { gains[i] = (uint16_t)left; gains[i+1] = (uint16_t)right; }
	if (bits == 16)
	{
		if (stereo) MMI_MIX("lq $8,0(%0)\n\t", "", MMI_DIV256, "16");
		else MMI_MIX("ld $8,0(%0)\n\tpextlh $8,$8,$8\n\t", "", MMI_DIV256, "8");
	}
	else
	{
		if (stereo) MMI_MIX("ld $8,0(%0)\n\tpextlb $8,$zero,$8\n\tpsubh $8,$8,$15\n\t", MMI_BIAS128, "", "8");
		else MMI_MIX("lwu $8,0(%0)\n\tpextlb $8,$zero,$8\n\tpextlh $8,$8,$8\n\tpsubh $8,$8,$15\n\t", MMI_BIAS128, "", "4");
	}
	return block & ~(size_t)3;
}
#undef MMI_MIX
#undef MMI_DIV256
#undef MMI_BIAS128
#endif

#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
// PS2-318: the music block into the accumulator (accum = music * gain / 32768, truncating toward zero like Div32768) eight samples per iteration.
// gain 32768 only widens. music and accum 16-byte aligned; returns the number of samples done (a multiple of 8).
static __attribute__((noinline)) size_t MusicMMI(int32_t *accum, const int16_t *music, size_t samples, unsigned gain)
{
	size_t groups = samples / 8;
	uint16_t g[8] __attribute__((aligned(16)));
	unsigned i;
	if (!groups || ((uintptr_t)music & 15) || ((uintptr_t)accum & 15)) return 0;
	if (gain == 32768)
	{
		__asm__ volatile(
			"1:\n\t"
			"lq $8,0(%1)\n\t"
			"pextlh $9,$8,$8\n\t"
			"pextuh $10,$8,$8\n\t"
			"psraw $9,$9,16\n\t"
			"psraw $10,$10,16\n\t"
			"sq $9,0(%0)\n\t"
			"sq $10,16(%0)\n\t"
			"addiu %1,%1,16\n\t"
			"addiu %0,%0,32\n\t"
			"addiu %2,%2,-1\n\t"
			"bnez %2,1b\n\t"
			"nop\n\t"
			: "+r"(accum), "+r"(music), "+r"(groups)
			: : "$8", "$9", "$10", "memory");
		return samples & ~(size_t)7;
	}
	for (i = 0; i < 8; i++) g[i] = (uint16_t)gain;
	__asm__ volatile(
		"lq $9,0(%3)\n\t"
		"1:\n\t"
		"lq $8,0(%1)\n\t"
		"pmulth $zero,$8,$9\n\t"
		"pmflo $10\n\t"
		"pmfhi $11\n\t"
		"pcpyld $12,$11,$10\n\t"
		"pcpyud $13,$10,$11\n\t"
		"psraw $14,$12,31\n\t"
		"psraw $15,$13,31\n\t"
		"psrlw $14,$14,17\n\t"
		"psrlw $15,$15,17\n\t"
		"paddw $12,$12,$14\n\t"
		"paddw $13,$13,$15\n\t"
		"psraw $12,$12,15\n\t"
		"psraw $13,$13,15\n\t"
		"sq $12,0(%0)\n\t"
		"sq $13,16(%0)\n\t"
		"addiu %1,%1,16\n\t"
		"addiu %0,%0,32\n\t"
		"addiu %2,%2,-1\n\t"
		"bnez %2,1b\n\t"
		"nop\n\t"
		: "+r"(accum), "+r"(music), "+r"(groups)
		: "r"(g)
		: "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "hi", "lo", "memory");
	return samples & ~(size_t)7;
}

// PS2-318: a voice whose source rate is twice the output rate (step exactly 2.0: 44.1 kHz sound effects, most of the stock ones) reads every second
// source frame and the fraction never changes, so sixteen (8 bit) / eight (16 bit) output frames are the even elements of 32 source bytes, gathered with
// PPACB / PPACH from unaligned 64-bit loads, then the same products as the native path (8-bit: no divide; 16-bit: the truncating divide by 256).
// p = the source at the current frame, n = output frames (a multiple of 16 / 8); both need the source bytes p[0 .. 2*n*stride-1] to be inside the sample.
#define MMI_PRODUCTS(G, DIVIDE) \
	"pmulth $zero," G ",%[gn]\n\t" \
	"pmflo %[a]\n\t" \
	"pmfhi %[b]\n\t" \
	"pcpyld %[c],%[b],%[a]\n\t" \
	"pcpyud %[d],%[a],%[b]\n\t" DIVIDE \
	"lq %[a],0(%[o])\n\t" \
	"lq %[b],16(%[o])\n\t" \
	"paddw %[c],%[c],%[a]\n\t" \
	"paddw %[d],%[d],%[b]\n\t" \
	"sq %[c],0(%[o])\n\t" \
	"sq %[d],16(%[o])\n\t" \
	"addiu %[o],%[o],32\n\t"
#define MMI_DIV256_ABCD \
	"psraw %[a],%[c],31\n\t" \
	"psraw %[b],%[d],31\n\t" \
	"psrlw %[a],%[a],24\n\t" \
	"psrlw %[b],%[b],24\n\t" \
	"paddw %[c],%[c],%[a]\n\t" \
	"paddw %[d],%[d],%[b]\n\t" \
	"psraw %[c],%[c],8\n\t" \
	"psraw %[d],%[d],8\n\t"
static __attribute__((noinline)) size_t MixDec2Mono8(int32_t *accum, const uint8_t *p, size_t n, int left, int right)
{
	uint16_t gains[8] __attribute__((aligned(16)));
	static const uint16_t bias[8] __attribute__((aligned(16))) = {128, 128, 128, 128, 128, 128, 128, 128};
	size_t groups = n / 16;
	long t0, t1, t2, t3, a, b, c, d, e, f, gn, bi;
	unsigned i;
	if (!groups || left < 0 || left > 256 || right < 0 || right > 256 || ((uintptr_t)accum & 15)) return 0;
	for (i = 0; i < 8; i += 2) { gains[i] = (uint16_t)left; gains[i+1] = (uint16_t)right; }
	__asm__ volatile(
		"lq %[gn],0(%[gp])\n\t"
		"lq %[bi],0(%[bp])\n\t"
		"1:\n\t"
		"ldl %[t0],7(%[p])\n\t" "ldr %[t0],0(%[p])\n\t"
		"ldl %[t1],15(%[p])\n\t" "ldr %[t1],8(%[p])\n\t"
		"ldl %[t2],23(%[p])\n\t" "ldr %[t2],16(%[p])\n\t"
		"ldl %[t3],31(%[p])\n\t" "ldr %[t3],24(%[p])\n\t"
		"pcpyld %[t0],%[t1],%[t0]\n\t"
		"pcpyld %[t2],%[t3],%[t2]\n\t"
		"ppacb %[e],%[t2],%[t0]\n\t"          /* e = the 16 even source bytes */
		"pextlb %[f],$zero,%[e]\n\t"          /* frames 0..7 as halfwords */
		"pextub %[e],$zero,%[e]\n\t"          /* frames 8..15 */
		"psubh %[f],%[f],%[bi]\n\t"
		"psubh %[e],%[e],%[bi]\n\t"
		"pextlh %[t0],%[f],%[f]\n\t"          /* frames 0..3, each twice (left source, right source) */
		"pextuh %[t1],%[f],%[f]\n\t"          /* frames 4..7 */
		"pextlh %[t2],%[e],%[e]\n\t"          /* frames 8..11 */
		"pextuh %[t3],%[e],%[e]\n\t"          /* frames 12..15 */
		MMI_PRODUCTS("%[t0]", "")
		MMI_PRODUCTS("%[t1]", "")
		MMI_PRODUCTS("%[t2]", "")
		MMI_PRODUCTS("%[t3]", "")
		"addiu %[p],%[p],32\n\t"
		"addiu %[g],%[g],-1\n\t"
		"bnez %[g],1b\n\t"
		"nop\n\t"
		: [p] "+r"(p), [o] "+r"(accum), [g] "+r"(groups),
		  [t0] "=&r"(t0), [t1] "=&r"(t1), [t2] "=&r"(t2), [t3] "=&r"(t3), [a] "=&r"(a), [b] "=&r"(b), [c] "=&r"(c), [d] "=&r"(d),
		  [e] "=&r"(e), [f] "=&r"(f), [gn] "=&r"(gn), [bi] "=&r"(bi)
		: [gp] "r"(gains), [bp] "r"(bias)
		: "hi", "lo", "memory");
	return n & ~(size_t)15;
}
static __attribute__((noinline)) size_t MixDec2Mono16(int32_t *accum, const uint8_t *p, size_t n, int left, int right)
{
	uint16_t gains[8] __attribute__((aligned(16)));
	size_t groups = n / 8;
	long t0, t1, t2, t3, a, b, c, d, e, gn;
	unsigned i;
	if (!groups || left < 0 || left > 256 || right < 0 || right > 256 || ((uintptr_t)accum & 15) || ((uintptr_t)p & 1)) return 0;
	for (i = 0; i < 8; i += 2) { gains[i] = (uint16_t)left; gains[i+1] = (uint16_t)right; }
	__asm__ volatile(
		"lq %[gn],0(%[gp])\n\t"
		"1:\n\t"
		"ldl %[t0],7(%[p])\n\t" "ldr %[t0],0(%[p])\n\t"
		"ldl %[t1],15(%[p])\n\t" "ldr %[t1],8(%[p])\n\t"
		"ldl %[t2],23(%[p])\n\t" "ldr %[t2],16(%[p])\n\t"
		"ldl %[t3],31(%[p])\n\t" "ldr %[t3],24(%[p])\n\t"
		"pcpyld %[t0],%[t1],%[t0]\n\t"
		"pcpyld %[t2],%[t3],%[t2]\n\t"
		"ppach %[e],%[t2],%[t0]\n\t"          /* e = the 8 even source halfwords */
		"pextlh %[t0],%[e],%[e]\n\t"          /* frames 0..3, each twice */
		"pextuh %[t1],%[e],%[e]\n\t"          /* frames 4..7 */
		MMI_PRODUCTS("%[t0]", MMI_DIV256_ABCD)
		MMI_PRODUCTS("%[t1]", MMI_DIV256_ABCD)
		"addiu %[p],%[p],32\n\t"
		"addiu %[g],%[g],-1\n\t"
		"bnez %[g],1b\n\t"
		"nop\n\t"
		: [p] "+r"(p), [o] "+r"(accum), [g] "+r"(groups),
		  [t0] "=&r"(t0), [t1] "=&r"(t1), [t2] "=&r"(t2), [t3] "=&r"(t3), [a] "=&r"(a), [b] "=&r"(b), [c] "=&r"(c), [d] "=&r"(d),
		  [e] "=&r"(e), [gn] "=&r"(gn)
		: [gp] "r"(gains)
		: "hi", "lo", "memory");
	return n & ~(size_t)7;
}
#undef MMI_PRODUCTS
#undef MMI_DIV256_ABCD
#endif

#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
// PS2-318: a mono voice with step exactly 2.0 (44.1 kHz source at normal pitch): the fraction never changes and the source frame advances by 2 per
// output frame, so the MMI kernel takes whole groups (16 frames for 8-bit, 8 for 16-bit) while all 2*group source frames are inside the sample,
// then a scalar loop finishes the rest and ends the voice at the right frame (the same arithmetic as the generic loop of MixVoice).
static __attribute__((noinline)) void MixDec2Voice(ps2_voice *v, int32_t *accum, size_t block, const uint8_t *data,
	uint32_t frames, int bits, int left, int right)
{
	uint32_t frame = v->frame;
	const size_t want = bits == 8 ? 16 : 8;
	const size_t cap = frame < frames ? (size_t)((frames - frame) / (2 * want)) * want : 0;
	const size_t n = block < cap ? block : cap;
	size_t f = 0;
	if (n >= want)
	{
		f = bits == 8 ? MixDec2Mono8(accum, data + (size_t)frame, n, left, right) : MixDec2Mono16(accum, data + (size_t)frame * 2, n, left, right);
		frame += (uint32_t)(2 * f);
	}
	for (; f < block; f++, frame += 2)
	{
		int l;
		if (frame >= frames) { v->sample = NULL; break; }
		if (bits == 8) { l = (int)data[frame] - 128; accum[f*2] += l * left; accum[f*2+1] += l * right; }
		else
		{
			l = (int16_t)(data[(size_t)frame * 2] | (unsigned)data[(size_t)frame * 2 + 1] << 8);
			accum[f*2] += Div256(l * left); accum[f*2+1] += Div256(l * right);
		}
	}
	v->frame = frame;
}
#endif

// One voice into the accumulator for up to `block` output frames. Same arithmetic as PS2_PCMValue + the per-sample
// cursor update, with the cursor in registers: nothing is re-read through the voice after each store.
// bits: 8 or 16 (constant at each call site), stereo: source has two channels, aligned: 16-bit data is 2-byte aligned.
static ALWAYS_INLINE void MixVoice(ps2_voice *v, int32_t *accum, size_t block, const uint8_t *data,
	uint32_t frames, int bits, int stereo, int aligned, int left, int right)
{
	uint32_t frame = v->frame, fraction = v->fraction;
	const uint32_t step = v->step;
	const unsigned stride = (unsigned)(bits / 8) * (stereo ? 2u : 1u);
	size_t f;
	if (step == 65536 && fraction < 65536 && frame < frames)
	{
		const uint8_t *p = data + (size_t)frame * stride;
		size_t available = frames - frame;
		if (block > available) block = available;
		// Native-rate playback is contiguous; the fractional cursor is unchanged.
		f = 0;
#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
		f = MixNativeMMI(accum, p, block, bits, stereo, left, right);
		p += f * stride;
#endif
		for (; f < block; f++, p += stride)
		{
			int l, r;
			if (bits == 8) l = (int)p[0] - 128; else l = Sample16(p, aligned);
			if (!stereo) r = l;
			else if (bits == 8) r = (int)p[1] - 128;
			else r = Sample16(p + 2, aligned);
			// 8-bit conversion scales by 256, exactly cancelled by the gain divisor.
			if (bits == 8) { accum[f*2] += l * left; accum[f*2+1] += r * right; }
			else { accum[f*2] += Div256(l * left); accum[f*2+1] += Div256(r * right); }
		}
		v->frame = frame + (uint32_t)block;
		return;
	}
#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
	// PS2-318: step exactly 2.0 (44.1 kHz mono source): a function of its own, so that the generic loop below keeps the code GCC made of it
	// before this step (with the pre-pass inside, its loop got a second counter and ran 10 % slower)
	if (step == 131072 && !stereo) { MixDec2Voice(v, accum, block, data, frames, bits, left, right); return; }
#endif
	for (f = 0; f < block; f++)
	{
		const uint8_t *p;
		int l, r;
		if (frame >= frames) { v->sample = NULL; break; }
		p = data + (size_t)frame * stride;
		if (bits == 8) l = (int)p[0] - 128; else l = Sample16(p, aligned);
		if (!stereo) r = l;
		else if (bits == 8) r = (int)p[1] - 128;
		else r = Sample16(p + 2, aligned);
		if (bits == 8) { accum[f*2] += l * left; accum[f*2+1] += r * right; }
		else { accum[f*2] += Div256(l * left); accum[f*2+1] += Div256(r * right); }
		fraction += step;
		frame += fraction >> 16; fraction &= 65535;
	}
	v->frame = frame; v->fraction = fraction;
}

#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
// Keep each constant-format loop separate from renderer register allocation.
// The MMI packing helper otherwise makes GCC spill hot voice-loop registers.
#define MIX_WRAPPER(name, bits, stereo, aligned) \
static __attribute__((noinline)) void name(ps2_voice *v, int32_t *accum, size_t block, \
	const uint8_t *data, uint32_t frames, int left, int right) \
{ MixVoice(v, accum, block, data, frames, bits, stereo, aligned, left, right); }
MIX_WRAPPER(Mix8Mono, 8, 0, 0)
MIX_WRAPPER(Mix8Stereo, 8, 1, 0)
MIX_WRAPPER(Mix16Mono, 16, 0, 1)
MIX_WRAPPER(Mix16Stereo, 16, 1, 1)
MIX_WRAPPER(Mix16MonoOdd, 16, 0, 0)
MIX_WRAPPER(Mix16StereoOdd, 16, 1, 0)
#undef MIX_WRAPPER
#else
#define Mix8Mono(v,a,b,d,f,l,r) MixVoice(v,a,b,d,f,8,0,0,l,r)
#define Mix8Stereo(v,a,b,d,f,l,r) MixVoice(v,a,b,d,f,8,1,0,l,r)
#define Mix16Mono(v,a,b,d,f,l,r) MixVoice(v,a,b,d,f,16,0,1,l,r)
#define Mix16Stereo(v,a,b,d,f,l,r) MixVoice(v,a,b,d,f,16,1,1,l,r)
#define Mix16MonoOdd(v,a,b,d,f,l,r) MixVoice(v,a,b,d,f,16,0,0,l,r)
#define Mix16StereoOdd(v,a,b,d,f,l,r) MixVoice(v,a,b,d,f,16,1,0,l,r)
#endif

void PS2_MixerRender(ps2_mixer *m, int16_t *out, const int16_t *music, size_t n, unsigned gain)
{
	size_t f;
	unsigned c;
	// Fixed-size scratch; arbitrary callers are handled in blocks.
#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
	int32_t accum[PS2_AUDIO_BLOCK * 2] __attribute__((aligned(16)));
#else
	int32_t accum[PS2_AUDIO_BLOCK * 2];
#endif
	if (gain > 32768) gain = 32768;
	while (n)
	{
		size_t block = n > PS2_AUDIO_BLOCK ? PS2_AUDIO_BLOCK : n;
		if (!music || !gain) memset(accum, 0, block * 2 * sizeof accum[0]);
		else
		{
			f = 0;
#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
			f = MusicMMI(accum, music, block * 2, gain);
#endif
			if (gain == 32768) for (; f < block * 2; f++) accum[f] = music[f];
			else for (; f < block * 2; f++) accum[f] = Div32768((int32_t)music[f] * (int)gain);
		}
		for (c = 0; c < PS2_AUDIO_CHANNELS; c++)
		{
			ps2_voice *v = &m->voices[c];
			const ps2_sample *s = v->sample;
			const uint8_t *data;
			unsigned left, right;
			if (!s) continue;
			// Center is full amplitude on both channels; linear pan to either edge.
			left = v->pan <= 128 ? 128 : 255 - v->pan;
			right = v->pan >= 128 ? 128 : v->pan;
			left = left * v->volume * m->volume * 256 / (128 * 255 * 31);
			right = right * v->volume * m->volume * 256 / (128 * 255 * 31);
			data = s->bytes + s->pcm.offset;
			if (s->pcm.bits == 8)
			{
				if (s->pcm.channels == 2) Mix8Stereo(v, accum, block, data, s->pcm.frames, (int)left, (int)right);
				else Mix8Mono(v, accum, block, data, s->pcm.frames, (int)left, (int)right);
			}
			else if (!((uintptr_t)data & 1))
			{
				if (s->pcm.channels == 2) Mix16Stereo(v, accum, block, data, s->pcm.frames, (int)left, (int)right);
				else Mix16Mono(v, accum, block, data, s->pcm.frames, (int)left, (int)right);
			}
			else
			{
				if (s->pcm.channels == 2) Mix16StereoOdd(v, accum, block, data, s->pcm.frames, (int)left, (int)right);
				else Mix16MonoOdd(v, accum, block, data, s->pcm.frames, (int)left, (int)right);
			}
			if (v->sample && v->frame >= s->pcm.frames) v->sample = NULL;
		}
		f = 0;
#if defined(_EE) && !defined(PS2_NOOPT_AUDIO_MMI)
		f = ClipMMI(out, accum, block * 2);
#endif
		for (; f < block*2; f++) out[f] = Clip(accum[f]);
		out += block*2; if (music) music += block*2; n -= block;
	}
}
