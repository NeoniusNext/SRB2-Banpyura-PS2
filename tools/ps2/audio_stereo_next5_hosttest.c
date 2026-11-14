/* Actual frozen/current functions, complete mixer state, and bounded stereo gathers. */
#include "audio_half_next4_hosttest.inc"
static unsigned stereo_negative, stereo_state_negative;

static void StereoCases(void)
{
	static const uint32_t fractions[] = {0, 1, 32767, 32768, 65535, 65536, 0x8000ffffu, UINT32_MAX};
	static const uint32_t steps[] = {131071, 131072, 131073};
	static const unsigned lengths[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 511, 512, 513, 1025};
	static const unsigned blocks[] = {0, 1, 2, 3, 4, 7, 8, 9, 31, 511, 512, 513, 1025};
	unsigned bits, off, fi, si, li, bi, start;
	for (bits = 8; bits <= 16; bits += 8)
	for (off = 0; off < 16; off++)
	for (fi = 0; fi < sizeof fractions / sizeof *fractions; fi++)
	for (si = 0; si < sizeof steps / sizeof *steps; si++)
	for (li = 0; li < sizeof lengths / sizeof *lengths; li++)
	for (start = 0; start < 4; start++)
	{
		ps2_sample sample = {{off, lengths[li], 44100, bits, 2}, pcm};
		PS2_MixerInit(&initial);
		PS2_MixerStart(&initial, &sample, 0, 255, (li * 29) & 255, 128);
		initial.voices[0].step = steps[si]; initial.voices[0].fraction = fractions[fi];
		initial.voices[0].frame = start == 0 ? 0 : start == 1 ? lengths[li] - 1 : start == 2 ? lengths[li] : UINT32_MAX;
		for (bi = 0; bi < sizeof blocks / sizeof *blocks; bi++)
		{
			a = b = initial;
			if (start == 0 && lengths[li] >= 8 && blocks[bi] >= 4 && fi == 0 && si == 1)
			{
				negative = stereo_negative; state_negative = stereo_state_negative;
				stereo_negative = stereo_state_negative = 0;
			}
			Compare(blocks[bi], bi & 1 ? 12345 : 32768, bi & 3, (int)(bi & 1));
		}
	}
	printf("STEREO5 8/16-bit, all 16 offsets, canonical/noncanonical fractions, adjacent steps, EOF, odd blocks PASS\n");
}

static void StereoGainEdges(void)
{
	static const unsigned pans[] = {0, 1, 64, 127, 128, 129, 192, 254, 255}, volumes[] = {0, 1, 31};
	unsigned bits, off, volume, pan, global;
	for (bits = 8; bits <= 16; bits += 8)
	for (off = 0; off < 2; off++)
	for (volume = 0; volume < 256; volume++)
	for (pan = 0; pan < sizeof pans / sizeof *pans; pan++)
	for (global = 0; global < sizeof volumes / sizeof *volumes; global++)
	{
		ps2_sample sample = {{off, 65, 44100, bits, 2}, pcm};
		PS2_MixerInit(&initial); initial.volume = volumes[global];
		PS2_MixerStart(&initial, &sample, 0, volume, pans[pan], 128);
		a = b = initial;
		Compare(32, 32768, 0, 0); Compare(1, 32768, 0, 0); Compare(1, 32768, 0, 0);
	}
	printf("STEREO5 signed PCM extrema, all 256 volumes, pan/global gains, exact expiry PASS\n");
}

static void StereoPolyphony(void)
{
	static ps2_sample samples[64];
	static const uint32_t steps[] = {131072, 131072, 131071, 131073, 65536, 32768, UINT32_MAX};
	unsigned test, i, k;
	for (test = 0; test < 1024; test++)
	{
		unsigned voices = 1 + Random() % 64;
		PS2_MixerInit(&initial); initial.volume = Random() % 32;
		for (i = 0; i < voices; i++)
		{
			ps2_sample *s = &samples[i];
			memset(s, 0, sizeof *s);
			s->bytes = pcm; s->pcm.offset = Random() % 16; s->pcm.frames = 1 + Random() % 4096;
			s->pcm.rate = 44100; s->pcm.bits = Random() & 1 ? 8 : 16; s->pcm.channels = 1 + Random() % 2;
			PS2_MixerStart(&initial, s, (int)i, Random() % 256, Random() % 256, 128);
			initial.voices[i].fraction = Random() & 65535;
			initial.voices[i].frame = Random() % (s->pcm.frames + 1);
			initial.voices[i].step = steps[Random() % (sizeof steps / sizeof *steps)];
		}
		a = b = initial;
		for (k = 0; k < 5; k++)
		{
			unsigned n = Random() % 1026, gain = Random() % 40000, offset = Random() % 4, alias = Random() & 1;
			int h = (int)(a.voices[k].generation * 64 + k);
			unsigned volume = Random() % 256, pan = Random() % 256, pitch = 127 + Random() % 3;
			Compare(n, gain, offset, (int)alias);
			Ref_MixerParams(&a, h, volume, pan, pitch); PS2_MixerParams(&b, h, volume, pan, pitch);
			CHECK(!memcmp(&a, &b, sizeof a));
		}
	}
	printf("STEREO5 1..64 voices, mixed rates/formats, parameters, aliased music PASS\n");
}

static void StereoBench(void)
{
	static const uint32_t fractions[] = {0, 65535, 65536};
	unsigned bits, offset, phase, voices, rep, i;
	for (bits = 8; bits <= 16; bits += 8)
	for (offset = 0; offset < 2; offset++)
	for (phase = 0; phase < sizeof fractions / sizeof *fractions; phase++)
	for (voices = 1; voices <= 64; voices *= 8)
	{
		ps2_sample sample = {{offset, 4096, 44100, bits, 2}, pcm};
		unsigned best_a = UINT32_MAX, best_b = UINT32_MAX;
		PS2_MixerInit(&initial);
		for (i = 0; i < voices; i++)
		{
			PS2_MixerStart(&initial, &sample, (int)i, 127, (i * 31) & 255, 128);
			initial.voices[i].fraction = fractions[phase];
		}
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
		printf("STEREO5 BENCH bits=%u offset=%u fraction=%u voices=%u baseline=%u candidate=%u\n",
			bits, offset, (unsigned)fractions[phase], voices, best_a, best_b);
	}
}

int main(int argc, char **argv)
{
	unsigned i, bench_only = 0;
	for (i = 1; i < (unsigned)argc; i++)
	{
		stereo_negative |= !strcmp(argv[i], "--stereo-negative-control");
		stereo_state_negative |= !strcmp(argv[i], "--stereo-state-negative-control");
		bench_only |= !strcmp(argv[i], "--stereo-benchmark-only");
	}
	if (stereo_negative || stereo_state_negative || bench_only)
	{
#ifdef _EE
		init_scr();
#endif
		for (i = 0; i < sizeof pcm; i++) pcm[i] = (uint8_t)Random();
		for (i = 0; i < sizeof music / sizeof *music; i++) music[i] = (int16_t)Random();
		HalfPCMEdges();
		if (bench_only) { StereoBench(); DispatchBench(); HalfBench(); printf("STEREO5 BENCH DONE\n"); return 0; }
		StereoCases();
		return 2;
	}
	AudioHalfNext4Main(argc, argv);
	StereoCases(); StereoGainEdges(); StereoPolyphony();
	if (!getenv("PS2_AUDIO_NO_BENCH")) { StereoBench(); DispatchBench(); }
	printf("STEREO5 DONE checks=%u frames=%llu\n", checks, (unsigned long long)frames_checked);
	return 0;
}
