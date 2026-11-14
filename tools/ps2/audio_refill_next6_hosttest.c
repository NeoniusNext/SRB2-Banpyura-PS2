// Paired decoded-buffer refill proof against the complete preceding source.
// The established PCM, transport, codec and malformed-input cases run too.
#define main AudioLosslessPreviousMain
#include "audio_lossless_hosttest.c"
#undef main

size_t PS2_TestMidiBytes(unsigned);
size_t Ref_TestMidiBytes(unsigned);
size_t PS2_TestMidiTrackBytes(void);
size_t Ref_TestMidiTrackBytes(void);
void PS2_TestMusicFlipCursor(ps2_music *);
unsigned PS2_TestMusicStep(const ps2_music *);

void *__real_calloc(size_t, size_t);
static unsigned capture_calloc, fail_calloc, calloc_calls;
static size_t calloc_bytes;
void *__wrap_calloc(size_t count, size_t bytes)
{
	if (capture_calloc)
	{
		calloc_calls++; calloc_bytes = count * bytes;
		if (fail_calloc) return NULL;
	}
	return __real_calloc(count, bytes);
}

static void Allocation(source *s, unsigned tracks, unsigned fail)
{
	ps2_audio_input in = {s, s->length, Read};
	ps2_midi *m[2]; unsigned w;
	for (w = 0; w < 2; w++)
	{
		s->which = w; capture_calloc = 1; fail_calloc = fail; calloc_calls = 0; calloc_bytes = 0;
		m[w] = w ? PS2_MIDIOpen(&in) : Ref_MIDIOpen(&in);
		capture_calloc = fail_calloc = 0;
		CHECK(calloc_calls == 1 && calloc_bytes == (w ? PS2_TestMidiBytes(tracks) : Ref_TestMidiBytes(tracks)));
		CHECK((m[w] == NULL) == (fail != 0));
	}
	CHECK(s->trace[0] == s->trace[1] && s->reads[0] == s->reads[1]);
	if (!fail) MidiState(m[0], m[1], s);
	Ref_MIDIClose(m[0]); PS2_MIDIClose(m[1]);
}

static void TrackTests(void)
{
	unsigned t;
	context = "all track allocation counts";
	CHECK(Ref_TestMidiTrackBytes() == PS2_TestMidiTrackBytes());
	CHECK(Ref_TestMidiBytes(32) == PS2_TestMidiBytes(32));
	for (t = 1; t <= 32; t++)
	{
		source s = {0};
		s.data = midi; s.length = MakeMidi(t, 96, 0);
		CHECK(Ref_TestMidiBytes(t) == PS2_TestMidiBytes(t));
		Allocation(&s, t, 0); Allocation(&s, t, 1);
		DirectMidi(&s); Exercise(&s, 0);
		printf("%s MIDI allocation tracks=%u baseline=%zu candidate=%zu saved=%zu\n", TEST_PLATFORM,
			t, Ref_TestMidiBytes(t), PS2_TestMidiBytes(t), Ref_TestMidiBytes(t) - PS2_TestMidiBytes(t));
	}
	printf("MIDI all 1..32 track counts, actual allocation requests/failures, complete state/read/PCM checks PASS\n");
}

static void GainTests(void)
{
	unsigned pattern, k;
	context = "MIDI channel gain controls";
	for (pattern = 0; pattern < 8; pattern++)
	{
		source s = {0};
		size_t off;
		memcpy(midi, "MThd\0\0\0\6", 8); BE16(midi + 8, 0); BE16(midi + 10, 1); BE16(midi + 12, 32767); mp = 14;
		off = mp; memcpy(midi + mp, "MTrk", 4); mp += 8;
		for (k = 0; k < 24; k++) Msg(0, 144 | (k % 16), 48 + k, 1 + k * 5);
		for (k = 0; k < 300; k++)
		{
			unsigned dt = pattern & 1 ? 0 : 1 + k % 3;
			if (k % 8 == 0) Msg(dt, 192 | (k % 16), k % 128, 0);
			else if (k % 8 == 1) Msg(dt, 224 | (k % 16), k % 128, (k * 7) % 128);
			else if (k % 8 == 2) Msg(dt, 176 | (k % 16), 121, 0);
			else if (k % 8 == 3) Msg(dt, 128 | (k % 16), 48 + k % 24, 0);
			else if (k % 8 == 4) Msg(dt, 144 | (k % 16), 48 + k % 24, k % 128);
			else Msg(dt, 176 | (k % 16), k & 1 ? 7 : 11, pattern & 2 ? 127 : (k * 17) % 128);
		}
		VLQ(32767); Put(255); Put(47); Put(0); BE32(midi + off + 4, (unsigned)(mp - off - 8));
		s.data = midi; s.length = mp; DirectMidi(&s); Exercise(&s, 0);
	}
	printf("MIDI dense channel gain/program/bend/reset/note-steal events, zero/one-frame spacing PASS\n");
}

typedef struct
{
	source input;
	ps2_midi *midi[2];
	uint64_t callback_state[2];
} callback_source;

static size_t CallbackRead(void *user, size_t off, void *dst, size_t n)
{
	callback_source *s = user;
	unsigned which = s->input.which;
	if (s->midi[which])
	{
		unsigned char bytes[16384];
		size_t i, length = which ? PS2_TestMidiState(s->midi[which], bytes) : Ref_TestMidiState(s->midi[which], bytes);
		CHECK(length <= sizeof bytes);
		for (i = 0; i < length; i++) s->callback_state[which] = s->callback_state[which] * 16777619u ^ bytes[i];
	}
	return Read(&s->input, off, dst, n);
}

static void MidiCallbackBoundaries(void)
{
	static const unsigned spacing[] = {0, 1, 2, 7, 511, 512, 513, 1023, 1024, 1025};
	static const unsigned blocks[] = {0, 1, 2, 7, 511, 512, 513, 1023, 1024, 1025, 2053};
	unsigned pattern, fail, k;
	context = "MIDI exact event/output/EOF callback boundaries";
	for (pattern = 0; pattern < 4; pattern++) for (fail = 0; fail < 2; fail++)
	{
		callback_source trace = {0}; source *s = &trace.input;
		ps2_audio_input in; ps2_midi *a, *b;
		size_t off;
		memcpy(midi, "MThd\0\0\0\6", 8); BE16(midi + 8, 0); BE16(midi + 10, 1); BE16(midi + 12, PS2_AUDIO_RATE); mp = 14;
		off = mp; memcpy(midi + mp, "MTrk", 4); mp += 8;
		Tempo(0, 1000000);
		for (k = 0; k < 32; k++)
		{
			unsigned ch = pattern == 2 ? 9 : k % 16;
			if (pattern) Msg(0, 144 | ch, 40 + k, 1 + k * 3);
		}
		for (k = 0; k < 200; k++)
		{
			unsigned dt = spacing[k % (sizeof spacing / sizeof *spacing)], ch = pattern == 2 ? 9 : k % 16;
			if (pattern == 0) Tempo(dt, k & 1 ? 999983 : 1000000);
			else if (k % 6 == 0) Msg(dt, 192 | ch, k % 128, 0);
			else if (k % 6 == 1) Msg(dt, 176 | ch, 10, k % 128);
			else if (k % 6 == 2) Msg(dt, 176 | ch, 7, k % 128);
			else if (k % 6 == 3) Msg(dt, 128 | ch, 40 + k % 32, 0);
			else if (k % 6 == 4) Msg(dt, 144 | ch, 40 + k % 32, 100);
			else Msg(dt, 176 | ch, pattern == 3 ? 120 : 123, 0);
		}
		VLQ(1); Put(255); Put(47); Put(0); BE32(midi + off + 4, (unsigned)(mp - off - 8));
		s->data = midi; s->length = mp; in.user = &trace; in.size = s->length; in.read_at = CallbackRead;
		s->which = 0; a = Ref_MIDIOpen(&in); s->which = 1; b = PS2_MIDIOpen(&in); CHECK(a && b);
		trace.midi[0] = a; trace.midi[1] = b;
		MidiState(a, b, s);
		if (fail) s->fail_at = s->length / 2;
		for (k = 0; k < 220; k++)
		{
			MidiRead(a, b, s, blocks[k % (sizeof blocks / sizeof *blocks)]);
			CHECK(trace.callback_state[0] == trace.callback_state[1]);
		}
		Ref_MIDIClose(a); PS2_MIDIClose(b);
	}
	printf("MIDI complete input-callback state, empty/sine/other/percussion/stealing/release tails and deferred input failures PASS\n");
}

static void BenchDensity(void)
{
	static const unsigned spacings[] = {1, 16};
	unsigned density, k, repeat;
	context = "MIDI dense-event benchmark";
	for (density = 0; density < 2; density++)
	{
		source s = {0}; ps2_audio_input in; ps2_midi *a, *b; int16_t out[1024]; double ta[5], tb[5];
		size_t off;
		memcpy(midi, "MThd\0\0\0\6", 8); BE16(midi + 8, 0); BE16(midi + 10, 1); BE16(midi + 12, PS2_AUDIO_RATE); mp = 14;
		off = mp; memcpy(midi + mp, "MTrk", 4); mp += 8;
		Tempo(0, 1000000); // one tick per output frame
		for (k = 0; k < 24; k++) Msg(0, 144 | (k % 16), 48 + k, 100);
		for (k = 0; k < 16384 / spacings[density]; k++) Msg(spacings[density], 176 | (k % 16), 11, k % 128);
		VLQ(PS2_AUDIO_RATE); Put(255); Put(47); Put(0); BE32(midi + off + 4, (unsigned)(mp - off - 8));
		s.data = midi; s.length = mp; in.user = &s; in.size = s.length; in.read_at = Read;
		s.which = 0; a = Ref_MIDIOpen(&in); s.which = 1; b = PS2_MIDIOpen(&in); CHECK(a && b);
		MidiState(a, b, &s);
		for (k = 0; k < 32; k++) MidiRead(a, b, &s, 512);
		for (repeat = 0; repeat < 5; repeat++)
		{
			double t;
			s.which = 0; CHECK(Ref_MIDISeek(a, 0)); t = Clock(); for (k = 0; k < 32; k++) Ref_MIDIRead(a, out, 512); ta[repeat] = Clock() - t;
			s.which = 1; CHECK(PS2_MIDISeek(b, 0)); t = Clock(); for (k = 0; k < 32; k++) PS2_MIDIRead(b, out, 512); tb[repeat] = Clock() - t;
		}
		for (k = 1; k < 5; k++) { if (ta[k] < ta[0]) ta[0] = ta[k]; if (tb[k] < tb[0]) tb[0] = tb[k]; }
		printf("%s MIDI voices=24 event_spacing=%u frames=16384 baseline_ms=%.3f candidate_ms=%.3f reduction=%.2f%%\n",
			TEST_PLATFORM, spacings[density], ta[0] * 1000, tb[0] * 1000, (ta[0] - tb[0]) / ta[0] * 100);
		Ref_MIDIClose(a); PS2_MIDIClose(b);
	}
}

static uint32_t Counter(void)
{
#ifdef _EE
	uint32_t now;
	__asm__ volatile("mfc0 %0,$9" : "=r"(now));
	return now;
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((uint64_t)now.tv_sec * 1000000000u + (unsigned)now.tv_nsec);
#endif
}

static void BenchMusicRow(unsigned bits, unsigned channels, unsigned rate, float speed, size_t block)
{
	static unsigned char bytes[44 + 4097 * 4];
	int16_t out[1024];
	source s = {0};
	pair p;
	unsigned k, repeat, which, calls = block == 1 ? 4096 : block == 7 ? 586 : 16;
	uint32_t best[2] = {UINT32_MAX, UINT32_MAX};
	unsigned step;
	context = "music buffered-refill timing row";
	s.data = bytes; s.length = MakeWav(bytes, bits, channels, rate, 4097);
	CHECK(block <= 512 && Open(&p, &s));
	SAME(&p, Ref_MusicPlay(p.a, 1), PS2_MusicPlay(p.b, 1));
	// Zero is a fixture sentinel: keep MusicOpen's exact integer default step.
	// EE float speed-setting can truncate even a unit ratio to step 65535.
	if (speed) SAME(&p, Ref_MusicSpeed(p.a, speed), PS2_MusicSpeed(p.b, speed));
	step = PS2_TestMusicStep(p.b);
	for (k = 0; k < calls; k++) Render(&p, block);
	if (!getenv("PS2_AUDIO_NO_BENCH"))
	{
		for (repeat = 0; repeat < 5; repeat++) for (which = 0; which < 2; which++)
		{
			uint32_t start, elapsed;
			s.which = which;
			CHECK(which ? PS2_MusicPlay(p.b, 1) : Ref_MusicPlay(p.a, 1));
			start = Counter();
			for (k = 0; k < calls; k++)
			{
				if (which) PS2_MusicRender(p.b, out, block);
				else Ref_MusicRender(p.a, out, block);
			}
			elapsed = Counter() - start;
			if (elapsed < best[which]) best[which] = elapsed;
		}
		printf("MUSIC6 TIME bits=%u channels=%u rate=%u speed=%.3f step=%u block=%zu frames=%u baseline=%u candidate=%u\n",
			bits, channels, rate, speed, step, block, calls * (unsigned)block, (unsigned)best[0], (unsigned)best[1]);
	}
	Close(&p);
}

static void BenchMusic(void)
{
	static const unsigned rates[] = {4000, 11025, 22050, 24000, 44100, 48000, 96000};
	static const float speeds[] = {0.25f, 0.5f, 1.0f, 1.375f, 2.0f, 4.0f};
	unsigned bits, channels, r, speed;
	for (bits = 8; bits <= 16; bits += 8) for (channels = 1; channels <= 2; channels++)
	for (r = 0; r < sizeof rates / sizeof *rates; r++)
	for (speed = 0; speed < sizeof speeds / sizeof *speeds; speed++)
		BenchMusicRow(bits, channels, rates[r], speeds[speed], 512);
	for (r = 1; r <= 3; r++)
	{
		BenchMusicRow(16, 2, rates[r], 1.0f, 1);
		BenchMusicRow(16, 2, rates[r], 1.0f, 7);
	}
	printf("MUSIC6 buffered/refill PCM, complete private state, reads, rate/speed/small-call rows PASS\n");
}

static void NativeMusic(void)
{
	unsigned bits, channels;
	for (bits = 8; bits <= 16; bits += 8) for (channels = 1; channels <= 2; channels++)
		BenchMusicRow(bits, channels, PS2_AUDIO_RATE, 0.0f, 512);
	BenchMusicRow(16, 2, PS2_AUDIO_RATE, 0.0f, 1);
	BenchMusicRow(16, 2, PS2_AUDIO_RATE, 0.0f, 7);
	printf("NATIVE6 DONE checks=%u frames=%llu PASS\n", checks, pcm_frames);
}

int main(int argc, char **argv)
{
	int i;
	unsigned bench_only = argc > 1 && !strcmp(argv[1], "--benchmark-only");
	unsigned state_negative = argc > 1 && !strcmp(argv[1], "--state-negative-control");
	unsigned native_only = argc > 1 && !strcmp(argv[1], "--native-only");
	if (argc > 1 && !strcmp(argv[1], "--negative-control")) negative_control = 1;
#ifdef _EE
	if (argc && !strcmp(argv[0], "--negative-control")) negative_control = 1;
	if (argc && !strcmp(argv[0], "--state-negative-control")) state_negative = 1;
	if (argc && !strcmp(argv[0], "--benchmark-only")) bench_only = 1;
	if (argc && !strcmp(argv[0], "--native-only")) native_only = 1;
#endif
	if (native_only) { NativeMusic(); return 0; }
	if (bench_only) { Bench(); BenchDensity(); BenchMusic(); printf("Audio refill next6: benchmark-only PASS\n"); return 0; }
	if (state_negative)
	{
		unsigned char bytes[44 + 2053 * 4]; source s = {0}; pair p;
		context = "private music cursor bit-flip negative control";
		s.data = bytes; s.length = MakeWav(bytes, 16, 2, PS2_AUDIO_RATE, 2053);
		CHECK(Open(&p, &s));
		SAME(&p, Ref_MusicPlay(p.a, 0), PS2_MusicPlay(p.b, 0));
		Render(&p, 512);
		PS2_TestMusicFlipCursor(p.b); State(&p);
		return 2;
	}
	printf("%s unchanged music allocation: %zu -> %zu bytes\n", TEST_PLATFORM, Ref_TestMusicBytes(), PS2_TestMusicBytes());
	CHECK(Ref_TestMusicBytes() == PS2_TestMusicBytes());
	TrackTests(); GainTests(); MidiCallbackBoundaries(); WavTests(); MidiTests();
	for (i = 1; i < argc && !negative_control; i++) FileTests(argv[i]);
	BenchMusic(); NativeMusic();
	context = "benchmark"; if (!getenv("PS2_AUDIO_NO_BENCH")) { Bench(); BenchDensity(); }
	printf("Audio refill next6: checks=%u frames=%llu files=%d PASS\n", checks, pcm_frames, negative_control ? 0 : argc - 1);
	return 0;
}
