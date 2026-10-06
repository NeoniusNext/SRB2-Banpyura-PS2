// PS2-41 equivalence of the optimised audio code with the original, on the host (MSVC):
//  1. PS2_MixerRender == the original reference loop (copied below, unchanged arithmetic), bit for bit, on random
//     voices (8/16-bit, mono/stereo, odd data offsets, any step/pan/volume/gain, block lengths) incl. the voice state after it;
//  2. PS2_FloatToS16 == clamp(floor((double)(f*32768.0f) + 0.5)) (what libvorbis' ov_read does) on random and edge floats;
//  3. PS2_MusicRender at 22050 Hz / speed 1 (straight-copy path) == the data of the file, with looping and odd block sizes.
// usage: audio_equiv_hosttest [--negative-control]   (the control perturbs the new mixer and must fail)
#include "ps2_audio.h"
#include "ps2_music.h"
#include "ps2_pcmconv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static unsigned long long rng_state = 88172645463325252ull;
static unsigned Rnd(void)
{
	rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
	return (unsigned)(rng_state >> 32);
}

// ---- the original PS2_MixerRender, verbatim apart from the names ----
static int16_t RefClip(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static void RefRender(ps2_mixer *m, int16_t *out, const int16_t *music, size_t n, unsigned gain)
{
	size_t f;
	unsigned c;
	int32_t accum[PS2_AUDIO_BLOCK * 2];
	if (gain > 32768) gain = 32768;
	while (n)
	{
		size_t block = n > PS2_AUDIO_BLOCK ? PS2_AUDIO_BLOCK : n;
		for (f = 0; f < block * 2; f++) accum[f] = music ? (int32_t)music[f] * (int)gain / 32768 : 0;
		for (c = 0; c < PS2_AUDIO_CHANNELS; c++)
		{
			ps2_voice *v = &m->voices[c];
			const ps2_sample *s = v->sample;
			unsigned left, right, stride;
			if (!s) continue;
			stride = s->pcm.channels * (s->pcm.bits/8);
			left = v->pan <= 128 ? 128 : 255 - v->pan;
			right = v->pan >= 128 ? 128 : v->pan;
			left = left * v->volume * m->volume * 256 / (128 * 255 * 31);
			right = right * v->volume * m->volume * 256 / (128 * 255 * 31);
			for (f = 0; f < block; f++)
			{
				const uint8_t *p;
				int l, r;
				if (v->frame >= s->pcm.frames) { v->sample = NULL; break; }
				p = s->bytes + s->pcm.offset + (size_t)v->frame * stride;
				l = PS2_PCMValue(p, s->pcm.bits);
				r = s->pcm.channels == 2 ? PS2_PCMValue(p+s->pcm.bits/8, s->pcm.bits) : l;
				accum[f*2] += l * (int)left / 256; accum[f*2+1] += r * (int)right / 256;
				v->fraction += v->step;
				v->frame += v->fraction >> 16; v->fraction &= 65535;
			}
			if (v->frame >= s->pcm.frames) v->sample = NULL;
		}
		for (f = 0; f < block*2; f++) out[f] = RefClip(accum[f]);
		out += block*2; if (music) music += block*2; n -= block;
	}
}

static int negative_control;
static void NativeMixerTest(void)
{
	static uint8_t data[4096];
	static int16_t music[1200], outA[1200], outB[1200];
	unsigned bits, channels, offset, frames, fraction;
	unsigned checks = 0, i;
	for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)Rnd();
	for (i = 0; i < 1200; i++) music[i] = (int16_t)Rnd();
	for (bits = 8; bits <= 16; bits += 8)
	for (channels = 1; channels <= 2; channels++)
	for (offset = 0; offset <= 1; offset++)
	for (frames = 1; frames <= 700; frames += 233)
	for (fraction = 0; fraction <= 65535; fraction += 21845)
	{
		ps2_sample s = { { offset, frames, PS2_AUDIO_RATE, bits, channels }, data };
		ps2_mixer a, b;
		PS2_MixerInit(&a);
		for (i = 0; i < PS2_AUDIO_CHANNELS; i++)
		{
			int h = PS2_MixerStart(&a, &s, (int)i, (i * 61) & 255, (i * 17) & 255, 128);
			a.voices[h & (PS2_AUDIO_CHANNELS - 1)].fraction = fraction;
		}
		b = a;
		for (i = 0; i < 3; i++)
		{
			RefRender(&a, outA, music, 600, 32768);
			PS2_MixerRender(&b, outB, music, 600, 32768);
			CHECK(!memcmp(outA, outB, sizeof outA));
			CHECK(!memcmp(&a, &b, sizeof a));
			checks++;
		}
	}
	printf("Native mixer: %u exact PCM/cursor checks (64 voices, fraction, EOF, odd offsets)\n", checks);
}

static void MixerTest(void)
{
	enum { NS = 12, DATA = 4096 };
	static union { uint64_t align; uint8_t b[DATA + 8]; } pool_u[NS];
	static int16_t music[1500 * 2], outA[1500 * 2], outB[1500 * 2], inPlace[1500 * 2];
	ps2_sample samples[NS];
	unsigned round, i;
	unsigned long long compared = 0;
	for (round = 0; round < 4000; round++)
	{
		ps2_mixer a, b;
		unsigned rounds_in = 1 + Rnd() % 3;
		PS2_MixerInit(&a);
		for (i = 0; i < NS; i++)
		{
			ps2_sample *s = &samples[i];
			unsigned k, offset = Rnd() % 5; // odd and even offsets
			unsigned bits = (Rnd() & 1) ? 16 : 8, channels = 1 + (Rnd() & 1);
			unsigned frames = 1 + Rnd() % (DATA / 8 - 1);
			for (k = 0; k < sizeof pool_u[i].b; k++) pool_u[i].b[k] = (uint8_t)((Rnd() & 3) ? Rnd() : ((Rnd() & 1) ? 0xff : 0x80)); // extremes too
			memset(s, 0, sizeof *s);
			s->bytes = pool_u[i].b; s->pcm.offset = offset; s->pcm.frames = frames; s->pcm.bits = bits; s->pcm.channels = channels;
			s->pcm.rate = 4000 + Rnd() % 60000;
		}
		for (i = 0; i < PS2_AUDIO_CHANNELS; i++)
			if (Rnd() % 4 == 0)
				PS2_MixerStart(&a, &samples[Rnd() % NS], (int)i, Rnd() % 300, Rnd() % 300, Rnd() % 300);
		a.volume = Rnd() % 40;
		b = a;
		for (i = 0; i < 1500 * 2; i++) music[i] = (int16_t)((Rnd() & 7) ? Rnd() : (Rnd() & 1 ? 32767 : -32768));
		while (rounds_in--)
		{
			size_t n = 1 + Rnd() % 1499;
			unsigned gain = (Rnd() & 3) == 0 ? 32768 : Rnd() % 40000; // also > 32768
			const int16_t *mus = (Rnd() & 3) ? music : NULL;
			ps2_mixer alias = b;
			if (round % 7 == 0) gain = 0;
			memcpy(inPlace, music, n * 4);
			RefRender(&a, outA, mus, n, gain);
			PS2_MixerRender(&b, outB, mus, n, gain);
			PS2_MixerRender(&alias, inPlace, mus ? inPlace : NULL, n, gain);
			if (negative_control && round == 1000) outB[0] ^= 1;
			CHECK(!memcmp(outA, outB, n * 4));
			CHECK(!memcmp(outA, inPlace, n * 4));
			CHECK(!memcmp(&a, &b, sizeof a)); // frames, fractions, active voices
			CHECK(!memcmp(&a, &alias, sizeof a));
			compared += n;
		}
	}
	printf("Mixer: %llu stereo frames identical to the original, in-place music and voice state identical\n", compared);
}

static void ConvTest(void)
{
	unsigned long long n = 0;
	unsigned i;
	// edge values around every rounding boundary of the first 2^16 + random
	for (i = 0; i < 20000000; i++)
	{
		float f;
		union { float f; unsigned u; } cvt;
		double ref;
		int val;
		if (i < 140000) f = ((int)i - 70000) / 65536.0f * 1.0001f;
		else if (i & 1) { cvt.u = Rnd(); f = cvt.f; if (!(f == f) || f > 1e30f || f < -1e30f) f = 0.5f; }
		else f = ((float)Rnd() / 4294967296.0f - 0.5f) * 2.2f;
		if (i % 7 == 0) { f = nextafterf(f, 1.0f); }
		if (i % 11 == 0) { f = (float)(int)(f * 32768.0f) / 32768.0f; f += (i & 2) ? 0.5f / 32768.0f : -0.5f / 32768.0f; }
		ref = floor((double)(f * 32768.0f) + 0.5);
		val = ref > 32767 ? 32767 : ref < -32768 ? -32768 : (int)ref;
		CHECK(PS2_FloatToS16(f) == val);
		n++;
	}
	printf("FloatToS16: %llu values identical to clamp(floor(double(f*32768)+0.5))\n", n);
}

typedef struct { const unsigned char *data; size_t length; } input;
static size_t Read(void *user, size_t off, void *dst, size_t n) { input *s = user; CHECK(off <= s->length && n <= s->length - off); memcpy(dst, s->data + off, n); return n; }
static void LE16(unsigned char *p, unsigned n) { p[0] = (unsigned char)n; p[1] = (unsigned char)(n>>8); }
static void LE32(unsigned char *p, unsigned n) { LE16(p,n); LE16(p+2,n>>16); }

static void MusicTest(void)
{
	enum { FRAMES = 5000, LOOP_MS = 100 };
	static unsigned char b[44 + FRAMES * 4];
	static int16_t expect[FRAMES * 2], out[700 * 2];
	input src = { b, sizeof b };
	ps2_audio_input in = { &src, sizeof b, Read };
	ps2_music *m;
	unsigned i, pos, total = 0;
	unsigned loop_frame = (unsigned)((unsigned long long)LOOP_MS * PS2_AUDIO_RATE / 1000);
	memset(b, 0, sizeof b);
	memcpy(b, "RIFF", 4); LE32(b+4, 36 + FRAMES * 4); memcpy(b+8, "WAVEfmt ", 8); LE32(b+16, 16); LE16(b+20, 1); LE16(b+22, 2);
	LE32(b+24, PS2_AUDIO_RATE); LE32(b+28, PS2_AUDIO_RATE * 4); LE16(b+32, 4); LE16(b+34, 16); memcpy(b+36, "data", 4); LE32(b+40, FRAMES * 4);
	for (i = 0; i < FRAMES * 2; i++) { expect[i] = (int16_t)(Rnd() >> 3); LE16(b+44+i*2, (unsigned)(uint16_t)expect[i]); }
	m = PS2_MusicOpen(&in); CHECK(m && PS2_MusicType(m) == PS2_MUSIC_WAV);
	CHECK(PS2_MusicSetLoop(m, LOOP_MS) && PS2_MusicPlay(m, 1));
	pos = 0;
	while (total < FRAMES * 3)
	{
		size_t n = 1 + Rnd() % 699, got = PS2_MusicRender(m, out, n);
		CHECK(got == n);
		for (i = 0; i < n; i++)
		{
			CHECK(out[i*2] == expect[pos*2] && out[i*2+1] == expect[pos*2+1]);
			if (++pos == FRAMES) pos = loop_frame;
		}
		total += (unsigned)n;
	}
	CHECK(PS2_MusicPlaying(m));
	CHECK(PS2_MusicPlay(m, 0)); // plays out once and stops, the tail is zeroed
	for (total = 0; PS2_MusicPlaying(m); ) { size_t got = PS2_MusicRender(m, out, 700); total += (unsigned)got; CHECK(got <= 700); }
	CHECK(total == FRAMES);
	PS2_MusicClose(m);
	printf("Music: straight-copy path identical to the file data (looping, odd block sizes, EOF)\n");
}

int main(int argc, char **argv)
{
	negative_control = argc > 1 && !strcmp(argv[1], "--negative-control");
	MixerTest();
	NativeMixerTest();
	ConvTest();
	MusicTest();
	puts("PS2 audio equivalence PASS");
	return 0;
}
