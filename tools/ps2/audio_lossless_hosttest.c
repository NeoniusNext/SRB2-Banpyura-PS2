// Paired pre-change/current music and MIDI tests. Built by audio_lossless_check.py.
// The wrapper appends read-only private-state probes to copied production units;
// the baseline and candidate share the same input object and independent traces.
#include "ps2_music.h"
#include "ps2_midi.h"
#include "reference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _EE
#define TEST_PLATFORM "EE"
#else
#define TEST_PLATFORM "HOST"
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "audio-lossless:%d: %s [%s]\n", __LINE__, #x, context); exit(1); } } while (0)
static const char *context = "startup";
static unsigned checks, negative_control;
static unsigned long long pcm_frames;
size_t PS2_TestMusicBytes(void);
size_t Ref_TestMusicBytes(void);
size_t PS2_TestMusicState(const ps2_music *, uint64_t *);
size_t Ref_TestMusicState(const ps2_music *, uint64_t *);
size_t PS2_TestMidiState(const ps2_midi *, unsigned char *);
size_t Ref_TestMidiState(const ps2_midi *, unsigned char *);

typedef struct
{
	const unsigned char *data;
	size_t length, fail_at;
	unsigned which;
	uint64_t trace[2], reads[2];
} source;
typedef struct { source *s; ps2_music *a, *b; } pair;

static size_t Read(void *user, size_t off, void *dst, size_t n)
{
	source *s = user;
	unsigned w = s->which;
	s->trace[w] = (s->trace[w] * 16777619u ^ off) * 16777619u ^ n;
	s->reads[w]++;
	if (off > s->length || n > s->length - off) return 0;
	if (s->fail_at && off + n > s->fail_at)
	{
		if (off >= s->fail_at) return 0;
		n = s->fail_at - off;
	}
	memcpy(dst, s->data + off, n); return n;
}

static void State(pair *p)
{
	uint64_t a[40] = {0}, b[40] = {0};
	CHECK(Ref_TestMusicState(p->a, a) == PS2_TestMusicState(p->b, b));
	CHECK(!memcmp(a, b, sizeof a));
	CHECK(p->s->trace[0] == p->s->trace[1] && p->s->reads[0] == p->s->reads[1]);
	checks++;
}

static int Open(pair *p, source *s)
{
	ps2_audio_input in = {s, s->length, Read};
	p->s = s;
	s->which = 0; p->a = Ref_MusicOpen(&in);
	s->which = 1; p->b = PS2_MusicOpen(&in);
	CHECK((p->a != NULL) == (p->b != NULL));
	CHECK(s->trace[0] == s->trace[1] && s->reads[0] == s->reads[1]);
	if (p->a) State(p);
	return p->a != NULL;
}

static void Close(pair *p)
{
	p->s->which = 0; Ref_MusicClose(p->a);
	p->s->which = 1; PS2_MusicClose(p->b);
}

#define SAME(p, old, current) do { \
	uint64_t result_a, result_b; \
	(p)->s->which = 0; result_a = (uint64_t)(old); \
	(p)->s->which = 1; result_b = (uint64_t)(current); \
	CHECK(result_a == result_b); State(p); \
} while (0)

static void Render(pair *p, size_t n)
{
	int16_t a[2 * 2053 + 16], b[2 * 2053 + 16];
	size_t na, nb;
	CHECK(n <= 2053);
	memset(a, 0x5a, sizeof a); memset(b, 0x5a, sizeof b);
	p->s->which = 0; na = Ref_MusicRender(p->a, a + 4, n);
	p->s->which = 1; nb = PS2_MusicRender(p->b, b + 4, n);
	if (negative_control && checks > 20 && n) b[4] ^= 1;
	CHECK(na == nb && !memcmp(a, b, sizeof a));
	pcm_frames += na; State(p);
}

static void Exercise(source *s, int short_case)
{
	static const size_t blocks[] = {0, 1, 2, 7, 511, 512, 513, 1023, 1024, 1025, 2053};
	static const float speeds[] = {0.25f, 0.5f, 1.0f, 1.375f, 2.0f, 4.0f};
	pair p;
	unsigned i;
	uint32_t length;
	if (!Open(&p, s)) return;
	length = PS2_MusicLength(p.b);
	SAME(&p, Ref_MusicPlay(p.a, 0), PS2_MusicPlay(p.b, 0));
	for (i = 0; i < sizeof blocks / sizeof *blocks; i++) Render(&p, blocks[i]);
	s->which = 0; Ref_MusicPause(p.a, 1); s->which = 1; PS2_MusicPause(p.b, 1); State(&p); Render(&p, 513);
	s->which = 0; Ref_MusicPause(p.a, 0); s->which = 1; PS2_MusicPause(p.b, 0); State(&p);
	for (i = 0; i < sizeof speeds / sizeof *speeds; i++)
	{
		uint32_t ms = i & 1 ? length / 2 : 0;
		SAME(&p, Ref_MusicSeek(p.a, ms), PS2_MusicSeek(p.b, ms));
		SAME(&p, Ref_MusicSpeed(p.a, speeds[i]), PS2_MusicSpeed(p.b, speeds[i]));
		Render(&p, blocks[i + 4]);
	}
	SAME(&p, Ref_MusicSpeed(p.a, 1.0f), PS2_MusicSpeed(p.b, 1.0f));
	SAME(&p, Ref_MusicSetLoop(p.a, length / 2), PS2_MusicSetLoop(p.b, length / 2));
	SAME(&p, Ref_MusicPlay(p.a, 1), PS2_MusicPlay(p.b, 1));
	SAME(&p, Ref_MusicSeek(p.a, length ? length - 1 : 0), PS2_MusicSeek(p.b, length ? length - 1 : 0));
	Render(&p, 2053); Render(&p, 1025);
	SAME(&p, Ref_MusicSeek(p.a, length), PS2_MusicSeek(p.b, length)); Render(&p, 1024);
	SAME(&p, Ref_MusicSeek(p.a, length + 1), PS2_MusicSeek(p.b, length + 1));
	SAME(&p, Ref_MusicPlay(p.a, 0), PS2_MusicPlay(p.b, 0));
	SAME(&p, Ref_MusicSeek(p.a, length ? length - 1 : 0), PS2_MusicSeek(p.b, length ? length - 1 : 0));
	Render(&p, 2053); Render(&p, 1);
	s->which = 0; Ref_MusicStop(p.a); s->which = 1; PS2_MusicStop(p.b); State(&p); Render(&p, 512);
	SAME(&p, Ref_MusicPlay(p.a, 0), PS2_MusicPlay(p.b, 0));
	SAME(&p, Ref_MusicCanSkip(p.a, 512), PS2_MusicCanSkip(p.b, 512));
	SAME(&p, Ref_MusicSkip(p.a, 512), PS2_MusicSkip(p.b, 512)); Render(&p, 1025);
	if (short_case)
	{
		SAME(&p, Ref_MusicPlay(p.a, 0), PS2_MusicPlay(p.b, 0));
		s->fail_at = s->length / 2;
		Render(&p, 2053); Render(&p, 2053);
		s->fail_at = 0;
		SAME(&p, Ref_MusicPlay(p.a, 0), PS2_MusicPlay(p.b, 0)); Render(&p, 1025);
	}
	Close(&p);
}

static void LE16(unsigned char *p, unsigned n) { p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8); }
static void LE32(unsigned char *p, unsigned n) { LE16(p, n); LE16(p + 2, n >> 16); }
static void BE16(unsigned char *p, unsigned n) { p[0] = (unsigned char)(n >> 8); p[1] = (unsigned char)n; }
static void BE32(unsigned char *p, unsigned n) { BE16(p, n >> 16); BE16(p + 2, n); }
static unsigned random_state = 0x2416109u;
static unsigned Rnd(void) { random_state ^= random_state << 13; random_state ^= random_state >> 17; random_state ^= random_state << 5; return random_state; }

static size_t MakeWav(unsigned char *b, unsigned bits, unsigned channels, unsigned rate, unsigned frames)
{
	size_t i, bytes = frames * channels * (bits / 8);
	memset(b, 0, 44);
	memcpy(b, "RIFF", 4); LE32(b + 4, (unsigned)(36 + bytes));
	memcpy(b + 8, "WAVEfmt ", 8); LE32(b + 16, 16); LE16(b + 20, 1); LE16(b + 22, channels);
	LE32(b + 24, rate); LE32(b + 28, rate * channels * (bits / 8)); LE16(b + 32, channels * (bits / 8)); LE16(b + 34, bits);
	memcpy(b + 36, "data", 4); LE32(b + 40, (unsigned)bytes);
	for (i = 0; i < bytes; i++) b[44 + i] = (unsigned char)(i % 5 ? Rnd() : (i & 1 ? 0xff : 0x80));
	return 44 + bytes;
}

static void WavTests(void)
{
	static const unsigned rates[] = {4000, 8000, 11025, 22050, 44100, 96000};
	static const unsigned sizes[] = {1, 2, 1023, 1024, 1025, 2053, 4097};
	unsigned char b[44 + 4097 * 4];
	unsigned bits, channels, rate, size, cases = 0;
	context = "synthetic WAV";
	for (bits = 8; bits <= 16; bits += 8) for (channels = 1; channels <= 2; channels++)
	for (rate = 0; rate < sizeof rates / sizeof *rates; rate++) for (size = 0; size < sizeof sizes / sizeof *sizes; size++)
	{
		source s = {0}; s.data = b; s.length = MakeWav(b, bits, channels, rates[rate], sizes[size]);
		Exercise(&s, 1); cases++;
	}
	printf("WAV: %u format/rate/length cases, extremes, backward expansion, speed/loop/seek/short reads PASS\n", cases);
}

static unsigned char midi[262144];
static size_t mp;
static void Put(unsigned n) { CHECK(mp < sizeof midi); midi[mp++] = (unsigned char)n; }
static void VLQ(unsigned n)
{
	unsigned char v[4]; unsigned k = 0;
	v[k++] = n & 127; while ((n >>= 7) != 0) { CHECK(k < 4); v[k++] = (n & 127) | 128; }
	while (k) Put(v[--k]);
}
static void Msg(unsigned dt, unsigned status, unsigned a, unsigned b)
{ VLQ(dt); Put(status); Put(a); if ((status >> 4) != 12 && (status >> 4) != 13) Put(b); }
static void Tempo(unsigned dt, unsigned n)
{ VLQ(dt); Put(255); Put(81); Put(3); Put(n >> 16); Put(n >> 8); Put(n); }
static size_t MakeMidi(unsigned tracks, unsigned division, unsigned variant)
{
	unsigned t, i;
	memcpy(midi, "MThd\0\0\0\6", 8); BE16(midi + 8, tracks == 1 ? 0 : 1); BE16(midi + 10, tracks); BE16(midi + 12, division); mp = 14;
	for (t = 0; t < tracks; t++)
	{
		size_t off = mp;
		unsigned ch = t & 15;
		memcpy(midi + mp, "MTrk", 4); mp += 8;
		if (!t) { Tempo(0, variant == 3 ? 1 : 500001); Tempo(0, variant == 3 ? 1 : 500003); }
		Msg(0, 192 | ch, t * 9 % 128, 0);
		Msg(0, 176 | ch, 7, 127); Msg(0, 176 | ch, 10, t * 37 % 128);
		Msg(0, 144 | ch, 48 + t % 32, 100);
		// Running status, simultaneous and non-integral-frame event times.
		VLQ(0); Put(72 + t % 32); Put(127);
		for (i = 0; i < (variant == 2 && !t ? 4200u : 24u); i++)
		{
			VLQ(variant == 2 ? 0 : i % 3); Put(48 + (i + t) % 60); Put(i & 1 ? 0 : 1 + i % 127);
		}
		Msg(1, 176 | ch, 64, 127); Msg(0, 128 | ch, 72 + t % 32, 0);
		Msg(0, 224 | ch, 127, 127); Msg(1, 176 | ch, 11, 0);
		if (!t) { Tempo(1, variant == 1 ? 0xffffff : 333337); Tempo(0, variant == 3 ? 1 : 250001); }
		Msg(1, 176 | ch, 121, 0); Msg(1, 176 | ch, 64, 0);
		VLQ(0); Put(240); VLQ(3); Put(1); Put(2); Put(247);
		VLQ(0); Put(255); Put(1); VLQ(5); for (i = 0; i < 5; i++) Put(64 + i);
		Msg(1, 176 | ch, 123, 0); Msg(1, 176 | ch, 120, 0);
		VLQ(variant == 3 ? 0xfffffff : division / 3 + 1); Put(255); Put(47); Put(0);
		BE32(midi + off + 4, (unsigned)(mp - off - 8));
	}
	return mp;
}

static void MidiState(ps2_midi *a, ps2_midi *b, source *s)
{
	unsigned char sa[16384], sb[16384];
	size_t na = Ref_TestMidiState(a, sa), nb = PS2_TestMidiState(b, sb);
	CHECK(na == nb && na <= sizeof sa && !memcmp(sa, sb, na));
	CHECK(s->trace[0] == s->trace[1] && s->reads[0] == s->reads[1]); checks++;
}

static void MidiRead(ps2_midi *a, ps2_midi *b, source *s, size_t frames)
{
	int16_t oa[4112], ob[4112]; size_t na, nb;
	CHECK(frames <= 2053); memset(oa, 0x3c, sizeof oa); memset(ob, 0x3c, sizeof ob);
	s->which = 0; na = Ref_MIDIRead(a, oa + 2, frames);
	s->which = 1; nb = PS2_MIDIRead(b, ob + 2, frames);
	CHECK(na == nb && !memcmp(oa, ob, sizeof oa)); pcm_frames += na; MidiState(a, b, s);
}

static void DirectMidi(source *s)
{
	ps2_audio_input in = {s, s->length, Read};
	ps2_midi *a, *b;
	static const unsigned blocks[] = {0, 1, 7, 511, 512, 1023, 1024, 1025, 2053};
	uint32_t ms[7]; unsigned i;
	s->which = 0; a = Ref_MIDIOpen(&in); s->which = 1; b = PS2_MIDIOpen(&in);
	CHECK((a != NULL) == (b != NULL)); if (!a) return;
	MidiState(a, b, s);
	for (i = 0; i < 32; i++) MidiRead(a, b, s, blocks[i % 9]);
	ms[0] = 0; ms[1] = 1; ms[2] = 299; ms[3] = 997; ms[4] = PS2_MIDILength(b) - 1; ms[5] = PS2_MIDILength(b); ms[6] = ms[5] + 1;
	for (i = 0; i < sizeof ms / sizeof *ms; i++)
	{
		int ra, rb;
		s->which = 0; ra = Ref_MIDISeek(a, ms[i]); s->which = 1; rb = PS2_MIDISeek(b, ms[i]); CHECK(ra == rb);
		MidiState(a, b, s); MidiRead(a, b, s, blocks[i + 2]);
	}
	Ref_MIDIClose(a); PS2_MIDIClose(b);
}

static void MidiTests(void)
{
	static const unsigned tracks[] = {1, 2, 8, 32}, divisions[] = {1, 7, 96, 32767};
	unsigned t, d, v, cases = 0, i;
	context = "synthetic MIDI";
	for (t = 0; t < 4; t++) for (d = 0; d < 4; d++) for (v = 0; v < 4; v++)
	{
		source s = {0}; s.data = midi; s.length = MakeMidi(tracks[t], divisions[d], v);
		DirectMidi(&s); Exercise(&s, 0); cases++;
	}
	// Rejection equivalence across truncation, bad statuses, tempo and VLQ payloads.
	MakeMidi(2, 96, 0);
	for (i = 0; i < 512; i++)
	{
		source s = {0}; pair p;
		s.data = midi; s.length = i < mp ? i : mp;
		if (Open(&p, &s)) Close(&p);
	}
	for (i = 0; i < 100; i++)
	{
		unsigned at = 14 + Rnd() % (unsigned)(mp - 14), old = midi[at];
		source s = {0}; pair p;
		midi[at] = (unsigned char)Rnd(); s.data = midi; s.length = mp;
		if (Open(&p, &s)) { SAME(&p, Ref_MusicPlay(p.a, 0), PS2_MusicPlay(p.b, 0)); Render(&p, 1025); Close(&p); }
		midi[at] = (unsigned char)old;
	}
	printf("MIDI: %u track/division/event cases, complete private state, same-tick order, tempo, budget, seek, malformed input PASS\n", cases);
}

static void FileTests(const char *path)
{
	FILE *f = fopen(path, "rb"); long length; unsigned char *bytes; source s = {0};
	context = path; CHECK(f != NULL); CHECK(!fseek(f, 0, SEEK_END)); length = ftell(f); CHECK(length > 0);
	bytes = malloc((size_t)length); CHECK(bytes != NULL); CHECK(!fseek(f, 0, SEEK_SET)); CHECK(fread(bytes, 1, (size_t)length, f) == (size_t)length); fclose(f);
	s.data = bytes; s.length = (size_t)length;
	if (!memcmp(bytes, "MThd", 4)) DirectMidi(&s);
	Exercise(&s, 0); free(bytes);
}

static double Clock(void)
{
#ifdef _EE
	static uint32_t last;
	static uint64_t high;
	uint32_t now;
	__asm__ volatile("mfc0 %0,$9" : "=r"(now));
	if (now < last) high += (uint64_t)1 << 32;
	last = now;
	return (double)(high + now) / 294912000.0;
#else
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9;
#endif
}
static void Bench(void)
{
	static const unsigned counts[] = {1, 4, 16, 32};
	unsigned c, k, repeat;
	context = "MIDI benchmark";
	for (c = 0; c < sizeof counts / sizeof *counts; c++)
	{
		source s = {0}; ps2_audio_input in; ps2_midi *a, *b; int16_t out[1024]; double ta[5], tb[5];
		unsigned count = counts[c];
		memcpy(midi, "MThd\0\0\0\6", 8); BE16(midi + 8, count == 1 ? 0 : 1); BE16(midi + 10, count); BE16(midi + 12, 96); mp = 14;
		for (k = 0; k < count; k++)
		{
			size_t off = mp;
			memcpy(midi + mp, "MTrk", 4); mp += 8;
			Msg(0, 144 | (k & 15), 48 + k % 32, 100);
			VLQ(960); Put(255); Put(47); Put(0); // sustain for five seconds
			BE32(midi + off + 4, (unsigned)(mp - off - 8));
		}
		s.data = midi; s.length = mp; in.user = &s; in.size = s.length; in.read_at = Read;
		a = Ref_MIDIOpen(&in); b = PS2_MIDIOpen(&in); CHECK(a && b);
		for (repeat = 0; repeat < 5; repeat++)
		{
			double t;
			CHECK(Ref_MIDISeek(a, 0)); t = Clock(); for (k = 0; k < 128; k++) Ref_MIDIRead(a, out, 512); ta[repeat] = Clock() - t;
			CHECK(PS2_MIDISeek(b, 0)); t = Clock(); for (k = 0; k < 128; k++) PS2_MIDIRead(b, out, 512); tb[repeat] = Clock() - t;
		}
		// Minimum minimizes unrelated contention in the shared workspace.
		for (k = 1; k < 5; k++) { if (ta[k] < ta[0]) ta[0] = ta[k]; if (tb[k] < tb[0]) tb[0] = tb[k]; }
		printf("%s MIDI tracks=%u frames=65536 baseline_ms=%.3f candidate_ms=%.3f reduction=%.2f%%\n", TEST_PLATFORM, count, ta[0] * 1000, tb[0] * 1000, (ta[0] - tb[0]) / ta[0] * 100);
		Ref_MIDIClose(a); PS2_MIDIClose(b);
	}
}

int main(int argc, char **argv)
{
	int i;
	if (argc > 1 && !strcmp(argv[1], "--negative-control")) negative_control = 1;
#ifdef _EE
	if (argc && !strcmp(argv[0], "--negative-control")) negative_control = 1;
#endif
	printf("%s music allocation: %zu -> %zu bytes (%zu saved)\n", TEST_PLATFORM, Ref_TestMusicBytes(), PS2_TestMusicBytes(), Ref_TestMusicBytes() - PS2_TestMusicBytes());
	CHECK(Ref_TestMusicBytes() - PS2_TestMusicBytes() == 4096);
	WavTests(); MidiTests();
	for (i = 1; i < argc && !negative_control; i++) FileTests(argv[i]);
	context = "benchmark"; if (!getenv("PS2_AUDIO_NO_BENCH")) Bench();
	printf("Audio lossless: checks=%u frames=%llu files=%d PASS\n", checks, pcm_frames, negative_control ? 0 : argc - 1);
	return 0;
}
