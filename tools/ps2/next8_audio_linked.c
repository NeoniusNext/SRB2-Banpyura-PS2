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
	printf("LINKED8 FAIL row=%u %s\n", row_number, what);
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
	if ((codec == 7 || codec == 8)) printf("LINKED8 STAGE codec=%u open\n", codec);
	music = PS2_MusicOpen(&in);
	if ((codec == 7 || codec == 8)) printf("LINKED8 STAGE codec=%u play opened=%u\n", codec, music != NULL);
	if (!music || !PS2_MusicPlay(music, 1)) Fail("open/play");
	if (speed && !PS2_MusicSpeed(music, speed)) Fail("speed");
	if ((codec == 7 || codec == 8)) printf("LINKED8 STAGE codec=%u render\n", codec);
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
	printf("LINKED8 ROW id=%u codec=%u bits=%u channels=%u rate=%u speed=%.3f frames=%u count=%u pcm=%08x%08x state=%08x%08x reads=%u readhash=%08x%08x\n",
		row_number++, codec, bits, channels, rate, speed, frames, (unsigned)best,
		(unsigned)(pcm >> 32), (unsigned)pcm, (unsigned)(state >> 32), (unsigned)state, reads,
		(unsigned)(read_hash >> 32), (unsigned)read_hash);
	PS2_MusicClose(music);
}


static unsigned char midi8[262144];
static size_t midi8_pos;
static void Put8(unsigned value) { if(midi8_pos >= sizeof midi8) Fail("MIDI capacity"); midi8[midi8_pos++]=(unsigned char)value; }
static void BE8(unsigned char *p,unsigned n) { p[0]=(unsigned char)(n>>24);p[1]=(unsigned char)(n>>16);p[2]=(unsigned char)(n>>8);p[3]=(unsigned char)n; }
static void VLQ8(unsigned value) {
 unsigned char tmp[4];unsigned count=0;
 tmp[count++]=(unsigned char)(value&127);
 while((value>>=7)!=0) { if(count==4) Fail("MIDI VLQ");tmp[count++]=(unsigned char)((value&127)|128); }
 while(count)Put8(tmp[--count]);
}
static void Message8(unsigned delta,unsigned status,unsigned a,unsigned b) {
 VLQ8(delta);Put8(status);Put8(a);if((status>>4)!=12&&(status>>4)!=13)Put8(b);
}
static size_t MakeMidi8(unsigned voices,unsigned spacing,unsigned waveform) {
 unsigned i,ch;size_t start;
 memcpy(midi8,"MThd\0\0\0\6\0\0\0\1",12);midi8[12]=(unsigned char)(PS2_AUDIO_RATE>>8);midi8[13]=(unsigned char)PS2_AUDIO_RATE;midi8_pos=14;
 start=midi8_pos;memcpy(midi8+midi8_pos,"MTrk",4);midi8_pos+=8;
 VLQ8(0);Put8(255);Put8(81);Put8(3);Put8(15);Put8(66);Put8(64);
 for(i=0;i<voices;i++) {
  ch=waveform==4?9:i%16;
  Message8(0,192|ch,waveform*8,0);Message8(0,144|ch,48+i,100);
 }
 if(spacing)for(i=0;i<16384/spacing;i++) {
  ch=waveform==4?9:i%16;
  Message8(spacing,176|ch,11,i%128);
 }
 VLQ8(PS2_AUDIO_RATE);Put8(255);Put8(47);Put8(0);BE8(midi8+start+4,(unsigned)(midi8_pos-start-8));
 return midi8_pos;
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
	if (!mp3_only) {
		static const unsigned voices8[]={1,8,24},spacing8[]={0,1,16};
		unsigned voice,spacing,waveform;
		for (voice=0;voice<3;voice++) for(spacing=0;spacing<3;spacing++) for(waveform=0;waveform<5;waveform++) {
			size_t bytes8=MakeMidi8(voices8[voice],spacing8[spacing],waveform);
			Row(midi8,bytes8,0,2,PS2_AUDIO_RATE,0.0f,9+voice*15+spacing*5+waveform);
		}
	}
	printf("LINKED8 DONE rows=%u\n", row_number);
	return 0;
}
