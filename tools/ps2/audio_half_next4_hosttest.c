/* Actual frozen/current mixer functions, including the complete voice state. */
#define main SFXNext3Main
#include "audio_sfx_next3_hosttest.c"
#undef main
static unsigned half_negative, half_state_negative;

static void HalfPCMEdges(void)
{
	static const int16_t values[] = {-32768, -32767, -257, -256, -255, -1, 0, 1, 255, 256, 257, 32766, 32767};
	unsigned i;
	// Both source parities contain every signed boundary; leave the remaining
	// random PCM intact. Every offset sees both sets in the longer test cases.
	for (i = 0; i < sizeof values / sizeof *values; i++)
	{
		unsigned v = (uint16_t)values[i];
		pcm[i*2] = pcm[65+i*2] = (uint8_t)v;
		pcm[i*2+1] = pcm[66+i*2] = (uint8_t)(v >> 8);
	}
}

static void HalfCases(void)
{
	static const uint32_t fractions[] = {0, 1, 32767, 32768, 65535, UINT32_MAX};
	static const unsigned lengths[] = {1, 2, 3, 7, 8, 31, 511, 512, 513, 1025};
	static const unsigned blocks[] = {0, 1, 2, 3, 4, 7, 8, 31, 511, 512, 513, 1025};
	unsigned bits, channels, off, fi, li, bi, start;
	for (bits = 8; bits <= 16; bits += 8)
	for (channels = 1; channels <= 2; channels++)
	for (off = 0; off < 16; off++)
	for (fi = 0; fi < sizeof fractions / sizeof *fractions; fi++)
	for (li = 0; li < sizeof lengths / sizeof *lengths; li++)
	for (start = 0; start < 3; start++)
	{
		ps2_sample sample = {{off, lengths[li], 11025, bits, channels}, pcm};
		PS2_MixerInit(&initial);
		PS2_MixerStart(&initial, &sample, 0, 255, (li * 29) & 255, 128);
		initial.voices[0].fraction = fractions[fi];
		initial.voices[0].frame = start == 0 ? 0 : lengths[li] - (start == 1);
		for (bi = 0; bi < sizeof blocks / sizeof *blocks; bi++)
		{
			a = b = initial;
			if (start == 0 && lengths[li] >= 2 && blocks[bi] >= 4 && fi == 0)
			{
				negative = half_negative; state_negative = half_state_negative;
				half_negative = half_state_negative = 0;
			}
			Compare(blocks[bi], bi & 1 ? 12345 : 32768, bi & 3, (int)(bi & 1));
		}
	}
	printf("HALF4 canonical/noncanonical fractions, all 16 PCM offsets, EOF, odd blocks PASS\n");
}

static void HalfGainEdges(void)
{
	static const unsigned pans[] = {0, 64, 128, 192, 255}, volumes[] = {0, 1, 31};
	unsigned bits, channels, off, volume, pan, global;
	for (bits = 8; bits <= 16; bits += 8)
	for (channels = 1; channels <= 2; channels++)
	for (off = 0; off < 2; off++)
	for (volume = 0; volume < 256; volume++)
	for (pan = 0; pan < sizeof pans / sizeof *pans; pan++)
	for (global = 0; global < sizeof volumes / sizeof *volumes; global++)
	{
		ps2_sample sample = {{off, 64, 11025, bits, channels}, pcm};
		PS2_MixerInit(&initial); initial.volume = volumes[global];
		PS2_MixerStart(&initial, &sample, 0, volume, pans[pan], 128);
		a = b = initial;
		Compare(128, 32768, 0, 0); Compare(1, 32768, 0, 0);
	}
	printf("HALF4 signed PCM extrema, all 256 voice volumes, pan/global gain boundaries, exact expiry PASS\n");
}

static void HalfPolyphony(void)
{
	static ps2_sample samples[64];
	static const uint32_t steps[] = {32768, 32768, 32767, 32769, 65536, 131072, UINT32_MAX};
	unsigned test, i, k;
	for (test = 0; test < 512; test++)
	{
		unsigned voices = 1 + Random() % 64;
		PS2_MixerInit(&initial); initial.volume = Random() % 32;
		for (i = 0; i < voices; i++)
		{
			ps2_sample *s = &samples[i];
			memset(s, 0, sizeof *s);
			s->bytes = pcm; s->pcm.offset = Random() % 16; s->pcm.frames = 1 + Random() % 4096;
			s->pcm.rate = 11025; s->pcm.bits = Random() & 1 ? 8 : 16; s->pcm.channels = 1 + Random() % 2;
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
	printf("HALF4 1..64 voices, adjacent steps, interleaved parameters, aliased music PASS\n");
}

static void HalfBench(void)
{
	unsigned bits, channels, offset, phase, voices, rep, i;
	for (bits = 8; bits <= 16; bits += 8)
	for (channels = 1; channels <= 2; channels++)
	for (offset = 0; offset < 2; offset++)
	for (phase = 0; phase < 2; phase++)
	for (voices = 1; voices <= 64; voices *= 8)
	{
		ps2_sample sample = {{offset, 4096, 11025, bits, channels}, pcm};
		unsigned best_a = UINT32_MAX, best_b = UINT32_MAX;
		PS2_MixerInit(&initial);
		for (i = 0; i < voices; i++)
		{
			PS2_MixerStart(&initial, &sample, (int)i, 127, (i * 31) & 255, 128);
			initial.voices[i].fraction = phase * 32768;
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
		printf("HALF4 BENCH bits=%u channels=%u offset=%u phase=%u voices=%u baseline=%u candidate=%u\n",
			bits, channels, offset, phase, voices, best_a, best_b);
	}
}

static void DispatchBench(void)
{
	unsigned mode, voices, rep, i;
	for (mode = 0; mode < 8; mode++)
	for (voices = 1; voices <= 64; voices *= 8)
	{
		unsigned bits = mode == 2 || mode >= 4 ? 16 : 8, channels = mode == 2 || mode >= 5 ? 2 : 1;
		ps2_sample sample = {{mode == 2 || mode == 6, 4096, mode == 4 ? 44100 : mode >= 5 ? 11025 : 22050, bits, channels}, pcm};
		unsigned best_a = UINT32_MAX, best_b = UINT32_MAX;
		PS2_MixerInit(&initial);
		for (i = 0; i < voices; i++)
		{
			PS2_MixerStart(&initial, &sample, (int)i, mode == 1 ? 0 : 127, (i * 31) & 255, mode == 2 ? 93 : 128);
			initial.voices[i].fraction = mode == 7 ? 32768 : 0;
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
		printf("DSP4 BENCH mode=%u voices=%u baseline=%u candidate=%u\n", mode, voices, best_a, best_b);
	}
	printf("DSP4 DONE\n");
}

int main(int argc, char **argv)
{
	unsigned i, dispatch_bench = 0;
	for (i = 1; i < (unsigned)argc; i++)
	{
		half_negative |= !strcmp(argv[i], "--half-negative-control");
		half_state_negative |= !strcmp(argv[i], "--half-state-negative-control");
		dispatch_bench |= !strcmp(argv[i], "--dispatch-benchmark-only");
	}
	if (half_negative || half_state_negative || dispatch_bench)
	{
#ifdef _EE
		init_scr();
#endif
		for (i = 0; i < sizeof pcm; i++) pcm[i] = (uint8_t)Random();
		for (i = 0; i < sizeof music / sizeof *music; i++) music[i] = (int16_t)Random();
		HalfPCMEdges();
		if (dispatch_bench) { DispatchBench(); return 0; }
		HalfCases();
		return 2;
	}
	SFXNext3Main(argc, argv);
	HalfPCMEdges(); HalfCases(); HalfGainEdges(); HalfPolyphony();
	if (!getenv("PS2_AUDIO_NO_BENCH")) HalfBench();
	printf("HALF4 DONE checks=%u frames=%llu\n", checks, (unsigned long long)frames_checked);
	return 0;
}
