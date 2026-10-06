// Real decoder/mixer tests. CHECK remains enabled in release host builds.
#include "ps2_audio.h"
#include "ps2_music.h"
#include "ps2_midi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
typedef struct { const unsigned char *data; size_t length, calls, maxread, bytes; } input;
static size_t Read(void *user, size_t off, void *dst, size_t n)
{
	input *s = user;
	CHECK(off <= s->length && n <= s->length - off);
	s->calls++; s->bytes += n; if (n > s->maxread) s->maxread = n;
	memcpy(dst, s->data + off, n); return n;
}
static ps2_audio_input In(input *s, const unsigned char *p, size_t n)
{
	ps2_audio_input in;
	memset(s, 0, sizeof *s); s->data = p; s->length = n;
	in.user = s; in.size = n; in.read_at = Read; return in;
}
static void LE16(unsigned char *p, unsigned n) { p[0] = (unsigned char)n; p[1] = (unsigned char)(n>>8); }
static void LE32(unsigned char *p, unsigned n) { LE16(p,n); LE16(p+2,n>>16); }
static size_t Wave(unsigned char *p, unsigned rate, unsigned channels, unsigned bits, unsigned frames)
{
	unsigned n = frames * channels * bits/8;
	memset(p, 0, 56+n);
	memcpy(p, "RIFF", 4); LE32(p+4, 48+n); memcpy(p+8, "WAVEJUNK", 8);
	LE32(p+16,3); p[20] = 42; // odd-sized unknown chunk and pad
	memcpy(p+24,"fmt ",4); LE32(p+28,16); LE16(p+32,1); LE16(p+34,channels);
	LE32(p+36,rate); LE32(p+40,rate*channels*bits/8); LE16(p+44,channels*bits/8); LE16(p+46,bits);
	memcpy(p+48,"data",4); LE32(p+52,n); return 56+n;
}
static void PCMTests(void)
{
	unsigned char b[256], dmx[] = {3,0,0x22,0x56,4,0,0,0,0,128,255,64};
	input s;
	ps2_audio_input in = In(&s, dmx, sizeof dmx);
	ps2_sample sample;
	ps2_mixer m;
	int h, next;
	int16_t out[12];
	CHECK(PS2_ParsePCM(&in,&sample.pcm,1)); sample.bytes = dmx;
	CHECK(sample.pcm.frames == 4 && sample.pcm.rate == PS2_AUDIO_RATE);
	dmx[4] = 1; in.size = 9; CHECK(PS2_ParsePCM(&in,&sample.pcm,1));
	dmx[4] = 4; in.size = sizeof dmx;
	CHECK(!PS2_ParsePCM(&in,&sample.pcm,0)); CHECK(PS2_ParsePCM(&in,&sample.pcm,1));
	PS2_MixerInit(&m); h = PS2_MixerStart(&m,&sample,0,255,128,128); CHECK(h >= 0);
	PS2_MixerRender(&m,out,NULL,6,0);
	CHECK(out[0] == -32768 && out[1] == -32768 && out[2] == 0 && out[4] == 32512);
	CHECK(out[8] == 0 && out[10] == 0 && !PS2_MixerPlaying(&m,h));
	h = PS2_MixerStart(&m,&sample,0,255,0,128);
	PS2_MixerRender(&m,out,NULL,1,0); CHECK(out[0] == -32768 && out[1] == 0);
	next = PS2_MixerStart(&m,&sample,0,255,255,64); CHECK(next != h);
	PS2_MixerStop(&m,h); CHECK(PS2_MixerPlaying(&m,next));
	PS2_MixerRender(&m,out,NULL,2,0); CHECK(out[0] == 0 && out[1] == -32768 && out[3] == -32768);
	PS2_MixerParams(&m,next,0,128,255); PS2_MixerRender(&m,out,NULL,2,0); CHECK(out[0] == 0);
	PS2_MixerForget(&m,&sample); CHECK(!PS2_MixerPlaying(&m,next));
	CHECK(PS2_MixerStart(&m,&sample,-1,255,128,128) == -1);
	CHECK(PS2_MixerStart(&m,&sample,64,255,128,128) == -1);
	for (h = 0; h < 64; h++) PS2_MixerStart(&m,&sample,h,255,128,128);
	PS2_MixerRender(&m,out,NULL,1,0); CHECK(out[0] == -32768); // saturation, not wrap
	PS2_MixerInit(&m); out[0] = -20000; out[1] = 20000;
	PS2_MixerRender(&m,out,out,1,16384); CHECK(out[0] == -10000 && out[1] == 10000);
	in = In(&s,b,Wave(b,11025,2,16,4));
	LE16(b+56,12345); LE16(b+58,(unsigned)-12345);
	CHECK(PS2_ParsePCM(&in,&sample.pcm,1)); CHECK(sample.pcm.offset == 56 && sample.pcm.channels == 2);
	sample.bytes = b; PS2_MixerStart(&m,&sample,0,255,128,128);
	PS2_MixerRender(&m,out,NULL,2,0); CHECK(out[0] == 12345 && out[1] == -12345 && out[2] == 12345);
	LE16(b+44,7); CHECK(!PS2_ParsePCM(&in,&sample.pcm,0)); LE16(b+44,4);
	LE32(b+52,UINT_MAX); CHECK(!PS2_ParsePCM(&in,&sample.pcm,0));
	for (h = 0; h < 56; h++) { in.size = h; CHECK(!PS2_ParsePCM(&in,&sample.pcm,0)); }
	// Stock WAV compatibility: odd data with LIST immediately after it.
	Wave(b,22050,1,8,3); memcpy(b+59,"LIST",4); LE32(b+63,0); LE32(b+4,59);
	in = In(&s,b,67); CHECK(PS2_ParsePCM(&in,&sample.pcm,0) && sample.pcm.frames == 3);
	LE32(b+63,100); CHECK(!PS2_ParsePCM(&in,&sample.pcm,0));
	printf("PCM: DMX, chunked WAV, bounds, pan, pitch, clipping, stale handles PASS\n");
}

static void MusicTests(void)
{
	unsigned char *b = malloc(56+PS2_AUDIO_RATE*4);
	input s;
	ps2_audio_input in;
	ps2_music *m;
	int16_t out[1024];
	unsigned i;
	CHECK(b);
	in = In(&s,b,Wave(b,PS2_AUDIO_RATE,2,16,PS2_AUDIO_RATE));
	for (i = 0; i < PS2_AUDIO_RATE; i++) { LE16(b+56+i*4,10000+i%100); LE16(b+58+i*4,(unsigned)-10000); }
	m = PS2_MusicOpen(&in); CHECK(m && PS2_MusicType(m) == PS2_MUSIC_WAV);
	CHECK(s.bytes < 100 && PS2_MusicLength(m) == 1000); // no full-track read
	CHECK(!PS2_MusicPlaying(m) && PS2_MusicRender(m,out,512) == 0 && out[0] == 0);
	CHECK(PS2_MusicPlay(m,0)); CHECK(PS2_MusicRender(m,out,512) == 512 && out[0] == 10000 && out[1] == -10000);
	CHECK(PS2_MusicPosition(m) == 23);
	PS2_MusicPause(m,1); CHECK(PS2_MusicPlaying(m) && PS2_MusicPaused(m));
	CHECK(PS2_MusicRender(m,out,512) == 0 && PS2_MusicPosition(m) == 23);
	PS2_MusicPause(m,0); CHECK(PS2_MusicSeek(m,500)); CHECK(PS2_MusicPosition(m) == 500);
	CHECK(!PS2_MusicSeek(m,1001) && !PS2_MusicSpeed(m,0.0f));
	CHECK(PS2_MusicSpeed(m,2.0f)); PS2_MusicRender(m,out,512); CHECK(PS2_MusicPosition(m) == 546);
	CHECK(PS2_MusicSeek(m,990)); PS2_MusicRender(m,out,512); CHECK(!PS2_MusicPlaying(m));
	CHECK(PS2_MusicPlay(m,1) && PS2_MusicSetLoop(m,500) && !PS2_MusicSetLoop(m,1000));
	CHECK(PS2_MusicSeek(m,990)); CHECK(PS2_MusicRender(m,out,512) == 512 && PS2_MusicPlaying(m));
	CHECK(PS2_MusicPosition(m) >= 500 && PS2_MusicPosition(m) < 600);
	PS2_MusicStop(m); CHECK(!PS2_MusicPlaying(m) && !PS2_MusicPaused(m) && PS2_MusicPosition(m) == 0);
	CHECK(s.maxread <= 4096); PS2_MusicClose(m);
	memset(b,0,56); in.size = 56; CHECK(!PS2_MusicOpen(&in)); free(b);
	printf("Music: bounded WAV decoding, seek, speed, EOF, pause, loop, rejection PASS\n");
}

// Format 1: tempo conductor (500ms then 250ms/quarter), note track with running status.
static unsigned char midi[] = {
	'M','T','h','d',0,0,0,6,0,1,0,2,0,96,
	'M','T','r','k',0,0,0,18,
	0,255,81,3,7,161,32, 96,255,81,3,3,208,144, 96,255,47,0,
	'M','T','r','k',0,0,0,22,
	0,192,8, 0,144,69,100, 96,69,0, 0,144,72,100, 96,128,72,0, 0,255,47,0
};
static void MIDITests(void)
{
	input s;
	ps2_audio_input in = In(&s,midi,sizeof midi);
	ps2_music *m = PS2_MusicOpen(&in);
	int16_t a[1024], b[1024];
	unsigned i, nonzero = 0;
	CHECK(m && PS2_MusicType(m) == PS2_MUSIC_MIDI);
	CHECK(PS2_MusicLength(m) == 850); CHECK(s.maxread <= 256);
	CHECK(PS2_MusicPlay(m,0)); CHECK(PS2_MusicRender(m,a,512) == 512);
	for (i = 0; i < 1024; i++) if (a[i]) nonzero++;
	CHECK(nonzero > 800);
	// Compare seeking with actual uninterrupted synthesis, including phase
	// and envelope state. This catches a sequencer that merely moves a timer.
	CHECK(PS2_MusicPlay(m,0));
	for (i = 0; i < 12; i++) CHECK(PS2_MusicRender(m,b,512) == 512);
	CHECK(PS2_MusicRender(m,b,471) == 471); // exactly 300ms = 6615 frames
	CHECK(PS2_MusicRender(m,b,512) == 512);
	CHECK(PS2_MusicSeek(m,300)); CHECK(PS2_MusicRender(m,a,512) == 512);
	CHECK(!memcmp(a,b,sizeof a));
	CHECK(PS2_MusicSeek(m,300)); CHECK(PS2_MusicRender(m,a,512) == 512);
	CHECK(PS2_MusicSeek(m,300)); CHECK(PS2_MusicRender(m,b,512) == 512 && !memcmp(a,b,sizeof a));
	CHECK(PS2_MusicSeek(m,840)); PS2_MusicRender(m,a,512); CHECK(!PS2_MusicPlaying(m));
	CHECK(PS2_MusicPlay(m,1)); CHECK(PS2_MusicSeek(m,840)); CHECK(PS2_MusicRender(m,a,512) == 512);
	CHECK(PS2_MusicPlaying(m) && PS2_MusicPosition(m) < 100);
	PS2_MusicClose(m);
	// Format 0 uses the note track alone (default tempo).
	{
		unsigned char type0[14+8+22];
		memcpy(type0,midi,14); type0[9] = 0; type0[11] = 1;
		memcpy(type0+14,midi+40,30);
		in = In(&s,type0,sizeof type0); m = PS2_MusicOpen(&in);
		CHECK(m && PS2_MusicLength(m) == 1100); PS2_MusicClose(m);
	}
	in = In(&s,midi,sizeof midi);
	for (i = 0; i < sizeof midi; i++) { in.size = i; CHECK(!PS2_MusicOpen(&in)); }
	in.size = sizeof midi; midi[13] = 0; CHECK(!PS2_MusicOpen(&in)); midi[13] = 96;
	midi[9] = 2; CHECK(!PS2_MusicOpen(&in)); midi[9] = 1;
	midi[12] = 128; CHECK(!PS2_MusicOpen(&in)); midi[12] = 0;
	// Missing EOT, overlong VLQ and tempo payload truncation are failures.
	midi[sizeof midi-3] = 254; CHECK(!PS2_MusicOpen(&in)); midi[sizeof midi-3] = 255;
	midi[22] = 255; midi[23] = 255; midi[24] = 255; midi[25] = 255;
	CHECK(!PS2_MusicOpen(&in));
	printf("MIDI: type-1 track merge, tempo, running status, audible synthesis, deterministic seek, EOF/loop, truncation PASS\n");
}

int main(void) { PCMTests(); MusicTests(); MIDITests(); puts("PS2 audio host tests PASS"); return 0; }
