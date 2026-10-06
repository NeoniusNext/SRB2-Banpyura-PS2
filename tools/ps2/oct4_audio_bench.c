/* Exact native-rate mixer regression and EE COP0 benchmark. */
#include "ps2_audio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _EE
#include <debug.h>
#endif

void PS2_Baseline_MixerRender(ps2_mixer *, int16_t *, const int16_t *, size_t, unsigned);
void PS2_Baseline_MixerParams(ps2_mixer *, int, unsigned, unsigned, unsigned);
#ifdef _MSC_VER
#define ALIGN16 __declspec(align(16))
#else
#define ALIGN16 __attribute__((aligned(16)))
#endif
static ALIGN16 uint8_t pcm[8192];
static ALIGN16 int16_t music[PS2_AUDIO_BLOCK * 2], before[PS2_AUDIO_BLOCK * 2], after[PS2_AUDIO_BLOCK * 2];
static ps2_mixer initial, a, b;
static ALIGN16 int16_t edge_music[2058], edge_before[2058], edge_after[2058];
static unsigned random_state = 0x87654321u;
static unsigned random32(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static unsigned EdgeCases(unsigned *checks, int negative)
{
	static const unsigned lengths[] = { 0,1,2,3,4,5,7,8,15,16,17,127,511,512,513,1025 };
	static const unsigned gains[] = { 0,1,127,16384,32767,32768,32769 };
	unsigned bi, stereo, off, ni, gi, failures = 0, i;
	for (bi = 8; bi <= 16; bi += 8)
	for (stereo = 0; stereo < 2; stereo++)
	for (off = 0; off < 3; off++)
	for (ni = 0; ni < sizeof lengths / sizeof lengths[0]; ni++)
	for (gi = 0; gi < sizeof gains / sizeof gains[0]; gi++)
	{
		ps2_sample sample = { { off, 1300, PS2_AUDIO_RATE, bi, stereo + 1 }, pcm };
		unsigned n = lengths[ni], output_offset = (ni + gi) & 7, in_place;
		for (i = 0; i < 2058; i++) edge_music[i] = (int16_t)random32();
		for (in_place = 0; in_place < 3; in_place++)
		{
			unsigned voices = gi == 4 ? 64 : (random32() & 15);
			PS2_MixerInit(&initial);
			initial.volume = gi == 5 ? 0 : 31;
			for (i = 0; i < voices; i++)
			{
				PS2_MixerStart(&initial, &sample, (int)i, gi == 4 ? 255 : random32() & 255,
					gi == 4 ? 128 : random32() & 255, 128);
				initial.voices[i].frame = i % 3 == 0 ? 1299 : random32() & 7;
				initial.voices[i].fraction = random32() & 65535;
				if (i % 3 == 1) initial.voices[i].step = random32();
			}
			a = b = initial;
			memcpy(edge_before, edge_music, sizeof edge_music);
			memcpy(edge_after, edge_music, sizeof edge_music);
			PS2_Baseline_MixerRender(&a, edge_before + output_offset,
				in_place == 0 ? NULL : in_place == 1 ? edge_music : edge_before + output_offset, n, gains[gi]);
			PS2_MixerRender(&b, edge_after + output_offset,
				in_place == 0 ? NULL : in_place == 1 ? edge_music : edge_after + output_offset, n, gains[gi]);
			if (negative && *checks == 0) edge_after[0] ^= 1;
			if (memcmp(edge_before, edge_after, sizeof edge_before) || memcmp(&a, &b, sizeof a))
			{
				if (!failures) printf("AM edge first failure bits=%u stereo=%u off=%u n=%u gain=%u inplace=%u\n",bi,stereo,off,n,gains[gi],in_place);
				failures++;
			}
			(*checks)++;
		}
	}
	printf("AM edges checks=%u failures=%u\n", *checks, failures);
	return failures;
}
static unsigned count(void)
{
#ifdef _EE
	unsigned v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
#else
	return 0;
#endif
}

int main(int argc, char **argv)
{
	unsigned i, bits, stereo, voices, native, failures = 0, checks = 0;
	int negative = 0;
#ifdef _EE
	init_scr();
#endif
	for (i = 0; i < sizeof pcm; i++) pcm[i] = (uint8_t)(i * 73 + (i >> 4));
	for (i = 0; i < PS2_AUDIO_BLOCK * 2; i++) music[i] = (int16_t)(i * 9237);
	for (i = 0; i < (unsigned)argc; i++)
		if (argv[i] && !strcmp(argv[i], "--negative-control")) negative = 1;
	failures += EdgeCases(&checks, negative);
	{
		static const unsigned rates[] = { 11025, 22050, 44100, 8000, 96000 };
		static const unsigned pitches[] = { 0, 1, 93, 128, 255, 0xffffffffu };
		unsigned ri, pi;
		for (ri = 0; ri < sizeof rates / sizeof rates[0]; ri++)
		{
			ps2_sample sample = { { 0, 2000, rates[ri], 8, 1 }, pcm };
			unsigned basecycles, optcycles, start;
			int h;
			PS2_MixerInit(&a);
			h = PS2_MixerStart(&a, &sample, 0, 255, 128, 128);
			b = a;
			for (pi = 0; pi < sizeof pitches / sizeof pitches[0]; pi++)
			{
				PS2_Baseline_MixerParams(&a, h, 17, 129, pitches[pi]);
				PS2_MixerParams(&b, h, 17, 129, pitches[pi]);
				if (memcmp(&a, &b, sizeof a)) failures++;
			}
			start = count();
			for (pi = 0; pi < 8192; pi++) PS2_Baseline_MixerParams(&a, h, pi & 255, 128, pi & 255);
			basecycles = count() - start;
			start = count();
			for (pi = 0; pi < 8192; pi++) PS2_MixerParams(&b, h, pi & 255, 128, pi & 255);
			optcycles = count() - start;
			if (memcmp(&a, &b, sizeof a)) failures++;
			printf("AM params rate=%u calls=8192 baseline=%u candidate=%u\n", rates[ri], basecycles, optcycles);
		}
	}
	for (bits = 8; bits <= 16; bits += 8)
	for (stereo = 0; stereo <= 1; stereo++)
	for (native = 0; native <= 1; native++)
	for (voices = 1; voices <= 64; voices *= 4)
	{
		ps2_sample sample = { { 0, 2000, PS2_AUDIO_RATE, bits, stereo + 1 }, pcm };
		unsigned basecycles = 0, optcycles = 0, repeat;
		PS2_MixerInit(&initial);
		for (i = 0; i < voices; i++)
		{
			PS2_MixerStart(&initial, &sample, (int)i, 75 + i, (i * 31) & 255, native ? 128 : 93);
			initial.voices[i].fraction = i * 991;
		}
		for (repeat = 0; repeat < 64; repeat++)
		{
			unsigned start;
			a = b = initial;
			start = count();
			PS2_Baseline_MixerRender(&a, before, music, PS2_AUDIO_BLOCK, 24576);
			basecycles += count() - start;
			start = count();
			PS2_MixerRender(&b, after, music, PS2_AUDIO_BLOCK, 24576);
			optcycles += count() - start;
			if (memcmp(before, after, sizeof before) || memcmp(&a, &b, sizeof a)) failures++;
			checks++;
		}
		printf("AM bits=%u channels=%u native=%u voices=%u baseline=%u candidate=%u\n",
			bits, stereo + 1, native, voices, basecycles / 64, optcycles / 64);
	}
	printf("AM DONE checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}
