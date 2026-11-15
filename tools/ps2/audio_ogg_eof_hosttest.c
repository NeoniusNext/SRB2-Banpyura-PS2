// Real Ogg setup/reads, with a deterministic valid zero-frame decoder result.
// audio_lossless_check.py runs the unguarded baseline in a separate process.
#include "ps2_music.h"
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "ogg-eof:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned eof_reads;
long __real_ov_read_float(OggVorbis_File *, float ***, int, int *);
long __wrap_ov_read_float(OggVorbis_File *vf, float ***pcm, int frames, int *section)
{
	if (eof_reads)
	{
		eof_reads--; *pcm = NULL; *section = 0; return 0;
	}
	return __real_ov_read_float(vf, pcm, frames, section);
}

typedef struct { unsigned char *data; size_t length; } source;
static size_t Read(void *user, size_t off, void *dst, size_t n)
{
	source *s = user;
	if (off > s->length || n > s->length - off) return 0;
	memcpy(dst, s->data + off, n); return n;
}

int main(int argc, char **argv)
{
	FILE *f;
	long length;
	source s;
	ps2_audio_input in;
	ps2_music *m;
	int16_t out[1024], expect[1024];
	unsigned i;
	CHECK(argc == 2); f = fopen(argv[1], "rb"); CHECK(f != NULL);
	CHECK(!fseek(f, 0, SEEK_END)); length = ftell(f); CHECK(length > 0);
	s.length = (size_t)length; s.data = malloc(s.length); CHECK(s.data != NULL);
	CHECK(!fseek(f, 0, SEEK_SET)); CHECK(fread(s.data, 1, s.length, f) == s.length); fclose(f);
	in.user = &s; in.size = s.length; in.read_at = Read;
	m = PS2_MusicOpen(&in); CHECK(m != NULL && PS2_MusicType(m) == PS2_MUSIC_OGG);
	CHECK(PS2_MusicPlay(m, 0));
	memset(out, 0x5a, sizeof out); eof_reads = 1;
	fprintf(stderr, "forcing zero-frame Ogg result with NULL PCM\n");
	CHECK(PS2_MusicRender(m, out, 512) == 0);
	CHECK(!PS2_MusicPlaying(m) && !PS2_MusicError(m) && PS2_MusicPosition(m) == 0);
	for (i = 0; i < 1024; i++) CHECK(out[i] == 0);
	// Seeking an ended song and reviving it still produces its original PCM.
	CHECK(PS2_MusicSeek(m, 100)); PS2_MusicRevive(m);
	CHECK(PS2_MusicRender(m, expect, 512) == 512 && !PS2_MusicError(m));
	// A zero-frame result in a looping song seeks to the configured loop point
	// and then decodes the next block normally, preserving every output sample.
	CHECK(PS2_MusicPlay(m, 1)); CHECK(PS2_MusicSetLoop(m, 100)); eof_reads = 1;
	CHECK(PS2_MusicRender(m, out, 512) == 512 && PS2_MusicPlaying(m) && !PS2_MusicError(m));
	CHECK(!memcmp(out, expect, sizeof out));
	PS2_MusicClose(m); free(s.data);
	puts("Ogg NULL-PCM EOF: zero output, ended state, seek/revive and loop PCM PASS");
	return 0;
}
