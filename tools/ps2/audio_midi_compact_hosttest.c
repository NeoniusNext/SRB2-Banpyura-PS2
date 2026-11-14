// Reuse the established PCM, transport, codec, malformed-MIDI and read-trace
// cases; the iteration-two reference is the saved current source tree.
#define main AudioLosslessPreviousMain
#include "audio_lossless_hosttest.c"
#undef main

size_t PS2_TestMidiBytes(unsigned);
size_t Ref_TestMidiBytes(unsigned);
size_t PS2_TestMidiTrackBytes(void);
size_t Ref_TestMidiTrackBytes(void);
void PS2_TestMidiFlipPhase(ps2_midi *);

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
	CHECK(Ref_TestMidiBytes(32) >= PS2_TestMidiBytes(32));
	for (t = 1; t <= 32; t++)
	{
		source s = {0};
		s.data = midi; s.length = MakeMidi(t, 96, 0);
		CHECK(Ref_TestMidiBytes(t) - PS2_TestMidiBytes(t) ==
			(32 - t) * PS2_TestMidiTrackBytes() + Ref_TestMidiBytes(32) - PS2_TestMidiBytes(32));
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

int main(int argc, char **argv)
{
	int i;
	unsigned bench_only = argc > 1 && !strcmp(argv[1], "--benchmark-only");
	unsigned state_negative = argc > 1 && !strcmp(argv[1], "--state-negative-control");
	if (argc > 1 && !strcmp(argv[1], "--negative-control")) negative_control = 1;
#ifdef _EE
	if (argc && !strcmp(argv[0], "--negative-control")) negative_control = 1;
	if (argc && !strcmp(argv[0], "--state-negative-control")) state_negative = 1;
	if (argc && !strcmp(argv[0], "--benchmark-only")) bench_only = 1;
#endif
	if (bench_only) { Bench(); BenchDensity(); printf("Audio MIDI compact: benchmark-only PASS\n"); return 0; }
	if (state_negative)
	{
		source s = {0}; ps2_audio_input in; ps2_midi *a, *b;
		context = "private-state bit-flip negative control";
		s.data = midi; s.length = MakeMidi(1, 96, 0); in.user = &s; in.size = s.length; in.read_at = Read;
		s.which = 0; a = Ref_MIDIOpen(&in); s.which = 1; b = PS2_MIDIOpen(&in); CHECK(a && b);
		PS2_TestMidiFlipPhase(b); MidiState(a, b, &s);
		return 2;
	}
	printf("%s unchanged music allocation: %zu -> %zu bytes\n", TEST_PLATFORM, Ref_TestMusicBytes(), PS2_TestMusicBytes());
	CHECK(Ref_TestMusicBytes() == PS2_TestMusicBytes());
	TrackTests(); GainTests(); WavTests(); MidiTests();
	for (i = 1; i < argc && !negative_control; i++) FileTests(argv[i]);
	context = "benchmark"; if (!getenv("PS2_AUDIO_NO_BENCH")) { Bench(); BenchDensity(); }
	printf("Audio MIDI compact: checks=%u frames=%llu files=%d PASS\n", checks, pcm_frames, negative_control ? 0 : argc - 1);
	return 0;
}
