// Cycles per 512-frame block of PS2_MixerRender (src/ps2/ps2_audio.c) for typical loads: music alone (full gain / reduced gain), N voices of the
// formats the engine produces (DMX 8-bit mono 11025 Hz at normal pitch, pitched, 16-bit 22050 mono/stereo from decoded Ogg sound effects).
// Prints a hash of the output of every configuration so that two builds can be compared bit for bit.   EETEST mixer lines.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <kernel.h>
#ifdef MIXER_SRC
#include MIXER_SRC   // e.g. -DMIXER_SRC='"/path/ps2_audio_old.c"': the mixer of an earlier commit, to compare cycles and hashes
#else
#include "../../../src/ps2/ps2_audio.c"
#endif

static inline unsigned CopCount(void) { unsigned v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }
static int16_t music[1024] __attribute__((aligned(64))), out[1024] __attribute__((aligned(64)));
static uint8_t buf8[200000] __attribute__((aligned(64)));
static int16_t buf16[200000] __attribute__((aligned(64)));
static int16_t buf16s[400000] __attribute__((aligned(64)));
static ps2_mixer mixer;

static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; while (n--) h = (h ^ *b++) * 1099511628211ull; return h; }

static void run(const char *name, int nvoices, const ps2_sample *s, unsigned pitch, int use_music, unsigned gain)
{
	int it, v;
	uint64_t h = 14695981039346656037ull, cyc = 0;
	PS2_MixerInit(&mixer);
	for (it = 0; it < 40; it++)
	{
		unsigned t0;
		if (it == 0) for (v = 0; v < nvoices; v++) PS2_MixerStart(&mixer, s, v, 200 - v, 60 + v * 20, pitch);
		t0 = CopCount();
		PS2_MixerRender(&mixer, out, use_music ? music : NULL, 512, gain);
		cyc += CopCount() - t0;
		h = fnv(h, out, 2048);
	}
	printf("EETEST mixer %-34s voices %2d: %6llu cycles/block  hash %016llx\n", name, nvoices, (unsigned long long)(cyc / 40), (unsigned long long)h);
}

int main(void)
{
	unsigned seed = 3;
	int i;
	ps2_sample s8, s16, s16s;
	for (i = 0; i < 1024; i++) { seed = seed * 1664525u + 1013904223u; music[i] = (int16_t)((int)(seed >> 8) - 8388608 >> 9); }
	for (i = 0; i < 200000; i++) { seed = seed * 1664525u + 1013904223u; buf8[i] = (uint8_t)(seed >> 16); buf16[i] = (int16_t)(seed >> 8); }
	for (i = 0; i < 400000; i++) { seed = seed * 1664525u + 1013904223u; buf16s[i] = (int16_t)(seed >> 8); }
	memset(&s8, 0, sizeof s8); memset(&s16, 0, sizeof s16); memset(&s16s, 0, sizeof s16s);
	s8.bytes = buf8; s8.pcm.offset = 0; s8.pcm.frames = 200000; s8.pcm.rate = 11025; s8.pcm.bits = 8; s8.pcm.channels = 1;
	s16.bytes = (const uint8_t *)buf16; s16.pcm.offset = 0; s16.pcm.frames = 200000; s16.pcm.rate = 22050; s16.pcm.bits = 16; s16.pcm.channels = 1;
	s16s.bytes = (const uint8_t *)buf16s; s16s.pcm.offset = 0; s16s.pcm.frames = 200000; s16s.pcm.rate = 22050; s16s.pcm.bits = 16; s16s.pcm.channels = 2;
	run("music only, gain 32768", 0, &s8, 128, 1, 32768);
	run("music only, gain 20000", 0, &s8, 128, 1, 20000);
	run("music silent (gain 0)", 0, &s8, 128, 1, 0);
	run("8-bit mono 11025 normal pitch", 8, &s8, 128, 0, 0);
	run("8-bit mono 11025 pitch 140", 8, &s8, 140, 0, 0);
	run("16-bit mono 22050 native", 8, &s16, 128, 0, 0);
	run("16-bit stereo 22050 native", 8, &s16s, 128, 0, 0);
	run("music 20000 + 8 DMX 11025", 8, &s8, 128, 1, 20000);
	{
		ps2_sample d8, d16;
		memset(&d8, 0, sizeof d8); memset(&d16, 0, sizeof d16);
		d8 = s8; d8.pcm.rate = 44100;
		d16 = s16; d16.pcm.rate = 44100;
		run("8-bit mono 44100 (step 2.0)", 8, &d8, 128, 0, 0);
		run("16-bit mono 44100 (step 2.0)", 8, &d16, 128, 0, 0);
		run("music 20000 + 4 DMX 44100", 4, &d8, 128, 1, 20000);
		/* voices that end inside a block / start at odd frames / odd pan and gain, many short samples */
		{
			int k, it, v;
			uint64_t h = 14695981039346656037ull;
			unsigned st = 99;
			for (it = 0; it < 300; it++)
			{
				ps2_sample sm[6];
				PS2_MixerInit(&mixer);
				for (v = 0; v < 6; v++)
				{
					sm[v] = (v & 1) ? d16 : d8;
					st = st * 1664525u + 1013904223u; sm[v].pcm.frames = 30 + (st >> 8) % 3000;
					st = st * 1664525u + 1013904223u; sm[v].bytes = (const uint8_t *)(v & 1 ? (const uint8_t *)buf16 + 2 * ((st >> 8) % 200) : buf8 + (st >> 8) % 200);
					st = st * 1664525u + 1013904223u;
					PS2_MixerStart(&mixer, &sm[v], v, 1 + (st >> 8) % 255, (st >> 4) % 256, 128);
				}
				for (k = 0; k < 8; k++)
				{
					PS2_MixerRender(&mixer, out, (k & 1) ? music : NULL, 512, 1 + (st >> 8) % 32768);
					h = fnv(h, out, 2048);
					st = st * 1664525u + 1013904223u;
				}
			}
			printf("EETEST mixer edge cases (voices ending inside blocks, odd start/pan/gain)   hash %016llx\n", (unsigned long long)h);
			/* the pitch changes between blocks: the voice reaches step 2.0 with a non-zero fraction and leaves it again */
			h = 14695981039346656037ull;
			for (it = 0; it < 300; it++)
			{
				ps2_sample sm[6];
				int hd[6];
				PS2_MixerInit(&mixer);
				for (v = 0; v < 6; v++)
				{
					sm[v] = (v & 1) ? d16 : d8;
					st = st * 1664525u + 1013904223u; sm[v].pcm.frames = 500 + (st >> 8) % 6000;
					st = st * 1664525u + 1013904223u; sm[v].bytes = (const uint8_t *)(v & 1 ? (const uint8_t *)buf16 + 2 * ((st >> 8) % 200) : buf8 + (st >> 8) % 200);
					st = st * 1664525u + 1013904223u;
					hd[v] = PS2_MixerStart(&mixer, &sm[v], v, 1 + (st >> 8) % 255, (st >> 4) % 256, 100 + (st >> 20) % 29);
				}
				for (k = 0; k < 8; k++)
				{
					for (v = 0; v < 6; v++) { st = st * 1664525u + 1013904223u; PS2_MixerParams(&mixer, hd[v], 1 + (st >> 8) % 255, (st >> 4) % 256, (st >> 20) % 3 ? 128 : 100 + (st >> 12) % 29); }
					PS2_MixerRender(&mixer, out, NULL, 512, 0);
					h = fnv(h, out, 2048);
				}
			}
			printf("EETEST mixer pitch changes (step 2.0 reached with a fraction)              hash %016llx\n", (unsigned long long)h);
		}
	}
	printf("EETEST DONE\n");
	SleepThread();
	return 0;
}
