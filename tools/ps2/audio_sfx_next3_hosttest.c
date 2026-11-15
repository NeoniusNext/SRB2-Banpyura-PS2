/* Compare the saved pre-pass mixer with production, including every voice field. */
#include "ps2_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _EE
#include <debug.h>
#else
#include <time.h>
#endif

void Ref_MixerRender(ps2_mixer *, int16_t *, const int16_t *, size_t, unsigned);
void Ref_MixerParams(ps2_mixer *, int, unsigned, unsigned, unsigned);
static unsigned checks, negative, state_negative;
static uint64_t frames_checked;
static uint8_t pcm[20000] __attribute__((aligned(64)));
static int16_t music[4096] __attribute__((aligned(64)));
static int16_t oa[4104] __attribute__((aligned(64))), ob[4104] __attribute__((aligned(64)));
static ps2_mixer initial, a, b;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "sfx-next3:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint32_t seed = 0x4678a192u;
static uint32_t Random(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static unsigned Now(void)
{
#ifdef _EE
	unsigned v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v;
#else
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned)((uint64_t)t.tv_sec * 1000000000u + t.tv_nsec);
#endif
}
static void Compare(size_t n, unsigned gain, unsigned offset, int alias)
{
	memset(oa, 0x5a, sizeof oa); memcpy(ob, oa, sizeof oa);
	if (alias) { memcpy(oa + offset, music, n * 4); memcpy(ob + offset, music, n * 4); }
	Ref_MixerRender(&a, oa + offset, alias ? oa + offset : music, n, gain);
	PS2_MixerRender(&b, ob + offset, alias ? ob + offset : music, n, gain);
	if (negative && n) { ob[offset] ^= 1; negative = 0; }
	if (state_negative) { b.voices[0].fraction ^= 1; state_negative = 0; }
	CHECK(!memcmp(oa, ob, sizeof oa));
	CHECK(!memcmp(&a, &b, sizeof a));
	frames_checked += n;
}
static void Exhaustive(void)
{
	static const uint32_t steps[] = {0, 1, 511, 8191, 16384, 32768, 65535, 65536, 65537, 131072,
		285326, 0xfffeffffu, 0xffff0000u, 0xffff0001u, 0xffffffffu};
	static const uint32_t fractions[] = {0, 1, 32767, 32768, 65535, 0xffffffffu};
	static const unsigned lengths[] = {1, 2, 3, 7, 511, 512, 513, 4096};
	static const unsigned blocks[] = {0, 1, 2, 3, 7, 511, 512, 513, 1025};
	unsigned bits, channels, off, si, fi, li, bi, mode;
	for (bits = 8; bits <= 16; bits += 8)
	for (channels = 1; channels <= 2; channels++)
	for (off = 0; off < 4; off++)
	for (si = 0; si < sizeof steps / sizeof *steps; si++)
	for (fi = 0; fi < sizeof fractions / sizeof *fractions; fi++)
	for (li = 0; li < sizeof lengths / sizeof *lengths; li++)
	{
		ps2_sample sample = {{off, lengths[li], 22050, bits, channels}, pcm};
		for (mode = 0; mode < 3; mode++)
		{
			PS2_MixerInit(&initial);
			PS2_MixerStart(&initial, &sample, 0, mode ? 1 : 0, (si * 19) & 255, 128);
			initial.volume = mode == 1 ? 1 : mode == 2 ? 0 : 31;
			initial.voices[0].step = steps[si]; initial.voices[0].fraction = fractions[fi];
			initial.voices[0].frame = li & 1 ? lengths[li] - 1 : 0;
			/* Include both the first invalid source frame and normal starts. */
			if (fi == 1) initial.voices[0].frame = lengths[li];
			for (bi = 0; bi < sizeof blocks / sizeof *blocks; bi++)
			{
				a = b = initial;
				Compare(blocks[bi], bi & 1 ? 12345 : 32768, bi & 3, (int)(bi & 1));
			}
		}
	}
}
static void Randomized(void)
{
	unsigned test, i;
	static ps2_sample samples[64];
	for (test = 0; test < 2048; test++)
	{
		unsigned count = 1 + Random() % 64;
		PS2_MixerInit(&initial); initial.volume = Random() % 32;
		for (i = 0; i < count; i++)
		{
			ps2_sample *s = &samples[i];
			memset(s, 0, sizeof *s);
			s->bytes = pcm; s->pcm.offset = Random() % 16; s->pcm.frames = 1 + Random() % 4096;
			s->pcm.rate = 4000 + Random() % 92001; s->pcm.bits = Random() & 1 ? 8 : 16;
			s->pcm.channels = 1 + Random() % 2;
			PS2_MixerStart(&initial, s, (int)i, Random() % 256, Random() % 256, Random() % 256);
			initial.voices[i].fraction = Random() & 65535;
			initial.voices[i].frame = Random() % (s->pcm.frames + 1);
			if ((Random() & 3) == 0) initial.voices[i].volume = 0;
		}
		a = b = initial;
		for (i = 0; i < 5; i++)
		{
			int h = (int)(a.voices[0].generation * 64);
			unsigned volume = Random() % 256, pan = Random() % 256, pitch = Random();
			Compare(Random() % 1026, Random() % 40000, Random() % 4, (int)(Random() & 1));
			Ref_MixerParams(&a, h, volume, pan, pitch); PS2_MixerParams(&b, h, volume, pan, pitch);
			CHECK(!memcmp(&a, &b, sizeof a));
		}
	}
}
static void Benchmark(void)
{
	unsigned bits, channels, mode, voices, rep, i;
	for (bits = 8; bits <= 16; bits += 8)
	for (channels = 1; channels <= 2; channels++)
	for (mode = 0; mode < 5; mode++)
	for (voices = 1; voices <= 64; voices *= 8)
	{
		ps2_sample sample = {{0, 4096, mode == 3 ? 44100 : 22050, bits, channels}, pcm};
		unsigned best_a = 0xffffffffu, best_b = 0xffffffffu;
		PS2_MixerInit(&initial);
		for (i = 0; i < voices; i++)
		{
			PS2_MixerStart(&initial, &sample, (int)i, mode == 1 ? 0 : 127, (i * 31) & 255, mode == 2 ? 93 : 128);
			initial.voices[i].fraction = i * 991;
		}
		if (mode == 0) initial.volume = 0;
		a = b = initial; Compare(512, 24576, 0, 0);
		for (rep = 0; rep < 5; rep++)
		{
			unsigned t = Now(), elapsed;
			for (i = 0; i < 32; i++) { a = initial; Ref_MixerRender(&a, oa, music, 512, 24576); }
			elapsed = Now() - t; if (elapsed < best_a) best_a = elapsed;
			t = Now();
			for (i = 0; i < 32; i++) { b = initial; PS2_MixerRender(&b, ob, music, 512, 24576); }
			elapsed = Now() - t; if (elapsed < best_b) best_b = elapsed;
		}
		CHECK(!memcmp(&a, &b, sizeof a)); CHECK(!memcmp(oa, ob, 2048));
		printf("SFX3 BENCH bits=%u channels=%u mode=%u voices=%u baseline=%u candidate=%u\n", bits, channels, mode, voices, best_a, best_b);
	}
}
int main(int argc, char **argv)
{
	unsigned i;
#ifdef _EE
	init_scr();
#endif
	for (i = 1; i < (unsigned)argc; i++)
	{
		negative |= !strcmp(argv[i], "--negative-control");
		state_negative |= !strcmp(argv[i], "--state-negative-control");
	}
	for (i = 0; i < sizeof pcm; i++) pcm[i] = (uint8_t)Random();
	for (i = 0; i < sizeof music / sizeof *music; i++) music[i] = (int16_t)Random();
	Exhaustive(); Randomized();
	if (!getenv("PS2_AUDIO_NO_BENCH")) Benchmark();
	printf("SFX3 DONE checks=%u frames=%llu\n", checks, (unsigned long long)frames_checked);
	return 0;
}
