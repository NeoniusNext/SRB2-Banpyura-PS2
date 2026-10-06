/* Exact native-rate mixer regression and EE COP0 benchmark. */
#include "ps2_audio.h"
#include <stdio.h>
#include <string.h>
#ifdef _EE
#include <debug.h>
#endif

void PS2_Baseline_MixerRender(ps2_mixer *, int16_t *, const int16_t *, size_t, unsigned);
void PS2_Baseline_MixerParams(ps2_mixer *, int, unsigned, unsigned, unsigned);
static uint8_t pcm[8192];
static int16_t music[PS2_AUDIO_BLOCK * 2], before[PS2_AUDIO_BLOCK * 2], after[PS2_AUDIO_BLOCK * 2];
static ps2_mixer initial, a, b;
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

int main(void)
{
	unsigned i, bits, stereo, voices, native, failures = 0, checks = 0;
#ifdef _EE
	init_scr();
#endif
	for (i = 0; i < sizeof pcm; i++) pcm[i] = (uint8_t)(i * 73 + (i >> 4));
	for (i = 0; i < PS2_AUDIO_BLOCK * 2; i++) music[i] = (int16_t)(i * 9237);
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
