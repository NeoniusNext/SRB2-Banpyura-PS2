// Reuse the preceding complete PCM/state/read matrices without its allocation
// size assertion: this candidate changes storage, not decoded-buffer behavior.
#define PS2_AUDIO_REFILL_NEXT6_NO_MAIN
#include "audio_refill_next6_hosttest.c"
#include <malloc.h>
#ifdef PS2_AUDIO_VORBIS
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>
#endif
#ifdef PS2_STORAGE_NEXT7_ASSETS
#include "audio_storage_next7_assets.h"
#endif

size_t PS2_TestMusicAllocationBytes(unsigned);
size_t Ref_TestMusicAllocationBytes(unsigned);
size_t PS2_TestOggBytes(void);
size_t Ref_TestOggBytes(void);
int PS2_TestMusicBufferZero(const ps2_music *);
int Ref_TestMusicBufferZero(const ps2_music *);

void *__real_memalign(size_t, size_t);
void __real_free(void *);
static unsigned capture_memalign, fail_memalign, memalign_calls, ogg_zero_checks;
static unsigned allocation_side = 2, ogg_open_calls[2], ogg_clear_calls[2], ogg_cleanup_checks;
static int ogg_open_result[2];
static size_t memalign_bytes;
static struct { unsigned char *base, *pointer; size_t bytes; } guarded[8];

static void Guards(unsigned i)
{
	unsigned k;
	for (k = 0; k < 16; k++)
		CHECK(guarded[i].base[k] == 0x96 && guarded[i].pointer[guarded[i].bytes + k] == 0x69);
}

void *__wrap_memalign(size_t alignment, size_t bytes)
{
	unsigned i;
	unsigned char *base;
	if (!capture_memalign) return __real_memalign(alignment, bytes);
	memalign_calls++; memalign_bytes = bytes;
	CHECK(alignment == 16);
	if (fail_memalign) return NULL;
	for (i = 0; i < 8 && guarded[i].base; i++) {}
	CHECK(i < 8);
	base = __real_memalign(alignment, bytes + 32); CHECK(base != NULL);
	memset(base, 0x96, 16); memset(base + 16, 0xa5, bytes); memset(base + 16 + bytes, 0x69, 16);
	guarded[i].base = base; guarded[i].pointer = base + 16; guarded[i].bytes = bytes;
	CHECK(!((uintptr_t)guarded[i].pointer & 15));
	return guarded[i].pointer;
}

void __wrap_free(void *pointer)
{
	unsigned i;
	for (i = 0; i < 8; i++) if (guarded[i].pointer && pointer == guarded[i].pointer)
	{
		Guards(i); __real_free(guarded[i].base);
		memset(&guarded[i], 0, sizeof guarded[i]); return;
	}
	__real_free(pointer);
}

#ifdef PS2_AUDIO_VORBIS
int __real_ov_open_callbacks(void *, OggVorbis_File *, const char *, long, ov_callbacks);
int __wrap_ov_open_callbacks(void *user, OggVorbis_File *ogg, const char *initial, long bytes, ov_callbacks callbacks)
{
	int result;
	if (capture_memalign)
	{
		const unsigned char *p = (const unsigned char *)ogg;
		size_t i;
		unsigned found = 0;
		for (i = 0; i < 8; i++) if (guarded[i].base && (uintptr_t)p >= (uintptr_t)guarded[i].pointer
			&& (uintptr_t)p - (uintptr_t)guarded[i].pointer <= guarded[i].bytes
			&& sizeof *ogg <= guarded[i].bytes - ((uintptr_t)p - (uintptr_t)guarded[i].pointer)) found = 1;
		CHECK(found);
		for (i = 0; i < sizeof *ogg; i++) CHECK(p[i] == 0);
		ogg_zero_checks++;
	}
	result = __real_ov_open_callbacks(user, ogg, initial, bytes, callbacks);
	if (allocation_side < 2) { ogg_open_calls[allocation_side]++; ogg_open_result[allocation_side] = result; }
	return result;
}
int __real_ov_clear(OggVorbis_File *);
int __wrap_ov_clear(OggVorbis_File *ogg)
{
	if (allocation_side < 2) ogg_clear_calls[allocation_side]++;
	return __real_ov_clear(ogg);
}
#endif

static void MusicAllocation(source *s, unsigned fail)
{
	ps2_audio_input in = {s, s->length, Read};
	ps2_music *m[2];
	unsigned w, i, ogg = s->length >= 4 && !memcmp(s->data, "OggS", 4);
	s->trace[0] = s->trace[1] = s->reads[0] = s->reads[1] = 0;
	for (w = 0; w < 2; w++)
	{
		allocation_side = w; ogg_open_calls[w] = ogg_clear_calls[w] = 0; ogg_open_result[w] = 0;
		s->which = w; capture_memalign = 1; fail_memalign = fail;
		memalign_calls = 0; memalign_bytes = 0;
		m[w] = w ? PS2_MusicOpen(&in) : Ref_MusicOpen(&in);
		capture_memalign = fail_memalign = 0;
		CHECK(memalign_calls == (s->length >= 12 ? 1u : 0u));
		if (memalign_calls)
			CHECK(memalign_bytes == (w ? PS2_TestMusicAllocationBytes(ogg) : Ref_TestMusicAllocationBytes(ogg)));
		if (fail) CHECK(m[w] == NULL);
		if (m[w]) CHECK(w ? PS2_TestMusicBufferZero(m[w]) : Ref_TestMusicBufferZero(m[w]));
		for (i = 0; i < 8; i++) if (guarded[i].base) Guards(i);
	}
	CHECK((m[0] != NULL) == (m[1] != NULL));
	CHECK(s->trace[0] == s->trace[1] && s->reads[0] == s->reads[1]);
	if (m[0]) { pair p = {s, m[0], m[1]}; State(&p); }
	s->which = allocation_side = 0; Ref_MusicClose(m[0]);
	s->which = allocation_side = 1; PS2_MusicClose(m[1]); allocation_side = 2;
	CHECK(ogg_open_calls[0] == ogg_open_calls[1] && ogg_open_result[0] == ogg_open_result[1]);
	CHECK(ogg_clear_calls[0] == ogg_clear_calls[1]);
	for (w = 0; w < 2; w++) CHECK(ogg_clear_calls[w] == (ogg_open_calls[w] && !ogg_open_result[w] ? 1u : 0u));
	ogg_cleanup_checks++;
	for (i = 0; i < 8; i++) CHECK(!guarded[i].base);
	checks++;
}

typedef struct
{
	source input;
	ps2_music *music[2];
	uint64_t callback_state[2];
} music_callback_source;

static size_t MusicCallbackRead(void *user, size_t off, void *dst, size_t n)
{
	music_callback_source *s = user;
	unsigned w = s->input.which;
	if (s->music[w])
	{
		uint64_t state[40] = {0}; size_t i, count;
		count = w ? PS2_TestMusicState(s->music[w], state) : Ref_TestMusicState(s->music[w], state);
		CHECK(count <= 40);
		for (i = 0; i < count; i++) s->callback_state[w] = s->callback_state[w] * 16777619u ^ state[i];
	}
	return Read(&s->input, off, dst, n);
}

static void MusicCallbacks(const source *input)
{
	static const unsigned blocks[] = {0, 1, 7, 511, 512, 513, 1023, 1024, 1025, 2053};
	static const float speeds[] = {0.25f, 0.5f, 1.0f, 1.375f, 2.0f, 4.0f};
	music_callback_source trace = {0}; source *s = &trace.input;
	ps2_audio_input in; pair p; unsigned k;
	s->data = input->data; s->length = input->length;
	in.user = &trace; in.size = s->length; in.read_at = MusicCallbackRead;
	s->which = 0; p.a = Ref_MusicOpen(&in); s->which = 1; p.b = PS2_MusicOpen(&in); p.s = s;
	CHECK((p.a != NULL) == (p.b != NULL));
	CHECK(s->trace[0] == s->trace[1] && s->reads[0] == s->reads[1]);
	if (!p.a) return;
	trace.music[0] = p.a; trace.music[1] = p.b;
	SAME(&p, Ref_MusicPlay(p.a, 1), PS2_MusicPlay(p.b, 1));
	for (k = 0; k < 36; k++)
	{
		if (!(k % 6)) SAME(&p, Ref_MusicSpeed(p.a, speeds[k / 6]), PS2_MusicSpeed(p.b, speeds[k / 6]));
		if (k == 12) SAME(&p, Ref_MusicSeek(p.a, PS2_MusicLength(p.b) / 2), PS2_MusicSeek(p.b, PS2_MusicLength(p.b) / 2));
		if (k == 18) SAME(&p, Ref_MusicSeek(p.a, PS2_MusicLength(p.b) - 1), PS2_MusicSeek(p.b, PS2_MusicLength(p.b) - 1));
		if (k == 24) s->fail_at = s->length / 2;
		Render(&p, blocks[k % (sizeof blocks / sizeof *blocks)]);
		CHECK(trace.callback_state[0] == trace.callback_state[1]);
	}
	Close(&p);
}

static void MusicAllocations(void)
{
	unsigned char bytes[44 + 2053 * 4];
	source s = {0};
	unsigned bits, channels, n;
	context = "music allocation requests, failures, redzones and full initialization";
	CHECK(Ref_TestOggBytes() == PS2_TestOggBytes());
	CHECK(PS2_TestMusicAllocationBytes(1) <= Ref_TestMusicAllocationBytes(1));
	CHECK(PS2_TestMusicAllocationBytes(0) <= Ref_TestMusicAllocationBytes(0));
	printf("%s MUSIC7 STORAGE baseline=%zu nonogg=%zu ogg=%zu oggstate=%zu saved=%zu\n", TEST_PLATFORM,
		Ref_TestMusicAllocationBytes(0), PS2_TestMusicAllocationBytes(0), PS2_TestMusicAllocationBytes(1),
		PS2_TestOggBytes(), Ref_TestMusicAllocationBytes(0) - PS2_TestMusicAllocationBytes(0));
	for (bits = 8; bits <= 16; bits += 8) for (channels = 1; channels <= 2; channels++)
	{
		s.data = bytes; s.length = MakeWav(bytes, bits, channels, PS2_AUDIO_RATE, 2053);
		MusicAllocation(&s, 0); MusicAllocation(&s, 1);
		MusicCallbacks(&s);
	}
	for (n = 1; n <= 32; n++)
	{
		s.data = midi; s.length = MakeMidi(n, 96, 0);
		MusicAllocation(&s, 0); MusicAllocation(&s, 1);
		MusicCallbacks(&s);
	}
	memset(bytes, 0, sizeof bytes); s.data = bytes;
	for (n = 0; n < 70; n++)
	{
		s.length = n;
		memcpy(bytes, n & 1 ? "OggS" : n & 2 ? "RIFF" : "MThd", 4);
		MusicAllocation(&s, 0); MusicAllocation(&s, 1);
	}
}

static void StorageFileTests(const char *path)
{
	FILE *file = fopen(path, "rb"); long length; unsigned char *bytes; source s = {0};
	context = path; CHECK(file != NULL); CHECK(!fseek(file, 0, SEEK_END));
	length = ftell(file); CHECK(length > 0); CHECK(!fseek(file, 0, SEEK_SET));
	bytes = malloc((size_t)length); CHECK(bytes != NULL);
	CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length); fclose(file);
	s.data = bytes; s.length = (size_t)length;
	MusicAllocation(&s, 0); MusicAllocation(&s, 1);
	MusicCallbacks(&s);
	free(bytes); FileTests(path);
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
		unsigned char bytes[44 + 2053 * 4]; source s = {0}; pair p;
		context = "private music cursor bit-flip negative control";
		s.data = bytes; s.length = MakeWav(bytes, 16, 2, PS2_AUDIO_RATE, 2053);
		CHECK(Open(&p, &s));
		SAME(&p, Ref_MusicPlay(p.a, 0), PS2_MusicPlay(p.b, 0)); Render(&p, 512);
		PS2_TestMusicFlipCursor(p.b); State(&p); return 2;
	}
	MusicAllocations(); TrackTests(); GainTests(); MidiCallbackBoundaries(); WavTests(); MidiTests();
	for (i = 1; i < argc && !negative_control; i++) StorageFileTests(argv[i]);
#ifdef PS2_STORAGE_NEXT7_ASSETS
	for (i = 0; i < (int)(sizeof music_assets7 / sizeof *music_assets7); i++)
	{
		source s = {0}; context = music_assets7[i].name;
		s.data = music_assets7[i].bytes; s.length = music_assets7[i].length;
		MusicAllocation(&s, 0); MusicAllocation(&s, 1); MusicCallbacks(&s); Exercise(&s, 1);
	}
#endif
	BenchMusic(); NativeMusic();
	context = "benchmark"; if (!getenv("PS2_AUDIO_NO_BENCH")) { Bench(); BenchDensity(); }
	printf("MUSIC7 DONE checks=%u frames=%llu files=%d oggzero=%u cleanup=%u PASS\n", checks, pcm_frames,
		negative_control ? 0 : argc - 1, ogg_zero_checks, ogg_cleanup_checks);
	return 0;
}
