// Reuse the complete prior transport, streaming MIDI and callback proof.
#define PS2_AUDIO_REFILL_NEXT6_NO_MAIN
#define TrackTests PreviousTrackTests
#define __wrap_calloc PreviousWrapCalloc
#include "audio_refill_next6_hosttest.c"
#undef TrackTests
#undef __wrap_calloc
#ifdef NEXT8_AUDIO_ASSETS
#include "next8_audio_assets.h"
#endif

void PS2_TestMidiFlipPhase(ps2_midi *);
unsigned PS2_TestMidiEnvelope(const ps2_midi *);
unsigned Ref_TestMidiEnvelope(const ps2_midi *);
size_t PS2_TestMidiChannelBytes(void);
size_t Ref_TestMidiChannelBytes(void);
size_t PS2_TestMidiNoteBytes(void);
size_t Ref_TestMidiNoteBytes(void);
void __real_free(void *);
static struct { unsigned char *base, *pointer; size_t bytes; } midi_guard[2];

static void MidiGuard(unsigned i)
{
	unsigned j;
	for (j = 0; j < 16; j++) CHECK(midi_guard[i].base[j] == 0x96 && midi_guard[i].pointer[midi_guard[i].bytes+j] == 0x69);
}

void *__wrap_calloc(size_t count, size_t bytes)
{
	unsigned i;
	unsigned char *base;
	if (!capture_calloc) return __real_calloc(count, bytes);
	calloc_calls++; calloc_bytes = count * bytes;
	if (fail_calloc) return NULL;
	for (i = 0; i < 2 && midi_guard[i].base; i++) {}
	CHECK(i < 2);
	base = __real_calloc(1, calloc_bytes + 32); CHECK(base != NULL);
	memset(base, 0x96, 16); memset(base + 16 + calloc_bytes, 0x69, 16);
	midi_guard[i].base = base; midi_guard[i].pointer = base + 16; midi_guard[i].bytes = calloc_bytes;
	return midi_guard[i].pointer;
}

void __wrap_free(void *pointer)
{
	unsigned i;
	for (i = 0; i < 2; i++) if (midi_guard[i].pointer && pointer == midi_guard[i].pointer)
	{
		MidiGuard(i); __real_free(midi_guard[i].base);
		memset(&midi_guard[i], 0, sizeof midi_guard[i]); return;
	}
	__real_free(pointer);
}

static void TrackTests(void)
{
	unsigned t;
	context = "MIDI private field widths and all track allocations";
	(void)PreviousTrackTests;
	CHECK(Ref_TestMidiTrackBytes() >= PS2_TestMidiTrackBytes());
	CHECK(Ref_TestMidiChannelBytes() >= PS2_TestMidiChannelBytes());
	CHECK(Ref_TestMidiNoteBytes() >= PS2_TestMidiNoteBytes());
	printf("AUDIO8 LAYOUT channel=%zu/%zu note=%zu/%zu track=%zu/%zu\n", Ref_TestMidiChannelBytes(), PS2_TestMidiChannelBytes(),
		Ref_TestMidiNoteBytes(), PS2_TestMidiNoteBytes(), Ref_TestMidiTrackBytes(), PS2_TestMidiTrackBytes());
	for (t = 1; t <= 32; t++)
	{
		source s = {0}; unsigned i;
		s.data = midi; s.length = MakeMidi(t, 96, 0);
		CHECK(Ref_TestMidiBytes(t) >= PS2_TestMidiBytes(t));
		Allocation(&s, t, 0); Allocation(&s, t, 1);
		for (i = 0; i < 2; i++) CHECK(!midi_guard[i].base);
		DirectMidi(&s); Exercise(&s, 0);
		printf("AUDIO8 ALLOCATION tracks=%u baseline=%zu candidate=%zu saved=%zu\n", t,
			Ref_TestMidiBytes(t), PS2_TestMidiBytes(t), Ref_TestMidiBytes(t) - PS2_TestMidiBytes(t));
	}
}

static void FieldBoundaries(void)
{
	unsigned k, value, track, format;
	static const unsigned controllers[] = {0, 7, 10, 11, 64, 120, 121, 123, 127};
	context = "MIDI narrowed fields, all controller values and running status";
	for (format = 0; format <= 1; format++)
	{
		source s = {0};
		memcpy(midi, "MThd\0\0\0\6", 8); BE16(midi + 8, format); BE16(midi + 10, format ? 2 : 1); BE16(midi + 12, PS2_AUDIO_RATE); mp = 14;
		for (track = 0; track < (format ? 2u : 1u); track++)
		{
			size_t off = mp; memcpy(midi + mp, "MTrk", 4); mp += 8; Tempo(0, 1000000);
			for (value = 0; value < 128; value++)
			{
				unsigned ch = value % 16;
				Msg(1, 192 | ch, value, 0); Msg(0, 224 | ch, value, 127-value);
				Msg(0, 144 | ch, value, value);
				// Explicit running status reuses both note-on and controller status.
				VLQ(0); Put(127-value); Put(127-value);
				for (k = 0; k < sizeof controllers / sizeof *controllers; k++)
				{
					Msg(1, 176 | ch, controllers[k], value);
					VLQ(0); Put(controllers[k]); Put(127-value);
				}
				Msg(1, 128 | ch, value, 127);
			}
			VLQ(PS2_AUDIO_RATE); Put(255); Put(47); Put(0); BE32(midi + off + 4, (unsigned)(mp-off-8));
		}
		s.data = midi; s.length = mp; DirectMidi(&s); Exercise(&s, 0);
	}
	context = "MIDI long seek ages and envelope boundaries";
	for (value = 0; value < 3; value++)
	{
		source s = {0}; ps2_audio_input in; ps2_midi *a, *b; size_t off;
		static const uint32_t seeks[] = {0, 1, 5, 6, 149, 150, 166, 167, 999, 1000000, 80000000, 0};
		memcpy(midi, "MThd\0\0\0\6", 8); BE16(midi+8, 0); BE16(midi+10, 1); BE16(midi+12, 1); mp = 14;
		off=mp; memcpy(midi+mp,"MTrk",4); mp+=8; Tempo(0, 1000000);
		Msg(0, 144 | (value == 2 ? 9 : 15), 127, 127);
		if (value == 1) { Msg(0, 176 | 15, 64, 127); Msg(1, 128 | 15, 127, 0); }
		VLQ(80000); Put(255); Put(47); Put(0); BE32(midi+off+4,(unsigned)(mp-off-8));
		s.data=midi; s.length=mp; in.user=&s; in.size=s.length; in.read_at=Read;
		s.which=0; a=Ref_MIDIOpen(&in); s.which=1; b=PS2_MIDIOpen(&in); CHECK(a&&b);
		for(k=0;k<sizeof seeks/sizeof *seeks;k++)
		{
			int x,y; s.which=0; x=Ref_MIDISeek(a,seeks[k]); s.which=1; y=PS2_MIDISeek(b,seeks[k]); CHECK(x==y);
			MidiState(a,b,&s); MidiRead(a,b,&s,1025);
		}
		Ref_MIDIClose(a); PS2_MIDIClose(b);
	}
	printf("AUDIO8 FIELD BOUNDARIES PASS\n");
}

typedef struct { callback_source trace; unsigned policy; } return_source;

static size_t ReturnCountRead(void *user, size_t off, void *dst, size_t n)
{
	return_source *s = user;
	size_t got = CallbackRead(&s->trace, off, dst, n);
	if (off < 22 || !s->policy) return got;
	if (s->policy == 1) return n + 1;
	if (s->policy == 2) return n + 65536;
	if (s->policy == 3) return SIZE_MAX;
	return s->policy == 4 && got ? got - 1 : 0;
}

static void ReturnCounts(void)
{
	unsigned policy;
	context = "MIDI complete callback return counts and failed state";
	for (policy = 1; policy <= 5; policy++)
	{
		return_source trace = {0}; source *s = &trace.trace.input;
		ps2_audio_input in; ps2_midi *a, *b; int x,y;
		s->data=midi; s->length=MakeMidi(2,96,0);
		in.user=&trace; in.size=s->length; in.read_at=ReturnCountRead;
		s->which=0; a=Ref_MIDIOpen(&in); s->which=1; b=PS2_MIDIOpen(&in); CHECK(a&&b);
		trace.trace.midi[0]=a; trace.trace.midi[1]=b; trace.policy=policy;
		s->which=0; x=Ref_MIDISeek(a,1); s->which=1; y=PS2_MIDISeek(b,1); CHECK(x==y&&!x);
		MidiState(a,b,s); MidiRead(a,b,s,2053);
		CHECK(trace.trace.callback_state[0]==trace.trace.callback_state[1]);
		trace.policy=0;
		s->which=0; x=Ref_MIDISeek(a,0); s->which=1; y=PS2_MIDISeek(b,0); CHECK(x==y&&x);
		MidiState(a,b,s); MidiRead(a,b,s,2053);
		Ref_MIDIClose(a); PS2_MIDIClose(b);
	}
	printf("AUDIO8 FULL RETURN COUNTS PASS\n");
}

static void ResumeAttack(void)
{
	source s={0}; ps2_audio_input in; ps2_midi *a,*b; size_t off;
	context="MIDI resumed attack above32768";
	memcpy(midi,"MThd\0\0\0\6",8); BE16(midi+8,0); BE16(midi+10,1); BE16(midi+12,PS2_AUDIO_RATE); mp=14;
	off=mp; memcpy(midi+mp,"MTrk",4); mp+=8; Tempo(0,1000000);
	Msg(0,144,60,127); Msg(129,128,60,0); Msg(1,176,64,127); Msg(0,128,60,0); Msg(1,176,64,0);
	VLQ(PS2_AUDIO_RATE); Put(255); Put(47); Put(0); BE32(midi+off+4,(unsigned)(mp-off-8));
	s.data=midi; s.length=mp; in.user=&s; in.size=s.length; in.read_at=Read;
	s.which=0; a=Ref_MIDIOpen(&in); s.which=1; b=PS2_MIDIOpen(&in); CHECK(a&&b);
	MidiRead(a,b,&s,128); CHECK(Ref_TestMidiEnvelope(a)==32768&&PS2_TestMidiEnvelope(b)==32768);
	MidiRead(a,b,&s,1); CHECK(Ref_TestMidiEnvelope(a)==32736&&PS2_TestMidiEnvelope(b)==32736);
	MidiRead(a,b,&s,1); CHECK(Ref_TestMidiEnvelope(a)==32992&&PS2_TestMidiEnvelope(b)==32992);
	MidiRead(a,b,&s,1); CHECK(Ref_TestMidiEnvelope(a)==32960&&PS2_TestMidiEnvelope(b)==32960);
	MidiRead(a,b,&s,2053); Ref_MIDIClose(a); PS2_MIDIClose(b);
	printf("AUDIO8 RESUMED ATTACK32992 PASS\n");
}

int main(int argc, char **argv)
{
	int i;
	unsigned state_negative = argc > 1 && !strcmp(argv[1], "--state-negative-control");
	if (argc > 1 && !strcmp(argv[1], "--negative-control")) negative_control = 1;
#ifdef _EE
	if (argc && !strcmp(argv[0], "--negative-control")) negative_control = 1;
	if (argc && !strcmp(argv[0], "--state-negative-control")) state_negative = 1;
#endif
	if (state_negative)
	{
		source s = {0}; ps2_audio_input in; ps2_midi *a,*b;
		context="MIDI private phase negative control"; s.data=midi; s.length=MakeMidi(1,96,0);
		in.user=&s; in.size=s.length; in.read_at=Read;
		s.which=0; a=Ref_MIDIOpen(&in); s.which=1; b=PS2_MIDIOpen(&in); CHECK(a&&b);
		PS2_TestMidiFlipPhase(b); MidiState(a,b,&s); return 2;
	}
	TrackTests(); FieldBoundaries(); ReturnCounts(); ResumeAttack(); GainTests(); MidiCallbackBoundaries(); WavTests(); MidiTests();
	for (i=1;i<argc&&!negative_control;i++) FileTests(argv[i]);
#ifdef NEXT8_AUDIO_ASSETS
	for (i=0;i<(int)(sizeof next8_audio_assets/sizeof *next8_audio_assets);i++)
	{
		source s={0}; context=next8_audio_assets[i].name; s.data=next8_audio_assets[i].bytes; s.length=next8_audio_assets[i].length;
		Exercise(&s,1); if (s.length>=4&&!memcmp(s.data,"MThd",4)) DirectMidi(&s);
	}
#endif
	BenchMusic(); NativeMusic();
	context="benchmark"; if (!getenv("PS2_AUDIO_NO_BENCH")) { Bench(); BenchDensity(); }
	printf("AUDIO8 DONE checks=%u frames=%llu files=%d PASS\n",checks,pcm_frames,negative_control?0:argc-1);
	return 0;
}
