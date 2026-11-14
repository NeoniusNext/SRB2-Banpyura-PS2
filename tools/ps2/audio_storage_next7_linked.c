// Calls the already-generated 512-frame clone in a saved complete engine object.
#include "ps2_music.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "linked_codec7.h"

extern size_t Engine_MusicRender512(ps2_music *, int16_t *);

typedef struct
{
	const unsigned char *bytes;
	size_t length;
	uint64_t hash;
	unsigned reads;
} source;

static unsigned char wav[44 + 4097 * 4];
static unsigned row_number;
static unsigned negative_control;
static unsigned skip_mp3, mp3_only;

static void Fail(const char *what)
{
	printf("LINKED7 FAIL row=%u %s\n", row_number, what);
	exit(1);
}

static size_t Read(void *user, size_t offset, void *out, size_t bytes)
{
	source *s = user;
	s->hash = (s->hash * 16777619u ^ offset) * 16777619u ^ bytes;
	s->reads++;
	if (offset > s->length || bytes > s->length - offset) return 0;
	memcpy(out, s->bytes + offset, bytes);
	return bytes;
}

static void LE16(unsigned char *p, unsigned n) { p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8); }
static void LE32(unsigned char *p, unsigned n) { LE16(p, n); LE16(p + 2, n >> 16); }

static size_t MakeWav(unsigned bits, unsigned channels, unsigned rate)
{
	size_t i, bytes = 4097 * channels * (bits / 8);
	uint32_t random = 0x8719123u;
	memset(wav, 0, 44);
	memcpy(wav, "RIFF", 4); LE32(wav + 4, (unsigned)(36 + bytes));
	memcpy(wav + 8, "WAVEfmt ", 8); LE32(wav + 16, 16); LE16(wav + 20, 1); LE16(wav + 22, channels);
	LE32(wav + 24, rate); LE32(wav + 28, rate * channels * (bits / 8));
	LE16(wav + 32, channels * (bits / 8)); LE16(wav + 34, bits);
	memcpy(wav + 36, "data", 4); LE32(wav + 40, (unsigned)bytes);
	for (i = 0; i < bytes; i++)
	{
		random ^= random << 13; random ^= random >> 17; random ^= random << 5;
		wav[44 + i] = (unsigned char)(i % 5 ? random : i & 1 ? 255 : 128);
	}
	return 44 + bytes;
}

static uint32_t Count(void)
{
	uint32_t value;
	__asm__ volatile("mfc0 %0,$9" : "=r"(value));
	return value;
}

static void Row(const unsigned char *bytes, size_t length, unsigned bits, unsigned channels,
	unsigned rate, float speed, unsigned codec)
{
	source s = {bytes, length, 0, 0};
	ps2_audio_input in = {&s, length, Read};
	ps2_music *music;
	int16_t out[1024 + 16];
	unsigned repeat, call, i, frames = 0;
	uint32_t best = UINT32_MAX;
	uint64_t pcm = 14695981039346656037ull, state = 14695981039346656037ull, read_hash;
	unsigned reads;
	if (codec >= 7) printf("LINKED7 STAGE codec=%u open\n", codec);
	music = PS2_MusicOpen(&in);
	if (codec >= 7) printf("LINKED7 STAGE codec=%u play opened=%u\n", codec, music != NULL);
	if (!music || !PS2_MusicPlay(music, 1)) Fail("open/play");
	if (speed && !PS2_MusicSpeed(music, speed)) Fail("speed");
	if (codec >= 7) printf("LINKED7 STAGE codec=%u render\n", codec);
	for (call = 0; call < 16; call++)
	{
		size_t n;
		memset(out, 0x65, sizeof out);
		n = Engine_MusicRender512(music, out + 8);
		if (n != 512) Fail("render count");
		for (i = 0; i < 8; i++) if (out[i] != 0x6565 || out[1032 + i] != 0x6565) Fail("output canary");
		if (negative_control && !call) out[8] ^= 1;
		for (i = 8; i < 1032; i++) pcm = (pcm ^ (uint16_t)out[i]) * 1099511628211ull;
		state = (state ^ PS2_MusicPosition(music)) * 1099511628211ull;
		state = (state ^ PS2_MusicPlaying(music)) * 1099511628211ull;
		state = (state ^ PS2_MusicPaused(music)) * 1099511628211ull;
		state = (state ^ PS2_MusicError(music)) * 1099511628211ull;
		state = (state ^ PS2_MusicType(music)) * 1099511628211ull;
		state = (state ^ PS2_MusicChannels(music)) * 1099511628211ull;
		state = (state ^ PS2_MusicLength(music)) * 1099511628211ull;
		state = (state ^ PS2_MusicLoop(music)) * 1099511628211ull;
		frames += (unsigned)n;
	}
	read_hash = s.hash; reads = s.reads;
	for (repeat = 0; repeat < 5; repeat++)
	{
		uint32_t start, elapsed;
		if (!PS2_MusicPlay(music, 1)) Fail("repeat play");
		start = Count();
		for (call = 0; call < 16; call++) Engine_MusicRender512(music, out + 8);
		elapsed = Count() - start;
		if (elapsed < best) best = elapsed;
	}
	printf("LINKED7 ROW id=%u codec=%u bits=%u channels=%u rate=%u speed=%.3f frames=%u count=%u pcm=%08x%08x state=%08x%08x reads=%u readhash=%08x%08x\n",
		row_number++, codec, bits, channels, rate, speed, frames, (unsigned)best,
		(unsigned)(pcm >> 32), (unsigned)pcm, (unsigned)(state >> 32), (unsigned)state, reads,
		(unsigned)(read_hash >> 32), (unsigned)read_hash);
	PS2_MusicClose(music);
}

int __wrap_main(int argc, char **argv)
{
	static const unsigned rates[] = {4000, 11025, 22050, 24000, 44100, 48000, 96000};
	static const float speeds[] = {0.25f, 0.5f, 1.0f, 1.375f, 2.0f, 4.0f};
	unsigned bits, channels, r, s;
	int arg;
	for (arg = 0; arg < argc; arg++)
	{
		if (!strcmp(argv[arg], "--negative-control")) negative_control = 1;
		if (!strcmp(argv[arg], "--skip-mp3")) skip_mp3 = 1;
		if (!strcmp(argv[arg], "--mp3-only")) mp3_only = 1;
	}
	for (bits = 8; !mp3_only && bits <= 16; bits += 8) for (channels = 1; channels <= 2; channels++)
	{
		for (r = 0; r < sizeof rates / sizeof *rates; r++)
		for (s = 0; s < sizeof speeds / sizeof *speeds; s++)
		{
			size_t length = MakeWav(bits, channels, rates[r]);
			Row(wav, length, bits, channels, rates[r], speeds[s], 0);
		}
		Row(wav, MakeWav(bits, channels, PS2_AUDIO_RATE), bits, channels, PS2_AUDIO_RATE, 0.0f, 0);
	}
	for (r = 0; r < sizeof linked_inputs7 / sizeof *linked_inputs7; r++)
	{
		if ((skip_mp3 && linked_inputs7[r].codec >= 7) || (mp3_only && linked_inputs7[r].codec < 7)) continue;
		Row(linked_inputs7[r].bytes, linked_inputs7[r].length, 0,
			linked_inputs7[r].channels, linked_inputs7[r].rate, 0.0f, linked_inputs7[r].codec);
	}
	printf("LINKED7 DONE rows=%u\n", row_number);
	return 0;
}
